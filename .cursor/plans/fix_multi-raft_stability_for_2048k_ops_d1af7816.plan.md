---
name: Fix Multi-Raft stability for 2048K ops
overview: "Analyze the FlexRaft multi-raft benchmark architecture, identify root causes of timeouts at 4K/100ops, and provide a concrete plan to make it stable at 2048K/500ops. Root cause is a cascade of bottlenecks: insufficient apply threads, no backpressure, oversized poll batches, and a hidden memory allocation in StripePut."
todos:
  - id: fix-apply-threads
    content: Increase apply_threads to 28-56 in bench_server_multiraft.cc
    status: pending
  - id: fix-apply-poll
    content: Add work-stealing to ApplyPollLoop or process all groups per iteration
    status: pending
  - id: fix-stripe-hooks
    content: Move RegisterStripeRaftHooks out of StripePut hot path
    status: pending
  - id: fix-slice-copy
    content: Replace new/memcpy with raft::Slice::Copy in StripePut
    status: pending
  - id: fix-mailbox-backpressure
    content: Set bounded capacity on ApplyFsm mailboxes for backpressure
    status: pending
  - id: fix-batch-size
    content: Reduce max_batch from 64 to 16 in bench_server_multiraft.cc
    status: pending
  - id: fix-backoff
    content: Add exponential backoff to StripePut commit polling loop
    status: pending
  - id: fix-monitoring
    content: Add mailbox monitoring to BatchSystem
    status: pending
  - id: test-progression
    content: "Rebuild and run progressive tests: 4K/100 -> 4K/500 -> 512K/100 -> 2048K/500"
    status: pending
isProject: false
---

# FlexRaft Multi-Raft Stability Fix Plan: 4K/100 -> 2048K/500

## Architecture Summary

The system is a **7-node local test cluster**, each physical node running **7 Raft groups** (uniform groups, LRC encoding). Every node has:

- 14 PeerPoll threads + 14 ApplyPoll threads
- 32-thread RCF pool for KV RPC (port 600xx)
- 32-thread RCF pool for unified Raft RPC (port 510xx)
- 7 PeerFsm + 7 ApplyFsm instances
- 7 RaftNode instances (one ticker thread each)

## Root Cause Analysis

### The Failure Cascade

```
Client RPC (10s timeout)
  └─> GroupRegistry::DealWithRequest()
        └─> RaftStore::StripePut() [BLOCKING]
              ├─> RegisterStripeRaftHooks() [called EVERY write]
              ├─> raft_node->Propose()
              └─> Poll CommitIndex for up to 1s (20 x 50ms retries)
                   └─> TIMEOUT if entries backlog behind CommitIndex
                        └─> kRequestExecTimeout → client sees "RCP failed"
```

### Root Cause 1: Insufficient ApplyFsm Threads (Critical)

`ApplyPollLoop` has **14 threads for 7 groups** (partition = 1 group per 2 threads on average). With entries piling up in the commit queue, **ApplyFsm drain rate is too slow**:

- Node0 log shows entry index 2 (WRITE-IN for key=key7924...) **TIMED OUT after 1000ms** — it waited for commit that never arrived
- This caused commit queue backlog to cascade: entry 3, 4 all piled up behind it
- With op_count=100 and ~14 writes per group, the commit queue depth grows faster than ApplyFsm can drain

Each ApplyFsm processes one entry at a time from its mailbox (sequential per-group). With `max_batch_=64`, each drain pops up to 64 entries, but if the mailbox is nearly empty, the 10ms CV wait adds latency.

### Root Cause 2: No Backpressure Between Raft and ApplyFsm

- `Mailbox<ApplyMsg>` has **unbounded capacity** (max_capacity_=0, `push()` never blocks)
- Entries pile up in ApplyFsm mailboxes faster than ApplyFsm drains them
- The server has no mechanism to slow down incoming writes

### Root Cause 3: RegisterStripeRaftHooks Called Every Write

`StripePut()` calls `RegisterStripeRaftHooks()` on **every single write** (line 15 of `raft_store_stripe.cc`). This should only be called once during initialization.

### Root Cause 4: Memory Allocation in Hot Path

`StripePut()` allocates memory with `new char[ser.size()]` and relies on `delete[]` in the `CommandData` destructor. For 2048K entries, this is a large allocation per write.

### Root Cause 5: BatchTransport max_buffer_size=256 Limits Throughput

The network-layer backpressure of 256 messages per node is reasonable, but when the apply path backs up, the BatchTransport queues also grow.

---

## Implementation Plan

### Step 1: Fix ApplyFsm Thread Scaling

**File**: [`multi-raft/raft_store_batch.cc`](multi-raft/raft_store_batch.cc)

The partition-based ApplyPollLoop currently distributes groups across threads **statically**. Change to **work-stealing** so all threads can help drain all groups:

```cpp
// Change ApplyPollLoop to work-stealing: each idle thread steals from neighboring partitions
void BatchSystem::ApplyPollLoop(int thread_id) {
    // Instead of processing only its partition, try all partitions in a round-robin
    // Use a shared atomic index for work stealing
}
```

