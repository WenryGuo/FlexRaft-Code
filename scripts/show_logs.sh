#!/bin/bash

# 配置信息
BOARDS=("172.30.14.188" "172.30.14.42" "172.30.15.179" "172.30.15.117" "172.30.15.211" "172.30.15.19" "172.30.14.101")
USER="orangepi"
PASSWORD="orangepi"

echo "==============================================================="
echo "🔍 正在扫描 Flexraft 集群节点日志 (最后 15 行)..."
echo "==============================================================="

for id in "${!BOARDS[@]}"; do
    IP=${BOARDS[$id]}
    CONTAINER_NAME="flexraft-node-$id"

    # 使用颜色高亮 ID 和 IP
    echo -e "\n\033[1;33m[Node $id | $IP]\033[0m ------------------------------------"
    
    # 远程获取日志，如果不在线会显示错误
    sshpass -p "$PASSWORD" ssh -o StrictHostKeyChecking=no -o ConnectTimeout=2 "${USER}@${IP}" \
        "docker logs --tail 15 $CONTAINER_NAME 2>&1" || echo -e "\033[1;31m❌ 无法连接到板子或容器未运行\033[0m"
done

echo -e "\n==============================================================="
echo "✅ 巡检结束"