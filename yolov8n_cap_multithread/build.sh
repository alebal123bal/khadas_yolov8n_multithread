set -e

GCC_COMPILER=aarch64-linux-gnu

export CC=${GCC_COMPILER}-gcc
export CXX=${GCC_COMPILER}-g++

ROOT_PWD=$( cd "$( dirname $0 )" && cd -P "$( dirname "$SOURCE" )" && pwd )

# Point pkg-config to the arm64 sysroot so CMake finds the cross-compiled GStreamer
export PKG_CONFIG_LIBDIR=/usr/lib/aarch64-linux-gnu/pkgconfig
export PKG_CONFIG_PATH=/usr/lib/aarch64-linux-gnu/pkgconfig

# build
BUILD_DIR=${ROOT_PWD}/build/build_linux_aarch64

if [[ ! -d "${BUILD_DIR}" ]]; then
  mkdir -p ${BUILD_DIR}
fi

cd ${BUILD_DIR}
cmake ../.. \
  -DTARGET_NAME=yolov8n_cap_multithread \
  -DCMAKE_TOOLCHAIN_FILE=${ROOT_PWD}/toolchain-aarch64.cmake
make -j$(nproc)
make install
cd -
