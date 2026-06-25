# FlexRaft Multi-Raft 架构设计文档

## 1. 系统架构概览

### 1.1 当前架构问题

当前FlexRaft的Multi-Raft实现存在以下问题：

1. **RPC服务器冗余**：每个KvServiceNode都创建独立的RPC服务器，导致端口冲突
2. **缺乏统一入口**：没有统一的消息路由入口，Raft实例直接进行网络通信
3. **列表管理混乱**：没有明确的peer_list和通信列表分离
4. **消息路由低效**：缺乏基于group_id的高效消息分发机制

### 1.2 新架构目标

1. **单一RPC服务器**：每个物理节点只运行一个RPC服务器，作为所有网络通信的统一入口
2. **多Raft实例共享**：多个Raft实例共享同一个RPC服务器，通过group_id区分
3. **列表解耦管理**：实现集群peer_list与通信列表的分离与解耦
4. **高效消息路由**：实现基于group_id的消息分发机制

## 2. 物理节点架构设计

### 2.1 核心组件

```
+------------------------------------------------------------------+
|                      Physical Node (P0)                          |
|                                                                   |
|  +------------------------------------------------------------+  |
|  |              Unified RPC Server (Port 50010)               |  |
|  |  - RequestVote RPC                                         |  |
|  |  - AppendEntries RPC                                       |  |
|  |  - RequestFragments RPC                                    |  |
|  +------------------------+-----------------------------------+  |
|                           |                                      |
|                           v                                      |
|  +------------------------------------------------------------+  |
|  |              Message Router (基于 group_id)                |  |
|  |  - 解析 group_id                                           |  |
|  |  - 分发到对应 mailbox                                       |  |
|  +------------------------+-----------------------------------+  |
|                           |                                      |
|         +-----------------+------------------+                   |
|         |                 |                  |                   |
|         v                 v                  v                   |
|  +------------+    +------------+     +------------+             |
|  |  Mailbox   |    |  Mailbox   |     |  Mailbox   |             |
|  |  (G0)      |    |  (G1)      |     |  (G2)      |             |
|  +-----+------+    +-----+------+     +-----+------+             |
|        |                 |                  |                     |
|        v                 v                  v                     |
|  +------------+    +------------+     +------------+             |
|  |  PeerFsm   |    |  PeerFsm   |     |  PeerFsm   |             |
|  |  (G0)      |    |  (G1)      |     |  (G2)      |             |
|  +------------+    +------------+     +------------+             |
|                                                                   |
+------------------------------------------------------------------+
```

### 2.2 组件说明

| 组件 | 职责 | 说明 |
|------|------|------|
| **Unified RPC Server** | 网络通信入口 | 所有RPC请求的统一入口，监听物理节点的唯一端口 |
| **Message Router** | 消息路由 | 解析group_id，将消息分发到对应的mailbox |
| **Mailbox** | 消息队列 | 每个Raft实例的独立消息队列，保证消息顺序 |
| **PeerFsm** | Raft状态机 | 处理Raft协议消息，维护Raft状态 |

## 3. 列表管理与解耦设计

### 3.1 集群peer_list列表

**数据结构**：
```cpp
struct GroupMembership {
  GroupId group_id;
  std::vector<raft::raft_node_id_t> members;  // 该group的所有成员
  std::map<raft::raft_node_id_t, PhysicalNodeInfo> node_info;  // 成员详细信息
};

struct PhysicalNodeInfo {
  int physical_node_id;
  std::string raft_rpc_addr;  // Raft RPC地址
  std::string kv_rpc_addr;    // KV RPC地址
};

// 按group_id分组管理
std::map<GroupId, GroupMembership> peer_list_;
```

**职责**：
- 记录每个Raft group的成员信息
- 提供group成员查询接口
- 支持动态成员变更

### 3.2 通信列表（P2P网络）

**数据结构**：
```cpp
struct P2PNetwork {
  // 物理节点ID -> RPC客户端
  std::map<int, std::unique_ptr<RpcClient>> rpc_clients_;
  
  // 本节点的物理节点ID
  int my_physical_node_id_;
  
  // 本节点的RPC地址
  std::string my_raft_rpc_addr_;
};

// 覆盖整个集群的P2P通信网络
P2PNetwork p2p_network_;
```

**职责**：
- 维护与其他物理节点的RPC连接
- 提供节点间直接通信能力
- 支持跨group通信

### 3.3 自动同步机制

```cpp
class ListSyncManager {
 public:
  // 定期同步peer_list
  void SyncPeerList();
  
  // 处理peer_list更新
  void OnPeerListUpdate(const PeerListUpdateMsg& msg);
  
  // 处理节点加入/离开
  void OnNodeJoin(int physical_node_id, const std::string& addr);
  void OnNodeLeave(int physical_node_id);
  
 private:
  std::map<GroupId, GroupMembership> peer_list_;
  P2PNetwork p2p_network_;
  std::mutex sync_mutex_;
};
```

## 4. 消息路由机制

### 4.1 消息格式

