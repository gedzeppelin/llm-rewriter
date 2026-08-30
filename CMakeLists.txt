cmake_minimum_required(VERSION 3.24)

project(
  llm-rewriter
  VERSION 0.1.0
  LANGUAGES C CXX
)

include(GNUInstallDirs)
if(ANDROID)
  message(FATAL_ERROR "Android builds are not supported in the native C++ repository")
endif()
if(APPLE)
  message(FATAL_ERROR "Apple builds are not supported in the native C++ repository")
endif()
include(CTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(LLM_REWRITER_BUILD_GTK4 "Build the GTK4 Linux frontend" OFF)
set(LLM_REWRITER_DEFAULT_WIDTH "600" CACHE STRING "Default frontend window width")
set(LLM_REWRITER_DEFAULT_HEIGHT "400" CACHE STRING "Default frontend window height")

find_package(nlohmann_json REQUIRED)
find_package(CLI11 REQUIRED)

set(LLM_REWRITER_WARNINGS
  $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-Wall -Wextra -Wpedantic>
  $<$<COMPILE_LANG_AND_ID:C,GNU,Clang>:-Wall -Wextra -Wpedantic>
)

set(LLM_REWRITER_BACKEND_SOURCES
  src/backend/BackendAbi.cpp
  src/backend/Config.cpp
  src/backend/Credentials.cpp
  src/backend/Diagnostics.cpp
  src/backend/History.cpp
  src/backend/LlmClient.cpp
  src/backend/TokenEstimate.cpp
)

add_library(llm_rewriter_backend STATIC
  ${LLM_REWRITER_BACKEND_SOURCES}
)

target_include_directories(llm_rewriter_backend
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(llm_rewriter_backend
  PUBLIC
    nlohmann_json::nlohmann_json
)

target_compile_options(llm_rewriter_backend PRIVATE ${LLM_REWRITER_WARNINGS})
target_compile_definitions(llm_rewriter_backend
  PUBLIC
    LLM_REWRITER_DEFAULT_WIDTH=${LLM_REWRITER_DEFAULT_WIDTH}
    LLM_REWRITER_DEFAULT_HEIGHT=${LLM_REWRITER_DEFAULT_HEIGHT}
)
set_target_properties(llm_rewriter_backend PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(UNIX AND NOT APPLE)
  find_package(CURL REQUIRED)

  set(LLM_REWRITER_PLATFORM_LINUX_SOURCES
    src/platform/linux/Clipboard.cpp
    src/platform/linux/CurlHttpTransport.cpp
    src/platform/linux/Doctor.cpp
    src/platform/linux/Notification.cpp
    src/platform/linux/Output.cpp
    src/platform/linux/Paths.cpp
    src/platform/linux/SecretStore.cpp
    src/platform/linux/Workflow.cpp
  )

  add_library(llm_rewriter_platform_linux STATIC
    ${LLM_REWRITER_PLATFORM_LINUX_SOURCES}
  )

  target_include_directories(llm_rewriter_platform_linux
    PUBLIC
      ${PROJECT_SOURCE_DIR}/include
  )

  target_link_libraries(llm_rewriter_platform_linux
    PUBLIC
      llm_rewriter_backend
    PRIVATE
      CURL::libcurl
  )

  target_compile_options(llm_rewriter_platform_linux
    PRIVATE
      ${LLM_REWRITER_WARNINGS}
  )
  set(LLM_REWRITER_PLATFORM_TARGET llm_rewriter_platform_linux)
elseif(WIN32)
  add_library(llm_rewriter_platform_windows STATIC
    src/platform/windows/Clipboard.cpp
    src/platform/windows/Doctor.cpp
    src/platform/windows/Notification.cpp
    src/platform/windows/Output.cpp
    src/platform/windows/Paths.cpp
    src/platform/windows/SecretStore.cpp
    src/platform/windows/WinHttpTransport.cpp
    src/platform/windows/Workflow.cpp
  )
  target_include_directories(llm_rewriter_platform_windows
    PUBLIC
      ${PROJECT_SOURCE_DIR}/include
  )
  target_link_libraries(llm_rewriter_platform_windows
    PUBLIC
      llm_rewriter_backend
    PRIVATE
      winhttp
      advapi32
  )
  target_compile_options(llm_rewriter_platform_windows
    PRIVATE
      ${LLM_REWRITER_WARNINGS}
  )
  set(LLM_REWRITER_PLATFORM_TARGET llm_rewriter_platform_windows)
endif()

add_library(llm_rewriter_cli_support STATIC
  src/cli/Cli.cpp
)

target_include_directories(llm_rewriter_cli_support
  PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(llm_rewriter_cli_support
  PUBLIC
    CLI11::CLI11
    llm_rewriter_backend
)

target_compile_options(llm_rewriter_cli_support
  PRIVATE
    ${LLM_REWRITER_WARNINGS}
)

add_library(llm_rewriter_peel INTERFACE)
target_include_directories(llm_rewriter_peel
  SYSTEM INTERFACE
    ${PROJECT_SOURCE_DIR}/peel/include
)

function(llm_rewriter_peel_generate GIR_NAME GIR_VERSION)
  set(options RECURSIVE)
  cmake_parse_arguments(PEEL_GENERATE "${options}" "" "" ${ARGN})

  if(PEEL_GENERATE_RECURSIVE)
    set(recursive_arg "--recursive")
  else()
    set(recursive_arg)
  endif()

  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  set(gen_dir "${CMAKE_CURRENT_BINARY_DIR}/peel-generated")
  set(output_header "${gen_dir}/peel/${GIR_NAME}/${GIR_NAME}.h")

  add_custom_command(
    OUTPUT "${output_header}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${gen_dir}"
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/peel/peel-gen.py"
            ${recursive_arg}
            --out-dir "${gen_dir}"
            "${GIR_NAME}" "${GIR_VERSION}"
    DEPENDS "${PROJECT_SOURCE_DIR}/peel/peel-gen.py"
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    VERBATIM
  )

  add_custom_target(llm_rewriter_peel_generate_${GIR_NAME}
    DEPENDS "${output_header}"
  )

  add_library(llm_rewriter_peel_${GIR_NAME} INTERFACE)
  add_dependencies(llm_rewriter_peel_${GIR_NAME}
    llm_rewriter_peel_generate_${GIR_NAME}
  )
  target_include_directories(llm_rewriter_peel_${GIR_NAME}
    SYSTEM INTERFACE
      "${gen_dir}"
      "${PROJECT_SOURCE_DIR}/peel/include"
  )
  target_link_libraries(llm_rewriter_peel_${GIR_NAME}
    INTERFACE
      llm_rewriter_peel
  )
endfunction()

add_executable(llm-rewriter
  src/cli/main.cpp
)

target_include_directories(llm-rewriter
  PRIVATE
    ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(llm-rewriter
  PRIVATE
    llm_rewriter_cli_support
    ${LLM_REWRITER_PLATFORM_TARGET}
)

target_compile_options(llm-rewriter PRIVATE ${LLM_REWRITER_WARNINGS})
install(TARGETS llm-rewriter RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(LLM_REWRITER_BUILD_GTK4)
  if(NOT UNIX OR APPLE)
    message(FATAL_ERROR "The GTK4 frontend is only supported on Linux in this tree")
  endif()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(LIBADWAITA REQUIRED IMPORTED_TARGET libadwaita-1)
  llm_rewriter_peel_generate(Adw 1 RECURSIVE)

  add_executable(llm-rewriter-gtk4
    src/frontends/gtk4/main.cpp
  )

  target_include_directories(llm-rewriter-gtk4
    PRIVATE
      ${PROJECT_SOURCE_DIR}/include
  )

  target_link_libraries(llm-rewriter-gtk4
    PRIVATE
      PkgConfig::LIBADWAITA
      llm_rewriter_cli_support
      llm_rewriter_backend
      ${LLM_REWRITER_PLATFORM_TARGET}
      llm_rewriter_peel_Adw
  )

  target_compile_options(llm-rewriter-gtk4 PRIVATE ${LLM_REWRITER_WARNINGS})
  install(TARGETS llm-rewriter-gtk4 RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(TARGET llm-rewriter-gtk4)
  install(FILES data/llm_rewriter.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
endif()

configure_file(
  cmake/uninstall.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/uninstall.cmake
  @ONLY
)
add_custom_target(uninstall
  COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/uninstall.cmake
  COMMENT "Removing files listed in the install manifest"
)

if(BUILD_TESTING)
  find_package(doctest REQUIRED)

  add_executable(llm-rewriter-tests
    tests/CoreTests.cpp
  )

  target_include_directories(llm-rewriter-tests
    PRIVATE
      ${PROJECT_SOURCE_DIR}/include
  )

  target_link_libraries(llm-rewriter-tests
    PRIVATE
      doctest::doctest
      llm_rewriter_backend
  )
  target_link_libraries(llm-rewriter-tests PRIVATE llm_rewriter_cli_support)

  if(LLM_REWRITER_PLATFORM_TARGET)
    target_link_libraries(llm-rewriter-tests
      PRIVATE
        ${LLM_REWRITER_PLATFORM_TARGET}
    )
  endif()

  target_compile_options(llm-rewriter-tests PRIVATE ${LLM_REWRITER_WARNINGS})
  add_test(NAME llm-rewriter-tests COMMAND llm-rewriter-tests)

  add_executable(llm-rewriter-abi-c-compile
    tests/AbiCCompile.c
  )
  target_include_directories(llm-rewriter-abi-c-compile
    PRIVATE
      ${PROJECT_SOURCE_DIR}/include
  )
  target_link_libraries(llm-rewriter-abi-c-compile
    PRIVATE
      llm_rewriter_backend
  )
  target_compile_options(llm-rewriter-abi-c-compile
    PRIVATE
      ${LLM_REWRITER_WARNINGS}
  )
  add_test(NAME llm-rewriter-abi-c-compile
           COMMAND llm-rewriter-abi-c-compile)
endif()
