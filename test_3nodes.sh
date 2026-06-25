#!/bin/bash
cd /home/wenry/programs/FlexRaft-Code

echo "========================================"
echo "Testing 3-node cluster startup"
echo "========================================"

# Cleanup
pkill -9 -f ycsb_server_multiraft 2>/dev/null
sleep 3
rm -f build/log/node*.log

# Start 3 nodes with moderate thread count
echo "Starting nodes..."
for i in 0 1 2; do
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
echo "Waiting 30 seconds..."
sleep 30

echo ""
echo "========================================"
echo "Status after 30 seconds:"
echo "========================================"

for i in 0 1 2; do
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
