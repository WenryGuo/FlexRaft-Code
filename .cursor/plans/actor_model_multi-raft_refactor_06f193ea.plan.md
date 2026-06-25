---
name: Actor Model Multi-Raft Refactor
overview: 基于 Actor 模型的 Multi-Raft 架构重构。评估当前代码架构，设计分步骤实现方案，确保复用已有组件，避免代码冗余。
todos:
  - id: step1
    content: "Step 1: 添加 SetRaftNode() 到 kv/kv_server.h"
    status: completed
  - id: step2
    content: "Step 2: 添加 SetRaftNode() 到 kv/kv_node.h 和 .cc"
    status: completed
  - id: step3
    content: "Step 3: 在 RaftStore 中添加 raft_nodes_ 存储"
    status: completed
  - id: step4
    content: "Step 4: 修改 CreateRaftInstances() 正确创建 RaftNode"
    status: completed
  - id: step5
    content: "Step 5: 修改 InitRaftInstances() 调整初始化顺序"
    status: completed
  - id: step6
    content: "Step 6: 添加 PostInitAll() 到 GroupRegistry"
    status: cancelled
  - id: step7
    content: "Step 7: 编译测试验证"
    status: completed
  - id: step7b
    content: "Step 7b: 修复 RaftNode rcf_server_ 空指针和 Multi-Raft 退出问题"
    status: completed
isProject: false
---

## 架构评估与修改方案

### 一、当前架构分析

**现有组件及对应关系：**

```
bench_server_multiraft.cc (Phase 1-8)
    │
    └── RaftStore (raft_store.h)
            │
            ├── RaftUnifiedRpcServer ── 单物理节点1个，复用端口路由所有 group
            │       └── RaftUnifiedRPCService → group_id → RaftState*
            │
            ├── BatchTransportManager ── 批量传输管理
            │
            ├── GroupRegistry
            │       └── KvServiceNode[] (每group 1个)
            │               └── KvServer → channel_, db_
            │                       └── (RaftNode* raft_ = nullptr)
            │
            ├── RaftRouter
            │       └── Mailbox<PeerMsg>[] (每group 1个)
            │
            ├── BatchSystem ── 线程池调度 PeerFsm / ApplyFsm
            │       ├── PeerFsm[] ── Actor-like per-group handler
            │       │       └── KvServiceNode* → RaftNode*
            │       └── ApplyFsm[] ── 将 committed entries 写入 RocksDB
            │
            ├── CrossGroupTracker ── 跨 group commit 聚合
            ├── RoutingTableManager ── 路由表管理
            └── GossipThread ── 路由表同步
```

**已有 Raft 层组件：**

```
raft/raft.h      — RaftState (核心 Raft 算法)
raft/raft_node.h — RaftNode (封装 RaftState + 线程 + RPC)
raft/raft_node.cc — Init(): 创建自己的 rcf_server_ + rcf_clients_ + raft_state_
raft/raft_unified_rpc_server.h — RaftUnifiedRpcServer (Multi-Raft RPC 路由)
raft/raft_unified_rpc_server.cc — 通过 group_id 路由到 RaftState*
```

---

### 二、Actor 模型映射（现有代码已部分对应）

| Actor 模型概念 | FlexRaft 对应 | 状态 |
|---|---|---|
| **Actor = State + Mailbox + Behavior** | PeerFsm | 已有但有 bug |
| **Node = ActorSystem** | RaftStore | 可复用 |
| **Group = Actor 实例** | PeerFsm (每group 1个) | 已设计 |
| **共享线程池** | BatchSystem | 已实现 |
| **共享 RPC** | RaftUnifiedRpcServer | 已实现 |
| **Actor.handle()** | PeerFsm::HandleBatch() | 已实现 |
| **Actor 的私有状态** | RaftState* (由 RaftNode 管理) | **缺失：RaftNode 未创建！** |

---

### 三、现有架构设计评估

**合理之处（可复用）：**

1. **RaftUnifiedRpcServer** — 正确设计，单端口按 group_id 路由
2. **BatchSystem** — 正确设计，少量线程轮询多个 Mailbox（TiKV BatchSystem 模式）
3. **PeerFsm / ApplyFsm** — Actor 模型的核心，可复用
4. **Mailbox** — MPSC 队列，Actor 间通信，可复用
5. **8-Phase 启动流程** — 清晰有序，可复用
6. **GroupRegistry** — KvServiceNode 管理，可复用

