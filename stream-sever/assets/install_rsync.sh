#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

BUILD_DIR="${AIPC_BUILD_DIR:-build/Debug}"
INSTALL_DIR="${AIPC_INSTALL_DIR:-${BUILD_DIR}/install}"
SYSROOT="${AIPC_SYSROOT:-/home/xikao/Luckfox/luckfox-pico/sysdrv/source/buildroot/buildroot-2023.02.6/output/host/arm-buildroot-linux-uclibcgnueabihf/sysroot}"
REMOTE_HOST="${AIPC_REMOTE_HOST:-root@192.168.5.9}"
REMOTE_DIR="${AIPC_REMOTE_DIR:-/root/aipc}"
SKIP_FRONTEND_BUILD="${AIPC_SKIP_FRONTEND_BUILD:-0}"
KEEP_LIBDATACHANNEL="${AIPC_KEEP_LIBDATACHANNEL:-0}"

# 步骤 0: 构建前端
if [ "$SKIP_FRONTEND_BUILD" != "1" ]; then
    "${SCRIPT_DIR}/build_frontend.sh"
fi

# 第一步：执行 cmake install
echo "========================================="
echo "执行 cmake --install ${BUILD_DIR}..."
echo "========================================="
cmake --install "${BUILD_DIR}"

# 第一步半：删除 libdatachannel 相关文件（已手动部署到设备）
if [ "$KEEP_LIBDATACHANNEL" != "1" ]; then
    echo ""
    echo "清理 libdatachannel 的动态库和头文件（保留其他依赖库）..."
    rm -f "${INSTALL_DIR}/lib/libdatachannel.so"*
    rm -f "${INSTALL_DIR}/lib/libdatachannel.a"
    rm -rf "${INSTALL_DIR}/include"
    echo "清理完成"
fi

# 第二步：补充板端运行所需的系统动态库
echo ""
echo "复制 liblzma.so* 到安装目录..."
mkdir -p "${INSTALL_DIR}/lib"
cp -av "${SYSROOT}"/usr/lib/liblzma.so* "${INSTALL_DIR}/lib/"
echo "=== liblzma 复制完成 ==="
ls -la "${INSTALL_DIR}"/lib/liblzma*

# 第三步：使用 rsync 增量同步
echo ""
echo "========================================="
echo "开始增量同步到 ${REMOTE_HOST}:${REMOTE_DIR}..."
echo "========================================="
rsync -avz --delete "${INSTALL_DIR}/" "${REMOTE_HOST}:${REMOTE_DIR}/"

echo ""
echo "========================================="
echo "完成！文件已同步到远程主机 ${REMOTE_HOST}:${REMOTE_DIR}"
echo "========================================="