**Alternatively (simpler)**: Increase apply_threads to 28-56 in [`bench/bench_server_multiraft.cc`](bench/bench_server_multiraft.cc) via command-line flag, and modify the partition so all threads can process all groups. Change the `did_work` logic to try all groups on every loop iteration:

```cpp
// Process ALL groups in each loop iteration, not just own partition
for (size_t idx = 0; idx < apply_entries_.size(); ++idx) {  // ALL groups
    auto& entry = apply_entries_[idx];
    // ...
}
```

### Step 2: Add Backpressure to ApplyFsm Mailboxes

**File**: [`multi-raft/raft_store.h`](multi-raft/raft_store.h)

Set bounded capacity on ApplyFsm mailboxes so `push()` blocks when the apply path is overwhelmed, creating backpressure on the Raft commit path:

```cpp
// In ApplyFsm constructor, initialize mailbox with backpressure limit
explicit ApplyFsm(..., int mailbox_capacity = 256)
    : kv_node_(kv_node), ..., mb_(mailbox_capacity) {}  // bounded queue
```

### Step 3: Remove RegisterStripeRaftHooks From Hot Path

**File**: [`multi-raft/raft_store_stripe.cc`](multi-raft/raft_store_stripe.cc)

Move `RegisterStripeRaftHooks()` out of `StripePut()`. Call it once in `RaftStore` initialization (`InitRaftInstances()` or `CreateRaftInstances()`):

```cpp
// In RaftStore::InitRaftInstances(), after creating Raft instances:
void RaftStore::InitRaftInstances() {
    RegisterStripeRaftHooks();  // Called ONCE here
    // ... rest of existing code
}

// Remove from StripePut():
std::tuple<kv::ErrorType, uint64_t, uint64_t>
RaftStore::StripePut(GroupId group_id, ...) {
    // NO RegisterStripeRaftHooks() call here
    // ...
}
```

### Step 4: Optimize Memory Allocation in StripePut

**File**: [`multi-raft/raft_store_stripe.cc`](multi-raft/raft_store_stripe.cc)

Replace heap allocation with `raft::Slice::Copy()` for the hot path:

```cpp
// Instead of:
char* copy = new char[ser.size()];
std::memcpy(copy, ser.data(), ser.size());
raft_node->Propose(raft::CommandData{0, raft::Slice(copy, ser.size())});

// Use:
raft_node->Propose(raft::CommandData{0, raft::Slice::Copy(ser)});  // safe, no manual delete
```

### Step 5: Reduce Poll Batch Size for Better Responsiveness

**File**: [`bench/bench_server_multiraft.cc`](bench/bench_server_multiraft.cc)

With `max_batch=64`, when a mailbox has many pending entries, one thread drains 64 at once, starving other groups. Reduce default to 8-16 and add a flag:

```cpp
DEFINE_int32(batch_size, 16, "max messages per batch per FSM");  // was 64
```

### Step 6: Increase Default Apply/Peer Threads

**File**: [`bench/bench_server_multiraft.cc`](bench/bench_server_multiraft.cc)

For 7 groups, increase default thread counts to provide more parallelism:

```cpp
DEFINE_int32(peer_threads, 7,  "number of Poll threads for PeerFsm");   // was 2
DEFINE_int32(apply_threads, 14, "number of Apply threads for ApplyFsm"); // was 2
```

### Step 7: Batch StripePut Commits with Adaptive Sleep

**File**: [`multi-raft/raft_store_stripe.cc`](multi-raft/raft_store_stripe.cc)

Instead of sleeping 50ms per retry, use **exponential backoff** and adaptive waiting:

```cpp
for (int retry = 0; retry < 20; ++retry) {
    if (raft_node->getRaftState()->CommitIndex() >= target) {
        // ...
    }
    // Exponential backoff: 10, 20, 40, 80, 80, 80, ...
    int sleep_ms = (retry < 4) ? (10 << retry) : 80;
    std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
}
```

### Step 8: Add ApplyFsm Mailbox Monitoring

**File**: [`multi-raft/raft_store.h`](multi-raft/raft_store.h)

Add mailbox size monitoring to detect backpressure early:

```cpp
// In BatchSystem, add method to report backpressure
bool IsAnyMailboxNearlyFull() const {
    for (auto& e : apply_entries_) {
        if (e->apply_fsm->mailbox().nearly_full()) return true;
    }
    return false;
}
```

---

## Testing Strategy (Progressive)

1. **Baseline**: Run 4K/100 — verify 0 failures
2. **Intermediate**: Run 4K/500, 512K/100 — verify 0 failures  
3. **Target**: Run 2048K/500 — verify 0 failures, measure throughput

## Expected Impact

| Fix | Effect |
|-----|--------|
| More apply threads | ~2-4x faster commit drain rate |
| Remove RegisterStripeRaftHooks | -1 function call per write, eliminates redundant initialization |
| Slice::Copy | Eliminates heap allocation per write |
| Backpressure | Prevents unbounded queue growth, backpressures client RPCs |
| Batch size 64->16 | More responsive per-group scheduling |
| Exponential backoff | Better sleep efficiency in StripePut |