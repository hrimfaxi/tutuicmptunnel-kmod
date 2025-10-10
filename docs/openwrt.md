# openwrt客户端指南

## 概述

`tutuicmptunnel-bpf` 可以在标准的 `OpenWrt` 固件上运行。

本指南将详细介绍如何通过自定义 `OpenWrt` 固件编译，并交叉编译 `tutuicmptunnel-bpf` 项目，使其在您的 `OpenWrt` 设备上成功运行客户端模式。

## 环境准备

需要下载路由器品牌对应的`openwrt-sdk`和`openwrt-toolchain`包，比如你是小米路由器ax3000t，就到
[小米ax3000t固件下载页面](https://downloads.openwrt.org/releases/24.10.3/targets/mediatek/filogic/)
页面下方下载：
- [openwrt-sdk](https://downloads.openwrt.org/releases/24.10.3/targets/mediatek/filogic/openwrt-sdk-24.10.3-mediatek-filogic_gcc-13.3.0_musl.Linux-x86_64.tar.zst)
- [openwrt-toolchain](https://downloads.openwrt.org/releases/24.10.3/targets/mediatek/filogic/openwrt-toolchain-24.10.3-mediatek-filogic_gcc-13.3.0_musl.Linux-x86_64.tar.zst)

然后解压到`~/temp`目录。

## 交叉编译

以下是在`aarch64`架构的`openwrt`下的交叉编译方法。

首先需要构建`libsodium`:

```bash
export STAGING_DIR=${HOME}/temp/openwrt-sdk-24.10.3-mediatek-filogic_gcc-13.3.0_musl.Linux-x86_64/staging_dir
git clone https://github.com/jedisct1/libsodium.git
cd libsodium
git checkout # 选择一个最新的稳定版发布
./autogen.sh
./configure --host=aarch64-openwrt-linux \
  CC=$STAGING_DIR/toolchain-aarch64_cortex-a53_gcc-13.3.0_musl/bin/aarch64-openwrt-linux-gcc
make -j $(nproc)
```

```bash
cd ..
git clone https://github.com/hrimfaxi/tutuicmptunnel-kmod.git
cd tutuicmptunnel-kmod
rm -f CMakeCache.txt
cmake -DENABLE_HARDEN_MODE=1 \
      -DCMAKE_TOOLCHAIN_FILE=$(pwd)/toolchains/openwrt-aarch64.cmake \
      -DSODIUM_INCLUDE_DIR=$(pwd)/../libsodium/src/libsodium/include \
      -DSODIUM_LIBRARY=$(pwd)/../libsodium/src/libsodium/.libs/libsodium.so \
      .
make VERBOSE=1 clean all
make install/strip DESTDIR=stripped

# 编译内核模块（示例内核源码路径请替换）
make \
     KSRC=$STAGING_DIR/../build_dir/target-aarch64_cortex-a53_musl/linux-mediatek_filogic/linux-6.6.104 \
     CROSS_COMPILE=$STAGING_DIR/toolchain-aarch64_cortex-a53_gcc-13.3.0_musl/bin/aarch64-openwrt-linux- \
     ARCH=arm64 \
     -C kmod
```

最后复制库到`openwrt`设备：

```
scp stripped/usr/local/bin/tuctl* router:/usr/bin/
scp stripped/usr/local/sbin/ktuctl router:/usr/sbin/
scp ../libsodium/src/libsodium/.libs/libsodium.so router:/usr/lib/
scp kmod/tutuicmptunnel.ko router:/lib/modules/6.6.104/
```

`ssh`到`openwrt`设备，设置开机启动模块:

```
echo tutuicmptunnel > /etc/modules.d/99-tutuicmptunnel
modprobe tutuicmptunnel
```

此时就可以在`openwrt`设备上使用`tuctl`，`tuctl_client`，`ktuctl`等程序。

## tuctl_client内存使用量问题

在内存受限的设备上，可通过降低密码哈希的内存占用来减少`tuctl_client`的内存使用。
设置环境变量`TUTUICMPTUNNEL_PWHASH_MEMLIMIT`（单位：字节）即可实现。

示例

    服务器（单元文件）：
    Environment=TUTUICMPTUNNEL_PWHASH_MEMLIMIT=1024768

    客户端：
    TUTUICMPTUNNEL_PWHASH_MEMLIMIT=1024768 tuctl_client ...

要点

    服务器与客户端必须使用相同的数值，否则将无法建立通信。
    该参数数值越小，内存占用越低，但安全性也会相应下降，请根据设备能力和风险评估进行权衡。
    服务器与客户端的TUTUICMPTUNNEL_PWHASH_MEMLIMIT必须一致；不一致将导致握手失败、无法通信。
