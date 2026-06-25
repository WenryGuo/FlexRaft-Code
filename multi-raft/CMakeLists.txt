cmake_minimum_required(VERSION 3.8)
SET(CMAKE_CXX_STANDARD 20)

# Multi-Raft library sources
# NOTE: The following modules were removed as they are unused:
#   - unified_rpc_server.cc/h: Replaced by raft/raft_unified_rpc_server.h
#   - unified_message_router.cc/h: Replaced by rpc_router.h
#   - p2p_network_manager.cc/h: Functionality provided by BatchTransport
#   - multi_raft_router.h: Replaced by raft/raft_unified_rpc_server.h
set(
  MULTI_RAFT_SRC
  rpc_router.cc
  group_registry.cc
  peer_list_manager.cc
  group_coordinator.cc
  stripe_format.cc
  stripe_encode.cc
  stripe_raft_adapter.cc
  stripe_apply.cc
  stripe_read.cc
  raft_store_stripe.cc
  raft_store_batch.cc
)

# Create multi-raft library
add_library(multi-raft ${MULTI_RAFT_SRC})
target_link_libraries(multi-raft raft kv rcf)
target_include_directories(multi-raft
    PUBLIC ${CMAKE_CURRENT_LIST_DIR}
    PUBLIC ${CMAKE_SOURCE_DIR}/raft
    PUBLIC ${CMAKE_SOURCE_DIR}/kv
    PUBLIC ${ROCKSDB_DIR}/include
)
