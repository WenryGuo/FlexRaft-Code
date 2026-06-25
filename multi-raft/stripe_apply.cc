#include "stripe_apply.h"

#include <cstdio>

#include "rocksdb/db.h"
#include "rocksdb/write_batch.h"
#include "stripe_encode.h"
#include "stripe_format.h"

namespace multiraft {

// ============================================================================
// ApplyStripePacked — Apply merged storage format (with legacy format compatibility)
//
// Storage Layout (for efficient read):
//   Primary Key (new): "__stripe/{group_id}/{user_key}" → Value: meta + local fragments (merged)
//   Frag Index (new):  "__frag_idx/{group_id}/{frag_id}" → Value: primary key
//   Legacy Format:
//     "__stripe_meta__/{user_key}" → Value: StripeLogMeta (for TryStripeGet compatibility)
//     "__frag__/{group_id}/{stripe_id}/{frag_id}" → Value: fragment bytes (for TryStripeGet)
//
// Benefits:
//   - Read by key: 1 IO to get meta + local frags (new)
//   - Read by frag_id: 2 IO (index lookup + primary record) for repair scenarios
//   - Legacy TryStripeGet continues to work with old format
// ============================================================================
bool ApplyStripePacked(kv::KvServiceNode* kv_node, GroupId group_id,
                       raft::raft_node_id_t node_id, const std::string& payload) {
  StripeLogMeta meta;
  std::vector<PackedNodeFragment> frags;
  if (!UnpackNodeFragments(payload.data(), payload.size(), &meta, &frags)) {
    fprintf(stderr, "[STRIPE-APPLY] N%d unpack failed\n", node_id);
    return false;
  }
  meta.group_id = group_id;

  auto* kv = kv_node->GetKvServer();
  if (!kv) return false;

  // Build keys
  std::string primary_key = MakeStripePrimaryKey(group_id, meta.user_key);
  std::string merged_value = SerializeMergedFragStore(meta, frags);

  // Batch all writes into a single RocksDB WriteBatch
  rocksdb::WriteBatch batch;

  // Write NEW format: merged storage (1 IO per read)
  batch.Put(primary_key, merged_value);

  // Write NEW format: frag_id secondary index (for repair scenarios)
  for (const auto& f : frags) {
    std::string idx_key = MakeFragIndexKey(group_id, f.frag_id);
    batch.Put(idx_key, primary_key);
  }

  // Write LEGACY format: TryStripeGet compatibility
  batch.Put(StripeMetaDbKey(group_id, meta.user_key), meta.Serialize());
  for (const auto& f : frags) {
    batch.Put(FragDbKey(group_id, meta.stripe_id, f.frag_id), f.bytes);
  }

  if (!kv->DB()->Write(&batch)) {
    fprintf(stderr, "[STRIPE-APPLY] N%d WriteBatch failed\n", node_id);
    return false;
  }

  printf("[STRIPE-APPLY][N%d] group=%u stripe=%lu key=%s frags=%zu k=%d\n", node_id, group_id,
         meta.stripe_id, meta.user_key.c_str(), frags.size(), meta.k);
  fflush(stdout);
  return true;
}

// [DEPRECATED] ApplyStripeCommand — No longer needed with peer-keyed mode
// Leader now encodes and packs before sending, follower just stores packed data
bool ApplyStripeCommand(kv::KvServiceNode* kv_node, GroupId group_id,
                        raft::raft_node_id_t node_id, EncodingMode mode, int cluster_n,
                        EntryId entry_id, const std::string& cmd_bytes) {
  // This function is deprecated. With peer-keyed mode, the leader already
  // encodes and packs fragments before sending. Followers receive and store
  // packed data directly via ApplyStripePacked.
  //
  // Keeping this function for backward compatibility with legacy StripeWriteCommand
  // entries in logs during migration.
  StripeWriteCommand cmd;
  if (!StripeWriteCommand::Deserialize(cmd_bytes.data(), cmd_bytes.size(), &cmd)) {
    return false;
  }
  StripeEncodeResult enc;
  raft::Slice payload(cmd.user_value.data(), cmd.user_value.size());
  if (!EncodeStripePayload(mode, cluster_n, group_id, entry_id, payload, &enc)) {
    return false;
  }
  enc.meta.stripe_id = entry_id;
  enc.meta.entry_id = entry_id;
  enc.meta.user_key = cmd.user_key;
  enc.meta.group_id = group_id;
  enc.meta.original_size = cmd.user_value.size();

  auto* kv = kv_node->GetKvServer();
  if (!kv) return false;

  // Batch all writes into a single RocksDB WriteBatch
  rocksdb::WriteBatch batch;

  // Build keys
  std::string primary_key = MakeStripePrimaryKey(group_id, cmd.user_key);
  std::vector<PackedNodeFragment> all_frags;

  // Collect ALL fragments (not just local) for placement info
  for (const auto& fp : enc.meta.placement) {
    if (fp.frag_id < 0 || fp.frag_id >= static_cast<int>(enc.all_fragments.size())) continue;
    PackedNodeFragment pf;
    pf.frag_id = fp.frag_id;
    pf.bytes.assign(enc.all_fragments[fp.frag_id].data(),
                    enc.all_fragments[fp.frag_id].size());
    all_frags.push_back(std::move(pf));
  }

  // NEW format: merged storage (1 IO per read)
  std::string merged_value = SerializeMergedFragStore(enc.meta, all_frags);
  batch.Put(primary_key, merged_value);

  // NEW format: frag_id index (for repair scenarios)
  for (const auto& f : all_frags) {
    std::string idx_key = MakeFragIndexKey(group_id, f.frag_id);
    batch.Put(idx_key, primary_key);
  }

  // LEGACY format: TryStripeGet compatibility
  batch.Put(StripeMetaDbKey(group_id, cmd.user_key), enc.meta.Serialize());
  for (const auto& f : all_frags) {
    batch.Put(FragDbKey(group_id, entry_id, f.frag_id), f.bytes);
  }

  auto status = kv->DB()->Write(&batch);
  if (!status) {
    fprintf(stderr, "[STRIPE-APPLY] N%d WriteBatch failed\n", node_id);
    return false;
  }

  printf("[STRIPE-APPLY][N%d] group=%u cmd key=%s index=%lu frags=%zu\n", node_id, group_id,
         cmd.user_key.c_str(), entry_id, all_frags.size());
  fflush(stdout);
  return true;
}

}  // namespace multiraft
