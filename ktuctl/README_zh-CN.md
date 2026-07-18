[English](./README.md) | [简体中文](./README_zh-CN.md)

---

# ktuctl

`ktuctl` 是 `tutuicmptunnel-kmod` 的用户层控制器。它不需要驻留内存，配置完成后立即退出；此后由驻扎在内存中的内核模块独立提供服务。

`ktuctl` 通过字符设备文件 `/dev/tutuicmptunnel` 控制 `tutuicmptunnel-kmod`。设备文件默认权限为 `0700`，可通过 `tutuicmptunnel.ko` 的 `dev_mode` 模块参数调整。

## 快速上手

### 加载模块并仅对接口 `eth0` 生效

```bash
sudo rmmod tutuicmptunnel
sudo modprobe tutuicmptunnel
sudo ktuctl load iface eth0
```

### 配置服务器

```bash
sudo ktuctl server                                    # 进入服务器模式
sudo ktuctl server-add uid 42 address 10.0.0.2 port 51820 comment "wg peer"
```

服务器会将来自 `10.0.0.2`、`ICMP code` 为 42 的 ICMP Echo 包，转换为目的端口 `51820` 的 UDP 包；对于该 UDP 地址的所有响应，服务器会将其转换回 ICMP 包并发回原始源地址。

### 配置客户端

```bash
sudo ktuctl client                                    # 进入客户端模式
sudo ktuctl client-add address 198.51.100.7 port 51820 uid 42
```

客户端发往目的地址 `198.51.100.7`、目的端口 `51820` 的 UDP 数据将被封装为 ICMP 包；来自 `198.51.100.7`、`ICMP code` 为 42 的 ICMP Reply 包，将被客户端转换回 UDP 包。

## 子命令参考

所有子命令均需以 `root` 执行。关键词不区分大小写，可简写（如 `addr` 代替 `address`）。

### `server`

> 切换为服务器角色。

```text
ktuctl server [max-age SECS]
```

| 参数 | 默认值 | 说明 |
| ---- | ------ | ---- |
| `max-age` | `60` | UDP 会话老化时间，单位：秒 |

### `server-add`

> 服务器端添加客户端条目。

```text
ktuctl server-add [OPTIONS] {uid UID | user USERNAME} address ADDR port PORT [icmp-id ID] [sport PORT] [xor KEY] [comment COMMENT]
```

| 参数 | 默认值 | 说明 |
| ---- | ------ | ---- |
| `uid` | 无 | 用户 UID |
| `user` | 无 | 用户名 |
| `address` | 无 | 客户端源地址：`IPv4` / `IPv6` / 域名均可 |
| `port` | 无 | 服务器的目的端口 |
| `icmp-id` | `0` | 可选：客户端使用的 ICMP ID |
| `sport` | `0` | 可选：客户端的 UDP 源端口 |
| `xor` | 无 | 可选：XOR 混淆密钥（十六进制格式，如 `a1b2c3d4`） |
| `comment` | 空字符串 | 可选：描述性注释，用于标识客户端身份 |

可选参数：

| 参数 | 说明 |
| ---- | ---- |
| `-4` | 解析域名时仅使用 `IPv4` 地址 |
| `-6` | 解析域名时仅使用 `IPv6` 地址 |
| `-n` | 显示用户时仅使用 UID 数字而不是用户名 |

### `server-del`

> 服务器端删除客户端条目。

```text
ktuctl server-del [OPTIONS] {uid UID | user USERNAME}
```

| 参数 | 默认值 | 说明 |
| ---- | ------ | ---- |
| `uid` | 无 | 用户 UID |
| `user` | 无 | 用户名 |

可选参数：

| 参数 | 说明 |
| ---- | ---- |
| `-n` | 显示用户时仅使用 UID 数字而不是用户名 |

### `client`

> 切换为客户端角色。

```text
ktuctl client [OPTIONS]
```

### `client-add`

> 向客户端配置中添加对端服务器条目。

```text
ktuctl client-add [OPTIONS] address ADDR port PORT {uid UID | user USERNAME} [xor KEY] [comment COMMENT]
```

