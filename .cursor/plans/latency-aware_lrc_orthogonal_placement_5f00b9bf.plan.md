---
name: Latency-Aware LRC Orthogonal Placement
overview: 实现延迟感知的 LRC 正交放置策略，重构编码和复制阶段，将 RS 编码替换为 LRC 编码，根据延迟矩阵对节点进行互补互斥分组。
todos:
  - id: step1-latency-matrix
    content: 新增 LatencyMatrix 类 (multi-raft/latency_matrix.h)
    status: completed
  - id: step2-stripe-update
    content: 更新 Stripe 结构体添加 LRC 元数据字段
    status: completed
  - id: step3-lrc-grouper
    content: 实现 LrcComplementaryGrouper 互补互斥分组器
    status: completed
  - id: step4-raftstore-integration
    content: 集成延迟矩阵和分组器到 RaftStore
    status: completed
  - id: step5-encode-raft-entry
    content: 重构 EncodeRaftEntry 使用 LRC 编码
    status: completed
  - id: step6-send-append-entries
    content: 重构 sendAppendEntries 支持正交放置
    status: completed
  - id: step7-replicate-entry
    content: 更新 ReplicateNewProposeEntry 调用新接口
    status: completed
  - id: todo-1779379815736-me27d4wbj
    content: rm -rf build && cmake -B build -DCMAKE_BUILD_TYPE=Debug  -DLOG=off -DPERF=off && cmake --build build -j 4 编译通过
    status: cancelled
isProject: false
---

# Latency-Aware LRC Orthogonal Placement - 执行计划

## 一、代码库现状分析

### 当前写入路径

```
Client → RoutePut → GroupRegistry.DealWithRequest → RaftStore.StripePut → 
RaftNode.Propose → RaftState.Propose → ReplicateNewProposeEntry → 
EncodeRaftEntry → sendAppendEntries → sendAsyncMessage
```

### 关键文件

- `raft/raft.cc`: `ReplicateNewProposeEntry` (L1217), `EncodeRaftEntry` (L1121), `sendAppendEntries` (L1506)
- `raft/raft.h`: `RaftState` 类, `Stripe` 结构体
- `multi-raft/lrc_encoder.h`: 已有 `LrcParams`, `LrcEncoder` 框架
- `multi-raft/lrc_group_builder.h`: 已有 `LrcGroupBuilder`, `LocalGroup` 结构
- `multi-raft/lrc_placement.h`: 已有 `OrthogonalPlacer`

---

## 二、需要新增的类/结构体

### 1. `LatencyMatrix` 类（新增文件）

**文件**: `multi-raft/latency_matrix.h`

```cpp
class LatencyMatrix {
public:
  LatencyMatrix(int N);
  
  void GenerateRandomMatrix(int min_ms=30, int max_ms=300);
  int GetLatency(int src, int dst) const;
  float GetAvgLatency(int node_id) const;
  const std::vector<std::pair<int, float>>& GetSortedPeersByLatency(int node_id) const;
  
private:
  int N_;
  std::vector<std::vector<int>> matrix_;  // N×N delay matrix in ms
  std::vector<std::vector<std::pair<int, float>>> sorted_peers_;  // sorted by latency
};
```

### 2. `LrcNodePlacement` 结构体（新增）

**文件**: `multi-raft/raft_store.h` 或 `raft/raft.h`

```cpp
struct LrcNodePlacement {
  int frag_id;           // fragment ID [0, k+l+r)
  int node_id;           // physical node ID [0, N)
  int lrc_group_id;      // which LRC local group this fragment belongs to
  FragmentKind kind;     // kData, kLocalParity, kGlobalParity
};
```

### 3. 更新 `Stripe` 结构体

**文件**: `raft/raft.h`

```cpp
struct Stripe {
  raft_index_t raft_index;
  raft_term_t raft_term;
  std::map<raft_frag_id_t, LogEntry> fragments;
  
  // LRC 相关新增字段
  int lrc_group_id;                              // 该条带所属的 LRC group ID
  std::vector<LrcNodePlacement> node_placements; // 每个 fragment → node 的映射
};
```

### 4. `LrcComplementaryGrouper` 类（新增）

**文件**: `multi-raft/lrc_group_builder.h` 或新建 `lrc_complementary_grouper.h`

```cpp
class LrcComplementaryGrouper {
public:
  // 根据延迟矩阵进行互补互斥分组
  // 将两两之间延迟较低的节点分到同一个 LRC 组中
  std::vector<LocalGroup> BuildComplementaryGroups(
      const LatencyMatrix& matrix,
      int N, int l, int k);
  
  // 返回给定节点的 2 个互补互斥分片节点
  std::pair<int, int> GetComplementaryNodes(int node_id, int frag_id);
  
private:
  std::vector<LocalGroup> groups_;
  std::unordered_map<int, std::vector<int>> node_to_frags_;  // node → frag IDs
};
```

---

## 三、延迟矩阵初始化模块的挂载位置

