#!/bin/bash
set -e
# 板子id
# BOARDS=("172.30.14.188" "172.30.14.42" "172.30.15.179" "172.30.15.117" "172.30.15.211" "172.30.15.19" "172.30.14.101")
BOARDS=("172.30.14.188" "172.30.14.42" "172.30.15.179" "172.30.15.117" "172.30.15.211" "172.30.15.19" "172.30.14.101")
# BOARDS=("172.30.15.117")
USER="orangepi"
PASSWORD="orangepi"
LOCAL_FILE="../conf/raft_cluster-7.conf"
REMOTE_DIR="/home/orangepi/gwy"

for IP in "${BOARDS[@]}"; do
    echo "==================== 复制配置文件到 $IP===================="
    sshpass -p "$PASSWORD" scp -o StrictHostKeyChecking=no \
        "$LOCAL_FILE" ${USER}@${IP}:${REMOTE_DIR}/
    echo "✅配置文件已复制到$IP"
done

echo "========================================"
echo "✅批量操作结束"