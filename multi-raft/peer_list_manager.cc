#include "peer_list_manager.h"

#include <algorithm>
#include <cstdio>
#include <mutex>

namespace multiraft {

void PeerListManager::AddGroup(GroupId group_id, const GroupMembership& membership) {
  std::unique_lock<std::shared_mutex> lock(rw_lock_);
  peer_list_[group_id] = membership;
  printf("[PEER-LIST] Added group %u with %zu members\n",
         group_id, membership.Size());
}

void PeerListManager::RemoveGroup(GroupId group_id) {
  std::unique_lock<std::shared_mutex> lock(rw_lock_);
  peer_list_.erase(group_id);
  printf("[PEER-LIST] Removed group %u\n", group_id);
}

void PeerListManager::UpdateGroupMembership(GroupId group_id, const GroupMembership& membership) {
  std::unique_lock<std::shared_mutex> lock(rw_lock_);
  peer_list_[group_id] = membership;
  printf("[PEER-LIST] Updated group %u with %zu members\n",
         group_id, membership.Size());
}

void PeerListManager::AddGroupMember(GroupId group_id, int physical_node_id, const PhysicalNodeInfo& info) {
  std::unique_lock<std::shared_mutex> lock(rw_lock_);

  auto it = peer_list_.find(group_id);
  if (it == peer_list_.end()) {
    printf("[PEER-LIST] ERROR: Group %u not found\n", group_id);
    return;
  }

  it->second.AddMember(physical_node_id, info);
  printf("[PEER-LIST] Added member %d to group %u\n", physical_node_id, group_id);
}

void PeerListManager::RemoveGroupMember(GroupId group_id, int physical_node_id) {
  std::unique_lock<std::shared_mutex> lock(rw_lock_);

  auto it = peer_list_.find(group_id);
  if (it == peer_list_.end()) {
    printf("[PEER-LIST] ERROR: Group %u not found\n", group_id);
    return;
  }

  it->second.RemoveMember(physical_node_id);
  printf("[PEER-LIST] Removed member %d from group %u\n", physical_node_id, group_id);
}

const GroupMembership* PeerListManager::GetGroupMembership(GroupId group_id) const {
  std::shared_lock<std::shared_mutex> lock(rw_lock_);

  auto it = peer_list_.find(group_id);
  if (it == peer_list_.end()) {
    return nullptr;
  }

  return &it->second;
}

std::vector<GroupId> PeerListManager::GetAllGroups() const {
  std::shared_lock<std::shared_mutex> lock(rw_lock_);

  std::vector<GroupId> groups;
  for (const auto& [gid, _] : peer_list_) {
    groups.push_back(gid);
  }
  return groups;
}

bool PeerListManager::HasGroup(GroupId group_id) const {
  std::shared_lock<std::shared_mutex> lock(rw_lock_);
  return peer_list_.find(group_id) != peer_list_.end();
}

void PeerListManager::PrintStatus() const {
  std::shared_lock<std::shared_mutex> lock(rw_lock_);

  printf("\n========================================\n");
  printf("       PEER LIST STATUS (%zu groups)\n", peer_list_.size());
  printf("========================================\n");

  for (const auto& [gid, membership] : peer_list_) {
    printf("Group %u: %zu members [", gid, membership.Size());
    for (size_t i = 0; i < membership.member_nodes.size(); ++i) {
      printf("%d", membership.member_nodes[i]);
      if (i < membership.member_nodes.size() - 1) {
        printf(", ");
      }
    }
    printf("]\n");
  }

  printf("========================================\n\n");
}

}  // namespace multiraft
