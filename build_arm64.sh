#!/bin/bash
set -e
BUILD_DIR=build_arm64

export CC=aarch64-linux-gnu-gcc
export CXX=aarch64-linux-gnu-g++ 
export AR=aarch64-linux-gnu-ar
export AS=aarch64-linux-gnu-as
export LD=aarch64-linux-gnu-ld
export STRIP=aarch64-linux-gnu-strip

ARM64_SYSROOT=/usr/aarch64-linux-gnu

rm -rf $BUILD_DIR
mkdir -p $BUILD_DIR
cd $BUILD_DIR

cmake .. \
  -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
  -DCMAKE_SYSTEM_NAME=Linux \
  -DCMAKE_C_COMPILER=${CC} \
  -DCMAKE_CXX_COMPILER=${CXX} \
  -DCMAKE_INSTALL_PREFIX=$ARM64_SYSROOT \
  -DCMAKE_PREFIX_PATH="$ARM64_SYSROOT" \
  -DCMAKE_LIBRARY_PATH="$ARM64_SYSROOT/lib" \
  -DCMAKE_INCLUDE_PATH="$ARM64_SYSROOT/include" \
  -DGTEST_ROOT="$ARM64_SYSROOT" \
  -DROCKSDB_ROOT="$ARM64_SYSROOT" \
  -DCMAKE_BUILD_TYPE=Release

make -j$(nproc)