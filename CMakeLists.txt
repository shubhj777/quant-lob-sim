cmake_minimum_required(VERSION 3.16)
project(QuantLOBSim VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -mcpu=native -DNDEBUG")

include(FetchContent)
FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
)
FetchContent_MakeAvailable(googletest)

enable_testing()

add_library(lob_engine INTERFACE)
target_include_directories(lob_engine INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_executable(lob_tests tests/test_order_book.cpp)
target_link_libraries(lob_tests PRIVATE lob_engine GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(lob_tests)