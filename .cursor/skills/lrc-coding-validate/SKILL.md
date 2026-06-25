---
name: lrc-coding-validate
description: Validate LRC (Locally Repairable Codes) encoding and get logic in FlexRaft-Code. Use when analyzing LRC fragment allocation, verifying get/recovery semantics, or debugging LRC-based storage systems.
---

# LRC Coding Validation Skill

## Core Requirements

### 1. Write Phase: userkey -> frag_id1, frag_id2

Each write operation maps a user key to two fragments stored on different nodes:
- `frag_id1` and `frag_id2` are stored on **different physical nodes**
- Each node stores **exactly 2 fragments** (per `FromCapacity` mode)
- Placement is deterministic based on LRC parameters and group topology

### 2. Get Phase: Requirements

**Requirement A**: 根据userkey获取对应的所有碎片
- Given a user key, must be able to locate ALL fragments belonging to that stripe
- Need `StripeLogMeta` containing `placement` vector with all fragment placements

**Requirement B**: 根据请求中所需要的frag_id读出对应的碎片并返回
- Given a specific `frag_id`, must be able to read that fragment
- Need secondary index `__frag_idx/{group_id}/{frag_id}` -> primary key

**Requirement C**: 每次get需要凑够k个块进行恢复
- Get operation must collect **k data fragments** for reconstruction
- Uses RS decoding: `DecodeSlice(input, k, m, &out)` where m is erasure count
- For LRC with no erasures: m=0, just concatenate k data fragments

## LRC Parameters (FromCapacity Mode)

```cpp
struct LrcParams {
  int k;  // data chunks
  int l;  // local groups
  int r;  // global parity chunks
};
// Total fragments = k + l + r = 2N (each node stores 2 fragments)
```

### Frag ID Convention

| Range | Type | Description |
|-------|------|-------------|
| `[0, k)` | Data | k data fragments |
| `[k, k+l)` | Local Parity | l local parity fragments |
| `[k+l, k+l+r)` | Global Parity | r global parity fragments |

## Validation Checklist

### Write Path Validation

- [ ] `LrcComplementaryGrouper::GetNodePlacementsVector()` generates correct placement
- [ ] Data frags `[0, k)` distributed across groups
- [ ] Local parity `[k, k+l)` on group parity nodes (group's last node)
- [ ] Global parity `[k+l, k+l+r)` fills remaining slots
- [ ] Each node has exactly 2 fragments
- [ ] Data fragment + local parity for same group never on same node

### Storage Layout Validation

- [ ] Primary key: `__stripe/{group_id}/{user_key}` -> meta + frags
- [ ] Index: `__frag_idx/{group_id}/{frag_id}` -> primary key
- [ ] Legacy: `__stripe_meta__/{group_id}/{user_key}` -> meta
- [ ] Legacy: `__frag__/{group_id}/{stripe_id}/{frag_id}` -> frag bytes

### Get Path Validation

- [ ] Read `StripeLogMeta` from `StripeMetaDbKey`
- [ ] Filter placement to get **all k data fragments** (kind == kData)
- [ ] For each data frag: determine node_id from placement, fetch from that node
- [ ] Decode using `Encoder::DecodeSlice(input, k, 0, &out)`
- [ ] Truncate to `meta.original_size`

### Get by frag_id Validation

- [ ] Read index: `MakeFragIndexKey(group_id, frag_id)` -> primary key
- [ ] Read primary: `MakeStripePrimaryKey(group_id, user_key)` -> merged value
- [ ] Deserialize and extract requested fragment

## Key Files

| File | Purpose |
|------|---------|
| `multi-raft/lrc_encoder.h` | LRC encoding, `LrcParams`, `LocalEncodeResult` |
| `multi-raft/lrc_complementary_grouper.h` | Fragment allocation, placement generation |
| `multi-raft/stripe_read.cc` | Get/recovery logic |
| `multi-raft/stripe_apply.cc` | Storage write logic |
| `multi-raft/stripe_format.h` | Storage key format, `StripeLogMeta` |
| `multi-raft/message.h` | `FragmentPlacement` struct |

## Current Implementation Analysis

### stripe_read.cc Get Logic (Lines 32-85)

```cpp
// Filters to only k DATA fragments
std::vector<FragmentPlacement> data_placement;
for (const auto& fp : meta.placement) {
  if (fp.kind != FragmentPlacement::Kind::kData) break;
  data_placement.push_back(fp);
}
```

**Correct**: Filters placement to k data fragments for reconstruction.

### Fetch Logic (Lines 48-85)

```cpp
auto fetch_frag = [&](int frag_id, int node_id, ...) -> bool {
  std::string fk = FragDbKey(gid, meta.stripe_id, frag_id);
  // Local: kv->DB()->Get(fk, &frag_bytes[frag_id]);
  // Remote: RPC GetValue to peer
};
```

**Correct**: Uses `FragDbKey` with `stripe_id` and `frag_id`.

### Decode Logic (Lines 87-103)

```cpp
raft::Encoder enc;
if (!enc.DecodeSlice(input, meta.k, 0, &out)) return false;
decoded.assign(out.data(), std::min(out.size(), meta.original_size));
```

**Correct**: Decodes using k fragments with m=0 (no erasures), truncates to original size.

## Potential Issues to Check

1. **Placement sort order**: Ensure `meta.placement` is sorted by `kind` (kData first)
2. **Frag ID consistency**: Verify `FetchFrag` uses correct `FragDbKey` format
3. **Stripe ID vs Entry ID**: Ensure `meta.stripe_id` matches what's used in storage keys
4. **Local vs Remote fetch**: Verify RPC mechanism works correctly
5. **Decode failure handling**: Check error paths are handled properly

## Test Scenarios

### Scenario 1: Normal Get (All k Fragments Available)

1. Write key "test_key" with value "test_value"
2. Get "test_key" -> should return "test_value"
3. Verify k data fragments were fetched and decoded

### Scenario 2: Get by Specific frag_id

1. Write key "test_key"
2. Request frag_id=0 for "test_key" -> should return that fragment's data
3. Request frag_id=1 -> should return that fragment's data

### Scenario 3: Recovery (Some Fragments Missing)

1. Simulate missing fragments
2. Get should still succeed using RS decoding with available fragments
