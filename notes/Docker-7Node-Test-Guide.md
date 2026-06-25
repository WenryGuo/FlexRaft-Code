# FlexRaft x86 Docker 7 节点集群测试指南

本文档说明如何在本地 x86 Linux 机器上，通过 Docker Compose 启动 7 节点 FlexRaft 集群并进行测试。

---

## 一、环境准备

### 1.1 检查环境

```bash
docker --version         # 推荐 >= 20.10
docker compose version   # 或 docker-compose version，推荐 >= 2.0
```

### 1.2 安装 Docker（若未安装）

```bash
# Ubuntu/Debian
curl -fsSL https://get.docker.com | sh
sudo usermod -aG docker $USER
newgrp docker           # 使当前会话生效，无需重新登录
```

### 1.3 确认项目目录

```bash
cd /home/wenry/programs/FlexRaft-Code
```

---

## 二、构建 Docker 镜像

### 2.1 构建 x86 镜像

> 镜像内部会自动编译整个 FlexRaft 项目（EC 模式，Release 版本）。
> 预计耗时：**15-30 分钟**（取决于机器配置）。

```bash
sudo docker build -t multi-rsraft:x86 -f "Dockerfile QUME_x86" .
```

**构建参数说明：**

| 参数 | 值 | 说明 |
|------|-----|------|
| `-t` | `multi-rsraft:x86` | 镜像名称和标签 |
| `-f` | `Dockerfile QUME_x86` | 使用 x86 本地编译的 Dockerfile |
| `.` | （当前目录） | 构建上下文，COPY 整个项目源码进容器 |

**关键依赖：** Ubuntu 22.04 + googletest + rocksdb + isa-l + gflags + glog + protobuf + uuid-dev

### 2.2 确认镜像构建成功

```bash
docker images | grep multi-rsraft
```

---

## 三、配置文件说明

### 3.1 集群配置

使用已有的 `conf/multi-raft-7-lrc.conf`：

```
7
0  node-0:51010  node-0:60010  /tmp/raft_log0  /tmp/mr_n0
1  node-1:51011  node-1:60011  /tmp/raft_log1  /tmp/mr_n1
2  node-2:51012  node-2:60012  /tmp/raft_log2  /tmp/mr_n2
3  node-3:51013  node-3:60013  /tmp/raft_log3  /tmp/mr_n3
4  node-4:51014  node-4:60014  /tmp/raft_log4  /tmp/mr_n4
5  node-5:51015  node-5:60015  /tmp/raft_log5  /tmp/mr_n5
6  node-6:51016  node-6:60016  /tmp/raft_log6  /tmp/mr_n6
```

> **地址说明：** 节点地址使用容器 hostname（node-0 ~ node-6），Docker 会在 `flexraft_net` 内部网络提供 DNS 解析，无需知道 IP。

**LRC 参数（N=7）：** k=4, l=2, r=1（自动由 `LrcParams::AutoFromClusterSize()` 计算）

### 3.2 端口映射

| 容器 | Raft RPC | KV RPC |
|------|----------|--------|
| node-0 | 51010 | 60010 |
| node-1 | 51011 | 60011 |
| node-2 | 51012 | 60012 |
| node-3 | 51013 | 60013 |
| node-4 | 51014 | 60014 |
| node-5 | 51015 | 60015 |
| node-6 | 51016 | 60016 |

---

## 四、启动集群

### 4.1 创建数据目录

```bash
mkdir -p conf data
```

> `conf/` 目录已挂载为只读，`data/` 目录持久化 RocksDB 数据。

### 4.2 启动所有 7 个节点

```bash
docker compose up -d
```

### 4.3 检查容器状态

```bash
docker compose ps
```

预期输出：7 个容器均为 `Up` 状态。

### 4.4 查看节点日志

```bash
# 查看 node-0 的启动日志
docker compose logs -f node-0

# 同时查看多个节点的日志
docker compose logs -f node-0 node-1 node-2
```

**成功启动的典型日志特征：**

```
[CONFIG] Building LRC groups for cluster_size=7
[CONFIG]   LRC(k=4, l=2, r=1) auto-calculated
[CONFIG]   Group 0: [0,1,2]  Group 1: [3,4,5,6]
[RaftStore] Created RaftStore with 7 groups
[RaftStore] Local groups: ...
[RaftNode] Node X starting election timer
```

**如果容器反复重启（Restarting）：**

```bash
# 查看具体错误
docker compose logs node-0

# 常见问题：
# - 配置文件路径不对  → 确认 /app/conf/multi-raft-7-lrc.conf 存在
# - 端口被占用       → docker compose ps 查看哪些端口冲突
# - 编译失败         → 重新构建镜像 docker compose build
```

### 4.5 等待选举完成

Raft 选举通常需要几秒钟。确认有节点成为 Leader：

```bash
docker compose logs node-0 node-1 node-2 node-3 node-4 node-5 node-6 2>&1 | grep -i "leader\|candidate"
```

---

## 五、运行测试

### 5.1 进入客户端容器

```bash
# 启动一个交互式客户端容器（不启动服务器进程）
sudo docker compose run --rm --no-deps client bash
```

> `--rm` 退出时自动删除容器，`--no-deps` 不启动 server 依赖的服务。

### 5.2 客户端写入测试

```bash
# 基础写入测试：100 次写入，每次 100 字节
/flexraft/build/bench/multi_raft_client \
    --conf /app/conf/multi-raft-7-lrc.conf \
    --id 99 \
    --size 4K \
    --write_num 100
```

