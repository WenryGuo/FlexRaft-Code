---
name: Remove Hooks - Direct Packing from LrcGrouper
overview: 取消hook机制，在Raft层编码后直接使用LrcComplementaryGrouper的放置规则打包fragments。完善接收端解析和读取检索。
todos:
  - id: modify-encode-entry-lrc
    content: 修改 EncodeRaftEntryLrc，存储所有fragments到Stripe.all_fragments
    status: completed
  - id: add-pack-method
    content: 添加 PackStripesToPeerKeyed 方法，使用 lrc_grouper_->GetFragmentsForNode() 打包
    status: completed
  - id: modify-send
    content: 修改 sendAppendEntries 支持 peer_keyed 模式发送
    status: completed
  - id: modify-receive
    content: 修改 checkConflictEntryAndAppendNew 解析打包数据，追加到日志
    status: completed
  - id: modify-apply
    content: 修改 Apply 层：简化 ApplyStripePacked，废弃 ApplyStripeCommand 中的编码逻辑
    status: completed
  - id: design-read
    content: 设计读取方式：读请求需要知道哪些 frag_id 可用/需要
    status: completed
  - id: deprecate-redundant
    content: 废弃 OrthogonalPlacer 和 stripe_raft_adapter hook 注册
    status: completed
  - id: verify-build
    content: 编译验证并测试
    status: completed
isProject: false
---

# Plan: 取消Hook机制，完善发送-接收-Apply-读取流程

## 整体架构

```
发送端 (Leader):
  EncodeRaftEntryLrc → Stripe.all_fragments → PackStripesToPeerKeyed → sendAppendEntries(peer_keyed)

接收端 (Follower):
  Process(AppendEntriesArgs) → checkConflictEntryAndAppendNew → lm_->AppendLogEntry → tryApplyLogEntries

Apply 层:
  ApplyStripePacked → 直接写入 RocksDB (frag_id → fragment data)

读取:
  读请求携带需要的 frag_ids，检索可用的 fragment，返回给客户端
```

## 修改步骤

### 1. 扩展 Stripe 结构（`raft/encoder.h`）

```cpp
struct Stripe {
  // ... 现有字段 ...
  
  // LRC 模式：存储所有编码后的 fragments
  std::vector<Slice> all_fragments;  // 按 frag_id 顺序存储 k+l+r 个 fragments
  
  // 标记是否已打包为 peer_keyed 格式
  bool peer_keyed_fragments = false;
};
```

### 2. 修改 EncodeRaftEntryLrc（`raft/raft.cc`）

在 LRC 编码后，存储所有 fragments 到 Stripe：

```cpp
int RaftState::EncodeRaftEntryLrc(raft_index_t raft_index,
                                   const multiraft::LrcParams& lrc_params,
                                   Stripe* stripe) {
  // ... 现有编码逻辑 ...

  // 新增：存储所有 fragments 到 Stripe.all_fragments
  stripe->all_fragments.clear();
  
  // Data fragments [0, k)
  for (int i = 0; i < lrc_params.k; ++i) {
    stripe->all_fragments.push_back(lrc_stripe.data_shards[i]);
  }
  // Local parity fragments [k, k+l)
  for (int i = 0; i < lrc_params.l; ++i) {
    stripe->all_fragments.push_back(lrc_stripe.local_parities[i]);
  }
  // Global parity fragments [k+l, k+l+r)
  for (int i = 0; i < lrc_params.r; ++i) {
    stripe->all_fragments.push_back(lrc_stripe.global_parities[i]);
  }
  
  stripe->peer_keyed_fragments = false;
  return lrc_group_id;
}
```

### 3. 添加 PackStripesToPeerKeyed 方法（`raft/raft.cc`）

使用 `lrc_grouper_->GetFragmentsForNode()` 打包：

