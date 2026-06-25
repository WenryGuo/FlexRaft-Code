cmake_minimum_required(VERSION 3.8)
SET(CMAKE_CXX_STANDARD 20)

find_package(GTest REQUIRED)
include_directories(${GTEST_INCLUDE_DIR})

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

find_library(libisal NAMES isal)
if (NOT libisal)
    MESSAGE(FATAL_ERROR "libraryl isa Not Found")
endif()


SET(LIBS ${libpthread} ${libdl} ${libuuid} ${libisal})

set(
  RAFT_SRC
  log_entry.cc
  serializer.cc
  encoder.cc
  util.cc
  log_manager.cc
  storage.cc
  persist_queue.cc
  rcf_rpc.cc
  raft.cc
  raft_node.cc
  raft_unified_rpc_server.cc
  batch_transport.cc
  batch_rpc_sender.cc
  batch_system_bridge.cc
  stripe_hooks.cc
)

# NOTE: raft_batch_transport_stub.cc was removed as it is unused
# The real implementation is in batch_rpc_sender.cc

add_library(raft ${RAFT_SRC})
target_include_directories(raft 
    PUBLIC ${CMAKE_CURRENT_LIST_DIR}
    PUBLIC ${CMAKE_SOURCE_DIR}/multi-raft
)
target_link_libraries(raft rcf ${LIBS})
# target_compile_definitions(raft PRIVATE ENABLE_RAFT_LOG="${ENABLE_RAFT_LOG}")

add_subdirectory(test)

