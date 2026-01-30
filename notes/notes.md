这个仓库 FlexRaft-Code 主要实现了“FlexRaft”——一种结合弹性纠删码的 Raft 共识算法变体，旨在动态优化分布式系统中的网络和存储成本。下面详细解释其代码结构和各部分功能：
## 顶层结构与主要目录
- `README.md`  
  项目说明文档，介绍了 FlexRaft 的设计目标、使用场景，以及如何编译和运行代码。强调了通过灵活纠删码方案降低 Raft 的网络和存储开销。
- `exp/`  
  实验脚本目录，包含用于复现论文实验结果的 Python 脚本（如 `write_bench.py`, `read_bench.py`）和相关文档（`README.md`）。主要用于性能测试和基准评测。
## 主要代码目录和核心模块
### 1. Raft 算法核心
- `raft/raft_type.h`  
  包含 Raft 算法相关的核心结构体，例如 `ChunkInfo`，用于描述编码块的信息（如纠删码参数 k 和 m）。这些结构体是实现弹性纠删码的基础，允许节点根据故障动态调整编码方式，以降低数据传输量。
- `raft/encoder.h` & `raft/encoder.cc`  
  `Encoder` 类负责日志条目的编码和解码，底层采用 RS (Reed-Solomon) 纠删码。该类维护编码、解码矩阵，支持根据不同参数动态进行数据分块和恢复，配合故障容忍能力实现弹性数据冗余。
### 2. RCF（远程过程调用框架）
- `RCF/include/RCF/`、`RCF/demo/`  
  这部分是用于远程过程调用的库和演示，包括序列化、参数封装、线程安全等底层机制实现（如 `Marshal.hpp`, `Heap.hpp`），以及客户端/服务端的演示代码（如 `DemoClient.cpp`, `DemoInterface.hpp`）。

  - `Marshal.hpp` 定义了参数封装、序列化、反序列化相关的各种类和模板，比如 `I_Parameters`、`ClientParameters` 等，支持 RPC 的参数传递和结果处理。
  - `Heap.hpp` 实现了一个堆结构，用于存储和管理优先级队列中的元素，可能被 Raft 算法或 RPC 框架用作任务调度。
  - `DemoClient.cpp`, `DemoInterface.hpp` 演示了如何通过 RCF 框架实现客户端与服务端的通信。

- `RCF/demo/cmake/CMakeLists.txt`  
  构建脚本，定义如何编译 RPC 演示程序及其依赖。

### 3. 实验与基准测试

- `exp/write_bench.py`, `exp/read_bench.py`  
  Python 脚本，用于评估写入和读取性能，分析不同数据块大小、节点数、故障数下的性能表现。通过调用底层 C++ 实现的服务，收集并解析延时、吞吐量等指标。

- `exp/README.md`  
  说明如何构建项目、如何运行各种基准测试程序，例如 `bench_server/bench_client`, `ycsb_server/ycsb_client`, `readbench_client`。



## 结构与功能组合

- **Raft 算法核心**（`raft/`）：  
  提供了弹性纠删码的实现和 Raft 协议的基本组件，支持动态切换编码参数，实现灵活的故障容忍和高效的数据同步。

- **RCF 框架**（`RCF/`）：  
  提供底层通信机制，支持分布式节点之间的远程调用、参数传递和序列化。为 Raft 算法的分布式部署和实验评测提供通信与管理支持。

- **存储**（`rocksdb/`）:  
  使用版本为6.x，目前2024年最新的10.x有多处改动，建议仍然使用6.x版本；注意，rocksdb自带的gtest版本很老了，自带 GoogleTest 1.8.1（非常老），而这个版本在新编译器（GCC 10/11/12）中会触发：`maybe-uninitialized`，因此要关闭这个警告输出为错误，忽略这个警告，在容器内编译时，添加一个参数：`-DCMAKE_CXX_FLAGS="-Wno-error -Wno-error=maybe-uninitialized"`


- **实验与评测工具**（`exp/`）：  
  通过自动化脚本和 benchmark 程序，量化和分析 FlexRaft 的性能，支持论文实验结果复现。

这些结构组合在一起，实现了一个“可动态调整冗余度的高效分布式共识系统”，即 FlexRaft，可以在节点故障或资源紧张时自动切换纠删码参数，从而优化数据同步的网络和存储开销。

---

## 总结

- **核心算法**：弹性纠删码的 Raft 实现，动态适应集群故障与资源变化。
- **通信框架**：RCF 库，保证分布式节点高效通信。
- **实验工具**：自动化脚本，复现和评测论文实验结果。

整体架构保证了“灵活、可扩展、高效”的分布式共识，并用科学的评测方法验证其性能优势。

## docker运行
构建镜像要先构建再运行，采用多阶段构建Multi-stage Build，避免交叉编译得到的镜像过大。多阶段构建也是官方推荐做法。

