#!/bin/bash
# FlexRaft 7节点集群启动脚本
#
# 功能：
#   1. 清理旧进程和占用端口
#   2. 启动所有节点（默认 --encoding=LRC，可通过环境变量 ENCODING 覆盖）
#   3. 轮询等待所有节点就绪（KV RPC 端口监听）
#   4. 报告启动状态
#
# 用法：
#   ./start_7nodes.sh                  # 默认 LRC 模式
#   ENCODING=RS_F  ./start_7nodes.sh   # RS(F+1, F),    commit=2F+1
#   ENCODING=RS_3F ./start_7nodes.sh   # RS(F+1, 3F+1), commit=ceil((3F+1)/2)
#   ENCODING=LRC   ./start_7nodes.sh   # LRC(F+1,2,2N-k-2), commit=ceil((3F+1)/2)

set -e

NODES=(0 1 2 3 4 5 6)
MAX_WAIT=120  # 最大等待时间（秒）
CHECK_INTERVAL=2  # 检查间隔（秒）

# Encoding mode (runtime gflag for bench_server_multiraft).
# 取值: RS_F | RS_3F | LRC
ENCODING="${ENCODING:-LRC}"

case "$ENCODING" in
    RS_F|RS_3F|LRC)
        ;;
    *)
        echo "ERROR: ENCODING must be one of RS_F | RS_3F | LRC (got '$ENCODING')"
        exit 1
        ;;
esac

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
LOG_DIR="${BUILD_DIR}/log"

echo "========================================"
echo "Stopping all running nodes..."
echo "========================================"
pkill -9 -f bench_server_multiraft 2>/dev/null || true

# 等待进程退出
sleep 3

# 强制清理占用端口的进程
echo "Force cleaning up ports..."
for i in {10..16}; do
    fuser -k 510$i/tcp 2>/dev/null || true
    fuser -k 600$i/tcp 2>/dev/null || true
done

# 额外等待，确保 TCP 连接完全关闭
sleep 5

mkdir -p "$LOG_DIR"
rm -f "$LOG_DIR"/node*.log
rm -rf /tmp/mr_n* /tmp/raft_log*

BINARY="${BUILD_DIR}/bench/bench_server_multiraft"
CONF="${SCRIPT_DIR}/conf/multi-raft-7-localtest.conf"

if [ ! -x "$BINARY" ]; then
    echo "ERROR: Binary not found or not executable: $BINARY"
    echo "Hint: Run 'make -C build' first"
    exit 1
fi

if [ ! -f "$CONF" ]; then
    echo "ERROR: Config file not found: $CONF"
    exit 1
fi

# Ignore HUP signal for this shell script (protects child processes)
trap '' HUP

echo ""
echo "========================================"
echo "Starting 7 Multi-Raft nodes (encoding=${ENCODING}, concurrent)..."
echo "========================================"

# 并发启动所有节点，peer_threads=5
for i in "${NODES[@]}"; do
    LOG_FILE="${LOG_DIR}/node${i}.log"
    # Start in background with setsid to create new session
    setsid "$BINARY" --conf "$CONF" --id "$i" --peer_threads=5 \
              --encoding="$ENCODING" > "$LOG_FILE" 2>&1 &
    disown $!
    echo "Node $i started (PID: $!, encoding=${ENCODING})"
done

echo ""
echo "========================================"
echo "Waiting for all nodes to be ready..."
echo "========================================"

# 轮询等待所有节点就绪
elapsed=0
while [ $elapsed -lt $MAX_WAIT ]; do
    running=0
    listening=0

    for i in "${NODES[@]}"; do
        if pgrep -f "bench_server_multiraft.*--id $i" > /dev/null 2>&1; then
            ((running++)) || true
        fi

        kv_port=$((60010 + i))
        if ss -tlnp 2>/dev/null | grep -q ":${kv_port}"; then
            ((listening++)) || true
        fi
    done

    printf "\r  Elapsed: %3ds | Running: %d/7 | KV Ports listening: %d/7" \
        "$elapsed" "$running" "$listening"

    if [ "$running" -eq 7 ] && [ "$listening" -eq 7 ]; then
        echo ""
        echo ""
        echo "All nodes ready!"
        break
    fi

    sleep $CHECK_INTERVAL
    elapsed=$((elapsed + CHECK_INTERVAL))
done

echo ""

if [ $elapsed -ge $MAX_WAIT ]; then
    echo "WARNING: Timeout waiting for all nodes (${MAX_WAIT}s)"
fi

echo ""
echo "========================================"
echo "Final Status:"
echo "========================================"

crash_count=0
for i in "${NODES[@]}"; do
    kv_port=$((60010 + i))
    raft_port=$((51010 + i))
    if pgrep -f "bench_server_multiraft.*--id $i" > /dev/null 2>&1; then
        if ss -tlnp 2>/dev/null | grep -q ":${kv_port}"; then
            echo "  Node $i: RUNNING (KV=$kv_port OK)"
        else
            echo "  Node $i: RUNNING but KV port $kv_port NOT listening"
            ((crash_count++)) || true
        fi
    else
        echo "  Node $i: STOPPED (crashed!)"
        ((crash_count++)) || true
    fi
done

echo ""
if [ $crash_count -eq 0 ]; then
    echo "SUCCESS: All 7 nodes started and ready! (encoding=${ENCODING})"
    exit 0
else
    echo "WARNING: $crash_count node(s) failed to start. (encoding=${ENCODING})"
    exit 1
fi
