# CMakeLists.txt — build MiniDB and its test/benchmark binaries.
#
# Usage:
#   cmake -S . -B build && cmake --build build
# Produces, in build/:
#   minidb         — the interactive SQL shell
#   bench          — the LSM vs B+ tree storage benchmark
#   test_storage / test_btree / test_lsm / test_locking — component tests
cmake_minimum_required(VERSION 3.16)
project(MiniDB CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O2")

find_package(Threads REQUIRED)

# All engine logic lives in headers under src/, so each binary is one .cpp.
include_directories(${CMAKE_SOURCE_DIR}/src)

add_executable(minidb src/main.cpp)
target_link_libraries(minidb PRIVATE Threads::Threads)

add_executable(bench benchmarks/bench.cpp)

add_executable(test_storage src/test_storage.cpp)
add_executable(test_btree   src/test_btree.cpp)
add_executable(test_lsm     src/test_lsm.cpp)
add_executable(test_locking src/test_locking.cpp)
target_link_libraries(test_locking PRIVATE Threads::Threads)

enable_testing()
add_test(NAME storage COMMAND test_storage)
add_test(NAME btree   COMMAND test_btree)
add_test(NAME lsm     COMMAND test_lsm)
add_test(NAME locking COMMAND test_locking)