| 参数 | 默认值 | 说明 |
| ---- | ------ | ---- |
| `address` | 无 | 服务器地址：`IPv4` / `IPv6` / 域名均可 |
| `port` | 无 | 服务器 UDP 端口 |
| `uid` | 无 | 用户 UID |
| `user` | 无 | 用户名 |
| `xor` | 无 | 可选：XOR 混淆密钥（十六进制格式，如 `a1b2c3d4`） |
| `comment` | 空字符串 | 可选：描述性注释，用于标识服务器身份 |

可选参数：

| 参数 | 说明 |
| ---- | ---- |
| `-4` | 解析域名时仅使用 `IPv4` 地址 |
| `-6` | 解析域名时仅使用 `IPv6` 地址 |
| `-n` | 显示用户时仅使用 UID 数字而不是用户名 |

> [!IMPORTANT]
> 不允许存在 UID 和主机地址相同、但端口不同的客户端配置。如果确实需要同一台服务器提供两个以上端口的服务，请为每个端口分配一个单独的 UID。

### `client-del`

> 从客户端配置中删除一个对端服务器配置。

```text
ktuctl client-del [OPTIONS] {uid UID | user USERNAME} address ADDRESS
```

| 参数 | 默认值 | 说明 |
| ---- | ------ | ---- |
| `address` | 无 | 服务器地址：`IPv4` / `IPv6` / 域名均可 |
| `uid` | 无 | 用户 UID |
| `user` | 无 | 用户名 |

客户端允许同一个 UID 使用不同的服务器地址，因此删除服务器配置时还需指定要删除的服务器地址。

### `status`

> 查看当前配置及状态。

```text
ktuctl status [OPTIONS] [debug]
```

参数：

| 参数 | 说明 |
| ---- | ---- |
| `debug` | 打印更多调试信息 |

可选参数：

| 参数 | 说明 |
| ---- | ---- |
| `-n` | 显示用户时仅使用 UID 数字而不是用户名 |

### `reaper`

> 服务器专用：清理过期的 NAT 会话。

```text
ktuctl reaper
```

> [!WARNING]
> 本命令已过时：`tutuicmptunnel-kmod` 会自动清理过期 NAT 会话。

### `script`

> 批处理模式：从文件或 `-`（标准输入）读取多条命令。

```text
ktuctl script FILE
```

语法：

- 以 `#` 开始的为注释（支持引号/转义）
- 空行将被忽略

### `dump`

> 将当前 `tutuicmptunnel-kmod` 配置输出到标准输出（stdout），可重定向保存为文件后配合 `ktuctl script` 恢复。

可选参数：

| 参数 | 说明 |
| ---- | ---- |
| `-n` | 显示用户时仅使用 UID 数字而不是用户名 |

### `version`

> 输出版本号。

### `help`

> 打印帮助信息。

## XOR 混淆

### 概述

`tutuicmptunnel-kmod` 支持简单的 XOR 混淆功能，用于对 ICMP 载荷进行轻量级混淆处理。这可以增加 DPI（深度包检测）识别和过滤的难度，但**不提供真正的加密安全性**。

### 原理

XOR 混淆的工作原理：

1. **密钥生成**：使用预共享的十六进制密钥（如 `a1b2c3d4e5f6`）。
2. **载荷处理**：对 ICMP 包的有效载荷逐字节与密钥进行异或运算。
3. **动态偏移**：使用 ICMP sequence 和载荷长度计算密钥起始位置，避免固定模式。
4. **双向处理**：发送时混淆，接收时解混淆（利用 XOR 运算的可逆性）。

### 使用示例

```bash
# 服务器端添加带 XOR 混淆的客户端
sudo ktuctl server-add uid 42 address 10.0.0.2 port 51820 xor a1b2c3d4

# 客户端添加带 XOR 混淆的服务器
sudo ktuctl client-add address 198.51.100.7 port 51820 uid 42 xor a1b2c3d4
```

### 注意事项

> [!CAUTION]
> - **双方必须使用相同的 XOR 密钥**，否则通信将失败。
> - **不是加密**：XOR 混淆仅提供有限的混淆效果，不应依赖它进行安全保护。
> - **建议配合加密工具使用**：如 WireGuard、Hysteria、Xray 等已具备强加密的工具。
> - **密钥格式**：必须是十六进制字符串（偶数长度），最大 64 字节（128 个十六进制字符）。
