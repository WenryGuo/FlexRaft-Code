#pragma once
// batch_system_bridge.h — 连接 RaftState 和 Multi-Raft ApplyFsm Mailbox
//
// 架构:
//   RaftState (raft namespace)
//     -> RSM interface (ApplyLogEntry)
//       -> BatchSystemBridge (static bridge)
//         -> Mailbox<ApplyMsg> (multiraft namespace)
//           -> ApplyFsm (multiraft namespace)
//             -> RocksDB
//
// 使用方法:
//   1. RaftStore::CreateRaftInstances() 中创建 ApplyFsm 后:
//        BatchSystemBridge::RegisterMailbox(group_id, node_id, apply_mb);
//        raft_nodes_[gid]->SetRsmAndApplyMailbox(..., &batch_system_, this);
//   2. RaftState::tryApplyLogEntries() 通过 RSM bridge 将 committed entries
//      推入 ApplyFsm mailbox

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

#include "../multi-raft/message.h"
#include "../multi-raft/mailbox.h"
#include "log_entry.h"

namespace raft {

// Forward declare the Multi-Raft BatchSystem
class BatchSystemForRaftBridge;

class BatchSystemBridge {
 public:
  // 注册 ApplyFsm mailbox 到 bridge（由 RaftStore 在创建 ApplyFsm 后调用）
  static void RegisterMailbox(uint32_t group_id,
                              uint32_t node_id,
                              void* mailbox_ptr);

  // 注销 mailbox（shutdown 时调用）
  static void UnregisterMailbox(uint32_t group_id, uint32_t node_id);

  // 检查是否已注册
  static bool HasMailbox(uint32_t group_id, uint32_t node_id);

  // 直接将 entry 推入指定 group 的 ApplyFsm mailbox
  static void ApplyEntryToGroup(uint32_t group_id,
                                 uint32_t node_id,
                                 const LogEntry& entry);

 private:
  static uint64_t MakeKey(uint32_t group_id, uint32_t node_id) {
    return (static_cast<uint64_t>(group_id) << 32) | node_id;
  }

  // Defined in .cc to avoid ODR issues
  static std::shared_mutex& GetMutex();
  static std::unordered_map<uint64_t, void*>& GetMailboxMap();

  // 非 static 的 ApplyLogEntry 实现（会被 RSM 接口调用）
  static void ApplyEntry(uint32_t group_id,
                         uint32_t node_id,
                         const LogEntry& entry);
};

}  // namespace raft