### 方案：挂在 `RaftStore` 下

在 `RaftStore` 类中增加延迟矩阵和分组管理：

```cpp
// multi-raft/raft_store.h
class RaftStore {
public:
  void InitLatencyAndTopology(int num_nodes);
  
private:
  LatencyMatrix latency_matrix_;
  LrcComplementaryGrouper lrc_grouper_;
  std::vector<LocalGroup> lrc_groups_;
};
```

**初始化流程**:

```
RaftStore::Init() 
  → InitLatencyAndTopology(num_nodes)
    → latency_matrix_.GenerateRandomMatrix(30, 300)
    → lrc_grouper_.BuildComplementaryGroups(latency_matrix_, N, l, k)
```

---

## 四、函数签名修改

### 1. `ReplicateNewProposeEntry`

**当前签名**:

```cpp
void RaftState::ReplicateNewProposeEntry(raft_index_t raft_index);
```

**修改后签名**:

```cpp
void RaftState::ReplicateNewProposeEntry(
    raft_index_t raft_index,
    int lrc_group_id = -1);  // 新增：指定该条带属于哪个 LRC group
```

**改动说明**:

- 参数 `lrc_group_id`: 由调用方传入，当前可以用 `group_id_ % l` 或轮询分配

### 2. `EncodeRaftEntry`

**当前签名**:

```cpp
void RaftState::EncodeRaftEntry(
    raft_index_t raft_index,
    raft_encoding_param_t k,
    raft_encoding_param_t m,
    Stripe *stripe);
```

**修改后签名**:

```cpp
void RaftState::EncodeRaftEntry(
    raft_index_t raft_index,
    const LrcParams& lrc_params,
    Stripe *stripe,
    const std::vector<LrcNodePlacement>& placements);  // 新增：正交放置映射
```

**改动说明**:

- 用 `LrcParams` 替代单独的 `k, m` 参数
- `placements` 向量描述每个 fragment 应该发送到哪个物理节点

### 3. `sendAppendEntries`

**当前签名**:

```cpp
void RaftState::sendAppendEntries(raft_node_id_t peer);
```

**修改后签名**:

```cpp
void RaftState::sendAppendEntries(
    raft_node_id_t peer,
    const std::vector<int>& assigned_frag_ids);  // 新增：该 peer 负责的 frag IDs
```

**改动说明**:

- 参数 `assigned_frag_ids`: 长度为 2，包含分配给该 peer 的 fragment IDs
- 查找 `encoded_stripe_[raft_index]->fragments[frag_id]` 而非 `fragments[peer]`

---

## 五、分步实施计划

### Step 1: 新增延迟矩阵类

- 创建 `multi-raft/latency_matrix.h`
- 实现 N×N 随机延迟矩阵生成 (30ms~300ms)
- 实现按延迟排序的节点查询

### Step 2: 更新 Stripe 结构体

- 在 `raft/raft.h` 的 `Stripe` 结构体中添加 LRC 元数据字段
- 确保向后兼容（添加默认值）

### Step 3: 实现互补互斥分组器

- 扩展 `multi-raft/lrc_group_builder.h`
- 实现基于延迟矩阵的互补互斥分组算法
- 实现 `GetComplementaryNodes()` 返回每个节点的 2 个目标 fragment

### Step 4: 集成到 RaftStore

- 在 `RaftStore` 中添加延迟矩阵和分组器成员
- 在 `Init()` 中初始化延迟矩阵
- 提供 `GetNodePlacements()` 方法供 RaftState 调用

### Step 5: 重构 EncodeRaftEntry

- 将 RS 编码替换为调用 `EncoderForLRC()`
- 根据 placements 填充 `stripe->node_placements`
- 更新 `fragments` map 的 key 策略（从 peer_id 改为 frag_id）

### Step 6: 重构 sendAppendEntries

- 修改 payload 组装逻辑
- 根据 `assigned_frag_ids` 抓取对应的 fragment
- 确保每个物理节点收到确切的 2 个编码块

### Step 7: 更新 ReplicateNewProposeEntry

- 使用新的编码参数（LrcParams 而非 k/m）
- 调用新的 `EncodeRaftEntry` 和 `sendAppendEntries`
- 处理 LRC 组的分配逻辑

---

## 六、关键设计决策点

1. **LrcParams 的获取方式**: 是硬编码 (k=4, l=2, r=2) 还是从系统参数推导？
2. **lrc_group_id 的分配策略**: 轮询、哈希还是基于拓扑感知？
3. **延迟矩阵的持久化**: 是否需要存储到配置文件中？
4. **跨组复制的处理**: 当节点失效需要跨组修复时的行为？
方案执行时设计的决策设计如下：
1. **LrcParams 的获取方式**: LRC编码参数根据系统参数推导，现在已经实现的方式k=F+1, l=2, r=2N-k-l.
2. **lrc_group_id 的分配策略**: lrc_group_id 不应由调用方简单地轮询或取模生成。相反，分配逻辑应该集