# 设置系统名称和架构
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# 指定交叉编译工具链的路径
set(TOOLCHAIN_PATH $ENV{STAGING_DIR}/toolchain-aarch64_cortex-a53_gcc-13.3.0_musl/bin/)
# 设置交叉编译器
set(CMAKE_C_COMPILER ${TOOLCHAIN_PATH}/aarch64-openwrt-linux-musl-gcc)
set(CMAKE_STRIP ${TOOLCHAIN_PATH}/aarch64-openwrt-linux-musl-strip)
set(CMAKE_SYSROOT $ENV{STAGING_DIR}/target-aarch64_cortex-a53_musl)
include_directories(${CMAKE_SYSROOT}/usr/include)
