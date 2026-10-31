cmake_minimum_required(VERSION 3.24)
project(itch_book VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(ITCH_BUILD_TESTS "Build unit tests" ON)
option(ITCH_NATIVE "Tune Release builds for the host CPU (-march=native)" ON)
option(ITCH_SANITIZE "Enable ASan + UBSan in Debug builds" ON)

set(ITCH_SAN $<AND:$<CONFIG:Debug>,$<BOOL:${ITCH_SANITIZE}>>)

# Shared warnings / flags for every target in this project.
add_library(itch_flags INTERFACE)
target_compile_options(itch_flags INTERFACE
  -Wall -Wextra -Wpedantic -Wconversion -Wshadow
  $<${ITCH_SAN}:-fsanitize=address,undefined -fno-omit-frame-pointer>
  $<$<AND:$<CONFIG:Release>,$<BOOL:${ITCH_NATIVE}>>:-march=native>
)
target_link_options(itch_flags INTERFACE
  $<${ITCH_SAN}:-fsanitize=address,undefined>
)

add_library(itch src/book.cpp src/mapped_file.cpp)
target_include_directories(itch PUBLIC include)
target_link_libraries(itch PUBLIC itch_flags)

add_executable(itch_replay src/main.cpp)
target_link_libraries(itch_replay PRIVATE itch)

if(ITCH_BUILD_TESTS)
  include(FetchContent)
  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP ON
  )
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  enable_testing()
  add_executable(itch_tests tests/test_decoder.cpp tests/test_book.cpp tests/test_mapped_file.cpp)
  target_link_libraries(itch_tests PRIVATE itch GTest::gtest_main)

  include(GoogleTest)
  gtest_discover_tests(itch_tests)
endif()
