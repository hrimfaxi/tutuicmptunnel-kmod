# 设置系统名称和架构
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR mipsel)

# 指定交叉编译工具链的路径
set(TOOLCHAIN_PATH $ENV{STAGING_DIR}/toolchain-mipsel_24kc_gcc-13.3.0_musl/bin)
# 设置交叉编译器
set(CMAKE_C_COMPILER ${TOOLCHAIN_PATH}/mipsel-openwrt-linux-musl-gcc)
set(CMAKE_STRIP ${TOOLCHAIN_PATH}/mipsel-openwrt-linux-musl-strip)
set(CMAKE_SYSROOT $ENV{STAGING_DIR}/target-mipsel_24kc_musl)
include_directories(${CMAKE_SYSROOT}/usr/include)
