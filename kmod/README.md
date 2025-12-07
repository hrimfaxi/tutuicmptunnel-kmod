# tutuicmptunnel.ko 说明

`tutuicmptunnel.ko` 是 `tutuicmptunnel-kmod` 的内核核心模块。
用户态工具 `ktuctl` 通过`netlink` 命令与该模块交互，用于创建、更新、查看和删除隧道配置。

## 模块参数

使用命令`modprobe`添加参数，例如：

```sh
rmmod tutuicmptunnel
modprobe tutuicmptunnel force_sw_checksum=1 allowed_uid=1000
```

参数说明

- force_sw_checksum
  - 说明：强制使用软件方式计算校验和。
  - 何时使用：在部分虚拟化/云环境（如某些阿里云实例、qemu 的 e1000e 网卡等）下，硬件校验和可能不可用或不可靠，需设为 1 才能发出正确的校验和。
  - 默认值：0（关闭）

- egress_peer_map_size
  - 说明: egress peer map大小，必须为2的幕次，不得小于256
  - 默认值: 1024

- ingress_peer_map_size
  - 说明: ingress peer map大小，必须为2的幕次，不得小于256
  - 默认值: 1024

- session_map_size
  - 说明: session map大小，必须为2的幕次，不得小于256
  - 默认值: 16384

- allowed_uid:
  - 说明: 允许能够修改`tutuicmptunnel-kmod` 配置的用户`UID`。<0表示不启用。
  - 默认值: -1

- allowed_gid:
  - 说明: 允许能够修改`tutuicmptunnel-kmod` 配置的组`GID`。<0表示不启用。
  - 默认值: -1

以上参数中的`force_sw_checksum` 与 `allowed_uid` / `allowed_gid` 支持运行时动态调整；其余参数作为加载时参数生效，不能修改。

## 运行时调整示例

- 启用软件校验和：
  - echo 1 > /sys/module/tutuicmptunnel/parameters/force_sw_checksum

## 备注与建议

- 在云/虚拟化场景遇到 `ICMP` 封装包校验和异常时，优先尝试开启 `force_sw_checksum` 进行验证。
