#!/bin/bash

# 配置信息
BOARDS=("172.30.14.188" "172.30.14.42" "172.30.15.179" "172.30.15.117" "172.30.15.211" "172.30.15.19" "172.30.14.101")
USER="orangepi"
PASSWORD="orangepi"
CONF_FILE="/home/orangepi/gwy/raft_cluster-5.conf"
IMAGE="flexraft:arm64"

echo "🚀 开始批量启动 Flexraft 集群..."

for id in "${!BOARDS[@]}"; do
    IP=${BOARDS[$id]}
    CONTAINER_NAME="flexraft-node-$id"
    
    echo "----------------------------------------------------"
    echo "正在启动节点 $id | IP: $IP"

    # 执行远程 Docker 命令
    # 使用 -d 替换 -it 以便脚本能继续执行，同时添加 --name 方便管理
    sshpass -p "$PASSWORD" ssh -o StrictHostKeyChecking=no "${USER}@${IP}" \
        "docker run -d --rm \
            --name $CONTAINER_NAME \
            --network host \
            -v /home/orangepi/gwy/data/raft_log${id}:/tmp/raft_log${id} \
            -v /home/orangepi/gwy/data/testdb${id}:/tmp/testdb${id} \
            -v $CONF_FILE:/app/raft_cluster-5.conf \
            $IMAGE \
            /bin/bash -c './bench/bench_server --conf=./raft_cluster-5.conf --id=$id'"

    if [ $? -eq 0 ]; then
        echo "✅ 节点 $id 启动命令已发送"
    else
        echo "❌ 节点 $id 启动失败，请检查网络或镜像"
    fi
done

echo "----------------------------------------------------"
echo "🎉 所有节点启动指令执行完毕！"