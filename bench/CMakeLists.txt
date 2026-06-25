cmake_minimum_required(VERSION 3.8)
SET(CMAKE_CXX_STANDARD 20)

find_package(GTest REQUIRED)
include_directories(${GTEST_INCLUDE_DIR})

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/..)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../kv)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../multi-raft)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../raft)

find_library(libpthread NAMES pthread)
if (NOT libpthread)
    MESSAGE(FATAL_ERROR "library pthread Not Found")
endif()

find_library(libdl NAMES dl)
if (NOT libdl)
    MESSAGE(FATAL_ERROR "library dl Not Found")
endif()

find_library(libuuid NAMES uuid)
if (NOT libuuid)
    MESSAGE(FATAL_ERROR "library uuid Not Found")
endif()


SET(LIBS ${libpthread} ${libdl} ${libuuid})

set(TEST_FILES)
file(GLOB TEST_FILES ./*.cc)
add_executable(bench_server_multiraft bench_server_multiraft.cc)
target_link_libraries(bench_server_multiraft multi-raft kv gflags)
foreach(test_file_source ${TEST_FILES}) 
  get_filename_component(test_file_name ${test_file_source} NAME)
  if(${test_file_name} MATCHES "_test.cc$")
    string(REPLACE ".cc" "" test_exec_name ${test_file_name})
    add_executable(${test_exec_name} ${test_file_name})
    target_link_libraries(${test_exec_name} kv raft rcf ${LIBS} ${GTEST_BOTH_LIBRARIES})
  endif()
endforeach()

add_executable(bench_server bench_server.cc)
target_link_libraries(bench_server kv gflags)


add_executable(bench_client bench_client.cc)
target_link_libraries(bench_client kv gflags)

add_executable(multi_raft_client multi_raft_client.cc)
target_link_libraries(multi_raft_client kv gflags)

add_executable(ycsb_server ycsb_server.cc)
target_link_libraries(ycsb_server kv gflags)

add_executable(ycsb_client ycsb_client.cc)
target_link_libraries(ycsb_client kv gflags)

add_executable(readbench_client readbench_client.cc)
target_link_libraries(readbench_client kv gflags)

add_executable(follower_recover_bench follower_recover_bench.cc)
target_link_libraries(follower_recover_bench kv gflags)

add_executable(ycsb_server_multiraft ycsb_server_multiraft.cc)
target_link_libraries(ycsb_server_multiraft multi-raft kv gflags pthread)

add_executable(ycsb_client_multiraft ycsb_client_multiraft.cc)
target_link_libraries(ycsb_client_multiraft kv gflags)