**参数说明：**

| 参数 | 说明 |
|------|------|
| `--conf` | 集群配置文件路径 |
| `--id 99` | 客户端 ID（任意非节点 ID） |
| `--size 100` | 每条 value 的字节数 |
| `--write_num 100` | 写入次数 |

**预期输出：**

```
[Client] Total writes: 100, avg_latency: X.XX ms
[Client] Total time: Y.YY s, ops: Z ops/s
```

### 5.3 更大规模写入测试

```bash
# 1000 次写入，每次 4KB
/flexraft/build/bench/multi_raft_client \
    --conf /app/conf/multi-raft-7-lrc.conf \
    --id 99 \
    --size 4096 \
    --write_num 1000
```

### 5.4 带详细日志的测试

```bash
# 每次操作打印详情
/flexraft/build/bench/multi_raft_client \
    --conf /app/conf/multi-raft-7-lrc.conf \
    --id 99 \
    --size 512 \
    --write_num 50 \
    --verbose
```

### 5.5 在宿主机上用原生客户端测试

如果不进入容器，也可以从宿主机直接连接节点：

```bash
# 先在宿主机编译原生客户端（如果尚未编译）
cd /home/wenry/programs/FlexRaft-Code
cmake --build build --target multi_raft_client -j$(nproc)

# 然后运行客户端（端口通过 docker compose 端口映射暴露）
# 注意：需要修改配置文件中的地址为 127.0.0.1
./build/bench/multi_raft_client \
    --conf ../conf/multi-raft-7-lrc.conf \
    --id 99 \
    --size 1024 \
    --write_num 100
```

---

## 六、LRC 功能验证

### 6.1 LRC 参数单元测试

```bash
# 在客户端容器内
/flexraft/build/raft/test_lrc_params
/flexraft/build/raft/test_lrc_group
```

### 6.2 模拟节点故障与恢复

**步骤 1：杀掉一个节点（模拟故障）**

```bash
docker compose stop node-6
```

**步骤 2：观察集群是否继续正常服务**

在客户端容器中继续写入：

```bash
/flexraft/build/bench/multi_raft_client \
    --conf /app/conf/multi-raft-7-lrc.conf \
    --id 99 \
    --write_num 50 \
    --size 1024
```

**步骤 3：恢复节点**

```bash
docker compose start node-6
```

**步骤 4：检查修复日志**

```bash
docker compose logs node-0 | grep -i "recover\|repair\|rebuild"
```

### 6.3 多节点故障测试

```bash
# 同时杀掉两个节点（本地组内可容忍 1 个节点故障）
docker compose stop node-5 node-6

# 测试集群是否仍可写入（预期：写入可能变慢，但不应失败）
/flexraft/build/bench/multi_raft_client \
    --conf /app/conf/multi-raft-7-lrc.conf \
    --id 99 \
    --write_num 100 \
    --size 1024

# 恢复
docker compose start node-5 node-6
```

---

## 七、集群规模扩展测试

### 7.1 9 节点集群

1. 复制并修改配置文件：

```bash
cp conf/multi-raft-7-lrc.conf conf/multi-raft-9-lrc.conf
# 编辑 conf/multi-raft-9-lrc.conf：第一行改为 9，并添加 node-7、node-8
```

2. 修改配置文件中 8 个节点的地址（node-0 ~ node-7），端口依次递增

3. 修改 `docker-compose.yml` 中的 `conf` 挂载路径，添加 node-7、node-8 服务定义

4. 重新构建并测试

### 7.2 11 / 15 节点

同上，参考 `conf/multi-raft-7-lrc.conf` 格式扩展，Docker Compose 服务定义中逐个添加节点容器。

---

## 八、清理环境

### 8.1 停止并清理所有容器

```bash
docker compose down
```

### 8.2 删除数据（重新测试时使用）

```bash
rm -rf data/*
```

### 8.3 完全重建镜像

```bash
docker compose build --no-cache
docker compose up -d
```

### 8.4 清理未使用的 Docker 资源

```bash
docker system prune -f
```

---

## 九、常见问题排查

### Q1：容器启动后立即退出

```bash
docker compose logs <container-name>
```
检查是否因为配置路径错误、端口冲突或编译失败。

### Q2：节点之间 RPC 通信失败

```bash
# 检查 Docker 网络是否正常
docker network inspect flexraft_net

# 测试容器间连通性
docker compose exec node-0 ping -c 1 node-1
```

### Q3：客户端写入很慢

可能原因：网络带宽限制、Raft 心跳间隔过短、或 LRC 编解码瓶颈。用 `docker stats` 检查资源使用。

### Q4：如何查看 LRC group 构建结果？

在服务器日志中搜索 `[CONFIG]`，会打印每个 group 的节点分配。

### Q5：如何开启详细日志？

在 `bench_server_multiraft.cc` 中修改 `FLAGS_v` 日志级别，或修改 CMakeLists.txt 开启 `-DLOG=ON`。

---

## 十、测试完成后

确认以下清单全部通过后，再进行 RK3588 ARM64 交叉编译测试：

- [ ] 7 节点集群全部启动成功
- [ ] Raft 选举正常完成，Leader 被选出
- [ ] 客户端写入 100 次成功，延迟在合理范围
- [ ] 客户端写入 1000 次成功，无数据丢失
- [ ] 杀掉 1 个节点后，写入仍正常
- [ ] 恢复节点后，集群自动同步
- [ ] LRC 参数单元测试通过
- [ ] 9 / 11 / 15 节点集群依次测试通过
