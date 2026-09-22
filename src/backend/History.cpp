#include "llm_rewriter/History.hpp"

#include "SecureFile.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <numeric>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace llm_rewriter {

namespace {

using Json = nlohmann::json;

std::string StringValue(const Json& object, const char* key) {
  if (!object.is_object()) return {};
  const auto it = object.find(key);
  return it != object.end() && it->is_string() ? it->get<std::string>() : "";
}

std::int64_t Int64Value(const Json& object, const char* key) {
  if (!object.is_object()) return 0;
  const auto it = object.find(key);
  if (it == object.end()) return 0;
  if (it->is_number_integer()) return it->get<std::int64_t>();
  if (it->is_number_unsigned()) {
    const auto value = it->get<std::uint64_t>();
    return value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
               ? std::numeric_limits<std::int64_t>::max()
               : static_cast<std::int64_t>(value);
  }
  return 0;
}

int IntValue(const Json& object, const char* key) {
  const auto value = Int64Value(object, key);
  if (value > std::numeric_limits<int>::max()) return std::numeric_limits<int>::max();
  if (value < std::numeric_limits<int>::min()) return std::numeric_limits<int>::min();
  return static_cast<int>(value);
}

bool BoolValue(const Json& object, const char* key) {
  if (!object.is_object()) return false;
  const auto it = object.find(key);
  return it != object.end() && it->is_boolean() && it->get<bool>();
}

bool IsTokenCharacter(unsigned char character) {
  return std::isalnum(character) || character >= 0x80 || character == '_';
}

std::vector<std::string> Tokenize(std::string_view value) {
  std::vector<std::string> tokens;
  std::string token;
  for (const unsigned char character : value) {
    if (!IsTokenCharacter(character)) {
      if (!token.empty()) {
        tokens.push_back(std::move(token));
        token.clear();
      }
      continue;
    }
    if (character >= 'A' && character <= 'Z') {
      token.push_back(static_cast<char>(character - 'A' + 'a'));
    } else {
      token.push_back(static_cast<char>(character));
    }
  }
  if (!token.empty()) tokens.push_back(std::move(token));
  return tokens;
}

void AppendSearchField(std::string& document, const std::string& field) {
  document.push_back(' ');
  document += field;
}

std::string SearchText(const HistoryEntry& entry) {
  std::string document;
  document.reserve(entry.input.size() + entry.output.size() +
                   entry.provider.size() + entry.model.size() +
                   entry.reasoning.size() + entry.error.size());
  AppendSearchField(document, entry.input);
  AppendSearchField(document, entry.output);
  AppendSearchField(document, entry.provider);
  AppendSearchField(document, entry.api_format);
  AppendSearchField(document, entry.model);
  AppendSearchField(document, entry.reasoning);
  AppendSearchField(document, entry.error);
  return document;
}

}  // namespace

std::string HistoryEntry::Identity() const {
  return request_id + ":" + std::to_string(timestamp_ms);
}

void AppendHistory(const std::filesystem::path& history_path,
                   const AppConfig& config,
                   const std::string& input,
                   const RewriteResult& result) {
  if (!config.history_enabled) {
    return;
  }

  if (history_path.empty() || !internal::IsRegularFileOrMissing(history_path) ||
      !internal::PrepareSecureParent(history_path)) {
    return;
  }

  std::ofstream output(history_path, std::ios::app);
  if (!output) {
    return;
  }
  if (!internal::SetPrivateFilePermissions(history_path)) return;

  const auto now = std::chrono::system_clock::now().time_since_epoch();
  const auto millis =
      std::chrono::duration_cast<std::chrono::milliseconds>(now).count();

  nlohmann::json line{{"timestamp_ms", millis},
                      {"provider", config.provider},
                      {"api_format", ToString(config.api_format)},
                      {"model", config.model},
                      {"reasoning", config.reasoning},
                      {"ok", result.ok},
                      {"request_id", result.request_id},
                      {"provider_request_id", result.provider_request_id},
                      {"http_status", result.http_status},
                      {"duration_ms", result.duration.count()},
                      {"input", input},
                      {"output", result.text},
                      {"error", result.error},
                      {"estimated_input_tokens",
                       result.estimated_input_tokens},
                      {"max_output_tokens", result.max_output_tokens}};
  output << line.dump() << '\n';
  output.flush();
}

std::vector<HistoryEntry> LoadHistory(
    const std::filesystem::path& history_path) {
  std::vector<HistoryEntry> entries;
  if (history_path.empty()) return entries;

  std::ifstream input(history_path);
  if (!input) return entries;

  std::string line;
  while (std::getline(input, line)) {
    if (line.empty()) continue;
    Json object;
    try {
      object = Json::parse(line);
    } catch (...) {
      continue;
    }
    if (!object.is_object()) continue;

    HistoryEntry entry;
    entry.timestamp_ms = Int64Value(object, "timestamp_ms");
    entry.provider = StringValue(object, "provider");
    entry.api_format = StringValue(object, "api_format");
    entry.model = StringValue(object, "model");
    entry.reasoning = StringValue(object, "reasoning");
    entry.ok = BoolValue(object, "ok");
    entry.request_id = StringValue(object, "request_id");
    entry.provider_request_id = StringValue(object, "provider_request_id");
    entry.http_status = IntValue(object, "http_status");
    entry.duration = std::chrono::milliseconds{Int64Value(object, "duration_ms")};
    entry.input = StringValue(object, "input");
    entry.output = StringValue(object, "output");
    entry.error = StringValue(object, "error");
    entry.estimated_input_tokens = IntValue(object, "estimated_input_tokens");
    entry.max_output_tokens = IntValue(object, "max_output_tokens");
    entries.push_back(std::move(entry));
  }

  std::stable_sort(entries.begin(), entries.end(),
                   [](const HistoryEntry& left, const HistoryEntry& right) {
                     return left.timestamp_ms > right.timestamp_ms;
                   });
  return entries;
}