```cpp
void RaftState::PackStripesToPeerKeyed(raft_index_t raft_index, Stripe* stripe) {
  if (stripe->peer_keyed_fragments) return;  // 已打包
  
  auto* grouper = static_cast<multiraft::LrcComplementaryGrouper*>(lrc_grouper_);
  const auto& lrc_params = grouper->GetLrcParams();
  
  // 清空 fragments，将按 peer_id 重新组织
  stripe->fragments.clear();
  
  // 遍历所有节点
  for (int node_id = 0; node_id < GetClusterServerNumber(); ++node_id) {
    const auto& frag_ids = grouper->GetFragmentsForNode(node_id);
    if (frag_ids.empty()) continue;
    
    // 收集该节点的 fragments
    std::vector<PackedNodeFragment> frags;
    for (int frag_id : frag_ids) {
      if (frag_id >= 0 && frag_id < static_cast<int>(stripe->all_fragments.size())) {
        PackedNodeFragment pf;
        pf.frag_id = frag_id;
        pf.bytes.assign(stripe->all_fragments[frag_id].data(),
                        stripe->all_fragments[frag_id].size());
        frags.push_back(std::move(pf));
      }
    }
    
    // 构建 meta
    StripeLogMeta meta;
    meta.k = lrc_params.k;
    meta.l = lrc_params.l;
    meta.r = lrc_params.r;
    meta.stripe_id = stripe->raft_index;
    meta.entry_id = stripe->raft_index;
    
    // 打包
    std::string packed = PackNodeFragments(meta, frags);
    
    // 创建 LogEntry
    raft::LogEntry encoded_ent;
    encoded_ent.SetIndex(raft_index);
    encoded_ent.SetTerm(stripe->raft_term);
    encoded_ent.SetType(kFragments);
    encoded_ent.SetChunkInfo(ChunkInfo{static_cast<raft_encoding_param_t>(lrc_params.k), raft_index});
    encoded_ent.SetStartOffset(0);
    encoded_ent.SetCommandLength(0);
    
    char* pack_copy = new char[packed.size()];
    std::memcpy(pack_copy, packed.data(), packed.size());
    encoded_ent.SetFragmentSlice(raft::Slice(pack_copy, packed.size()));
    
    // 按 peer_id (node_id) 索引
    stripe->fragments[static_cast<raft_frag_id_t>(node_id)] = encoded_ent;
  }
  
  stripe->peer_keyed_fragments = true;
}
```

### 4. 修改 sendAppendEntries（`raft/raft.cc`）

peer_keyed 模式下使用 node_id 作为 key：

```cpp
void RaftState::sendAppendEntries(raft_node_id_t peer) {
  // ... prev_log_index, prev_term 获取 ...
  
  auto it = encoded_stripe_.find(next_index);
  if (it != encoded_stripe_.end()) {
    Stripe* stripe = it->second;
    
    if (stripe->peer_keyed_fragments) {
      // peer_keyed 模式：直接使用 peer_id 作为 key
      auto frag_it = stripe->fragments.find(static_cast<raft_frag_id_t>(peer));
      if (frag_it != stripe->fragments.end()) {
        args.entries.push_back(frag_it->second);
      }
    } else {
      // 原始模式：按 frag_id 查找应发送的节点
      // ... 原有逻辑 ...
    }
  }
  
  // ... 发送 args ...
}
```

### 5. 修改 checkConflictEntryAndAppendNew（`raft/raft.cc`）

接收端直接追加打包数据到日志，无需解析：

```cpp
void RaftState::checkConflictEntryAndAppendNew(AppendEntriesArgs *args, AppendEntriesReply *reply) {
  // ... 冲突检测逻辑不变 ...
  
  // 追加新 entries（与原有逻辑相同，LogEntry 已包含打包的数据）
  for (auto i = array_index; i < args->entries.size(); ++i) {
    auto raft_index = args->prev_log_index + i + 1;
    lm_->AppendLogEntry(args->entries[i]);
    if (storage_) {
      storage_->AppendEntry(args->entries[i]);
    }
    // ... reply chunk_info ...
  }
}
```

**注意**：接收端无需额外解析，因为收到的 LogEntry 已经是打包格式（kFragments 类型）。

### 6. 修改 Apply 层（`multi-raft/stripe_apply.cc`）

简化 ApplyStripePacked，直接写入 RocksDB：

```cpp
bool ApplyStripePacked(kv::KvServiceNode* kv_node, GroupId group_id,
                       raft::raft_node_id_t node_id, const std::string& payload) {
  // 解包获取 meta 和 fragments
  StripeLogMeta meta;
  std::vector<PackedNodeFragment> frags;
  if (!UnpackNodeFragments(payload.data(), payload.size(), &meta, &frags)) {
    return false;
  }
  meta.group_id = group_id;
  
  // 直接写入 RocksDB
  rocksdb::WriteBatch batch;
  batch.Put(StripeMetaDbKey(meta.user_key), meta.Serialize());
  for (const auto& f : frags) {
    batch.Put(FragDbKey(group_id, meta.stripe_id, f.frag_id), f.bytes);
  }
  return kv_node->GetKvServer()->DB()->Write(&batch);
}
```

### 7. 废弃 ApplyStripeCommand 中的编码逻辑

当使用 peer_keyed 模式时，Leader 已经在发送前完成编码和打包，Follower 无需再调用 `EncodeStripePayload`。

简化 `ApplyStripeCommand` 或标记废弃：

