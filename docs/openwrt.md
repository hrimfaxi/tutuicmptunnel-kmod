# OpenWrt Client Guide

[English](./openwrt.md) | [简体中文](./openwrt_zh-CN.md)

---

## Overview

`tutuicmptunnel-kmod` can run on standard `OpenWrt` firmware.
This guide will detail how to cross-compile the `tutuicmptunnel-kmod` project to successfully run in client mode on an `OpenWrt` device.

## Environment Preparation

You need to download the `openwrt-sdk` and `openwrt-toolchain` packages corresponding to your router brand. For example, if you have a Xiaomi Router AX3000T, go to the
[Xiaomi AX3000T Firmware Download Page](https://downloads.openwrt.org/releases/24.10.3/targets/mediatek/filogic/)
and download the following files from the bottom of the page:
- [openwrt-sdk](https://downloads.openwrt.org/releases/24.10.3/targets/mediatek/filogic/openwrt-sdk-24.10.3-mediatek-filogic_gcc-13.3.0_musl.Linux-x86_64.tar.zst)
- [openwrt-toolchain](https://downloads.openwrt.org/releases/24.10.3/targets/mediatek/filogic/openwrt-toolchain-24.10.3-mediatek-filogic_gcc-13.3.0_musl.Linux-x86_64.tar.zst)

Then extract them to the `~/temp` directory.

## Cross-Compilation

The following is the cross-compilation method for `OpenWrt` on `aarch64` architecture.

First, you need to build `libsodium`:

```bash
export STAGING_DIR=${HOME}/temp/openwrt-sdk-24.10.3-mediatek-filogic_gcc-13.3.0_musl.Linux-x86_64/staging_dir
git clone https://github.com/jedisct1/libsodium.git
cd libsodium
git checkout # Choose a latest stable release
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

# Compile the kernel module (replace the kernel source path with yours)
make \
     KSRC=$STAGING_DIR/../build_dir/target-aarch64_cortex-a53_musl/linux-mediatek_filogic/linux-6.6.104 \
     CROSS_COMPILE=$STAGING_DIR/toolchain-aarch64_cortex-a53_gcc-13.3.0_musl/bin/aarch64-openwrt-linux- \
     ARCH=arm64 \
     -C kmod
```

Finally, copy the libraries to the `OpenWrt` device:

```
scp stripped/usr/local/bin/tuctl* router:/usr/bin/
scp stripped/usr/local/sbin/ktuctl router:/usr/sbin/
scp ../libsodium/src/libsodium/.libs/libsodium.so router:/usr/lib/
scp kmod/tutuicmptunnel.ko router:/lib/modules/6.6.104/
```

`ssh` into the `OpenWrt` device and set the module to load on boot:

```
echo tutuicmptunnel > /etc/modules.d/99-tutuicmptunnel
modprobe tutuicmptunnel
```

Now you can use programs like `tuctl`, `tuctl_client`, `ktuctl`, etc., on the `OpenWrt` device.

## tuctl_client Memory Usage Issue

On memory-constrained devices, you can reduce the memory usage of `tuctl_client` by lowering the memory cost of password hashing.
Set the environment variable `TUTUICMPTUNNEL_PWHASH_MEMLIMIT` (unit: bytes) to achieve this.

Example

    Server (Unit File):
    Environment=TUTUICMPTUNNEL_PWHASH_MEMLIMIT=1024768

    Client:
    TUTUICMPTUNNEL_PWHASH_MEMLIMIT=1024768 tuctl_client ...

Key Points

> The server and client must use the same value, otherwise communication cannot be established. \
> The smaller the value of this parameter, the lower the memory usage, but security will also decrease accordingly; please weigh this based on device capabilities and risk assessment. \
> The `TUTUICMPTUNNEL_PWHASH_MEMLIMIT` on the server and client must be consistent; inconsistency will result in handshake failure and inability to communicate.
