#include "stripe_read.h"

#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "lrc_encoder.h"
#include "rpc.h"
#include "stripe_format.h"

namespace multiraft {

bool TryStripeGet(kv::KvServiceNode* node, raft::raft_node_id_t my_id, int cluster_n,
                  const kv::GetValueRequest& req, kv::GetValueResponse* resp) {
  auto* kv = node->GetKvServer();
  if (!kv) return false;

  std::string meta_bytes;
  if (!kv->DB()->Get(StripeMetaDbKey(req.group_id, req.key), &meta_bytes)) {
    fprintf(stderr, "[STRIPE-READ-DEBUG] N%d meta NOT FOUND for key='%s' group=%u (probe='%s')\n",
            my_id, req.key.c_str(), req.group_id,
            StripeMetaDbKey(req.group_id, req.key).c_str());
    return false;
  }

  StripeLogMeta meta;
  if (!StripeLogMeta::Deserialize(meta_bytes.data(), meta_bytes.size(), &meta)) {
    return false;
  }

  GroupId gid = static_cast<GroupId>(req.group_id);
  std::map<int, std::string> frag_bytes;

  // For LRC with m=0: decode using only k DATA fragments.
  // Skip local parities (l of them) and global parities (r of them) —
  // they are NOT needed for reconstruction.
  std::vector<FragmentPlacement> data_placement;
  for (const auto& fp : meta.placement) {
    if (fp.kind != FragmentPlacement::Kind::kData) break;
    data_placement.push_back(fp);
  }
  if (data_placement.size() != static_cast<size_t>(meta.k)) {
    fprintf(stderr, "[STRIPE-READ-DEBUG] N%d key='%s' placement_size=%zu k=%d RACE: "
                    "meta.placement has %zu entries but k=%d (meta.group=%u req.group=%u)\n",
            my_id, req.key.c_str(), data_placement.size(), meta.k,
            meta.placement.size(), meta.k, meta.group_id, req.group_id);
    return false;
  }

  struct FetchResult {
    bool ok = false;
    int frag_id = -1;
    std::string value;
    std::string fail_reason;
  };

  // Per-task stub factory: each jthread constructs its own short-lived
  // KvServerRPCClient. The RcfClient instances inside KvServerRPCClient's
  // client_pool_ are NOT thread-safe (kv/rpc.h:67-68); sharing a single stub
  // across jthreads would cause the GetValue response to be lost on a
  // pool_size=2 RcfClient pool when more than 2 fragments hit the same peer
  // in parallel.
  auto make_stub = [&](int node_id) -> std::unique_ptr<kv::rpc::KvServerRPCClient> {
    auto* peer_stub = reinterpret_cast<kv::rpc::KvServerRPCClient*>(
        kv->GetKVPeerServerStub(static_cast<raft::raft_node_id_t>(node_id)));
    if (!peer_stub) return nullptr;
    return std::make_unique<kv::rpc::KvServerRPCClient>(
        peer_stub->GetAddress(), static_cast<raft::raft_node_id_t>(node_id));
  };

  auto fetch_frag_fn = [&](int frag_id, int node_id) -> FetchResult {
    std::string fk = FragDbKey(gid, meta.stripe_id, frag_id);
    FetchResult r;
    r.frag_id = frag_id;

    if (node_id == static_cast<int>(my_id)) {
      if (!kv->DB()->Get(fk, &r.value)) {
        r.fail_reason = "local_db_not_found";
        return r;
      }
      r.ok = true;
      return r;
    }

    auto stub = make_stub(node_id);
    if (!stub) {
      r.fail_reason = "peer_stub_null";
      fprintf(stderr, "[DIAG-STRIPE] N%d -> N%d: stub is NULL for key='%s'\n",
              my_id, node_id, fk.c_str());
      return r;
    }
    kv::GetValueRequest gr{fk, req.read_index, req.group_id};
    fprintf(stderr, "[DIAG-STRIPE] N%d -> N%d: RPC GetValue key='%s' group=%u\n",
            my_id, node_id, fk.c_str(), gr.group_id);
    auto resp = stub->GetValue(gr);
    fprintf(stderr, "[DIAG-STRIPE] N%d <- N%d: RPC resp.err=%d key='%s'\n",
            my_id, node_id, static_cast<int>(resp.err), fk.c_str());
    if (resp.err != kv::kOk) {
      r.fail_reason = "peer_rpc_failed(err=" + std::to_string(static_cast<int>(resp.err)) + ")";
      return r;
    }
    r.value = std::move(resp.value);
    r.ok = true;
    return r;
  };

  // Parallel fetch: one std::jthread per data fragment. Each thread produces
  // a FetchResult which is committed under result_mtx into either frag_bytes
  // (success) or failure_reasons (failure). This collapses wall time from
  // O(k * RTT) to O(max(RTT)) per TryStripeGet call.
  std::mutex result_mtx;
  std::vector<std::string> failure_reasons;
  {
    std::vector<std::jthread> fetchers;
    fetchers.reserve(data_placement.size());
    for (const auto& fp : data_placement) {
      fetchers.emplace_back([&, frag_id = fp.frag_id, nid = fp.node_id]() {
        FetchResult r = fetch_frag_fn(frag_id, nid);
        std::lock_guard<std::mutex> lk(result_mtx);
        if (r.ok) {
          frag_bytes[r.frag_id] = std::move(r.value);
        } else {
          fprintf(stderr,
                  "[STRIPE-READ] fetch failed: frag=%d node=%d my_id=%d stripe_id=%lu "
                  "gid=%u reason=%s\n",
                  r.frag_id, nid, my_id, meta.stripe_id, gid, r.fail_reason.c_str());
          failure_reasons.push_back("frag=" + std::to_string(r.frag_id) +
                                    " node=" + std::to_string(nid) +
                                    " reason=" + r.fail_reason);
        }
      });
    }
    // jthread destructors join automatically when the inner scope ends.
  }

  if (frag_bytes.size() < static_cast<size_t>(meta.k)) {
    fprintf(stderr,
            "[STRIPE-READ] insufficient fragments: have=%zu need=%d (failures=%zu) "
            "key='%s' group=%u stripe_id=%lu\n",
            frag_bytes.size(), meta.k, failure_reasons.size(),
            req.key.c_str(), req.group_id, meta.stripe_id);
    for (const auto& reason : failure_reasons) {
      fprintf(stderr, "[STRIPE-READ]   - %s\n", reason.c_str());
    }
    return false;
  }

  std::string decoded;
  switch (meta.encoding_mode) {
    case EncodingMode::kLrc: {
      // LRC decodes using only k data fragments (all k present here).
      // m=0 means no RS decoding is needed — just concatenate the k data fragments.
      raft::Encoder::EncodingResults input;
      for (const auto& [fid, bytes] : frag_bytes) {
        input[static_cast<raft::raft_frag_id_t>(fid)] =
            raft::Slice(const_cast<char*>(bytes.data()), bytes.size());
      }
      raft::Encoder enc;
      raft::Slice out;
      if (!enc.DecodeSlice(input, meta.k, 0, &out)) return false;
      // Encoder pads to fragment_size * k bytes. Truncate to original size.
      decoded.assign(out.data(), std::min(out.size(), meta.original_size));
      delete[] out.data();
      break;
    }
    case EncodingMode::kRs3F:
    case EncodingMode::kRsF: {
      raft::Encoder enc;
      raft::Encoder::EncodingResults input;
      for (const auto& [fid, bytes] : frag_bytes) {
        input[fid] = raft::Slice(const_cast<char*>(bytes.data()), bytes.size());
      }
      raft::Slice out;
      int m = meta.m;
      if (!enc.DecodeSlice(input, meta.k, m, &out)) return false;
      // Encoder pads to fragment_size * k bytes. Truncate to original size.
      decoded.assign(out.data(), std::min(out.size(), meta.original_size));
      delete[] out.data();
      break;
    }
  }

  printf("[STRIPE-READ][N%d] key=%s decoded_size=%zu\n", my_id, req.key.c_str(), decoded.size());
  fflush(stdout);

  resp->value = std::move(decoded);
  resp->err = kv::kOk;
  resp->reply_server_id = my_id;
  return true;
}

}  // namespace multiraft