```cpp
// [DEPRECATED] 使用 peer_keyed 模式后不再需要此函数
bool ApplyStripeCommand(...) {
  // 保留但简化：如果收到 StripeWriteCommand，直接使用
  // 或返回 false，让调用方使用 ApplyStripePacked
}
```

### 8. 读取设计

#### 读取流程

```
Client GET 请求
    ↓
RaftStore 查询该 key 对应的 StripeMeta (获取 meta)
    ↓
根据 meta.placement 获取该节点持有的 frag_ids
    ↓
从 RocksDB 读取本地 fragments
    ↓
如果 fragments 足够（k 个），解码恢复原始数据
    ↓
返回给客户端
```

#### 读取需要的 frag_ids

读取时需要知道：
1. 该 stripe 分配了哪些 frag_ids
2. 其中哪些 frag_ids 在本地节点
3. 需要多少个 fragments 才能解码（k 个）

从 `StripeLogMeta` 可以获取：
- `meta.k`, `meta.l`, `meta.r` - 编码参数
- `meta.placement` - 每个 frag_id 的放置信息

本地读取示例：

```cpp
// 在 raft_store.h 或 kv 服务中实现
std::string ReadStripeData(GroupId group_id, const std::string& user_key) {
  // 1. 读取 meta
  auto meta_data = db->Get(StripeMetaDbKey(user_key));
  StripeLogMeta meta;
  StripeLogMeta::Deserialize(meta_data.data(), meta_data.size(), &meta);
  
  // 2. 获取本地 frag_ids
  std::vector<int> local_frag_ids;
  for (const auto& fp : meta.placement) {
    if (fp.node_id == my_node_id) {
      local_frag_ids.push_back(fp.frag_id);
    }
  }
  
  // 3. 读取本地 fragments
  std::vector<Slice> frags;
  for (int frag_id : local_frag_ids) {
    auto frag_data = db->Get(FragDbKey(group_id, meta.stripe_id, frag_id));
    frags.push_back(Slice(frag_data.data(), frag_data.size()));
  }
  
  // 4. 如果有 k 个 fragments，解码
  if (frags.size() >= meta.k) {
    // 调用解码逻辑恢复数据
  }
}
```

#### 解码需要的辅助信息

读取时需要知道：
- `meta.k` - 需要多少个 data fragments
- `meta.placement` - 每个 frag_id 对应的节点

如果本地有 >= k 个 fragments，直接解码。
如果本地不足 k 个，需要：
1. 查询其他节点获取缺失的 fragments
2. 或返回 "数据不可用，需要修复"

### 9. 废弃 OrthogonalPlacer 相关函数

在 `multi-raft/lrc_placement.h` 中：

```cpp
// 以下函数废弃，使用 LrcComplementaryGrouper::GetFragmentsForNode() 替代
#if 0  // DEPRECATED
class OrthogonalPlacer { ... };
inline std::vector<LocalGroup> BuildTwoComplementaryPartitions(...) { ... }
inline std::vector<RsRandomPlacement> BuildRsRandomPlacement(...) { ... }
#endif
```

### 10. 清理 Hook 注册

注释掉 `multi-raft/stripe_raft_adapter.cc` 中的 hook 注册：

```cpp
// [DEPRECATED] 不再需要 hook 机制
// void RegisterStripeRaftHooks() {
//   raft::RegisterStripeHooks(IsStripeCmdHook, EncodeStripeHook);
// }
```

查找并注释所有调用 `RegisterStripeRaftHooks()` 的地方。

## 关键文件变更

| 文件 | 变更 |
|------|------|
| `raft/encoder.h` | Stripe 结构添加 all_fragments, peer_keyed_fragments |
| `raft/raft.cc` | EncodeRaftEntryLrc 添加存储，添加 PackStripesToPeerKeyed，修改 sendAppendEntries |
| `raft/raft.h` | 添加 PackStripesToPeerKeyed 声明 |
| `multi-raft/stripe_apply.cc` | 简化 ApplyStripePacked |
| `multi-raft/lrc_placement.h` | 废弃 OrthogonalPlacer 等函数 |
| `multi-raft/stripe_raft_adapter.cc` | 注释 hook 注册 |

## 验证点

1. 编译通过
2. Leader 编码后正确存储 fragments
3. PackStripesToPeerKeyed 正确使用 lrc_grouper_ 的放置规则
4. 每节点收到恰好 2 个 fragments
5. Follower 正确追加到日志
6. Apply 层正确写入 RocksDB
7. 读取能够检索到需要的 frag_ids 并解码

## 数据流总结

```
写入流程:
  Client → Propose → Leader Encode → Pack → sendAppendEntries → Follower Append → Apply → RocksDB

读取流程:
  Client GET → 查询 Meta → 获取本地 frag_ids → 读取 Fragments → 解码 → 返回
```
