#pragma once
#include <atomic>
#include <functional>
#include <thread>
#include <vector>

#include "RCF/ClientStub.hpp"
#include "RCF/Future.hpp"
#include "RCF/InitDeinit.hpp"
#include "RCF/RCF.hpp"
#include "RCF/RcfFwd.hpp"
#include "RCF/RcfMethodGen.hpp"
#include "RCF/RcfServer.hpp"
#include "RCF/TcpEndpoint.hpp"
#include "kv_server.h"
#include "raft_type.h"
#include "type.h"
#include "util.h"
namespace kv {
class KvServer;
namespace rpc {

// Define the RPC return value and parameter
RCF_BEGIN(I_KvServerRPCService, "I_KvServerRPCService")
RCF_METHOD_R1(Response, DealWithRequest, const Request &)
RCF_METHOD_R1(GetValueResponse, GetValue, const GetValueRequest &)
RCF_END(I_KvServerRPCService)

class KvServerRPCService {
 public:
  KvServerRPCService() = default;
  KvServerRPCService(KvServer *server) : server_(server) {}
  Response DealWithRequest(const Request &req) {
    Response resp;
    server_->DealWithRequest(&req, &resp);
    return resp;
  }

  GetValueResponse GetValue(const GetValueRequest &request) {
    raft::util::Timer timer;
    timer.Reset();
    // Spin until the entries before read index have been applied into the DB
    while (server_->LastApplyIndex() < request.read_index) {
      ;
    }
    std::string value;
    auto found = server_->DB()->Get(request.key, &value);
    if (found) {
      return GetValueResponse{std::move(value), kOk, server_->Id()};
    } else {
      return GetValueResponse{std::string(""), kKeyNotExist, server_->Id()};
    }
  }

  void SetKvServer(KvServer *server) { server_ = server; }

 private:
  KvServer *server_;
};

// RPC client issues a DealWithRequest RPC to specified KvNode by
// simply call "DealWithRequest()". The call is synchronized and might be
// blocked. We need a timeout to solve this problem.
//
// Each KvServerRPCClient object responds to a KvNode.
// A pool of RcfClient instances is used to support concurrent calls from
// multiple threads — RcfClient is NOT thread-safe.
class KvServerRPCClient {
 public:
  static constexpr int kRPCTimeout = 30000;  // 30s (increased from 10s)

  explicit KvServerRPCClient(const NetAddress& net_addr,
                             raft::raft_node_id_t id,
                             int pool_size = 2)
      : address_(net_addr),
        id_(id),
        rcf_init_(),
        pool_index_(0),
        pool_size_(pool_size) {
    RCF::TcpEndpoint ep(net_addr.ip, net_addr.port);
    for (int i = 0; i < pool_size_; ++i) {
      client_pool_.emplace_back(ep);
      auto& stub = client_pool_.back().getClientStub();
      stub.getTransport().setMaxIncomingMessageLength(raft::rpc::config::kMaxMessageLength);
      stub.getTransport().setMaxOutgoingMessageLength(raft::rpc::config::kMaxMessageLength);
      stub.setRemoteCallTimeoutMs(kRPCTimeout);
    }
  }

  Response DealWithRequest(const Request &request);

  void GetValue(const GetValueRequest &request, std::function<void(const GetValueResponse &)> cb);

  void onGetValueComplete(RCF::Future<GetValueResponse> ret,
                          std::function<void(const GetValueResponse &)> cb);

  GetValueResponse GetValue(const GetValueRequest &request);

  // Expose the address for callers that need to construct a thread-local clone
  // (e.g. parallel fragment fetch in stripe_read). RcfClient instances inside
  // the connection pool are NOT thread-safe, so each jthread must use its own.
  const NetAddress& GetAddress() const { return address_; }

  void SetRPCTimeOutMs(int cnt) {
    for (auto &client : client_pool_) {
      client.getClientStub().setRemoteCallTimeoutMs(cnt);
    }
  }

 private:
  RcfClient<I_KvServerRPCService> &NextClient() {
    int idx = pool_index_.fetch_add(1, std::memory_order_relaxed) % pool_size_;
    // #region agent log
    static std::atomic<int> call_count{0};
    int current_count = call_count.fetch_add(1, std::memory_order_relaxed);
    // fprintf(stderr, "[DEBUG-8de3d8] NextClient thread=%ld client_id=%d pool_size=%d idx=%d call=%d\n",
    //         (long)std::hash<std::thread::id>{}(std::this_thread::get_id()),
    //         id_, pool_size_, idx, current_count);
    // fflush(stderr);
    // #endregion
    return client_pool_[idx];
  }

  RCF::RcfInit rcf_init_;
  NetAddress address_;
  raft::raft_node_id_t id_;
  std::vector<RcfClient<I_KvServerRPCService>> client_pool_;
  std::atomic<int> pool_index_;
  int pool_size_;
};

// Server side of a KvNode, the server calls Start() to continue receive
// RPC request from client and deal with it.
class KvServerRPCServer {
 public:
  KvServerRPCServer(const NetAddress &net_addr, raft::raft_node_id_t id)
      : address_(net_addr),
        id_(id),
        server_(RCF::TcpEndpoint(net_addr.ip, net_addr.port)),
        started_(false) {
    fprintf(stderr, "[KV-RPC-SERVER-%d] RPC init (ip=%s port=%d)\n", id_, net_addr.ip.c_str(),
        net_addr.port);
  }
  KvServerRPCServer() = default;

  bool IsStarted() const { return started_.load(std::memory_order_acquire); }

  void Start() {
    bool expected = false;
    if (!started_.compare_exchange_strong(expected, true)) {
      return;  // Already started — idempotent
    }
    try {
      server_.getServerTransport().setMaxIncomingMessageLength(raft::rpc::config::kMaxMessageLength);
      // Dynamic thread pool sizing for KV RPC server.
      // Same formula as Raft Unified RPC Server (raft/raft_unified_rpc_server.cc):
      //   max(16, groups * 2)
      // The 7-group localtest config yields 16 threads (matches the minimum
      // used by the Raft RPC server, ensuring KV RPC concurrency does not
      // bottleneck YCSB client traffic).
      constexpr int kKvRpcThreadCount = 16;
      RCF::ThreadPoolPtr tp(new RCF::ThreadPool(kKvRpcThreadCount));
      server_.setThreadPool(tp);
      server_.start();
    } catch (const std::exception& e) {
      fprintf(stderr, "[KV-RPC-SERVER-%d] Start() failed: %s\n", id_, e.what());
      started_.store(false, std::memory_order_release);
      throw;
    } catch (...) {
      fprintf(stderr, "[KV-RPC-SERVER-%d] Start() failed: unknown exception\n", id_);
      started_.store(false, std::memory_order_release);
      throw;
    }
  }

  void Stop() {
    fprintf(stderr, "[KV-RPC-SERVER-%d] stop RPC server\n", id_);
    started_.store(false, std::memory_order_release);
    server_.stop();
  }

  void SetServiceContext(KvServer *server) { service_.SetKvServer(server); }

  template <typename ServiceT>
  void BindService(ServiceT* service) {
    server_.bind<I_KvServerRPCService>(*service);
  }

 private:
  RCF::RcfInit rcf_init_;
  NetAddress address_;
  raft::raft_node_id_t id_;
  RCF::RcfServer server_;
  KvServerRPCService service_;
  std::atomic<bool> started_{false};
};

}  // namespace rpc
}  // namespace kv
