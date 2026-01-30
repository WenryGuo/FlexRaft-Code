#!/bin/bash
set -e
# 板子id
# BOARDS=("172.30.14.188" "172.30.14.42" "172.30.15.179" "172.30.15.117" "172.30.15.211" "172.30.15.19")

BOARDS=("172.30.14.101")
USER="orangepi"
PASSWORD="orangepi"
LOCAL_FILE="../flexraft_arm64_image.tar"
REMOTE_DIR="/home/orangepi/gwy"

for IP in "${BOARDS[@]}"; do
    echo "==================== 正在处理：$IP ===================="

    # 1. 创建目录
    sshpass -p "$PASSWORD" ssh -o StrictHostKeyChecking=no ${USER}@${IP} \
        "sudo mkdir -p ${REMOTE_DIR} && sudo chown ${USER}:${USER} ${REMOTE_DIR}"
    echo "  目录准备完成"

    # 2. 传输镜像
    sshpass -p "$PASSWORD" scp -o StrictHostKeyChecking=no \
        "$LOCAL_FILE" ${USER}@${IP}:${REMOTE_DIR}/
    echo "  镜像文件传输完成"

    # 3. 加载 Docker 镜像
    sshpass -p "$PASSWORD" ssh -o StrictHostKeyChecking=no ${USER}@${IP} << EOF
        set -e
        cd ${REMOTE_DIR}
        sudo docker load -i flexraft_arm64_image.tar
        sudo docker images | grep flexraft
EOF

    echo "  Docker 镜像加载完成"
done

echo "========================================"
echo "✅批量操作结束"
