#pragma once
#include <memory>
#include <unordered_map>
#include <mutex>
#include "raft.h"  // 假定包含 RaftState, RaftConfig, raft_types...
namespace raft {

class RaftManager {
 public:
  RaftManager() = default;
  ~RaftManager() = default;

  // 创建一个 group（若已存在则返回 false 或替换，按策略决定）
  bool CreateGroup(raft_group_id_t group_id, const RaftConfig &config);

  // 删除 group（可选）
  bool RemoveGroup(raft_group_id_t group_id);

  // 获取 group 的 RaftState（线程安全）
  RaftState* GetRaftState(raft_group_id_t group_id);

  // 以下是用于 RPC 路由的便捷方法：把 RPC 转发给对应 RaftState
  void Process(raft_group_id_t group_id, RequestVoteArgs *args, RequestVoteReply *reply);
  void Process(raft_group_id_t group_id, RequestVoteReply *reply);
  void Process(raft_group_id_t group_id, AppendEntriesArgs *args, AppendEntriesReply *reply);
  void Process(raft_group_id_t group_id, AppendEntriesReply *reply);
  void Process(raft_group_id_t group_id, RequestFragmentsArgs *args, RequestFragmentsReply *reply);
  void Process(raft_group_id_t group_id, RequestFragmentsReply *reply);

 private:
  std::mutex mtx_;
  std::unordered_map<raft_group_id_t, std::shared_ptr<RaftState>> groups_;
};

} // namespace raft