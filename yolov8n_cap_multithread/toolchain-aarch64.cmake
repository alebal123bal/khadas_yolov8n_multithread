set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER  aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)

# Point pkg-config at the arm64 multiarch sysroot for cross-compilation
set(ENV{PKG_CONFIG_LIBDIR} /usr/lib/aarch64-linux-gnu/pkgconfig)
set(ENV{PKG_CONFIG_PATH}   /usr/lib/aarch64-linux-gnu/pkgconfig)

# Search for programs in the build host directories only
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)

# Search for libraries and headers in the target sysroot
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
