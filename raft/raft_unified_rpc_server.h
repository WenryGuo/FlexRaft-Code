#pragma once
// raft_unified_rpc_server.h — 统一的 Raft RPC 服务器
//
// 用于 Multi-Raft 架构：同一物理节点的所有 Raft 实例共享一个 RPC 服务器
// 通过 group_id 路由消息到对应的 RaftState

#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>

#include "rcf_rpc.h"
#include "raft_type.h"

namespace raft {

// 前向声明
class RaftState;

namespace rpc {

// =======================================================================
//  RaftUnifiedRPCService — 支持 group_id 路由的 Raft RPC 服务
//
//  该服务重新实现 RaftRPC 的三个 RPC 方法，通过 group_id 路由到对应的 RaftState
// =======================================================================
class RaftUnifiedRPCService {
 public:
  RaftUnifiedRPCService() = default;

  // 注册 group_id -> RaftState 的映射
  void RegisterRaftState(raft_group_id_t group_id, RaftState* raft_state);

  // 注销 group_id
  void UnregisterRaftState(raft_group_id_t group_id);

  // RPC 处理方法
  RCF::ByteBuffer RequestVote(const RCF::ByteBuffer& arg_buf);
  RCF::ByteBuffer AppendEntries(const RCF::ByteBuffer& arg_buf);
  RCF::ByteBuffer RequestFragments(const RCF::ByteBuffer& arg_buf);

  // Group Notification RPC — 用于分组同步
  RCF::ByteBuffer GroupNotification(const RCF::ByteBuffer& arg_buf);

  // 设置分组通知回调（用于通知 RaftStore 创建 Raft 实例）
  using GroupNotificationCallback = std::function<void(const GroupNotificationArgs&)>;
  void SetGroupNotificationCallback(GroupNotificationCallback cb) {
    group_notification_callback_ = std::move(cb);
  }
  bool HasGroupNotificationCallback() const {
    return static_cast<bool>(group_notification_callback_);
  }

  // 获取已注册的 group 数量（用于动态计算线程池大小）
  size_t GetRegisteredGroupCount() const {
    std::shared_lock<std::shared_mutex> lock(mu_);
    return raft_states_.size();
  }

 private:
  // 获取对应的 RaftState
  RaftState* GetRaftState(raft_group_id_t group_id);

  mutable std::shared_mutex mu_;
  std::map<raft_group_id_t, RaftState*> raft_states_;

  // 分组通知回调
  GroupNotificationCallback group_notification_callback_;
};

// =======================================================================
//  RaftUnifiedRpcServer — 统一的 Raft RPC 服务器
// =======================================================================
class RaftUnifiedRpcServer {
 public:
  RaftUnifiedRpcServer(int physical_node_id, const NetAddress& listen_addr);
  ~RaftUnifiedRpcServer();

  void Start() noexcept(false);
  void Stop();

  // 注册 group_id -> RaftState 的映射
  void RegisterRaftState(raft_group_id_t group_id, RaftState* raft_state);

  // 注销 group_id
  void UnregisterRaftState(raft_group_id_t group_id);

  int GetPhysicalNodeId() const { return physical_node_id_; }
  std::string GetAddress() const { return addr_.ip + ":" + std::to_string(addr_.port); }
  RaftUnifiedRPCService* GetService() { return &service_; }

  // Get/set the global singleton (used by RaftNode for Multi-Raft injection)
  static RaftUnifiedRPCService* GetGlobalService();
  static void SetGlobalService(RaftUnifiedRPCService* svc);

 private:
  int physical_node_id_;
  NetAddress addr_;

  RCF::RcfInit rcf_init_;
  RCF::RcfServer server_;
  RaftUnifiedRPCService service_;

  bool running_ = false;

  // Global singleton for cross-component access
  inline static RaftUnifiedRPCService* global_service_ = nullptr;
};

}  // namespace rpc
}  // namespace raft