std::vector<HistoryEntry> SearchHistory(
    const std::vector<HistoryEntry>& entries, std::string_view query) {
  return HistorySearchIndex{std::vector<HistoryEntry>{entries}}.Search(query);
}

std::vector<HistoryEntry> SearchHistory(
    const std::filesystem::path& history_path, std::string_view query) {
  return SearchHistory(LoadHistory(history_path), query);
}

HistorySearchIndex::HistorySearchIndex(std::vector<HistoryEntry> entries)
    : entries_(std::move(entries)) {
  std::stable_sort(entries_.begin(), entries_.end(),
                   [](const HistoryEntry& left, const HistoryEntry& right) {
                     return left.timestamp_ms > right.timestamp_ms;
                   });

  documents_.reserve(entries_.size());
  std::size_t total_length = 0;
  for (const auto& entry : entries_) {
    Document document;
    for (const auto& term : Tokenize(SearchText(entry))) {
      ++document.term_frequency[term];
    }
    document.length = std::accumulate(
        document.term_frequency.begin(), document.term_frequency.end(),
        std::size_t{0}, [](std::size_t total, const auto& item) {
          return total + item.second;
        });
    total_length += document.length;
    const auto document_index = documents_.size();
    for (const auto& [term, frequency] : document.term_frequency) {
      if (frequency != 0) {
        ++document_frequency_[term];
        postings_[term].push_back(document_index);
      }
    }
    documents_.push_back(std::move(document));
  }
  if (!documents_.empty()) {
    average_document_length_ =
        static_cast<double>(total_length) / documents_.size();
  }
}

std::vector<HistoryEntry> HistorySearchIndex::Search(
    std::string_view query) const {
  return SearchPage(query, 0, entries_.size()).entries;
}

HistorySearchPage HistorySearchIndex::SearchPage(std::string_view query,
                                                 std::size_t offset,
                                                 std::size_t limit) const {
  HistorySearchPage page;
  const auto query_terms = Tokenize(query);
  if (query_terms.empty()) {
    page.total_matches = entries_.size();
    const auto start = std::min(offset, entries_.size());
    const auto count = std::min(limit, entries_.size() - start);
    page.entries.reserve(count);
    page.entries.insert(page.entries.end(), entries_.begin() + start,
                        entries_.begin() + start + count);
    return page;
  }

  constexpr double k1 = 1.2;
  constexpr double b = 0.75;
  struct ScoredEntry {
    std::size_t index = 0;
    double score = 0.0;
  };
  std::unordered_map<std::size_t, double> scores;
  std::unordered_set<std::string> unique_terms;
  for (const auto& term : query_terms) unique_terms.insert(term);

  const double document_count = static_cast<double>(documents_.size());
  for (const auto& term : unique_terms) {
    const auto document_frequency = document_frequency_.find(term);
    const auto postings = postings_.find(term);
    if (document_frequency == document_frequency_.end() ||
        postings == postings_.end()) {
      continue;
    }
    const double idf = std::log(
        1.0 + (document_count - document_frequency->second + 0.5) /
                  (document_frequency->second + 0.5));
    for (const auto index : postings->second) {
      const auto& document = documents_[index];
      const auto frequency = document.term_frequency.find(term);
      const double length_norm = average_document_length_ == 0.0
                                     ? 1.0
                                     : 1.0 - b +
                                           b * document.length /
                                               average_document_length_;
      const double term_frequency = static_cast<double>(frequency->second);
      scores[index] += idf * (term_frequency * (k1 + 1.0)) /
                       (term_frequency + k1 * length_norm);
    }
  }

  std::vector<ScoredEntry> scored;
  scored.reserve(scores.size());
  for (const auto& [index, score] : scores) {
    if (score > 0.0) scored.push_back({index, score});
  }

  page.total_matches = scored.size();
  const auto start = std::min(offset, scored.size());
  const auto count = std::min(limit, scored.size() - start);
  if (count == 0) return page;
  const auto end = start + count;
  const auto compare = [this](const ScoredEntry& left,
                              const ScoredEntry& right) {
    if (left.score != right.score) return left.score > right.score;
    if (entries_[left.index].timestamp_ms != entries_[right.index].timestamp_ms) {
      return entries_[left.index].timestamp_ms >
             entries_[right.index].timestamp_ms;
    }
    return left.index < right.index;
  };
  if (end < scored.size()) {
    std::partial_sort(scored.begin(), scored.begin() + end, scored.end(),
                      compare);
  } else {
    std::stable_sort(scored.begin(), scored.end(), compare);
  }

  page.entries.reserve(count);
  for (std::size_t index = start; index < end; ++index) {
    page.entries.push_back(entries_[scored[index].index]);
  }
  return page;
}

}  // namespace llm_rewriter
