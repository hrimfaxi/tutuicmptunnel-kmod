[English](./README.md) | [简体中文](./README_zh-CN.md)

---

# tutuicmptunnel.ko

`tutuicmptunnel.ko` is the core kernel module of `tutuicmptunnel-kmod`.
The userspace tool `ktuctl` interacts with this module via **netlink** to create, update, view, and delete tunnel configurations.

## Module Parameters

### Loading Method

Unload the old module first, then reload it with parameters via `modprobe`:

```sh
rmmod tutuicmptunnel
modprobe tutuicmptunnel force_sw_checksum=1 allowed_uid=1000
```

### Runtime Parameters (Adjustable Dynamically)

| Parameter | Description | Default |
| ---- | ---- | ------ |
| `force_sw_checksum` | Force software checksum calculation. In some virtualization/cloud environments (e.g., certain Alibaba Cloud instances, QEMU's e1000e NIC), hardware checksum is unavailable or unreliable, and this must be set to `1` to send correct checksums. | `0` (disabled) |
| `allowed_uid` | UID of the user allowed to modify `tutuicmptunnel-kmod` configuration. `< 0` means disabled. | `-1` |
| `allowed_gid` | GID of the group allowed to modify `tutuicmptunnel-kmod` configuration. `< 0` means disabled. | `-1` |

### Load-Time Parameters (Only Effective at Module Load)

| Parameter | Description | Default |
| ---- | ---- | ------ |
| `egress_peer_map_size` | Size of the egress peer map, must be a power of two and no less than 256. | `1024` |
| `ingress_peer_map_size` | Size of the ingress peer map, must be a power of two and no less than 256. | `1024` |
| `session_map_size` | Size of the session map, must be a power of two and no less than 256. | `16384` |

> [!NOTE]
> Only `force_sw_checksum`, `allowed_uid`, and `allowed_gid` support dynamic runtime adjustment; the remaining parameters cannot be modified after the module is loaded and require reloading the module to change.

## Runtime Adjustment Example

Modify runtime parameters directly via the sysfs interface:

```sh
# Enable software checksum
echo 1 > /sys/module/tutuicmptunnel/parameters/force_sw_checksum
```

## Notes and Recommendations

> [!TIP]
> When encountering checksum issues with ICMP encapsulated packets in cloud/virtualization environments, it is recommended to first enable `force_sw_checksum` for verification:
>
> ```sh
> echo 1 > /sys/module/tutuicmptunnel/parameters/force_sw_checksum
> ```
