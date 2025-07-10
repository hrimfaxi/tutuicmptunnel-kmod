# TUTU-ICMP-Tunnel (`tuctl`)
`TUTU-ICMP-Tunnel` 是一个 **UDP ↔ ICMP** 隧道方案：
客户端把所有 UDP 报文改写为 ICMP Echo；
服务器把 ICMP Echo 还原为 UDP。
数据路径全部在内核 eBPF 中完成，`tuctl` 只是控制器（加载/卸载 BPF、维护 BPF 映射等）。

---

## 目录
1. [项目特点](#项目特点)
2. [整体架构](#整体架构)
3. [运行要求](#运行要求)
4. [编译与安装](#编译与安装)
5. [快速上手](#快速上手)
6. [子命令参考](#子命令参考)
7. [脚本模式](#脚本模式)
8. [常见问题](#常见问题)
9. [许可证](#许可证)

---

## 项目特点
* 穿透防火墙：很多环境仅放行 ICMP Echo。
* 完全内核实现：隧道收发都在 TC-clsact 钩子内完成，无用户态转发损耗。
* 不依赖 Raw Socket：普通 UDP 应用无需修改即可使用。
* 高吞吐：零拷贝/零 wake-up，适合大规模并发。

---

## 整体架构
```
┌────────────── 客户端 ───────────────┐        ┌────────────── 服务器 ───────────────┐
│ UDP App ⇄ 127.0.0.1:PORT           │        │           ⇄ UDP App                │
│      ▲                             │        │                                     │
│ eBPF (egress): UDP → ICMP Echo     │        │ eBPF (ingress): ICMP Echo → UDP     │
└────────────────────────────────────┘        └─────────────────────────────────────┘
                 ▲ tuctl                                ▲ tuctl
```
`tuctl` 只在配置期工作；映射表填充完毕后即可退出。

---

## 运行要求
* Linux ≥ 5.8（支持 BTF 与 `clsact`）
* `libbpf` ≥ 1.0
* `clang/llvm` ≥ 12（编译 BPF 时使用）
* root 权限（加载 BPF / 操作 `tc`）
* `tc` (iproute2)
* 若硬件校验和异常，可选加载 `tutu_csum_fixup` 内核模块

---

## 编译与安装
```bash
git clone https://github.com/hrimfaxi/tutuicmptunnel.git
cd tutuicmptunnel
make
sudo make install
```

---

## 快速上手

### 1. 两端加载 BPF
```bash
sudo tuctl load iface eth0
```

### 2. 配置服务器
```bash
sudo tuctl server                # 进入服务器模式
sudo tuctl add uid 42 address 10.0.0.2 port 51820 comment "wg peer"
```

### 3. 配置客户端
```bash
sudo tuctl client address 198.51.100.7 port 51820 uid 42
```
此后客户端发往本机 UDP 51820 的数据将被封装为 ICMP，服务器收到后还原为 UDP。

---

## 子命令参考
所有子命令均需 root 执行；关键词不区分大小写，可简写（如 `addr` 代替 `address`）。

### `load`
加载并挂载 BPF 程序。

```text
tuctl load iface IFACE [iface IFACE2 ...] [debug] [ethhdr] [no-ethhdr]
```

选项 | 说明
-----|-----
`iface IFACE` | 指定网卡，至少一个
`debug`       | 加载调试版（含日志/追踪）
`ethhdr`      | 强制按以太网帧解析
`no-ethhdr`   | 强制无以太网头（如 lo）
（如未指定则自动探测链路类型）

### `unload`
卸载 BPF 并删除 TC 过滤器。

```text
tuctl unload iface IFACE [iface IFACE2 ...]
```

### `server`
切换为服务器角色（ICMP → UDP）。

```text
tuctl server [interval SECS] [fixup]
```
参数 | 默认 | 说明
-----|------|----
`interval` | 60  | ICMP-ID 更新 / 会话老化秒数
`fixup`    | 无 | 启用硬件校验和修正

### `client`
切换为客户端角色（UDP → ICMP）。

```text
tuctl client address ADDR port PORT uid UID
            [interval SECS] [fixup] [clean] [-4] [-6]
```

字段 | 说明
-----|-----
`address`  | 服务器地址 / 主机名
`port`     | 服务器 UDP 端口
`uid`      | 0-255，需与服务器侧 `uid` 一致
`interval` | ICMP-ID 轮换周期，默认 300
`fixup`    | 开启校验和修正
`clean`    | 清空 peer_map（地址/端口/uid 置 0）
`-4/-6`    | 强制使用 IPv4 或 IPv6 解析

### `add`
服务器端添加 / 更新允许的客户端。

```text
tuctl add uid UID address ADDR port PORT [comment STR] [-4|-6]
```

### `del`
服务器端删除客户端条目。

```text
tuctl del uid UID
```

### `status`
查看当前配置及状态。

```text
tuctl status [debug]
```
* 默认输出角色、版本、定时器、Peer 列表
* 加 `debug` 额外打印会话表

### `reaper`
手动清理过期 NAT 会话，依据 `update_interval` 判定。

```text
tuctl reaper
```

### `script`
批处理模式；从文件或 `-`(stdin) 读取多条命令。

```text
tuctl script FILE
```
语法
* `#` 开始注释（支持引号/转义）
* 空行忽略
示例
```text
# 部署脚本
load iface eth0
server interval 30
add uid 1 addr 192.0.2.10 port 1234 comment "demo"
```

### `version`
输出版本号。

### `help`
打印帮助（等同无参数）。

---

## 脚本模式
示例 `users.conf`：
```text
add uid 10 addr 10.0.0.1 port 5000 comment "bob"
add uid 11 addr 2001:db8::42 port 5000 comment "alice"
```
执行：
```bash
sudo tuctl script users.conf
```

---

## 常见问题

问题 | 建议
---- | ----
执行 `status` 报 `EPERM` | 需 root 或 CAP\_SYS\_ADMIN
隧道能 ping 但应用报错 | 加载 `tutu_csum_fixup` 并在 `server/client` 命令里加 `fixup`
无线网卡 `load` 失败 | 某些驱动不支持 `clsact`，可尝试升级内核或换网卡
想抓包验证 | `tcpdump -ni eth0 icmp`，负载里应看到 UDP

---

## 许可证
本项目遵循 **GNU General Public License v2.0**（GPL-2.0）。
详见根目录 `LICENSE` 文件。
