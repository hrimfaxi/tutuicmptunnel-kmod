# OpenWrt Client Guide

[English](./openwrt.md) | [简体中文](./openwrt_zh-CN.md)

---

## Overview

`tutuicmptunnel-kmod` can run on standard `OpenWrt` firmware.
This guide will detail how to cross-compile the `tutuicmptunnel-kmod` project to successfully run in client mode on an `OpenWrt` device.

## Environment Preparation

You need to download the `openwrt-sdk` package corresponding to your router brand. For example, if you have a Xiaomi Router AX3000T, go to the
[Xiaomi AX3000T Firmware Download Page](https://downloads.openwrt.org/releases/24.10.5/targets/mediatek/filogic/)
and download the following files from the bottom of the page:
- [openwrt-sdk](https://downloads.openwrt.org/releases/24.10.5/targets/mediatek/filogic/openwrt-sdk-24.10.5-mediatek-filogic_gcc-13.3.0_musl.Linux-x86_64.tar.zst)

Then extract them to the `$HOME/temp` directory.

## Cross-Compilation

The following is the cross-compilation method for `OpenWrt` on `aarch64` architecture.

```bash
export STAGING_DIR=${HOME}/temp/openwrt-sdk-24.10.5-mediatek-filogic_gcc-13.3.0_musl.Linux-x86_64/staging_dir
export TOOLCHAIN_GLOB=$STAGING_DIR/toolchain-*
export TARGETROOT_GLOB=$STAGING_DIR/target-*
export TOOLCHAIN=( $TOOLCHAIN_GLOB )
export TARGETROOT=( $TARGETROOT_GLOB )
export KERNEL_DIR_GLOB=$STAGING_DIR/../build_dir/target-*/linux-*/linux-*
export KERNEL_DIR=( $KERNEL_DIR_GLOB )
export ARCH=aarch64
export CC=${TOOLCHAIN}/bin/$ARCH-openwrt-linux-gcc
export LIBSODIUM_TAG=1.0.20-RELEASE
export KERNEL_ARCH=arm64

echo ================================================================================
echo "TOOLCHAIN:  ${TOOLCHAIN}"
echo "TARGETROOT: ${TARGETROOT}"
echo "KERNEL_DIR:  ${KERNEL_DIR}"
echo "CC:  ${CC}"
echo ================================================================================

# Compile libsodium
git clone https://github.com/jedisct1/libsodium.git
cd libsodium
git checkout $LIBSODIUM_TAG # Choose a stable version
./autogen.sh
./configure --host=$ARCH-openwrt-linux \
  CC=${TOOLCHAIN}/bin/$ARCH-openwrt-linux-gcc
make -j $(nproc)

# Compile `libmnl`
cd ..
git clone https://github.com/justmirror/libmnl
cd libmnl
./autogen.sh
./configure \
  --host=$ARCH-openwrt-linux \
  --prefix=/usr \
  --exec-prefix=/usr \
  CC=${TOOLCHAIN}/bin/$ARCH-openwrt-linux-gcc \
  CFLAGS="--sysroot=${TARGETROOT} -O2 -pipe" \
  LDFLAGS="--sysroot=${TARGETROOT} -Wl,--gc-sections"

make V=1 clean all
make V=1 DESTDIR=${TARGETROOT} install

# Compile `tutuicmptunnel-kmod`
cd ..
git clone https://github.com/hrimfaxi/tutuicmptunnel-kmod.git
cd tutuicmptunnel-kmod
rm -f CMakeCache.txt

export STAGING_DIR_HOST="$STAGING_DIR/host"
export PKG_CONFIG_LIBDIR="$TARGETROOT/usr/lib/pkgconfig"
export PKG_CONFIG="$STAGING_DIR_HOST/bin/pkg-config"
export PATH="$STAGING_DIR_HOST/bin:$PATH"

cmake \
  -DCMAKE_TOOLCHAIN_FILE=$(pwd)/toolchains/openwrt-$ARCH.cmake \
  -DBISON_EXECUTABLE=/usr/bin/bison \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_HARDEN_MODE=ON \
  -DSODIUM_INCLUDE_DIR=$(pwd)/../libsodium/src/libsodium/include -DSODIUM_LIBRARY=$(pwd)/../libsodium/src/libsodium/.libs/libsodium.so \
  .
make VERBOSE=1 clean all
make install DESTDIR=stripped

# Compile kernel module
make \
  KSRC=$KERNEL_DIR \
  CROSS_COMPILE=${TOOLCHAIN}/bin/$ARCH-openwrt-linux- \
  ARCH=$KERNEL_ARCH \
  -C kmod
```


Finally, copy the libraries to the `OpenWrt` device:

```
scp stripped/usr/local/bin/tuctl* router:/usr/bin/
scp stripped/usr/local/sbin/ktuctl router:/usr/sbin/
scp ../libsodium/src/libsodium/.libs/libsodium.so router:/usr/lib/
scp kmod/tutuicmptunnel.ko router:/lib/modules/6.6.119/
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
