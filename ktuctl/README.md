# tuctl

`tuctl`是`tutuicmptunnel`的控制器。它不需要驻留内存，设置完成后立即退出。
此时`tutuicmptunnel`靠驻扎在内存中的`bpf`来完成服务。

## 快速上手

### 加载BPF并挂到接口设备`eth0`上

```bash
sudo tuctl load iface eth0
```

### 配置服务器

```bash
sudo tuctl server                # 进入服务器模式
sudo tuctl server-add uid 42 address 10.0.0.2 port 51820 comment "wg peer"
```

服务器会将来自`10.0.0.2`，`ICMP code` 为 42 的`ICMP Echo`包，转换为目标端口为`51820`的`UDP`包。\
对于发送到该`UDP`地址的所有响应，服务器会将其转换回`ICMP`包，并发送至原始源地址。

### 配置客户端

```bash
sudo tuctl client # 进入客户端模式
sudo tuctl client-add address 198.51.100.7 port 51820 uid 42
```

客户端发送到目的地址 `198.51.100.7` 的`UDP`目的端口`51820`的数据将被封装为`ICMP`包。\
而源地址`198.51.100.7`, `ICMP code`为 42 的`ICMP Reply`包，将被客户端转换回`UDP`包。

## 子命令参考

所有子命令均需`root` 执行\
关键词不区分大小写，可简写（如 `addr` 代替 `address`）

### `load`

> 加载`BPF`程序，并挂载到一个或多个接口上。必须使用这一步骤才能启用`tutuicmptunnel`。

```text
tuctl load iface IFACE [iface IFACE2 ...] [debug] [ethhdr] [no-ethhdr]
```

选项 | 说明
-----|-----
`iface IFACE` | 指定网卡，需要指定至少一个接口
`debug`       | 加载调试版（含日志/追踪）
`ethhdr`      | 强制按以太网帧解析 (大部分的接口类型为这个，比如`eth0`或`wlan0`)
`no-ethhdr`   | 强制无以太网头（如 `pppoe-wan`）

> ⚠️如未指定`ethhdr`或`no-ethhdr`则自动探测链路类型

### `unload`

> 删除接口上的`TC`过滤器，并卸载`BPF`。它会立即将`BPF`从内核文件系统中删除。

```text
tuctl unload iface IFACE [iface IFACE2 ...]
```

### `server`

> 切换为服务器角色

```text
tuctl server [max-age SECS] [no-fixup]
```

参数 | 默认 | 说明
-----|------|----
`max-age` | 60  | `UDP`会话老化时间，单位为秒数
`no-fixup`    | 无 | 禁用硬件校验和修正

> ⚠️如无必要，不要使用`no-fixup`。

### server-add

> 服务器端添加客户端条目。

```text
tuctl server-add [OPTIONS] {uid UID | user USERNAME} address ADDR port PORT [icmp-id ID] [sport PORT] [comment COMMENT]
```

参数 | 默认 | 说明
-----|------|----
`uid` | 无  | 用户`UID`
`username` | 无  | 用户名
`address` | 无  | 客户端源地址: `IPv4`/`IPv6`/域名均可
`port` | 无  | 服务器的目的端口
`icmp-id` | 0 | 可选：客户端使用的`ICMP ID`
`sport` | 0 | 可选：客户端的`UDP`源端口
`comment` | 空字符串 | 可选: 一个描述性注释用于标识客户端身份

可选参数:

参数 | 说明
---- | ----
-4 | 解析域名时仅使用`IPv4`地址
-6 | 解析域名时仅使用`IPv6`地址
-n | 显示用户时仅使用`UID`数字而不是用户名

### server-del

> 服务器端删除客户端条目。

```text
tuctl server-del [OPTIONS] {uid <id> | user <name>}
```

参数 | 默认 | 说明
-----|------|----
`uid` | 无  | 用户`UID`
`username` | 无  | 用户名

可选参数:

参数 | 说明
---- | ----
-n | 显示用户时仅使用`UID`数字而不是用户名

### `client`

> 切换为客户端角色

```text
tuctl client [OPTIONS]
```

可选参数:

参数 | 说明
---- | ----
`no-fixup` | 禁用硬件校验和修正

> ⚠️如无必要，不要使用`no-fixup`。

### client-add

> 向客户端配置添加对端服务器条目。

```text
tuctl client-add [OPTIONS] address ADDR port PORT {uid UID | user USERNAME} [comment COMMENT]
```

参数 | 默认 | 说明
-----| ---- | ----
`address` | 无  | 服务器地址: `IPv4`/`IPv6`/域名均可
`port`    | 无  | 服务器 UDP 端口
`uid` | 无  | 用户`UID`
`username` | 无  | 用户名
`comment` | 空字符串 | 可选：一个描述性注释用于标识服务器身份

可选参数:

参数 | 说明
---- | ----
-4 | 解析域名时仅使用`IPv4`地址
-6 | 解析域名时仅使用`IPv6`地址
-n | 显示用户时仅使用`UID`数字而不是用户名

### client-del

> 从客户端配置中删除一个对端服务器配置

```text
tuctl client-del [OPTIONS] {uid UID | user USERNAME} address ADDRESS
```

参数 | 默认 | 说明
-----| ---- | ----
`address` | 无  | 服务器地址: `IPv4`/`IPv6`/域名均可
`uid` | 无  | 用户`UID`
`username` | 无  | 用户名

允许客户端上可以有多个相同的`UID`使用不同的服务器地址，所以删除服务器配置时还需要指定要删除的服务器地址。

### `status`

> 查看当前配置及状态。

```text
tuctl status [OPTIONS] [debug]
```

参数:

debug: 打印更多调试信息

可选参数:

参数 | 说明
---- | ----
-n | 显示用户时仅使用`UID`数字而不是用户名

### `reaper`

> 服务器专用：清理过期的`NAT`会话

```text
tuctl reaper
```

> ⚠️如果支持`bpf_timer`则不需要调用本命令，`tutuicmptunnel`会自动清理过期`NAT`会话。

### `script`

> 批处理模式；从文件或 `-`(stdin) 读取多条命令。

```text
tuctl script FILE
```

语法:

* `#` 开始注释（支持引号/转义）
* 空行忽略

### dump

> 保存当前`tutuicmptunnel`配置到`stdin`。

可选参数:

参数 | 说明
---- | ----
-n | 显示用户时仅使用`UID`数字而不是用户名

### `version`

> 输出版本号

### `help`

> 打印帮助信息
