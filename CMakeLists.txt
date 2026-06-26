cmake_minimum_required(VERSION 3.24)

project(
  llm-rewriter
  VERSION 0.1.0
  LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(wxWidgets CONFIG QUIET COMPONENTS core base)
if(wxWidgets_FOUND AND TARGET wx::core AND TARGET wx::base)
  set(LLM_REWRITER_WX_LIBS wx::core wx::base)
else()
  find_package(wxWidgets REQUIRED COMPONENTS core base)
  include(${wxWidgets_USE_FILE})
  set(LLM_REWRITER_WX_LIBS ${wxWidgets_LIBRARIES})
endif()
find_package(CURL REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(CLI11 REQUIRED)
find_package(doctest REQUIRED)

if(UNIX AND NOT APPLE)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(GLIB2 REQUIRED glib-2.0)
endif()

set(LLM_REWRITER_SOURCES
  src/Cli.cpp
  src/Clipboard.cpp
  src/Config.cpp
  src/History.cpp
  src/LlmClient.cpp
  src/MainFrame.cpp
  src/Notification.cpp
  src/Output.cpp
  src/Paths.cpp
  src/RewriteService.cpp
  src/TokenEstimate.cpp
)

add_executable(llm-rewriter
  src/main.cpp
  src/App.cpp
  ${LLM_REWRITER_SOURCES}
)

target_include_directories(llm-rewriter
  PRIVATE
    ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(llm-rewriter
  PRIVATE
    ${LLM_REWRITER_WX_LIBS}
    CLI11::CLI11
    CURL::libcurl
    nlohmann_json::nlohmann_json
)

if(GLIB2_FOUND)
  target_include_directories(llm-rewriter
    PRIVATE
      ${GLIB2_INCLUDE_DIRS}
  )

  target_link_libraries(llm-rewriter
    PRIVATE
      ${GLIB2_LIBRARIES}
  )

  target_link_directories(llm-rewriter
    PRIVATE
      ${GLIB2_LIBRARY_DIRS}
  )

  target_compile_options(llm-rewriter
    PRIVATE
      ${GLIB2_CFLAGS_OTHER}
  )
endif()

target_compile_options(llm-rewriter
  PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
)

install(TARGETS llm-rewriter RUNTIME DESTINATION bin)
install(FILES data/llm_rewriter.desktop DESTINATION share/applications)

include(CTest)
if(BUILD_TESTING)
  add_executable(llm-rewriter-tests
    tests/CoreTests.cpp
    ${LLM_REWRITER_SOURCES}
  )

  target_include_directories(llm-rewriter-tests
    PRIVATE
      ${PROJECT_SOURCE_DIR}/include
  )

  target_link_libraries(llm-rewriter-tests
    PRIVATE
      ${LLM_REWRITER_WX_LIBS}
      CLI11::CLI11
      CURL::libcurl
      doctest::doctest
      nlohmann_json::nlohmann_json
  )

  target_compile_options(llm-rewriter-tests
    PRIVATE
      $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
  )

  add_test(NAME llm-rewriter-tests COMMAND llm-rewriter-tests)
endif()
