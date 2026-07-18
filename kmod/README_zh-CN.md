[English](./README.md) | [简体中文](./README_zh-CN.md)

---

# tutuicmptunnel.ko

`tutuicmptunnel.ko` 是 `tutuicmptunnel-kmod` 的内核核心模块。
用户态工具 `ktuctl` 通过 **netlink** 与该模块交互，用于创建、更新、查看和删除隧道配置。

## 模块参数

### 加载方式

先卸载旧模块，再通过 `modprobe` 携带参数重新加载：

```sh
rmmod tutuicmptunnel
modprobe tutuicmptunnel force_sw_checksum=1 allowed_uid=1000
```

### 运行时参数（可动态调整）

| 参数 | 说明 | 默认值 |
| ---- | ---- | ------ |
| `force_sw_checksum` | 强制使用软件方式计算校验和。部分虚拟化/云环境（如某些阿里云实例、QEMU 的 e1000e 网卡）下硬件校验和不可用或不可靠，需设为 `1` 才能发出正确的校验和。 | `0`（关闭） |
| `allowed_uid` | 允许修改 `tutuicmptunnel-kmod` 配置的用户 UID。`< 0` 表示不启用。 | `-1` |
| `allowed_gid` | 允许修改 `tutuicmptunnel-kmod` 配置的组 GID。`< 0` 表示不启用。 | `-1` |

### 加载时参数（仅在模块加载时生效）

| 参数 | 说明 | 默认值 |
| ---- | ---- | ------ |
| `egress_peer_map_size` | egress peer map 大小，必须为 2 的幂次，且不小于 256。 | `1024` |
| `ingress_peer_map_size` | ingress peer map 大小，必须为 2 的幂次，且不小于 256。 | `1024` |
| `session_map_size` | session map 大小，必须为 2 的幂次，且不小于 256。 | `16384` |

> [!NOTE]
> 只有 `force_sw_checksum`、`allowed_uid`、`allowed_gid` 支持运行时动态调整；其余参数在模块加载后无法修改，需重新加载模块才能变更。

## 运行时调整示例

通过 sysfs 接口直接修改运行时参数：

```sh
# 启用软件校验和
echo 1 > /sys/module/tutuicmptunnel/parameters/force_sw_checksum
```

## 备注与建议

> [!TIP]
> 在云/虚拟化场景中遇到 ICMP 封装包校验和异常时，建议优先开启 `force_sw_checksum` 进行验证：
>
> ```sh
> echo 1 > /sys/module/tutuicmptunnel/parameters/force_sw_checksum
> ```
