#!/usr/bin/env bash
# Downloads the third-party SDK files required to cross-compile the project.
# Run once after cloning:  bash setup_sdk.sh
# Versions: librga v1.10.5_[8], librknnrt v2.3.2
set -e

SDK="$(cd "$(dirname "$0")" && pwd)"

echo "=== librga v1.10.5_[8] ==="
git clone --depth 1 https://github.com/airockchip/librga.git /tmp/librga
mkdir -p "${SDK}/3rdparty/rga/lib/Linux"
cp -r /tmp/librga/include                "${SDK}/3rdparty/rga/"
cp -r /tmp/librga/libs/Linux/gcc-aarch64 "${SDK}/3rdparty/rga/lib/Linux/aarch64"
rm -rf /tmp/librga

echo "=== librknnrt v2.3.2 ==="
git clone --depth 1 --branch v2.3.2 \
    --filter=blob:none --sparse \
    https://github.com/airockchip/rknn-toolkit2.git /tmp/rknn-toolkit2
cd /tmp/rknn-toolkit2
git sparse-checkout set rknpu2/runtime/Linux/librknn_api
cd -
cp -r /tmp/rknn-toolkit2/rknpu2/runtime/Linux/librknn_api "${SDK}/runtime/"
rm -rf /tmp/rknn-toolkit2

echo "=== SDK setup complete ==="
