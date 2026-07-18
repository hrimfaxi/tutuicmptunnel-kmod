# 在 OpenWrt 上运行 tutuicmptunnel-kmod 客户端

[English](./openwrt.md) | [简体中文](./openwrt_zh-CN.md)

---

`tutuicmptunnel-kmod` 可以在标准的 OpenWrt 固件上运行。在 OpenWrt 设备上部署客户端有两条路线：

| 路线 | 说明 | 适合人群 |
| :--- | :--- | :--- |
| **方案一：SDK 打包（推荐）** | 使用 [openwrt-tutuicmptunnel-kmod](https://github.com/hrimfaxi/openwrt-tutuicmptunnel-kmod) 打包项目，基于 OpenWrt SDK 直接构建出 `.ipk` / `.apk` 安装包，用系统的包管理器安装、卸载、升级 | 绝大多数用户 |
| **方案二：手工交叉编译（本文）** | 手工配置工具链，逐个编译 libsodium、libmnl、主程序与内核模块，再 `scp` 到设备 | 需要修改源码、调试，或你的目标平台还没有打包配置 |

> [!TIP]
> 如果你只是想在路由器上用起来，请直接走方案一——打包项目已经处理了依赖、安装路径和开机自启等细节。本文介绍的是方案二，适合需要深入控制编译过程的开发者。

## 方案二：手工交叉编译

整体流程如下：

```mermaid
flowchart LR
    A[下载 OpenWrt SDK<br/>匹配路由器型号] --> B[交叉编译<br/>libsodium / libmnl<br/>tutuicmptunnel-kmod / kmod]
    B --> C[scp 复制到路由器<br/>tuctl* / ktuctl / .so / .ko]
    C --> D[加载内核模块<br/>并设置开机自启]
```

### 环境准备

下载与你的路由器型号对应的 OpenWrt SDK。以小米 AX3000T 为例，在[固件下载页面](https://downloads.openwrt.org/releases/24.10.5/targets/mediatek/filogic/)下方找到：

* [openwrt-sdk-24.10.5-mediatek-filogic_gcc-13.3.0_musl.Linux-x86_64.tar.zst](https://downloads.openwrt.org/releases/24.10.5/targets/mediatek/filogic/openwrt-sdk-24.10.5-mediatek-filogic_gcc-13.3.0_musl.Linux-x86_64.tar.zst)

下载后解压到 `$HOME/temp` 目录。

> [!IMPORTANT]
> SDK 必须与你设备的架构和固件版本严格对应，否则编译出的内核模块无法加载。

以下以 `aarch64` 架构的 OpenWrt 为例演示手工交叉编译。

### 1. 设置编译环境

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

确认输出的 `TOOLCHAIN`、`TARGETROOT`、`KERNEL_DIR` 路径均有效后再继续。

### 2. 编译依赖库

```bash
# 编译 libsodium
git clone https://github.com/jedisct1/libsodium.git
cd libsodium
git checkout $LIBSODIUM_TAG # 选择一个最新的稳定版发布
./autogen.sh
./configure --host=$ARCH-openwrt-linux \
  CC=${TOOLCHAIN}/bin/$ARCH-openwrt-linux-gcc
make -j $(nproc)

# 编译 libmnl
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

### 3. 编译 tutuicmptunnel-kmod 与内核模块

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

# 编译内核模块
make \
  KSRC=$KERNEL_DIR \
  CROSS_COMPILE=${TOOLCHAIN}/bin/$ARCH-openwrt-linux- \
  ARCH=$KERNEL_ARCH \
  -C kmod
```

## 部署到设备

将编译产物复制到 OpenWrt 设备：

```bash
scp stripped/usr/local/bin/tuctl* router:/usr/bin/
scp stripped/usr/local/sbin/ktuctl router:/usr/sbin/
scp ../libsodium/src/libsodium/.libs/libsodium.so router:/usr/lib/
scp kmod/tutuicmptunnel.ko router:/lib/modules/6.6.119/
```

> [!NOTE]
> `lib/modules/6.6.119/` 中的内核版本号需与设备实际运行的内核一致，可用 `uname -r` 在设备上确认。

通过 `ssh` 登录设备，加载模块并设置开机自启：

```bash
echo tutuicmptunnel > /etc/modules.d/99-tutuicmptunnel
modprobe tutuicmptunnel
```

此时即可在 OpenWrt 设备上使用 `tuctl`、`tuctl_client`、`ktuctl` 等程序。

## 降低 tuctl_client 内存占用

路由器等内存受限设备上，可以通过环境变量 `TUTUICMPTUNNEL_PWHASH_MEMLIMIT`（单位：字节）降低密码哈希的内存占用。

**服务端**（systemd 单元文件）：

```ini
Environment=TUTUICMPTUNNEL_PWHASH_MEMLIMIT=1048576
```

**客户端**：

```bash
TUTUICMPTUNNEL_PWHASH_MEMLIMIT=1048576 tuctl_client ...
```

> [!WARNING]
> * 服务端与客户端必须使用**相同的数值**，否则握手失败、无法建立通信。
> * 数值越小内存占用越低，但安全性也随之下降，请根据设备能力和风险评估自行权衡。
