#!/bin/bash

# 配置信息
BOARDS=("172.30.14.188" "172.30.14.42" "172.30.15.179" "172.30.15.117" "172.30.15.211" "172.30.15.19" "172.30.14.101")
USER="orangepi"
PASSWORD="orangepi"

echo "🛑 开始停止 Flexraft 集群容器..."

for id in "${!BOARDS[@]}"; do
    IP=${BOARDS[$id]}
    CONTAINER_NAME="flexraft-node-$id"

    echo "正在停止节点 $id | IP: $IP ..."
    
    # 停止容器，--rm 会自动删除它
    sshpass -p "$PASSWORD" ssh -o StrictHostKeyChecking=no "${USER}@${IP}" \
        "docker stop $CONTAINER_NAME 2>/dev/null || true"
done

echo "✅ 所有节点已停止并清理。"