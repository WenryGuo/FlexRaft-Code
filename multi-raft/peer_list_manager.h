#pragma once
// peer_list_manager.h — 集群peer_list管理器
//
// 核心设计：
//   1. 管理集群peer_list，按group_id分组
//   2. 提供group成员查询接口
//   3. 支持动态成员变更

#include <map>
#include <memory>
#include <shared_mutex>
#include <string>
#include <vector>

namespace multiraft {

using GroupId = uint32_t;

// 物理节点信息
struct PhysicalNodeInfo {
  int physical_node_id;
  std::string raft_rpc_addr;  // Raft RPC地址
  std::string kv_rpc_addr;    // KV RPC地址
};

// Group成员信息
struct GroupMembership {
  GroupId group_id;
  std::vector<int> member_nodes;  // 该group的所有成员（物理节点ID）
  std::map<int, PhysicalNodeInfo> node_info;  // 成员详细信息

  GroupMembership() = default;
  explicit GroupMembership(GroupId gid) : group_id(gid) {}

  void AddMember(int physical_node_id, const PhysicalNodeInfo& info) {
    if (std::find(member_nodes.begin(), member_nodes.end(), physical_node_id) == member_nodes.end()) {
      member_nodes.push_back(physical_node_id);
    }
    node_info[physical_node_id] = info;
  }

  void RemoveMember(int physical_node_id) {
    member_nodes.erase(
        std::remove(member_nodes.begin(), member_nodes.end(), physical_node_id),
        member_nodes.end());
    node_info.erase(physical_node_id);
  }

  bool HasMember(int physical_node_id) const {
    return node_info.find(physical_node_id) != node_info.end();
  }

  size_t Size() const { return member_nodes.size(); }
};

// PeerList管理器
class PeerListManager {
 public:
  PeerListManager() = default;
  ~PeerListManager() = default;

  // 添加group
  void AddGroup(GroupId group_id, const GroupMembership& membership);

  // 移除group
  void RemoveGroup(GroupId group_id);

  // 更新group成员
  void UpdateGroupMembership(GroupId group_id, const GroupMembership& membership);

  // 添加group成员
  void AddGroupMember(GroupId group_id, int physical_node_id, const PhysicalNodeInfo& info);

  // 移除group成员
  void RemoveGroupMember(GroupId group_id, int physical_node_id);

  // 获取group成员信息
  const GroupMembership* GetGroupMembership(GroupId group_id) const;

  // 获取所有group
  std::vector<GroupId> GetAllGroups() const;

  // 检查group是否存在
  bool HasGroup(GroupId group_id) const;

  // 获取group数量
  size_t GetGroupCount() const { return peer_list_.size(); }

  // 打印peer_list状态
  void PrintStatus() const;

 private:
  mutable std::shared_mutex rw_lock_;
  std::map<GroupId, GroupMembership> peer_list_;
};

}  // namespace multiraft
