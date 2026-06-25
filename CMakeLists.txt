cmake_minimum_required(VERSION 3.8)
project(raft)

set(CMAKE_CXX_STANDARD 20)

# Enable AddressSanitizer for memory debugging (set ASAN=1 when building)
option(ASAN "Enable AddressSanitizer" OFF)

if(ASAN)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address -g -O1")
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=address")
  message(STATUS "AddressSanitizer enabled")
endif()

# Encoding scheme used by multi-raft write path is now selected at runtime
# via bench_server_multiraft's --encoding=RS_F|RS_3F|LRC gflag. The legacy
# compile-time ENCODING option is no longer needed; pass --encoding=... when
# launching the binary instead.
if(DEFINED ENCODING)
  message(WARNING
    "ENCODING=${ENCODING} is ignored — encoding scheme is now selected at "
    "runtime via 'bench_server_multiraft --encoding=RS_F|RS_3F|LRC'")
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
add_subdirectory(${CMAKE_SOURCE_DIR}/multi-raft)
add_subdirectory(${CMAKE_SOURCE_DIR}/multi-raft/test)
add_subdirectory(${CMAKE_SOURCE_DIR}/bench)