**统一消息头**：
```cpp
struct UnifiedMessageHeader {
  uint32_t magic;        // 固定值 0x464C4558 ('FLEX')
  uint16_t group_id;     // Group ID
  uint16_t msg_type;     // 消息类型
  uint32_t payload_size; // 负载大小
};

enum class MessageType : uint16_t {
  kRequestVote = 0,
  kAppendEntries = 1,
  kRequestFragments = 2,
  kPeerListSync = 3,
  kCrossGroupMsg = 4,
};
```

### 4.2 消息路由流程

```
1. RPC请求到达 Unified RPC Server
   |
   v
2. 解析消息头，提取 group_id 和 msg_type
   |
   v
3. Message Router 查找对应的 mailbox
   |
   +-> 如果找到：push消息到mailbox
   |
   +-> 如果未找到：返回错误（group不存在）
   |
   v
4. PeerFsm 从 mailbox 中取出消息处理
   |
   v
5. 处理结果通过 RPC 返回
```

### 4.3 消息路由实现

```cpp
class UnifiedMessageRouter {
 public:
  // 注册 group 的 mailbox
  void RegisterGroupMailbox(GroupId group_id, Mailbox<PeerMsg>* mailbox);
  
  // 路由消息到对应的 mailbox
  bool RouteMessage(const UnifiedMessageHeader& header, 
                    const std::vector<uint8_t>& payload);
  
  // 处理 RPC 请求
  RCF::ByteBuffer HandleRequestVote(const RCF::ByteBuffer& buf);
  RCF::ByteBuffer HandleAppendEntries(const RCF::ByteBuffer& buf);
  RCF::ByteBuffer HandleRequestFragments(const RCF::ByteBuffer& buf);
  
 private:
  std::map<GroupId, Mailbox<PeerMsg>*> group_mailboxes_;
  std::mutex router_mutex_;
};
```

## 5. 通信流程设计

### 5.1 节点间通信流程

```
Physical Node 0                    Physical Node 1
+----------------+                +----------------+
| Raft Instance  |                | Raft Instance  |
| (G0, N0)       |                | (G0, N1)       |
+-------+--------+                +-------+--------+
        |                                 ^
        | RequestVote(G0)                 |
        v                                 |
+-------+--------+                +-------+--------+
| Unified RPC    |  RPC Request   | Unified RPC    |
| Server (P0)    | -------------> | Server (P1)    |
| Port: 50010    |                | Port: 50011    |
+----------------+                +----------------+
        |                                 |
        |                                 v
        |                         +-------+--------+
        |                         | Message Router |
        |                         | (解析 G0)      |
        |                         +-------+--------+
        |                                 |
        |                                 v
        |                         +-------+--------+
        |                         | Mailbox (G0)   |
        |                         +-------+--------+
        |                                 |
        |                                 v
        |                         +-------+--------+
        |                         | PeerFsm (G0,N1)|
        |                         +----------------+
        |                                 |
        |<--------------------------------+
        |         RPC Response            |
        v                                 |
+-------+--------+                        |
| Raft Instance  |                        |
| (G0, N0)       |                        |
+----------------+                        |
```

### 5.2 跨group通信流程

```
Physical Node 0                    Physical Node 1
+----------------+                +----------------+
| Raft Instance  |                | Raft Instance  |
| (G0, N0)       |                | (G1, N1)       |
+-------+--------+                +-------+--------+
        |                                 ^
        | CrossGroupMsg                   |
        | (from G0 to G1)                 |
        v                                 |
+-------+--------+                +-------+--------+
| Unified RPC    |  RPC Request   | Unified RPC    |
| Server (P0)    | -------------> | Server (P1)    |
+-------+--------+                +-------+--------+
        |                                 |
        |                                 v
        |                         +-------+--------+
        |                         | Message Router |
        |                         | (解析 G1)      |
        |                         +-------+--------+
        |                                 |
        |                                 v
        |                         +-------+--------+
        |                         | Mailbox (G1)   |
        |                         +-------+--------+
        |                                 |
        |                                 v
        |                         +-------+--------+
        |                         | PeerFsm (G1,N1)|
        |                         +----------------+
```

## 6. 关键模块设计

### 6.1 UnifiedRPCServer

**职责**：
- 监听物理节点的唯一端口
- 接收所有RPC请求
- 调用MessageRouter进行消息分发

**接口**：
```cpp
class UnifiedRPCServer {
 public:
  UnifiedRPCServer(int physical_node_id, const std::string& raft_addr);
  ~UnifiedRPCServer();
  
  void Start();
  void Stop();
  
  void SetMessageRouter(UnifiedMessageRouter* router);
  
 private:
  int physical_node_id_;
  std::string raft_addr_;
  RCF::RcfServer rpc_server_;
  UnifiedMessageRouter* router_;
};
```

### 6.2 UnifiedMessageRouter

**职责**：
- 解析RPC请求中的group_id
- 路由消息到对应的mailbox
- 管理group与mailbox的映射关系

