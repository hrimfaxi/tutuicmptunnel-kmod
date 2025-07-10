# 设置系统名称和架构
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} \
-fstrict-flex-arrays=3 \
-fstack-clash-protection -fstack-protector-strong \
-Wl,-z,nodlopen -Wl,-z,noexecstack \
-Wl,-z,relro -Wl,-z,now \
-Wl,--as-needed -Wl,--no-copy-dt-needed-entries \
-Werror -Wtrampolines -Wbidi-chars=any -fPIE -pie -mbranch-protection=standard -fno-delete-null-pointer-checks -fno-strict-overflow -fno-strict-aliasing -ftrivial-auto-var-init=zero \
-Werror=implicit -Werror=incompatible-pointer-types -Werror=int-conversion \
-Wl,-z,max-page-size=4096 -Wl,--gc-sections \
-fexceptions")

# 指定交叉编译工具链的路径
set(TOOLCHAIN_PATH $ENV{STAGING_DIR}/toolchain-aarch64_cortex-a53_gcc-13.3.0_musl/bin/)
# 设置交叉编译器
set(CMAKE_C_COMPILER ${TOOLCHAIN_PATH}/aarch64-openwrt-linux-musl-gcc)
set(CMAKE_STRIP ${TOOLCHAIN_PATH}/aarch64-openwrt-linux-musl-strip)
set(CMAKE_SYSROOT $ENV{STAGING_DIR}/target-aarch64_cortex-a53_musl)
include_directories(${CMAKE_SYSROOT}/usr/include)
