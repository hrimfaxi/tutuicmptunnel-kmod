# tutuicmptunnel.ko 说明

`tutuicmptunnel.ko` 是 `tutuicmptunnel-kmod` 的内核核心模块。用户态工具 `ktuctl` 通过字符设备 `/dev/tutuicmptunnel` 与该模块交互，用于创建、更新、查看和删除隧道配置。

## 模块参数

使用命令`modprobe`添加参数，例如：

```sh
rmmod tutuicmptunnel
modprobe tutuicmptunnel force_sw_checksum=1 dev_mode=0777 ifnames=wlan0,pppoe-wan,wan,enp4s0
```

参数说明

- force_sw_checksum
  - 说明：强制使用软件方式计算校验和。
  - 何时使用：在部分虚拟化/云环境（如某些阿里云实例、qemu 的 e1000e 网卡等）下，硬件校验和可能不可用或不可靠，需设为 1 才能发出正确的校验和。
  - 默认值：0（关闭）

- dev_mode
  - 说明：控制 `/dev/tutuicmptunnel` 的设备文件权限（八进制）。
  - 默认值：0700（仅 root 可访问）
  - 提示：若需允许任意用户使用 `ktuctl`，可设为 0777，但存在明显安全风险，请谨慎评估。

- ifnames
  - 说明：限定 `tutuicmptunnel` 生效的网络接口列表，使用逗号分隔多个接口名。
  - 示例：wlan0,pppoe-wan,wan
  - 默认值：空（不限制，作用于所有接口）

以上参数中的 force_sw_checksum 与 ifnames 支持运行时动态调整；dev_mode 通常作为加载时参数生效。

## 运行时调整示例

- 限定作用接口：
  - echo "wlan0,pppoe-wan,wan" > /sys/module/tutuicmptunnel/parameters/ifnames

- 启用软件校验和：
  - echo 1 > /sys/module/tutuicmptunnel/parameters/force_sw_checksum

## 备注与建议

- 接口名称需与系统中实际存在的 net_device 保持一致；重命名或删除接口后请相应更新 ifnames。
- 在云/虚拟化场景遇到 ICMP 封装包校验和异常时，优先尝试开启 force_sw_checksum 进行验证。
- 放宽 dev_mode 权限前，请确认多用户场景下的访问控制与审计需求，避免滥用造成隧道配置被非授权篡改。
