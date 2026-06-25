#pragma once
// rpc_router.h — Multi-Raft 统一 RPC 消息路由器
//
// 架构设计：
//   同一个物理节点只创建一个 RPC 服务器
//   根据 RPC 消息中的 group_id 将请求路由到对应的 PeerFsm Mailbox
//   实现 Actor 模型：PeerFsm 和 ApplyFsm 是真正的 Actor

#include <mutex>
#include <unordered_map>

#include "mailbox.h"
#include "message.h"
#include "raft_type.h"

namespace multiraft {

// 前向声明
class PeerFsm;

// =======================================================================
//  RpcRouter — 统一 RPC 消息路由器
//
//  管理某个物理节点上所有 group 的 PeerFsm Mailbox
//  根据 group_id 将客户端请求路由到对应的 PeerFsm
// =======================================================================
class RpcRouter {
 public:
  RpcRouter(int node_id);
  ~RpcRouter();

  // 注册某个 group 的 PeerFsm Mailbox
  // 必须在启动 RPC 服务器前完成注册
  void RegisterPeerMailbox(GroupId group_id, raft::raft_node_id_t local_id,
                           Mailbox<PeerMsg>* mb);

  // 取消注册某 group 的 PeerFsm Mailbox
  void UnregisterPeerMailbox(GroupId group_id, raft::raft_node_id_t local_id);

  // 获取本节点上属于指定 group 的 PeerFsm Mailbox
  Mailbox<PeerMsg>* GetGroupMailbox(GroupId to_group);

  // 获取本节点上属于指定 group 和 node 的 PeerFsm Mailbox
  Mailbox<PeerMsg>* GetPeerMailbox(GroupId group_id, raft::raft_node_id_t node_id);

  // 获取注册的 Peer 数量
  int GetRegisteredPeerCount() const;

  // 获取注册的 Group 数量
  int GetRegisteredGroupCount() const;

  // 获取本地节点 ID
  int GetNodeId() const { return node_id_; }

  // 检查是否已注册某个 group
  bool IsGroupRegistered(GroupId group_id) const;

  // 获取某个 group 的所有 Peer Mailbox
  std::vector<Mailbox<PeerMsg>*> GetGroupPeers(GroupId group_id);

 private:
  static uint64_t MakeKey(GroupId gid, raft::raft_node_id_t nid) {
    return (uint64_t(gid) << 32) | uint32_t(nid);
  }

  int node_id_;
  mutable std::mutex mu_;

  // key: (group_id << 32) | local_node_id, value: PeerMailbox*
  std::unordered_map<uint64_t, Mailbox<PeerMsg>*> peers_;

  // 用于快速查找某个 group 的所有 peer
  std::unordered_map<GroupId, std::vector<uint64_t>> group_peers_;
};

}  // namespace multiraft