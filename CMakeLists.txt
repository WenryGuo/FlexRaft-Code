cmake_minimum_required(VERSION 3.8)
project(raft)

set(CMAKE_CXX_STANDARD 20)

option(REPLICATION_MODE "Replication mode: FULL(k=1) or EC (erasure coding)" EC)

if(${REPLICATION_MODE} STREQUAL "FULL")
  add_compile_definitions(FULL_REPLICATION_MODE)
  message(STATUS "Replication mode: Full replication (k=1)")
elseif(${REPLICATION_MODE} STREQUAL "EC")
  message(STATUS "Replication mode: Erasure coding")
else()
  message(FATAL_ERROR "REPLICATION_MODE must be 'FULL' or 'EC'")
endif()

option(LOG "Enable log output" OFF)

if(LOG)
  add_compile_definitions(ENABLE_LOG)
endif()


# enable recording more fine granularity performance records
# if(${PERF} STREQUAL on)
#   add_compile_definitions(ENABLE_PERF_RECORDING)
# endif()
option(PERF "Enable performance recording" OFF)

if(PERF)
  add_compile_definitions(ENABLE_PERF_RECORDING)
endif()


include_directories(${CMAKE_SOURCE_DIR}/RCF/include)
add_subdirectory(${CMAKE_SOURCE_DIR}/RCF)

include_directories(${CMAKE_SOURCE_DIR}/third-party/leveldb/include)

add_subdirectory(${CMAKE_SOURCE_DIR}/third-party)
add_subdirectory(${CMAKE_SOURCE_DIR}/raft)
add_subdirectory(${CMAKE_SOURCE_DIR}/kv)
add_subdirectory(${CMAKE_SOURCE_DIR}/bench)



