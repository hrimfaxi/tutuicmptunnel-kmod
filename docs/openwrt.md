# openwrt客户端指南

## 概述

`tutuicmptunnel` 因其依赖 `eBPF` 和 `kprobes` 等高级内核特性，无法直接在标准的 `OpenWrt` 固件上运行。
标准的 `OpenWrt` 内核为了保持精简，默认裁剪了这些对于普通路由功能非必需的调试和追踪功能。

本指南将详细介绍如何通过自定义 `OpenWrt` 固件编译，并交叉编译 `tutuicmptunnel` 项目，使其在您的 `OpenWrt` 设备上成功运行客户端模式。

## 环境准备

在开始之前，请确保您已经拥有一个完整且可以正常工作的 `OpenWrt` 编译环境。如果您不熟悉，请先参考 `OpenWrt` 官方文档搭建编译环境。

    OpenWrt 源码: git clone https://github.com/openwrt/openwrt.git

## 内核配置

由于`openwrt`做了一定裁剪，所以`tutuicmptunnel`不能直接在`openwrt`上运行。
需要重新编译`openwrt`内核形成新的固件包，在`openwrt` `menuconfig`中添加或修改以下选项：

* `CONFIG_KERNEL_KPROBES=y`
* `CONFIG_KERNEL_KALLSYMS=y`
* `CONFIG_KERNEL_DEBUG_INFO=y`
* `CONFIG_KERNEL_DEBUG_INFO_BTF=y` (需要关闭`CONFIG_KERNEL_DEBUG_INFO_REDUCED`)
* `CONFIG_PACKAGE_kmod-sched-core=m`
* `CONFIG_PACKAGE_kmod-sched-bpf=m`

为了方便起见，建议你编译所有`openwrt`设备用过的`kmod`，或者添加选项：`CONFIG_ALL_KMOD=y`。

## 用户态工具

需要编译以下包用于查看`bpf`规则和检视`bpf`状态，它会自动依赖`libbpf`从而能让`tuctl`正常编译。

* `CONFIG_PACKAGE_tc-bpf`
* `CONFIG_PACKAGE_bpftool-full`

## 交叉编译

以下是在`aarch64`架构的`openwrt`下的交叉编译方法。

首先需要构建`libsodium`:

```bash
export STAGING_DIR=/path/to/openwrt-sdk/staging_dir
git clone https://github.com/jedisct1/libsodium.git
cd libsodium
git checkout # 选择一个最新的稳定版发布
./configure --host=aarch64-openwrt-linux \
  CC=$STAGING_DIR/toolchain-aarch64_cortex-a53_gcc-13.3.0_musl/bin/aarch64-openwrt-linux-gcc
make -j $(nproc)
```

```bash
cd ..
git clone https://github.com/hrimfaxi/tutuicmptunnel.git
rm -f CMakeCache.txt
cmake -DCMAKE_TOOLCHAIN_FILE=$(pwd)/toolchains/openwrt-aarch64.cmake \
      -DCMAKE_BUILD_TYPE=Release \
      -DUSE_SYSTEM_LIBBPF_BPFTOOL=1 \
      -DSODIUM_INCLUDE_DIR=$(pwd)/../libsodium/src/libsodium/include \
      -DSODIUM_LIBRARY=$(pwd)/../libsodium/src/libsodium/.libs/libsodium.so \
      .
make VERBOSE=1 ARCH=AARCH64 clean_bpf build_bpf
make VERBOSE=1 clean all
make install/strip DESTDIR=stripped

# 编译内核模块（示例内核源码路径请替换）
make \
     KSRC=/path/to/kernel/tree \
     CROSS_COMPILE=$STAGING_DIR/toolchain-aarch64_cortex-a53_gcc-13.3.0_musl/bin/aarch64-openwrt-linux- \
     ARCH=arm64 \
     -C tutu_csum_fixup
```

最后复制库到`openwrt`设备：

```
scp stripped/usr/local/bin/tuctl* router:/usr/bin/
scp stripped/usr/local/sbin/tuctl* router:/usr/sbin/
scp ../libsodium/src/libsodium/.libs/libsodium.so router:/usr/lib/
scp tutu_csum_fixup/tutu_csum_fixup.ko router:/lib/modules/内核版本号/
```

`ssh`到`openwrt`设备，设置开机启动模块:

```
echo tutu_csum_fixup > /etc/modules.d/99-tutu-csum-fixup
modprobe tutu_csum_fixup
```

此时就可以在`openwrt`设备上使用`tuctl`，`tuctl_client`等程序。

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