**问题所在（需修复）：**

| 问题 | 位置 | 原因 |
|---|---|---|
| **RaftNode 从未被创建** | Phase 6 `CreateRaftInstances()` | `NewKvServiceNodeWithoutRPC` 传入 `nullptr` |
| **RaftState 从未被创建** | RaftNode::Init() 未调用 | RaftNode 对象不存在 |
| **PostInit 依赖不存在的 RaftNode** | Phase 7 `InitRaftInstances()` | 调用 `node->GetRaftNode()->PostInit()` 但 RaftNode 为 nullptr |
| **UnifiedRpcServer 未注册 RaftState** | Phase 7 | `RegisterRaftState` 从未被调用 |

**崩溃路径：**

```
Phase 7: InitRaftInstances()
    ↓
registry_.InitAll()           ← Phase 6 创建的 KvServiceNode，raft_ = nullptr
    ↓
KvServiceNode::InitServiceNodeState()
    ↓
KvServer::Init() → raft_->Init()  ← nullptr -> Init()  崩溃！
```

---

### 四、完整修改方案

#### 第一步（最小可行修改）：修复 Phase 6-7 的 Raft 实例创建流程

**只需修改 Phase 6，在 `CreateRaftInstances()` 中正确创建 RaftNode：**

```cpp
// raft_store.h 的 CreateRaftInstances() 中，替换 registry_.Register()
// 原代码（错误）：
kv::KvServiceNode* node = registry_.Register(mem.group_id, mem.local_node_id, ...);

// 新代码：正确创建 RaftNode 并注册到 UnifiedRpcServer
kv::KvServiceNode* node = registry_.Register(mem.group_id, mem.local_node_id, ...);

// 找到刚注册的 KvServiceNode，创建 RaftNode 并注入
if (node) {
    // 1. 构建 NodeConfig
    raft::RaftNode::NodeConfig node_config;
    node_config.node_id_me = mem.local_node_id;
    for (const auto& [peer_id, addr_info] : mem.group_cluster_cfg) {
        node_config.servers[peer_id] = addr_info.raft_rpc_addr;
    }
    node_config.rsm = nullptr;  // 暂时不需要 RSM
    
    // 2. 创建 RaftNode
    auto raft_node = std::make_unique<raft::RaftNode>(node_config);
    
    // 3. 注入到 KvServiceNode（通过 RaftNode::SetRaftNode 或直接修改成员）
    node->GetRaftServer()->SetRaftNode(raft_node.get());
    
    // 4. 保存 raft_node 所有权
    // (在 RaftStore 中添加存储)
}
```

#### 第二步：修改 RaftStore 存储创建的 RaftNode

在 `raft_store.h` 的 `RaftStore` 类中添加：

```cpp
// 在 RaftStore 私有成员中添加：
std::unordered_map<GroupId, std::unique_ptr<raft::RaftNode>> raft_nodes_;

// 在 CreateRaftInstances() 中：
auto raft_node = std::make_unique<raft::RaftNode>(node_config);
raft_nodes_[mem.group_id] = std::move(raft_node);
```

#### 第三步：修改 Phase 7 的 InitRaftInstances()

Phase 7 不需要大改，但需要确保 `InitAll()` 在 `PostInit()` **之后**调用（因为 `InitAll()` 会调用 `raft_->Init()`）：

```cpp
// Phase 7 InitRaftInstances() 修改顺序：
// 1. 先 PostInit（注册到 UnifiedRpcServer）
// 2. 再 InitAll（启动 ticker）
registry_.PostInitAll();  // 新增方法：调用 RaftNode::PostInit() 而非 Init()

// 旧的 registry_.InitAll() 需要修改或移除对 RaftNode::Init() 的调用
// 或者让 RaftStore 直接调用 RaftNode::Init() 在 PostInit() 之后
```

#### 第四步：Actor 模型正式化（可选，保持 PeerFsm 作为 Actor）

当前 `PeerFsm` 已经是 Actor-like 的设计（State + Mailbox + Behavior），无需大改：
- `PeerFsm.group_id_` + `PeerFsm.local_node_id_` = ActorId
- `PeerFsm.mailbox_` = Mailbox
- `PeerFsm::HandleBatch()` = Behavior

唯一需要修改的是：PeerFsm 应该直接持有 `RaftState*`（而不是通过 `kv_node_->GetRaftNode()->getRaftState()` 获取）：

