#!/bin/bash
cd /home/wenry/programs/FlexRaft-Code

echo "========================================"
echo "Testing 7-node cluster with ORIGINAL thread config"
echo "========================================"

# Cleanup
pkill -9 -f ycsb_server_multiraft 2>/dev/null
sleep 3
rm -f build/log/node*.log

# Start 7 nodes with ORIGINAL thread count (14 threads each)
echo "Starting nodes with peer_threads=14, apply_threads=14..."
for i in 0 1 2 3 4 5 6; do
    ./build/bench/ycsb_server_multiraft \
        --conf conf/multi-raft-7-localtest.conf \
        --id $i \
        --peer_threads=14 \
        --apply_threads 14 \
        --batch_size 32 \
        --encoding=LRC \
        > build/log/node${i}.log 2>&1 &
    echo "Node $i started (PID: $!)"
done

echo ""
echo "Waiting 90 seconds..."
for t in 15 30 45 60 75 90; do
    sleep 15
    running=$(pgrep -c -f "ycsb_server_multiraft" 2>/dev/null || echo 0)
    echo "  t=${t}s: Running=$running"
    if [ "$running" -eq 0 ]; then
        echo "All nodes crashed!"
        break
    fi
    if [ "$running" -eq 7 ]; then
        echo "SUCCESS: All 7 nodes running!"
        break
    fi
done

echo ""
echo "========================================"
echo "Final Status:"
echo "========================================"

running_count=0
for i in 0 1 2 3 4 5 6; do
    if pgrep -f "ycsb_server_multiraft.*--id $i" > /dev/null 2>&1; then
        echo "Node $i: RUNNING"
        ((running_count++))
    else
        echo "Node $i: STOPPED"
        echo "  Last 5 lines from log:"
        tail -5 build/log/node${i}.log 2>/dev/null | sed 's/^/    /'
    fi
done

echo ""
echo "Running: $running_count/7"

echo ""
echo "========================================"
echo "Cleanup..."
echo "========================================"
pkill -9 -f ycsb_server_multiraft 2>/dev/null
echo "Done."
