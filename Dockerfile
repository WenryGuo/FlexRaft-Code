##############################
# Stage 1: Builder (交叉编译)
##############################
FROM ubuntu:22.04 AS builder
# 换镜像源不设置代理
# ARG http_proxy
# ARG https_proxy

# 禁止交互式配置，Ubuntu、Debian系统的特殊变量，适合自动化构建环境中使用
ENV DEBIAN_FRONTEND=noninteractive
# 安装系统依赖/安装系统级工具 正则替换为中国科学技术大学的镜像源
# RUN sed -i 's@http://.*.ubuntu.com@http://mirrors.ustc.edu.cn@' /etc/apt/sources.list && \
#     apt-get update && \
#     apt-get install -y --no-install-recommends \
#         build-essential cmake g++ python3 python3-pip git wget ca-certificates && \
#     rm -rf /var/lib/apt/lists/*

RUN sed -i 's@http://.*.ubuntu.com@http://mirrors.ustc.edu.cn@' /etc/apt/sources.list && \
    apt-get update && \
    apt-get install -y --no-install-recommends \
        build-essential cmake g++ python3 python3-pip git wget ca-certificates \
        libgflags-dev libsnappy-dev zlib1g-dev libbz2-dev liblz4-dev libzstd-dev \
        autoconf automake libtool pkg-config  nasm yasm && \
    rm -rf /var/lib/apt/lists/*

# ===== ARM64 CROSS TOOLCHAIN (添加交叉编译工具链) =====
RUN apt-get update && apt-get install -y --no-install-recommends \
    gcc-aarch64-linux-gnu g++-aarch64-linux-gnu

WORKDIR /tmp

# 一次性装好所有项目依赖，复制本地脚本到容器，最后执行
# 先手动安装gflags，交叉编译
COPY gflags /tmp/gflags
RUN cd /tmp/gflags && \
    cmake -B build \
    -DCMAKE_SYSTEM_NAME=Linux \
    -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
    -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc \
    -DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++ \
    -DCMAKE_CXX_FLAGS="-fPIC" \
    -DCMAKE_INSTALL_PREFIX=/usr/aarch64-linux-gnu \
    -DBUILD_GMOCK=OFF \
    -DBUILD_GTESR=ON &&\
    cmake --build build -j$(nproc)&& \
    cd build && \
    make install && \
    rm -rf /tmp/gflags
# 先手动安装googletest，交叉编译
COPY googletest /tmp/googletest
RUN cd /tmp/googletest && \
    cmake -B build \
    -DCMAKE_SYSTEM_NAME=Linux \
    -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
    -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc \
    -DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++ \
    -DCMAKE_INSTALL_PREFIX=/usr/aarch64-linux-gnu \
    -DBUILD_GMOCK=OFF \
    -DBUILD_GTESR=ON &&\
    cmake --build build -j$(nproc)&& \
    cd build && \
    make install && \
    rm -rf /tmp/googletest

COPY scripts/install_dependencies.py /tmp/
RUN python3 /tmp/install_dependencies.py
# 再手动安装rocksdb，交叉编译；脚本先安装它的依赖项
COPY rocksdb /tmp/rocksdb
RUN cd /tmp/rocksdb && \
    cmake -B build \
    -DCMAKE_SYSTEM_NAME=Linux \
    -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
    -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc \
    -DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++ \
    -DCMAKE_INSTALL_PREFIX=/usr/aarch64-linux-gnu \
    -DWITH_TESTS=OFF \
    -DCMAKE_CXX_FLAGS="-Wno-error -Wno-error=maybe-uninitialized" && \
    cmake --build build -j$(nproc) && \
    cd build && \
    make install && \
    rm -rf /tmp/rocksdb
# 再手动安装 isa-l 源码，交叉编译；复制到容器
COPY isa-l /tmp/isa-l
# 编译并安装 isa-l
RUN cd /tmp/isa-l && \
    ./autogen.sh && \
    CC=aarch64-linux-gnu-gcc \
    ./configure \
        --host=aarch64-linux-gnu \ 
        --prefix=/usr/aarch64-linux-gnu --libdir=/usr/aarch64-linux-gnu/lib  && \
    make -j$(nproc) && \
    make install && \
    ldconfig && \
    rm -rf /tmp/isa-l

# raft/CmakeLists.txt需要library uuid
RUN apt-get update && apt-get install -y uuid-dev\
    libprotobuf-dev protobuf-compiler libgoogle-glog-dev


# 复制项目源码到容器的/flexraft目录中
WORKDIR /flexraft
COPY . .
RUN rm -rf build &&\
    cmake -B build \
    -DCMAKE_SYSTEM_NAME=Linux \
    -DREPLICATION_MODE=FULL \
    -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc \
    -DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++ \
    -DCMAKE_PREFIX_PATH=/usr/aarch64-linux-gnu && \
    cmake --build build -j$(nproc)

# # 测试，看看kv有什么问题
# RUN mkdir -p build && cd build \
#     && cmake .. -DCMAKE_BUILD_TYPE=Release -Wno-deprecated-declarations\
#     && make kv -j$(nproc)

# RUN cp /tmp/kv_build.log /host/

# # 编译
# RUN mkdir build && cd build \
#  && cmake .. -DCMAKE_BUILD_TYPE=Release \
#  && make -j$(nproc)
##############################
# Stage 2: Runtime 镜像(极小)
##############################
FROM ubuntu:22.04 AS runtime

RUN apt-get update && apt-get install -y --no-install-recommends \
    libsnappy1v5 zlib1g libbz2-1.0 liblz4-1 libzstd1 && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /app

# 只复制运行需要的动态库
COPY --from=builder /usr/aarch64-linux-gnu /usr/aarch64-linux-gnu

# 复制 FlexRaft 可执行文件
COPY --from=builder /flexraft/build/bench /app/bench
COPY --from=builder /flexraft/build/raft /app/raft
COPY --from=builder /flexraft/build/kv /app/kv

ENV LD_LIBRARY_PATH="/usr/aarch64-linux-gnu/lib"

CMD ["/bin/bash"]
# # 设置环境变量与默认命令
# ENV PATH="/flexraft/build/bench:$PATH"
# CMD ["bash"]