```cpp
// peer_fsm.h 修改：
class PeerFsm {
  raft::RaftState* raft_state_;  // 直接持有，而非通过 kv_node_
  // ...
};
```

---

### 五、推荐执行顺序

| 步骤 | 内容 | 目标 |
|---|---|---|
| **Step 1** | 修复 `kv/kv_server.h`：添加 `SetRaftNode()` 方法 | 允许外部注入 RaftNode |
| **Step 2** | 修复 `kv/kv_node.h`：添加 `SetRaftNode()` 方法 | 允许外部注入 RaftNode |
| **Step 3** | 修改 `raft_store.h`：在 `RaftStore` 中添加 `raft_nodes_` 存储 | 保存创建的 RaftNode |
| **Step 4** | 修改 `raft_store.h` `CreateRaftInstances()`：正确创建 RaftNode 并注入 | 核心修复 |
| **Step 5** | 修改 `raft_store.h` `InitRaftInstances()`：调整初始化顺序 | 确保 Init 在 PostInit 之后 |
| **Step 6** | 修改 `group_registry.h`：添加 `PostInitAll()` 方法 | 支持分离 PostInit 和 Init |
| **Step 7** | 编译测试，验证 Phase 7 不再崩溃 | 验证修复 |
| **Step 8** | 可选：修改 `PeerFsm` 直接持有 `RaftState*` | Actor 模型正式化 |

---

### 六、架构总结图（修改后）

```
┌─────────────────────────────────────────────────────────────────┐
│                     Physical Node (ActorSystem)                   │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  RaftStore                                                       │
│    │                                                           │
│    ├── RaftUnifiedRpcServer (:51010)  ←── 共享，复用端口       │
│    │       └── group_id → RaftState* (通过 RegisterRaftState)  │
│    │                                                           │
│    ├── BatchTransportManager ── 批量传输                         │
│    │                                                           │
│    ├── BatchSystem ── 线程池 (PeerPoll / ApplyPoll / Tick)     │
│    │       │                                                   │
│    │       ├── PeerFsm[0] ── Actor G1 ── Mailbox → RaftState*  │
│    │       ├── PeerFsm[1] ── Actor G2 ── Mailbox → RaftState*  │
│    │       └── ...                                             │
│    │           │                                               │
│    │           └── ApplyFsm[] ── 写入 RocksDB                  │
│    │                                                           │
│    └── GroupRegistry ── KvServiceNode[]                         │
│            └── KvServer (raft_ = RaftNode*)                    │
│                    └── RaftNode* ── RaftState* ── Storage       │
│                            │                                   │
│                            ├── ticker 线程                      │
│                            └── (不再创建自己的 RPC Server)       │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

---

### 七、废弃代码标注

以下代码路径在修改后不再使用，应标注为废弃：

```cpp
// kv_node.h / kv_node.cc 中的以下函数：
KvServiceNode::NewKvServiceNode()        // 单 Raft 模式，不再用于 Multi-Raft
KvServiceNode::NewKvServiceNodeWithExternalRaftNode()  // 可保留用于外部注入
KvServiceNode::NewKvServiceNodeWithoutRPC()  // 废弃，原因：需要 RaftNode

// raft_node.cc 中的以下逻辑：
RaftNode::Init() 中的 rcf_server_ 创建  // 废弃：Multi-Raft 使用 UnifiedRpcServer
RaftNode::Init() 中的 rcf_clients_ 创建  // 保留：仍需 RPC 客户端（通过 BatchTransport）
```

---

### 八、关键文件修改摘要

| 文件 | 修改内容 |
|---|---|
| `kv/kv_server.h` | 添加 `SetRaftNode()` 方法 |
| `kv/kv_node.h` | 添加 `SetRaftNode()` 方法 |
| `kv/kv_node.cc` | 实现 `SetRaftNode()` |
| `multi-raft/raft_store.h` | 添加 `raft_nodes_` 存储；修改 `CreateRaftInstances()` 和 `InitRaftInstances()` |
| `multi-raft/group_registry.h` | 添加 `PostInitAll()` 方法 |
| `multi-raft/group_registry.cc` | 实现 `PostInitAll()` |
| `multi-raft/peer_fsm.h` | 可选：直接持有 `RaftState*` |

此方案**最大程度复用现有代码**，仅修复 Raft 实例创建的关键 bug，不引入新的抽象层，保持架构稳定。