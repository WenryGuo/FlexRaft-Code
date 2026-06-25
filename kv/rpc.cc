#include "rpc.h"

#include "RCF/Exception.hpp"
#include "kv_server.h"
#include "type.h"
#include "util.h"

namespace kv {
namespace rpc {
// Response KvServerRPCService::DealWithRequest(const Request &req) {
//   Response resp;
//   server_->DealWithRequest(&req, &resp);
//   return resp;
// }

Response KvServerRPCClient::DealWithRequest(const Request &request) {
  try {
    auto resp = NextClient().DealWithRequest(request);
    return resp;
  } catch (RCF::Exception &e) {
    // #region agent log
    int current_count = 0;
    static std::atomic<int> error_count{0};
    error_count.fetch_add(1, std::memory_order_relaxed);
    fprintf(stderr, "[DEBUG-8de3d8] DealWithRequest EXCEPTION client_id=%d error='%s' total_errors=%d\n",
            id_, e.getErrorMessage().c_str(), (int)error_count.load());
    fflush(stderr);
    // #endregion
    fprintf(stderr, "KvServerRPC Client %d RCP failed(%s)\n", id_,
        e.getErrorMessage().c_str());
    Response resp;
    resp.err = kRPCCallFailed;
    return resp;
  }
}

// Synchronize call GetValue
GetValueResponse KvServerRPCClient::GetValue(const GetValueRequest &request) {
  try {
    auto resp = NextClient().GetValue(request);
    return resp;
  } catch (RCF::Exception &e) {
    LOG(raft::util::kRaft, "KvServerRPC Client %d RCP failed(%s)", id_,
        e.getErrorMessage().c_str());
    GetValueResponse resp;
    resp.err = kRPCCallFailed;
    return resp;
  }
}

// Asynchronous call GetValue
void KvServerRPCClient::GetValue(const GetValueRequest &request,
                                 std::function<void(const GetValueResponse &)> cb) {
  auto client_ptr = std::make_shared<RcfClient<I_KvServerRPCService>>(
      RCF::TcpEndpoint(address_.ip, address_.port));
  client_ptr->getClientStub().getTransport().setMaxOutgoingMessageLength(512 * 1024 * 1024);
  client_ptr->getClientStub().setRemoteCallTimeoutMs(900);

  RCF::Future<GetValueResponse> ret;
  auto cmp_callback = [=, this]() { onGetValueComplete(ret, cb); };
  ret = client_ptr->GetValue(RCF::AsyncTwoway(cmp_callback), request);
  LOG(raft::util::kRaft, "KvServerRPCClient::GetValue request sent");
}

void KvServerRPCClient::onGetValueComplete(RCF::Future<GetValueResponse> ret,
                                           std::function<void(const GetValueResponse &)> cb) {
  auto ePtr = ret.getAsyncException();
  if (ePtr.get()) {
    LOG(raft::util::kRaft, "GetValue RPC Call Error: %s", ePtr->getErrorString().c_str());
  } else {
    cb(*ret);
  }
}
}  // namespace rpc
}  // namespace kv