**接口**：
```cpp
class UnifiedMessageRouter {
 public:
  void RegisterGroupMailbox(GroupId group_id, Mailbox<PeerMsg>* mailbox);
  void UnregisterGroupMailbox(GroupId group_id);
  
  RCF::ByteBuffer HandleRequestVote(const RCF::ByteBuffer& buf);
  RCF::ByteBuffer HandleAppendEntries(const RCF::ByteBuffer& buf);
  RCF::ByteBuffer HandleRequestFragments(const RCF::ByteBuffer& buf);
  
 private:
  std::map<GroupId, Mailbox<PeerMsg>*> group_mailboxes_;
  std::mutex mutex_;
};
```

### 6.3 PeerListManager

**职责**：
- 管理集群peer_list
- 提供group成员查询
- 支持动态成员变更

**接口**：
```cpp
class PeerListManager {
 public:
  void AddGroup(GroupId group_id, const GroupMembership& membership);
  void RemoveGroup(GroupId group_id);
  void UpdateGroupMembership(GroupId group_id, const GroupMembership& membership);
  
  const GroupMembership* GetGroupMembership(GroupId group_id) const;
  std::vector<GroupId> GetAllGroups() const;
  
 private:
  std::map<GroupId, GroupMembership> peer_list_;
  std::shared_mutex rw_lock_;
};
```

### 6.4 P2PNetworkManager

**职责**：
- 维护与其他物理节点的RPC连接
- 提供节点间直接通信能力
- 管理RPC客户端连接池

**接口**：
```cpp
class P2PNetworkManager {
 public:
  void AddPhysicalNode(int physical_node_id, const std::string& raft_addr);
  void RemovePhysicalNode(int physical_node_id);
  
  bool SendMessage(int target_physical_node_id, 
                   const UnifiedMessageHeader& header,
                   const std::vector<uint8_t>& payload);
  
 private:
  std::map<int, std::unique_ptr<RpcClient>> rpc_clients_;
  std::mutex mutex_;
};
```

## 7. 通信协议规范

### 7.1 消息格式

**统一消息格式**：
```
+------------------+------------------+------------------+
|  Header (8字节)  |  Payload (变长)  |  Checksum (4字节)|
+------------------+------------------+------------------+

Header:
  - magic (4字节): 0x464C4558 ('FLEX')
  - group_id (2字节): Group ID
  - msg_type (2字节): 消息类型

Payload:
  - 根据msg_type不同，包含不同的数据

Checksum:
  - CRC32校验和
```

### 7.2 RPC接口定义

**RCF接口定义**：
```cpp
RCF_BEGIN(I_UnifiedRaftRPCService, "I_UnifiedRaftRPCService")
  RCF_METHOD_R1(RCF::ByteBuffer, RequestVote, const RCF::ByteBuffer&)
  RCF_METHOD_R1(RCF::ByteBuffer, AppendEntries, const RCF::ByteBuffer&)
  RCF_METHOD_R1(RCF::ByteBuffer, RequestFragments, const RCF::ByteBuffer&)
  RCF_METHOD_R1(RCF::ByteBuffer, CrossGroupMessage, const RCF::ByteBuffer&)
RCF_END(I_UnifiedRaftRPCService)
```

### 7.3 消息类型说明

| 消息类型 | 值 | 说明 | Payload内容 |
|---------|---|------|------------|
| kRequestVote | 0 | Raft投票请求 | RequestVoteArgs序列化数据 |
| kAppendEntries | 1 | Raft日志追加请求 | AppendEntriesArgs序列化数据 |
| kRequestFragments | 2 | 请求分片数据 | RequestFragmentsArgs序列化数据 |
| kPeerListSync | 3 | peer_list同步 | PeerListUpdateMsg序列化数据 |
| kCrossGroupMsg | 4 | 跨group消息 | CrossGroupMsg序列化数据 |

## 8. 实现计划

### 8.1 阶段一：基础架构重构

1. 实现UnifiedRPCServer
2. 实现UnifiedMessageRouter
3. 修改RaftStore，移除多个RPC服务器
4. 修改KvServiceNode，不再创建独立RPC服务器

### 8.2 阶段二：列表管理实现

1. 实现PeerListManager
2. 实现P2PNetworkManager
3. 实现ListSyncManager
4. 集成到RaftStore

### 8.3 阶段三：消息路由优化

1. 优化消息解析性能
2. 实现消息批处理
3. 添加消息监控和统计

### 8.4 阶段四：测试和验证

1. 单元测试
2. 集成测试
3. 性能测试
4. 压力测试

## 9. 兼容性考虑

### 9.1 向后兼容

- 保持原有的Raft协议不变
- 保持原有的消息格式不变
- 只修改RPC通信层

### 9.2 配置文件兼容

- 保持原有配置文件格式
- 添加新的配置项支持

## 10. 性能优化

### 10.1 消息批处理

- 批量处理RPC请求
- 减少网络往返次数

### 10.2 连接池管理

- RPC客户端连接池
- 连接复用

### 10.3 消息压缩

- 大消息压缩传输
- 减少网络带宽占用
