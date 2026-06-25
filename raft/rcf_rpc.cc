#include "rcf_rpc.h"

#include <cmath>
#include <fstream>
#include <ratio>

#include "RCF/ByteBuffer.hpp"
#include "RCF/ClientStub.hpp"
#include "RCF/Endpoint.hpp"
#include "RCF/Future.hpp"
#include "RCF/RCF.hpp"
#include "RCF/TcpEndpoint.hpp"
#include "raft.h"
#include "raft_struct.h"
#include "raft_type.h"
#include "util.h"

namespace raft {
namespace rpc {

// =======================================================================
//  RaftRPCService — server-side RPC handler
// =======================================================================
RCF::ByteBuffer RaftRPCService::RequestVote(const RCF::ByteBuffer &arg_buf) {
  RequestVoteArgs args;
  RequestVoteReply reply;

  auto serializer = Serializer::NewSerializer();
  serializer.Deserialize(&arg_buf, &args);
  raft_->Process(&args, &reply);

  RCF::ByteBuffer reply_buf(serializer.getSerializeSize(reply));
  serializer.Serialize(&reply, &reply_buf);

  return reply_buf;
}

RCF::ByteBuffer RaftRPCService::AppendEntries(const RCF::ByteBuffer &arg_buf) {
  AppendEntriesArgs args;
  AppendEntriesReply reply;

  auto serializer = Serializer::NewSerializer();
  serializer.Deserialize(&arg_buf, &args);

#ifdef ENABLE_PERF_RECORDING
  util::RaftAppendEntriesProcessPerfCounter counter(arg_buf.getLength());
#endif

  if (raft_ != nullptr) {
    raft_->Process(&args, &reply);
  } else {
    reply.chunk_infos.reserve(args.entry_cnt);
    for (int i = 0; i < args.entry_cnt; ++i) {
      auto chunkinfo = args.entries[i].GetChunkInfo();
      reply.chunk_infos.push_back(chunkinfo);
    }
    reply.chunk_info_cnt = reply.chunk_infos.size();
  }

#ifdef ENABLE_PERF_RECORDING
  counter.Record();
  PERF_LOG(&counter);
#endif

  RCF::ByteBuffer reply_buf(serializer.getSerializeSize(reply));
  serializer.Serialize(&reply, &reply_buf);

  return reply_buf;
}

RCF::ByteBuffer RaftRPCService::RequestFragments(const RCF::ByteBuffer &arg_buf) {
  RequestFragmentsArgs args;
  RequestFragmentsReply reply;

  auto serializer = Serializer::NewSerializer();
  serializer.Deserialize(&arg_buf, &args);
  raft_->Process(&args, &reply);

  RCF::ByteBuffer reply_buf(serializer.getSerializeSize(reply));
  serializer.Serialize(&reply, &reply_buf);

  return reply_buf;
}

// =======================================================================
//  GroupNotification — creates a new connection per call (group setup only)
// =======================================================================
void RCFRpcClient::sendGroupNotification(const GroupNotificationArgs &args) {
  if (stopped_) return;

  using GrpClient = RcfClient<I_GroupNotificationService>;
  std::shared_ptr<GrpClient> client_ptr(
      new GrpClient(RCF::TcpEndpoint(target_address_.ip, target_address_.port)));

  client_ptr->getClientStub().getTransport().setMaxOutgoingMessageLength(config::kMaxMessageLength);
  client_ptr->getClientStub().getTransport().setMaxIncomingMessageLength(config::kMaxMessageLength);

  auto serializer = Serializer::NewSerializer();
  RCF::ByteBuffer arg_buf(serializer.getSerializeSize(args));
  serializer.Serialize(&args, &arg_buf);

  RCF::ByteBuffer ret_buf = client_ptr->GroupNotification(arg_buf);
  (void)ret_buf;
  printf("[RPC-CLIENT-N%u] Sent GroupNotification to %s:%d\n",
         id_, target_address_.ip.c_str(), target_address_.port);
}

// =======================================================================
//  Async RPC callbacks — static methods (no client_ptr needed)
// =======================================================================
void RCFRpcClient::onRequestVoteComplete(RCF::Future<RCF::ByteBuffer> ret,
                                         RaftState *raft, raft_node_id_t peer) {
  if (raft == nullptr) return;
  auto ePtr = ret.getAsyncException();
  if (ePtr.get()) {
    LOG(util::kRPC, "S%u RequestVote RPC Call Error: %s", peer, ePtr->getErrorString().c_str());
  } else {
    RCF::ByteBuffer ret_buf = *ret;
    RequestVoteReply reply;
    Serializer::NewSerializer().Deserialize(&ret_buf, &reply);
    raft->Process(&reply);
  }
}

void RCFRpcClient::onAppendEntriesComplete(RCF::Future<RCF::ByteBuffer> ret,
                                         RaftState *raft, raft_node_id_t peer,
                                         RPCArgStats rpc_stats,
                                         RPCStatsRecorder *recorder) {
  if (raft == nullptr) return;
  auto ePtr = ret.getAsyncException();
  if (ePtr.get()) {
    printf("[RPC-CLIENT] AppendEntries CALLBACK ERROR: peer=%u error=%s\n",
           peer, ePtr->getErrorString().c_str());
    return;
  }
  auto time = util::DurationToMicros(rpc_stats.start_time, util::NowTime());

  RCF::ByteBuffer ret_buf = *ret;
  AppendEntriesReply reply;
  Serializer::NewSerializer().Deserialize(&ret_buf, &reply);
  raft->Process(&reply);

  if (rpc_stats.arg_size > kAppendEntriesArgsHdrSize) {
    auto stat = RPCStats{rpc_stats.arg_size, ret_buf.getLength(), time, time - 0, 0};
    recorder->Add(stat);
  }
}

void RCFRpcClient::onAppendEntriesCompleteRecordTimer(RCF::Future<RCF::ByteBuffer> ret,
                                                     RaftState *raft, raft_node_id_t peer,
                                                     util::AppendEntriesRPCPerfCounter counter) {
  if (raft == nullptr) return;
  auto ePtr = ret.getAsyncException();
  if (ePtr.get()) {
    LOG(util::kRPC, "S%u AppendEntries RPC Call Error: %s", peer, ePtr->getErrorString().c_str());
  } else {
    counter.Record();
    PERF_LOG(&counter);

    RCF::ByteBuffer ret_buf = *ret;
    AppendEntriesReply reply;
    Serializer::NewSerializer().Deserialize(&ret_buf, &reply);
    raft->Process(&reply);
  }
}

void RCFRpcClient::onRequestFragmentsComplete(RCF::Future<RCF::ByteBuffer> ret,
                                             RaftState *raft, raft_node_id_t peer) {
  if (raft == nullptr) return;
  auto ePtr = ret.getAsyncException();
  if (ePtr.get()) {
    LOG(util::kRPC, "S%u RequestFragments RPC Call Error: %s", peer,
        ePtr->getErrorString().c_str());
  } else {
    RCF::ByteBuffer ret_buf = *ret;
    RequestFragmentsReply reply;
    Serializer::NewSerializer().Deserialize(&ret_buf, &reply);
    raft->Process(&reply);
  }
}

// =======================================================================
//  RCFRpcServer — server-side implementation
// =======================================================================
RCFRpcServer::RCFRpcServer(const NetAddress &my_address)
    : rcf_init_(), server_(RCF::TcpEndpoint(my_address.ip, my_address.port)), service_() {}

void RCFRpcServer::Start() {
  server_.getServerTransport().setMaxIncomingMessageLength(config::kMaxMessageLength);
  server_.bind<I_RaftRPCService>(service_);
  server_.start();
}

void RCFRpcServer::Stop() {
  LOG(util::kRaft, "stop running raft rpc server");
  server_.stop();
}

void RCFRpcServer::dealWithMessage(const RequestVoteArgs &reply) {}

// =======================================================================
//  RPCStatsRecorder
// =======================================================================
void RPCStatsRecorder::Dump(std::ofstream &of) {
  for (const auto &stat : history_) {
    of << stat.ToString() << "\n";
  }
}

void RPCStatsRecorder::Dump(const std::string &dst) {
  std::ofstream of;
  of.open(dst);

  int64_t total_total_time = 0;
  int64_t total_transfer_time = 0;
  int64_t total_process_time = 0;

  for (const auto &stat : history_) {
    of << stat.ToString() << "\n";
    total_total_time += stat.total_time;
    total_process_time += stat.process_time;
    total_transfer_time += stat.transfer_time;
  }

  int64_t avg_total_time = total_total_time / history_.size();
  int64_t avg_process_time = total_process_time / history_.size();
  int64_t avg_transfer_time = total_transfer_time / history_.size();

  of << "[Average Total Time]:" << total_total_time / history_.size()
     << "[Average Process Time]:" << total_process_time / history_.size()
     << "[Average Transfer Time]:" << total_transfer_time / history_.size();

  int64_t total_time_sq_sum = 0;
  int64_t process_time_sq_sum = 0;
  int64_t transfer_time_sq_sum = 0;
  for (const auto &stat : history_) {
    total_time_sq_sum += std::pow(stat.total_time - avg_total_time, 2);
    process_time_sq_sum += std::pow(stat.process_time - avg_process_time, 2);
    transfer_time_sq_sum += std::pow(stat.transfer_time - avg_transfer_time, 2);
  }

  of << "[StandardDev Total Time]: " << std::sqrt(total_time_sq_sum / history_.size())
     << "[StandardDev Process Time]: " << std::sqrt(process_time_sq_sum / history_.size())
     << "[StandardDev Transfer Time]: " << std::sqrt(transfer_time_sq_sum / history_.size());
  of.close();
}

}  // namespace rpc
}  // namespace raft
