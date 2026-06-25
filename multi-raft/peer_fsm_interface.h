#pragma once
// peer_fsm_interface.h  —  PeerFsm 依赖的接口定义
//
// 解决 peer_fsm.h 和 raft_store.h 之间的循环依赖
// 将需要相互引用的接口定义在这里

#include <functional>
#include <vector>

#include "message.h"

namespace multiraft {

// =======================================================================
//  CrossGroupTrackerInterface  —  跨组追踪器接口
// =======================================================================
class CrossGroupTrackerInterface {
 public:
  virtual ~CrossGroupTrackerInterface() = default;

  virtual void SetGroupCount(int num_groups) = 0;

  // ApplyFsm 调用：本 group 已将 entry 写入状态机
  virtual void OnLocalCommit(EntryId eid, GroupId gid) = 0;

  // 注册一个 entry 的完成回调
  virtual void Register(EntryId eid, std::function<void(EntryId)> cb) = 0;

  // 中止追踪
  virtual void Abort(EntryId eid) = 0;
};

// =======================================================================
//  RoutingTableManagerInterface  —  路由表管理器接口
// =======================================================================
class RoutingTableManagerInterface {
 public:
  virtual ~RoutingTableManagerInterface() = default;

  // 添加路由条目（支持互补分组关联）
  virtual void AddRoute(const GroupTopology& entry) = 0;

  // 合并来自其他节点的路由表
  virtual void MergeRoutes(const RoutingTable& other, int ttl) = 0;

  // 获取路由表大小
  virtual int Size() const = 0;

  // 获取完整路由表
  virtual RoutingTable GetAllRoutes() const = 0;

  // 获取路由表管理器本身（向下转型用）
  virtual void* GetSelf() = 0;
};

// =======================================================================
//  RaftStoreInterface  —  RaftStore 接口
// =======================================================================
class RaftStoreInterface {
 public:
  virtual ~RaftStoreInterface() = default;

  // 获取路由表管理器
  virtual RoutingTableManagerInterface* GetRoutingTableManager() = 0;

  // 获取跨组追踪器
  virtual CrossGroupTrackerInterface* GetTracker() = 0;

  // 获取路由表
  virtual RoutingTable GetRoutingTable() const = 0;

  // 合并路由表
  virtual void MergeRoutingTable(const RoutingTable& routes, int ttl) = 0;

  // 添加路由条目（支持互补分组关联）
  virtual void AddRouteEntry(StripeId stripe_id, EntryId entry_id,
                             const std::vector<GroupId>& data_groups,
                             bool has_global_parities = false,
                             const std::vector<GroupId>& global_parity_groups = {}) = 0;

  // 直接写入接口
  virtual void LocalProposeToGroup(GroupId group_id,
                                   const raft::Slice& data,
                                   std::function<void(bool, EntryId)> cb) = 0;

  // 更新 group_id
  virtual void UpdateGroupId(GroupId old_group_id, GroupId new_group_id,
                             std::function<void(bool)> cb) = 0;

  // 动态编码参数：为指定 group 设置固定的 k 值。
  // 0 = 自动模式（k = live_servers - F），非零 = 固定 k。
  // m 始终为 N - k。Commit 阈值始终为 F + k。
  // 用于对比实验（如固定 k=3, k=5, k=7）。
  virtual void SetGroupDynamicK(GroupId group_id, raft::raft_encoding_param_t k) = 0;

  // Multi-raft erasure mode for logging (0=RS_F, 1=RS_3F, 2=LRC), aligns with raft encoding_mode_.
  virtual int GetClusterEncodingMode() const { return 0; }
};

// =======================================================================
//  RaftRouterInterface  —  RaftRouter 接口
// =======================================================================
class RaftRouterInterface {
 public:
  using PeerMailbox = Mailbox<PeerMsg>;

  virtual ~RaftRouterInterface() = default;

  // 向 (to_group, to_node) 发送消息
  virtual bool SendToPeer(GroupId to_group, raft::raft_node_id_t to_node, PeerMsg msg) = 0;

  // 广播给指定 group 的所有节点
  virtual void BroadcastToGroup(GroupId to_group, PeerMsg msg) = 0;

  // 获取指定 group 的 mailbox
  virtual PeerMailbox* GetGroupMailbox(GroupId to_group) = 0;
};

}  // namespace multiraft
