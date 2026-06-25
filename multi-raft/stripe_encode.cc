#include "stripe_encode.h"

#include <cstdio>
#include <cstring>

#include "encoder.h"
#include "lrc_encoder.h"
#include "lrc_placement.h"

namespace multiraft {

static void DeepCopySlice(const raft::Slice& src, std::vector<raft::Slice>* out) {
  char* copy = new char[src.size()];
  std::memcpy(copy, src.data(), src.size());
  out->emplace_back(copy, src.size());
}

bool EncodeStripePayload(EncodingMode mode, int N, GroupId group_id, EntryId eid,
                         const raft::Slice& data, StripeEncodeResult* out) {
  out->ok = false;
  out->all_fragments.clear();
  out->meta = {};
  out->meta.stripe_id = eid;
  out->meta.entry_id = eid;
  out->meta.group_id = group_id;
  out->meta.original_size = data.size();
  out->meta.encoding_mode = mode;

  if (N < 3) {
    fprintf(stderr, "[STRIPE-ENC] N=%d too small\n", N);
    return false;
  }

  switch (mode) {
    case EncodingMode::kRsF: {
      int F = N / 2;
      int k_param = F + 1;
      int m_param = F;
      raft::Encoder enc;
      raft::Encoder::EncodingResults encoded;
      if (!enc.EncodeSlice(data, k_param, m_param, &encoded)) return false;
      out->meta.k = k_param;
      out->meta.m = m_param;
      out->meta.l = 0;
      out->meta.r = 0;
      for (int i = 0; i < N; ++i) {
        auto it = encoded.find(static_cast<raft::raft_frag_id_t>(i));
        if (it == encoded.end()) return false;
        DeepCopySlice(it->second, &out->all_fragments);
        FragmentPlacement fp;
        fp.frag_id = i;
        fp.node_id = i;
        fp.kind = (i < k_param) ? FragmentPlacement::kData : FragmentPlacement::kGlobalParity;
        out->meta.placement.push_back(fp);
      }
      out->ok = true;
      return true;
    }
    case EncodingMode::kRs3F: {
      if (N < 5) return false;
      int F = N / 2;
      int k_param = F + 1;
      int m_param = 3 * F + 1;
      int total = k_param + m_param;
      raft::Encoder enc;
      raft::Encoder::EncodingResults encoded;
      if (!enc.EncodeSlice(data, k_param, m_param, &encoded)) return false;
      if (static_cast<int>(encoded.size()) != total) return false;
      out->meta.k = k_param;
      out->meta.m = m_param;
      out->meta.l = 0;
      out->meta.r = 0;
      auto plan = BuildRsRandomPlacement(N, total, static_cast<std::uint64_t>(group_id) + 1);
      out->all_fragments.reserve(static_cast<size_t>(total));
      for (int i = 0; i < total; ++i) {
        DeepCopySlice(encoded.at(static_cast<raft::raft_frag_id_t>(i)), &out->all_fragments);
      }
      for (const auto& rp : plan) {
        FragmentPlacement fp;
        fp.frag_id = rp.frag_id;
        fp.node_id = rp.node_id;
        fp.kind = (rp.frag_id < k_param) ? FragmentPlacement::kData
                                         : FragmentPlacement::kGlobalParity;
        out->meta.placement.push_back(fp);
      }
      out->ok = true;
      return true;
    }
    case EncodingMode::kLrc: {
      if (N < 5) return false;
      try {
        LrcParams lp = LrcParams::FromCapacity(N, 2);
        LrcEncoder enc(lp);
        LrcStripe stripe;
        if (!enc.EncodeStripe(data, &stripe)) return false;
        auto partitions = BuildTwoComplementaryPartitions(
            N, static_cast<std::uint64_t>(group_id) + 1);
        OrthogonalPlacer placer(lp, partitions);
        auto placement = placer.Place2N();
        out->meta.k = lp.k;
        out->meta.l = lp.l;
        out->meta.r = lp.r;
        out->meta.m = 0;
        out->all_fragments.reserve(static_cast<size_t>(lp.total_shards()));
        for (int i = 0; i < lp.k; ++i) {
          DeepCopySlice(stripe.data_shards[i], &out->all_fragments);
        }
        for (int i = 0; i < lp.l; ++i) {
          DeepCopySlice(stripe.local_parities[i], &out->all_fragments);
        }
        for (int i = 0; i < lp.r; ++i) {
          DeepCopySlice(stripe.global_parities[i], &out->all_fragments);
        }
        out->meta.placement = std::move(placement);
        stripe.FreeMemory();
        out->ok = true;
        return true;
      } catch (const std::exception& e) {
        fprintf(stderr, "[STRIPE-ENC] LRC failed: %s\n", e.what());
        return false;
      }
    }
  }
  return false;
}

}  // namespace multiraft
