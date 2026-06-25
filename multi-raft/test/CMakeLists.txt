cmake_minimum_required(VERSION 3.8)
SET(CMAKE_CXX_STANDARD 20)

# Test sources - standalone tests that need include paths to the project root
# "multi-raft/*.h" style includes need CMAKE_SOURCE_DIR as include path
include_directories(${CMAKE_SOURCE_DIR})
include_directories(${CMAKE_SOURCE_DIR}/raft)

set(
  TEST_SRC
  test_lrc_params.cc
  test_lrc_group.cc
  memory_leak_test.cc
)

foreach(src ${TEST_SRC})
  get_filename_component(exec_name ${src} NAME_WE)
  add_executable(${exec_name} ${src})
endforeach()
