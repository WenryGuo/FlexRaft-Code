#!/bin/bash

# 配置信息
BOARDS=("172.30.14.188" "172.30.14.42" "172.30.15.179" "172.30.15.117" "172.30.15.211")
USER="orangepi"
PASSWORD="orangepi"

echo "⚠️  开始清理 5 台板子的 Flexraft 测试数据..."

for id in "${!BOARDS[@]}"; do
    IP=${BOARDS[$id]}
    echo "----------------------------------------------------"
    echo "正在清理节点 $id | IP: $IP"

    # 执行远程清理逻辑：
    # 1. 强制强制删除容器（释放挂载占用）
    # 2. 删除对应的 raft_log 和 testdb 文件夹
    # 3. 重新创建空的 data 目录（可选，确保环境整洁）
    sshpass -p "$PASSWORD" ssh -o StrictHostKeyChecking=no "${USER}@${IP}" \
        "docker rm -f flexraft-node-$id 2>/dev/null || true; \
         sudo rm -rf /home/orangepi/gwy/data/raft_log${id} \
                     /home/orangepi/gwy/data/testdb${id}; \
         echo '✅ 数据已清除'"
done

echo "----------------------------------------------------"
echo "✨ 所有节点数据清理完毕，现在可以重新开始测试了。"