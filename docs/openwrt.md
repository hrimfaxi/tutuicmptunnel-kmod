# Running tutuicmptunnel-kmod Client on OpenWrt

[English](./openwrt.md) | [简体中文](./openwrt_zh-CN.md)

---

`tutuicmptunnel-kmod` can run on standard OpenWrt firmware. There are two approaches for deploying the client on OpenWrt devices:

| Approach | Description | Target Audience |
| :--- | :--- | :--- |
| **Option 1: SDK Packaging (Recommended)** | Use the [openwrt-tutuicmptunnel-kmod](https://github.com/hrimfaxi/openwrt-tutuicmptunnel-kmod) packaging project to build `.ipk` / `.apk` packages based on OpenWrt SDK, install/upgrade/uninstall with system package manager | Most users |
| **Option 2: Manual Cross-compilation (This document)** | Manually configure toolchain, compile libsodium, libmnl, main program and kernel module one by one, then `scp` to device | Need to modify source code, debug, or your target platform has no packaging configuration yet |

> [!TIP]
> If you just want to use it on your router, please go with Option 1 — the packaging project already handles dependencies, installation paths, and auto-start on boot. This document describes Option 2, suitable for developers who need deep control over the compilation process.

## Option 2: Manual Cross-compilation

The overall process is as follows:

```mermaid
flowchart LR
    A[Download OpenWrt SDK<br/>Match router model] --> B[Cross-compile<br/>libsodium / libmnl<br/>tutuicmptunnel-kmod / kmod]
    B --> C[scp to router<br/>tuctl* / ktuctl / .so / .ko]
    C[D[Load kernel module<br/>and set auto-start]
```

### Environment Preparation

Download the OpenWrt SDK corresponding to your router model. Taking Xiaomi AX3000T as an example, find on the [firmware download page](https://downloads.openwrt.org/releases/24.10.5/targets/mediatek/filogic/):

* [openwrt-sdk-24.10.5-mediatek-filogic_gcc-13.3.0_musl.Linux-x86_64.tar.zst](https://downloads.openwrt.org/releases/24.10.5/targets/mediatek/filogic/openwrt-sdk-24.10.5-mediatek-filogic_gcc-13.3.0_musl.Linux-x86_64.tar.zst)

After downloading, extract to `$HOME/temp` directory.

> [!IMPORTANT]
> The SDK must strictly match your device's architecture and firmware version, otherwise the compiled kernel module cannot be loaded.

The following demonstrates manual cross-compilation using the `aarch64` architecture OpenWrt as an example.

### 1. Set Up Build Environment

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
```

Confirm that the output paths for `TOOLCHAIN`, `TARGETROOT`, and `KERNEL_DIR` are valid before continuing.

### 2. Compile Dependencies

```bash
# Compile libsodium
git clone https://github.com/jedisct1/libsodium.git
cd libsodium
git checkout $LIBSODIUM_TAG # Choose a latest stable release
./autogen.sh
./configure --host=$ARCH-openwrt-linux \
  CC=${TOOLCHAIN}/bin/$ARCH-openwrt-linux-gcc
make -j $(nproc)

# Compile libmnl
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
```

### 3. Compile tutuicmptunnel-kmod and Kernel Module

```bash
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

## Deploy to Device

Copy the compiled artifacts to the OpenWrt device:

```bash
scp stripped/usr/local/bin/tuctl* router:/usr/bin/
scp stripped/usr/local/sbin/ktuctl router:/usr/sbin/
scp ../libsodium/src/libsodium/.libs/libsodium.so router:/usr/lib/
scp kmod/tutuicmptunnel.ko router:/lib/modules/6.6.119/
```

> [!NOTE]
> The kernel version in `lib/modules/6.6.119/` must match the actual running kernel on the device. Use `uname -r` on the device to confirm.

Log in to the device via `ssh`, load the module and set up auto-start:

```bash
echo tutuicmptunnel > /etc/modules.d/99-tutuicmptunnel
modprobe tutuicmptunnel
```

At this point, you can use `tuctl`, `tuctl_client`, `ktuctl` and other programs on the OpenWrt device.

## Reduce tuctl_client Memory Usage

On memory-constrained devices like routers, you can reduce password hashing memory usage via the environment variable `TUTUICMPTUNNEL_PWHASH_MEMLIMIT` (unit: bytes).

**Server** (systemd unit file):

```ini
Environment=TUTUICMPTUNNEL_PWHASH_MEMLIMIT=1024768
```

**Client**:

```bash
TUTUICMPTUNNEL_PWHASH_MEMLIMIT=1024768 tuctl_client ...
```

> [!WARNING]
> * Server and client must use **the same value**, otherwise handshake fails and communication cannot be established.
> * Smaller values mean lower memory usage but also lower security. Please balance according to device capability and risk assessment.