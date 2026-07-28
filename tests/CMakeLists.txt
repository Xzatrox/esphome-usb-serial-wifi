cmake_minimum_required(VERSION 3.14)
project(usbip_protocol_tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- Fetch dependencies ---
include(FetchContent)

# Google Test
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.14.0
)

# Rapidcheck
FetchContent_Declare(
  rapidcheck
  GIT_REPOSITORY https://github.com/emil-e/rapidcheck.git
  GIT_TAG        master
)

FetchContent_MakeAvailable(googletest rapidcheck)

# Enable testing
enable_testing()

# --- Test executable ---
add_executable(test_protocol
  test_protocol.cpp
)

target_include_directories(test_protocol PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(test_protocol
  GTest::gtest
  GTest::gtest_main
  rapidcheck
  rapidcheck_gtest
)

include(GoogleTest)
gtest_discover_tests(test_protocol)
