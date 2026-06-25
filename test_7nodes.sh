#!/bin/bash
cd /home/wenry/programs/FlexRaft-Code

echo "========================================"
echo "Testing 7-node cluster startup (moderate threads)"
echo "========================================"

# Cleanup
pkill -9 -f ycsb_server_multiraft 2>/dev/null
sleep 3
rm -f build/log/node*.log

# Start 7 nodes with moderate thread count
echo "Starting nodes..."
for i in 0 1 2 3 4 5 6; do
    ./build/bench/ycsb_server_multiraft \
        --conf conf/multi-raft-7-localtest.conf \
        --id $i \
        --peer_threads=4 \
        --apply_threads 4 \
        --batch_size 32 \
        --encoding=LRC \
        > build/log/node${i}.log 2>&1 &
    echo "Node $i started (PID: $!)"
done

echo ""
echo "Waiting 60 seconds..."
for t in 10 20 30 40 50 60; do
    sleep 10
    running=$(pgrep -c -f "ycsb_server_multiraft" 2>/dev/null || echo 0)
    echo "  t=${t}s: Running=$running"
    if [ "$running" -eq 0 ]; then
        echo "All nodes crashed!"
        break
    fi
done

echo ""
echo "========================================"
echo "Status:"
echo "========================================"

for i in 0 1 2 3 4 5 6; do
    if pgrep -f "ycsb_server_multiraft.*--id $i" > /dev/null 2>&1; then
        echo "Node $i: RUNNING"
    else
        echo "Node $i: STOPPED"
        echo "  Last 3 lines from log:"
        tail -3 build/log/node${i}.log 2>/dev/null | sed 's/^/    /'
    fi
done

echo ""
echo "========================================"
echo "Cleanup..."
echo "========================================"
pkill -9 -f ycsb_server_multiraft 2>/dev/null
echo "Done."
