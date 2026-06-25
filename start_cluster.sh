#!/bin/bash
# start_cluster.sh - 启动 3 节点 Multi-Raft 集群

cd /home/wenry/programs/FlexRaft-Code

# 清理
rm -rf /tmp/mr3rs_n*.log /tmp/mr3rs_n*.db

echo "Starting Node 0..."
./build/bench/bench_server_multiraft \
    --conf conf/multi-raft-3x3-rs21.conf \
    --id 0 --peer_threads 2 --apply_threads 2 \
    > /tmp/node0.log 2>&1 &
NODE0_PID=$!
echo "Node 0 PID: $NODE0_PID"

sleep 2

echo "Starting Node 1..."
./build/bench/bench_server_multiraft \
    --conf conf/multi-raft-3x3-rs21.conf \
    --id 1 --peer_threads 2 --apply_threads 2 \
    > /tmp/node1.log 2>&1 &
NODE1_PID=$!
echo "Node 1 PID: $NODE1_PID"

sleep 2

echo "Starting Node 2..."
./build/bench/bench_server_multiraft \
    --conf conf/multi-raft-3x3-rs21.conf \
    --id 2 --peer_threads 2 --apply_threads 2 \
    > /tmp/node2.log 2>&1 &
NODE2_PID=$!
echo "Node 2 PID: $NODE2_PID"

echo ""
echo "=========================================="
echo "Cluster started!"
echo "=========================================="
echo "Node 0 PID: $NODE0_PID"
echo "Node 1 PID: $NODE1_PID"
echo "Node 2 PID: $NODE2_PID"
echo ""
echo "Test commands:"
echo "  kill -USR1 $NODE0_PID  # Trigger write on Node 0"
echo "  kill -USR2 $NODE0_PID  # Print routing table"
echo ""
echo "View logs:"
echo "  tail -f /tmp/node0.log"
echo "  tail -f /tmp/node1.log"
echo "  tail -f /tmp/node2.log"
echo ""
echo "Stop all:"
echo "  kill $NODE0_PID $NODE1_PID $NODE2_PID"
