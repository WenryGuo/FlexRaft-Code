#!/bin/bash
# test_lrc_7nodes.sh — 7节点LRC Multi-Raft集群测试脚本

set -e

BUILD_DIR="/home/wenry/programs/FlexRaft-Code/build"
CONF_FILE="/home/wenry/programs/FlexRaft-Code/conf/multi-raft-7-lrc.conf"
LOG_DIR="/tmp/mr7_test"

# 创建日志目录
mkdir -p $LOG_DIR

# 清理旧数据
echo "=== Cleaning old data ==="
rm -rf /tmp/mr7_n*.log /tmp/mr7_n*.db /tmp/mr7_n*.lg* 2>/dev/null || true

# 杀掉可能存在的旧进程
pkill -9 -f bench_server_multiraft 2>/dev/null || true
sleep 2

# 清理端口
for port in 50010 50011 50012 50013 50014 50015 50016 60010 60011 60012 60013 60014 60015 60016; do
  fuser -k $port/tcp 2>/dev/null || true
done
sleep 1

echo "=== Starting 7-node LRC cluster ==="
echo "Configuration: N=7, LRC(4,2,1)"
echo ""

# 启动所有节点
for i in 0 1 2 3 4 5 6; do
  $BUILD_DIR/bench/bench_server_multiraft \
    --conf $CONF_FILE \
    --id $i \
    > $LOG_DIR/node_${i}.log 2>&1 &
  PID=$!
  echo "Node $i started (PID: $PID)"
  sleep 2
  
  # 检查是否成功启动
  if ! kill -0 $PID 2>/dev/null; then
    echo "ERROR: Node $i failed to start!"
    tail -20 $LOG_DIR/node_${i}.log
    exit 1
  fi
done

echo ""
echo "=== All nodes started ==="
echo "Waiting 20 seconds for Raft election..."
sleep 20

# 检查节点状态
echo ""
echo "=== Checking node status ==="
PIDS=""
for i in 0 1 2 3 4 5 6; do
  PID=$(pgrep -f "bench_server_multiraft.*--id $i" | head -1)
  if [ -n "$PID" ]; then
    echo "Node $i: RUNNING (PID $PID)"
    PIDS="$PIDS $PID"
  else
    echo "Node $i: STOPPED"
  fi
done

# 检查 Leader 状态
echo ""
echo "=== Sending SIGUSR2 to check Leader status ==="
for i in 0 1 2 3 4 5 6; do
  PID=$(pgrep -f "bench_server_multiraft.*--id $i" | head -1)
  if [ -n "$PID" ]; then
    kill -USR2 $PID 2>/dev/null || true
  fi
done

sleep 3

# 检查 Leader 选举结果
echo ""
echo "=== Leader Status (from logs) ==="
for i in 0 1 2 3 4 5 6; do
  STATUS=$(grep -A2 "GROUP LEADER STATUS" $LOG_DIR/node_${i}.log 2>/dev/null | tail -1 | grep -o "LEADER\|FOLLOWER" || echo "UNKNOWN")
  echo "Node $i: $STATUS"
done

# 触发测试写入
echo ""
echo "=== Triggering test write ==="
PID0=$(pgrep -f "bench_server_multiraft.*--id 0" | head -1)
if [ -n "$PID0" ]; then
  kill -USR1 $PID0 2>/dev/null || true
  sleep 5
  
  # 检查写入结果
  echo ""
  echo "=== Test write result ==="
  grep -E "Propose result|Write.*FAILED|Write.*success" $LOG_DIR/node_0.log 2>/dev/null | tail -5
fi

echo ""
echo "=== Test complete ==="
echo "Logs saved to: $LOG_DIR/"
echo ""
echo "Commands to interact with cluster:"
echo "  kill -USR1 \$(pgrep -f 'bench_server_multiraft.*--id 0')  # Trigger write"
echo "  kill -USR2 \$(pgrep -f 'bench_server_multiraft.*--id 0')  # Check status"
echo "  kill -INT \$(pgrep -f 'bench_server_multiraft.*--id 0')   # Shutdown"
