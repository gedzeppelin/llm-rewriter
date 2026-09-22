#pragma once

#include "llm_rewriter/Config.hpp"
#include "llm_rewriter/LlmClient.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace llm_rewriter {

// A single append-only history record.  History is deliberately a separate
// content-bearing store from diagnostics: these fields may contain the user's
// prompt and the provider response.
struct HistoryEntry {
  std::int64_t timestamp_ms = 0;
  std::string provider;
  std::string api_format;
  std::string model;
  std::string reasoning;
  bool ok = false;
  std::string request_id;
  std::string provider_request_id;
  int http_status = 0;
  std::chrono::milliseconds duration{};
  std::string input;
  std::string output;
  std::string error;
  int estimated_input_tokens = 0;
  int max_output_tokens = 0;

  // Request IDs are generated for successful and failed requests.  Keeping
  // the timestamp in the key also makes records with a missing request ID
  // addressable without changing the on-disk JSONL schema.
  std::string Identity() const;
};

struct HistorySearchPage {
  std::vector<HistoryEntry> entries;
  std::size_t total_matches = 0;
};

// An in-memory BM25 index over parsed JSONL records.  JSONL remains the
// durable format; the index avoids reparsing and rescanning every field for
// each keystroke in the GTK history sidebar.
class HistorySearchIndex {
 public:
  explicit HistorySearchIndex(std::vector<HistoryEntry> entries);

  const std::vector<HistoryEntry>& Entries() const noexcept { return entries_; }
  // Return one relevance-ranked page without copying or sorting entries that
  // are outside the requested window.
  HistorySearchPage SearchPage(std::string_view query, std::size_t offset,
                               std::size_t limit) const;
  std::vector<HistoryEntry> Search(std::string_view query) const;

 private:
  struct Document {
    std::unordered_map<std::string, std::size_t> term_frequency;
    std::size_t length = 0;
  };

  std::vector<HistoryEntry> entries_;
  std::vector<Document> documents_;
  std::unordered_map<std::string, std::size_t> document_frequency_;
  std::unordered_map<std::string, std::vector<std::size_t>> postings_;
  double average_document_length_ = 0.0;
};

void AppendHistory(const std::filesystem::path& history_path,
                   const AppConfig& config,
                   const std::string& input,
                   const RewriteResult& result);

// Read one JSON object per line.  Invalid lines are ignored so a truncated or
// manually edited history file cannot make the entire history unavailable.
std::vector<HistoryEntry> LoadHistory(
    const std::filesystem::path& history_path);

// BM25 search over the user-visible fields used by the history page. Results
// are relevance-ranked with newest-first tie breaking.
std::vector<HistoryEntry> SearchHistory(
    const std::vector<HistoryEntry>& entries, std::string_view query);

std::vector<HistoryEntry> SearchHistory(
    const std::filesystem::path& history_path, std::string_view query);

}  // namespace llm_rewriter
