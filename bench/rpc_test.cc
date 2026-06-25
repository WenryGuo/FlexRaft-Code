#include <iostream>
#include "kv/client.h"
#include "kv/rpc.h"
#include "kv/type.h"
#include "bench/util.h"

int main() {
    auto cluster_cfg = ParseConfigurationFile("conf/multi-raft-7-lrc.conf");
    std::cout << "Parsed " << cluster_cfg.size() << " nodes\n";

    kv::KvServiceClient client(cluster_cfg, 999);

    for (int i = 0; i < 7; i++) {
        kv::Request req{kv::kDetectLeader, 999, 0, 0, "", ""};
        auto stub = client.GetRPCStub(i);
        auto resp = stub->DealWithRequest(req);
        std::cout << "Node " << i << ": err=" << static_cast<int>(resp.err)
                  << " term=" << resp.raft_term << " reply=" << resp.reply_server_id << "\n";
    }

    return 0;
}
