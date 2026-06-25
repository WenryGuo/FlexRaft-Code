# FlexRaft Multi-Raft 测试指南

## 目录

- [概述](##概述)
- [ycsb test](##吞吐量测试)
- [快速开始](#快速开始)
- [编译说明](#编译说明)
- [运行验证](#运行验证)
- [架构说明](#架构说明)
- [常见问题](#常见问题)

---

## 概述

FlexRaft-MultiRaft 是支持多 Raft 组的分布式存储系统。每个物理节点可以同时参与多个独立的 Raft 组，实现高效的并发写入。

### 主要特性

- **Multi-Raft 架构**：每个节点支持多个独立的 Raft 组
- **默认 4 个实例**：每个节点默认建立 4 个 Raft 实例
- **Actor 驱动**：使用 Poll 线程批量处理消息，提高吞吐量
- **路由表管理**：维护数据分布的路由信息

---
## 吞吐量测试 kpoolsize=N*N
### 服务器端启动
```bash
./ycsb_start_7node.sh
```
### 客户端启动
```bash
# 测试写
./build/bench/ycsb_client_multiraft --conf ./conf/multi-raft-7-localtest.conf --id 1 \
  --client_num 10 --size 4K --op_count 100 --type YCSB_WRITE
# 测试读
# 2. Read without warmup (data already exists)
./build/bench/ycsb_client_multiraft --conf ./conf/multi-raft-7-localtest.conf --id 99     --client_num 10 --size 4K --op_count 100     --type YCSB_READ --warmup_ops 100

./build/bench/ycsb_client_multiraft --conf ./conf/multi-raft-7-localtest.conf --id 1 \
  --client_num 20 --size 4K --op_count 100 --type YCSB_READ --skip_warmup
```
### 最后结果在ycsb_multiraft_result

## 快速开始
### 并发读写测试指令
```
./build/bench/multi_raft_client \
    --conf ./conf/multi-raft-7-localtest.conf \
    --id 99 \
    --size 4K \
    --write_num 100 \
    --concurrent_clients 4 \
    --concurrent_write_rounds 1
```
```
./build/bench/multi_raft_client \
    --conf ./conf/multi-raft-7-localtest.conf \
    --id 99 \
    --encoding=LRC \
    --size 4K \
    --write_num 100 \
    --concurrent_clients 2 \
    --concurrent_write_rounds 1
```
### 1. 编译项目

```bash
# 非第一次运行，需要清理旧的数据库文件
rm -rf /tmp/mr_single_n0.*
pkill -f bench_server_multiraft
cd /home/wenry/programs/FlexRaft-Code
rm -rf build && cmake -B build -DCMAKE_BUILD_TYPE=Debug -DENCODING=LRC -DLOG=off -DPERF=off && cmake --build build -j 4
```

### 2. 单节点测试（验证 Raft 选举和写入）

```bash
# 节点 0
./build/bench/bench_server_multiraft \
    --conf conf/multi-raft-single.conf \
    --id 0 \
    --groups_per_node 2 \
    --test \
    --data_size 256 \
    --num_writes 3

# 观察输出中的：
# - INITIALIZATION PHASE: 4 个 Raft 实例创建成功
# - TEST PHASE: 写入测试执行
# - 路由表更新
```

### 3. 多节点集群测试

```bash
# 在终端 1 启动节点 0
./build/bench/bench_server_multiraft \
    --conf conf/multi-raft-7.conf \
    --id 0 \
    --groups_per_node 4

# 在终端 2 启动节点 1
./build/bench/bench_server_multiraft \
    --conf conf/multi-raft-7.conf \
    --id 1 \
    --groups_per_node 4

# 在终端 3 启动节点 2
./build/bench/bench_server_multiraft \
    --conf conf/multi-raft-7.conf \
    --id 2 \
    --groups_per_node 4
```
# Multi-Raft 模式下通过 RaftStore 设置 k

```cpp
store->SetGroupDynamicK(group_id, 5);  // 固定 k=5
```
## 参数说明

|模式	|dynamic_k|	k 的值	|m|	Commit 阈值|
|-----------|------|------|-------|-----|
|自动模式	|0	|live_servers - F|	N - k	|F + k|
|固定 k=3	|3	|3	|N - 3|	F + 3|
|固定 k=5	|5	|5	|N - 5|	F + 5|
|固定 k=7	|7	|7	|N - 7|	F + 7|

---

## 编译说明

### 编译命令

```bash
# EC 模式（纠删码）
rm -rf build && cmake -B build -DCMAKE_BUILD_TYPE=Debug -DREPLICATION_MODE=EC -DLOG=off -DPERF=off && cmake --build build -j 4

# FULL 模式（全复制）
rm -rf build && cmake -B build -DCMAKE_BUILD_TYPE=Debug -DREPLICATION_MODE=FULL -DLOG=off -DPERF=off && cmake --build build -j 4
```

### 编译产物

| 可执行文件 | 路径 | 说明 |
|-----------|------|------|
| bench_server_multiraft | build/bench/ | Multi-Raft 服务器入口 |
| bench_server | build/bench/ | 单组 Raft 服务器 |
| ycsb_server | build/bench/ | YCSB 基准测试服务器 |

---

## 运行验证

### 命令行参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `--conf` | (必填) | 配置文件路径 |
| `--id` | (必填) | 节点 ID |
| `--groups_per_node` | 4 | 每个节点的 Raft 实例数 |
| `--test` | false | 启动后执行测试写入 |
| `--data_size` | 4096 | 测试数据大小（字节） |
| `--num_writes` | 3 | 测试写入次数 |
| `--peer_threads` | 2 | PeerFsm 线程数 |
| `--apply_threads` | 2 | ApplyFsm 线程数 |
| `--batch_size` | 64 | 批处理大小 |

### 验证步骤

#### 步骤 1：启动并观察初始化日志

```
################################################################
#                    INITIALIZATION PHASE                       #
################################################################
[STORE] Initializing RaftStore (Multi-Raft)
[STORE] Participates in 4 groups
[STORE] Created Raft instance for group=0 local_id=0
[STORE] Created Raft instance for group=1 local_id=0
[STORE] Created Raft instance for group=2 local_id=0
[STORE] Created Raft instance for group=3 local_id=0
################################################################
#                    STARTUP PHASE                             #
################################################################
```

**预期结果**：
- 显示 4 个 Raft 实例创建成功
- 每个实例对应一个独立的 Raft group

#### 步骤 2：等待 Raft 选举

```bash
# 等待约 3 秒让 Raft 完成选举
```

**预期日志**：
```
[PEER-G0-N0] PeerFsm created
[PEER-G1-N0] PeerFsm created
[PEER-G2-N0] PeerFsm created
[PEER-G3-N0] PeerFsm created
```

#### 步骤 3：执行测试写入

```bash
# 使用 --test 参数或发送 SIGUSR1 信号触发测试写入
kill -USR1 <pid>
```

**预期日志**：
```
########################################
#          TEST WRITE 1 START         #
########################################
[TEST] Generating test data of 1024 bytes...
[STORE] ===== LOCAL PROPOSE TO GROUP 0 =====
[STORE] entry_id=1000 data_size=1024
[PEER-G0-N0] ===== RECEIVED PROPOSE SHARD =====
[PEER-G0-N0] IsLeader: YES
[PEER-G0-N0] Propose result: is_leader=1 propose_index=1 propose_term=1
```

#### 步骤 4：检查路由表

```bash
# 发送 SIGUSR2 信号打印路由表
kill -USR2 <pid>
```

**预期输出**：
```
========================================
         ROUTING TABLE (4 entries)
========================================
stripe_id=1000 -> entry_id=1000
  groups: [0]
  has_global_parities=0
...
========================================
```

### 正常运行判定标准

1. **所有 Raft 实例创建成功**
   - 观察 `[STORE] Created Raft instance for group=X`
   - 应显示 4 个实例（groups_per_node=4）

2. **Raft 选举成功**
   - 观察 `IsLeader: YES` 或 `IsLeader: NO`
   - 说明 Raft 协议正常运作

3. **写入请求成功**
   - `Propose result: is_leader=1` 表示该节点是 leader，写入成功
   - `Propose result: is_leader=0` 表示该节点是 follower，写入会转发给 leader

4. **路由表更新**
   - 每次写入后路由表应有新条目
   - 条目格式：`stripe_id -> groups: [group_id]`

---
客户端测试：
./bench_client --conf=multi-raft-1-lrc.conf --id=0 --size=4KB --write_num=100 --route=true
## 架构说明

### Multi-Raft Actor 架构

```
+------------------+     +------------------+     +------------------+
|     PeerFsm     |     |     PeerFsm     |     |     PeerFsm     |
|  (Group 0)      |     |  (Group 1)      |     |  (Group 2)      |
+--------+---------+     +--------+---------+     +--------+---------+
         |                       |                       |
         v                       v                       v
+------------------+     +------------------+     +------------------+
|    ApplyFsm      |     |    ApplyFsm      |     |    ApplyFsm      |
|  (Group 0)      |     |  (Group 1)      |     |  (Group 2)      |
+--------+---------+     +--------+---------+     +--------+---------+
         |                       |                       |
         +-----------------------+-----------------------+
                                 |
                                 v
                    +-------------------------+
                    |       RaftStore         |
                    |  - BatchSystem          |
                    |  - GroupRegistry        |
                    |  - RoutingTableManager  |
                    |  - CrossGroupTracker    |
                    +-------------------------+
```

### 组件说明

| 组件 | 说明 |
|------|------|
| **PeerFsm** | 处理 Raft 协议消息和提案 |
| **ApplyFsm** | 将已提交的日志应用到 KV 状态机 |
| **BatchSystem** | Poll 线程批量处理消息 |
| **GroupRegistry** | 管理本节点的所有 KvServiceNode |
| **RoutingTableManager** | 维护数据分布路由表 |
| **CrossGroupTracker** | 跨组提交追踪（LRC 模式） |

---

## 常见问题

### Q1: 编译报错 "REPLICATION_MODE must be 'FULL' or 'EC'"

**原因**：CMake 缓存中有无效的 REPLICATION_MODE 值

**解决**：
```bash
rm -rf build
cmake -B build -DREPLICATION_MODE=EC -DLOG=off -DPERF=off
cmake --build build -j 4
```

### Q2: 启动报错 "no matching function for call to RaftStore constructor"

**原因**：配置文件中指定了 LRC 参数，但代码已移除 LRC 支持

**解决**：移除配置文件中的 `lrc k=X l=Y r=Z` 行，使用默认配置

### Q3: 所有写入都显示 "IsLeader: NO"

**原因**：该节点不是 Raft leader

**解决**：这是正常行为。Follower 节点会拒绝写入，数据会转发给 leader。等待 leader 选举完成后再测试。

### Q4: 路由表为空

**原因**：
1. 尚未执行任何写入
2. Raft 尚未完成选举
3. 所有 Raft 实例都在运行但未收到提案

**解决**：
```bash
# 触发测试写入
kill -USR1 <pid>

# 等待几秒后检查路由表
kill -USR2 <pid>
```

### Q5: 多个节点需要协调启动顺序吗？

**答**：不需要。FlexRaft 支持动态成员加入，新节点可以随时启动加入集群。

---

## 配置文件格式

### multi-raft-7.conf 示例

```
# FlexRaft Multi-Raft 示例配置文件
# 第一行：N = 7 表示 7 个物理节点
# 每个物理节点参与 4 个 Raft 组（由 --groups_per_node 参数指定）
7
0  127.0.0.1:50010  127.0.0.1:60010  /tmp/mr_n0.log  /tmp/mr_n0.db
1  127.0.0.1:50011  127.0.0.1:60011  /tmp/mr_n1.log  /tmp/mr_n1.db
2  127.0.0.1:50012  127.0.0.1:60012  /tmp/mr_n2.log  /tmp/mr_n2.db
3  127.0.0.1:50013  127.0.0.1:60013  /tmp/mr_n3.log  /tmp/mr_n3.db
4  127.0.0.1:50014  127.0.0.1:60014  /tmp/mr_n4.log  /tmp/mr_n4.db
5  127.0.0.1:50015  127.0.0.1:60015  /tmp/mr_n5.log  /tmp/mr_n5.db
6  127.0.0.1:50016  127.0.0.1:60016  /tmp/mr_n6.log  /tmp/mr_n6.db
```

### 字段说明

| 字段 | 说明 |
|------|------|
| 第一行 | 物理节点总数 N |
| node_id | 物理节点 ID |
| raft_addr | Raft RPC 地址 (IP:Port) |
| kv_addr | KV RPC 地址 (IP:Port) |
| raft_log | Raft 日志文件路径 |
| kv_db | KV 数据库路径 |

---

## 性能调优

### 线程数配置

```bash
# 高并发场景
./build/bench/bench_server_multiraft \
    --conf conf/multi-raft-7.conf \
    --id 0 \
    --groups_per_node 8 \
    --peer_threads 4 \
    --apply_threads 4 \
    --batch_size 128
```

### 测试数据大小

```bash
# 小数据（低延迟测试）
--data_size 256 --num_writes 100

# 大数据（高吞吐测试）
--data_size 65536 --num_writes 10
```
