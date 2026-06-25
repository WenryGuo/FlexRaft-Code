---
name: LRC Segfault Diagnosis and Fix
overview: 分析客户端写请求导致 node0-5 崩溃的根本原因，并提供定位和修复方案
todos:
  - id: analyze-logs
    content: 分析日志确认崩溃位置和时机
    status: completed
  - id: root-cause
    content: 定位根因：memcpy 与 SetFragments 冲突
    status: completed
  - id: fix-setfragments
    content: 实施方案A：修复 SetFragments 深拷贝
    status: completed
  - id: asan-verify
    content: 使用 ASAN 验证修复
    status: pending
  - id: regression-test
    content: 回归测试
    status: pending
isProject: false
---

## 问题根因分析

### 崩溃原因
**核心问题：`Serializer::deserialize_logentry_helper` 中的 `memcpy` 与后续 `SetFragments/SetPlacement` 之间的内存管理冲突**

当 Follower 反序列化 AppendEntries 中的 LogEntry 时：

```cpp
// serializer.cc:53
std::memcpy(entry, src, sizeof(LogEntry));  // 问题1: 浅拷贝 vectors
```

`memcpy` 复制 LogEntry 结构体时，会浅拷贝 `fragments_` 和 `placement_` 向量的内部指针。这些指针指向 `deserialize_logentry_helper` 函数栈上分配的堆内存。

然后：
```cpp
entry->SetFragments(frags);  // frags 是栈上局部变量
entry->SetPlacement(placement);
```

`SetFragments` 使用默认的 vector 赋值运算符（浅拷贝），将 `frags` 的指针复制给 `entry->fragments_`。当函数返回时 `frags` 被销毁，其指向的堆内存被释放，但 `entry->fragments_` 仍持有这些已释放的指针。

当 `args->entries` 中的 LogEntry 被拷贝到 `LogManager::entries_` 时，这些悬空指针被进一步传播，导致在后续访问时触发 **SIGSEGV (Signal=11)**。

### 为什么 node6 (Leader) 不崩溃
- Leader 不接收 AppendEntries RPC，不需要反序列化
- Leader 在 `Propose()` 中直接创建 LogEntry，使用深拷贝构造函数

---

## 定位方案

### 1. 添加内存调试日志

在以下位置添加验证日志：

**文件：`raft/serializer.cc`**
- `deserialize_logentry_helper` 函数，在 `SetFragments` 调用后添加：
```cpp
fprintf(stderr, "[DEBUG] Deserialize: entry=%d, frags=%zu, placement=%zu\n",
        entry->Index(), entry->Fragments().size(), entry->Placement().size());
```

**文件：`raft/log_manager.cc`**
- `appendEntryHelper` 函数，在赋值后添加：
```cpp
fprintf(stderr, "[DEBUG-LOG-MGR] Stored entry=%d, frags=%zu, placement=%zu\n",
        entries_[back_].Index(), entries_[back_].Fragments().size(), 
        entries_[back_].Placement().size());
```

### 2. 使用 AddressSanitizer 运行时检测

编译时添加：
```bash
CXXFLAGS="-fsanitize=address -fno-omit-frame-pointer"
LDFLAGS="-fsanitize=address"
```

运行测试后，ASAN 会准确定位 double-free 或 use-after-free 的位置。

---

## 修复方案

### 方案 A：修复 SetFragments 和 SetPlacement（推荐）

**文件：`raft/log_entry.h`**

修改 `SetFragments` 方法，执行深拷贝：

```cpp
void SetFragments(const std::vector<Slice> &frags) {
  // Free existing fragments first
  for (auto& frag : fragments_) {
    if (frag.valid()) {
      delete[] frag.data();
    }
  }
  
  // Deep copy
  fragments_.clear();
  fragments_.reserve(frags.size());
  for (const auto& frag : frags) {
    if (frag.valid()) {
      char* data = new char[frag.size()];
      std::memcpy(data, frag.data(), frag.size());
      fragments_.emplace_back(data, frag.size());
    } else {
      fragments_.emplace_back();
    }
  }
}
```

同样修改 `SetPlacement`：

```cpp
void SetPlacement(const std::vector<FragmentPlacement> &p) {
  placement_ = p;  // FragmentPlacement 是 POD 类型，浅拷贝安全
}
```

**注意**：`placement_` 使用 `FragmentPlacement` 结构体（包含 int 和 enum），默认赋值即可。

### 方案 B：修复 Serializer 中的 memcpy 问题

**文件：`raft/serializer.cc`**

在反序列化时，先将 LogEntry 设为默认值，再逐个设置字段：

```cpp
const char *Serializer::deserialize_logentry_helper(const char *src, LogEntry *entry) {
  // 先清零 LogEntry，避免 memcpy 后的悬空指针
  *entry = LogEntry();
  
  // 复制简单字段（逐个而非整体 memcpy）
  // ... 然后设置 slices 和 vectors
}
```

### 方案 C：使用 move semantics（更彻底的修复）

修改 `SetFragments` 和 `SetPlacement` 使用右值引用：

```cpp
void SetFragments(std::vector<Slice> &&frags) {
  fragments_ = std::move(frags);
}
```

并在调用处使用 `std::move`。

---

## 实施步骤

1. **立即验证**：使用 AddressSanitizer 编译并运行，确认 ASAN 报告的错误位置
2. **实施修复**：采用方案 A，这是最小改动且最安全的方案
3. **回归测试**：确保 Leader 选举、心跳、正常 AppendEntries 等功能不受影响
4. **性能测试**：验证 LRC 编码/解码性能没有明显下降

---

## 关键代码路径

```
Leader Propose()
  -> Propose() [raft.cc:519]
     -> LRC Encode [raft.cc:554-575]
     -> entry.SetFragments() / entry.SetPlacement()
     -> lm_->AppendLogEntry() 
     -> ReplicateNewProposeEntry()
        -> sendAppendEntriesLrc() [raft.cc:1969]
           -> RPC send

Follower 接收
  -> RpcClient callback [batch_transport.cc]
  -> Deserialize AppendEntriesArgs [serializer.cc:220-229]
     -> deserialize_logentry_helper() [serializer.cc:52-106]
        -> memcpy(entry, src, sizeof(LogEntry))  // BUG HERE
        -> SetFragments(frags)  // 悬空指针
  -> Process(args, reply) [raft.cc:221]
  -> checkConflictEntryAndAppendNew() [raft.cc:628]
  -> lm_->AppendLogEntry() [log_manager.cc:114]
     -> appendEntryHelper() [log_manager.cc:101]
        -> entries_[back_] = entry  // 拷贝悬空指针
  -> tryApplyLogEntries()
     -> 访问 fragments_ -> CRASH (SIGSEGV)
```
