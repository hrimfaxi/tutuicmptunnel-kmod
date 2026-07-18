[English](./README.md) | [简体中文](./README_zh-CN.md)

---

# ktuctl

`ktuctl` is the userspace controller for `tutuicmptunnel-kmod`. It does not need to stay resident in memory — it exits immediately after configuration; from that point on, the kernel module running in memory provides the service independently.

`ktuctl` controls `tutuicmptunnel-kmod` through the character device file `/dev/tutuicmptunnel`. The device file permissions default to `0700` and can be adjusted via the `dev_mode` module parameter of `tutuicmptunnel.ko`.

## Quick Start

### Load the module and apply it only to interface `eth0`

```bash
sudo rmmod tutuicmptunnel
sudo modprobe tutuicmptunnel
sudo ktuctl load iface eth0
```

### Configure the server

```bash
sudo ktuctl server                                    # Enter server mode
sudo ktuctl server-add uid 42 address 10.0.0.2 port 51820 comment "wg peer"
```

The server will convert ICMP Echo packets from `10.0.0.2` with `ICMP code` 42 into UDP packets destined for port `51820`; for all responses from that UDP address, the server will convert them back into ICMP packets and send them to the original source address.

### Configure the client

```bash
sudo ktuctl client                                    # Enter client mode
sudo ktuctl client-add address 198.51.100.7 port 51820 uid 42
```

UDP data sent by the client to destination address `198.51.100.7` on destination port `51820` will be encapsulated as ICMP packets; ICMP Reply packets from `198.51.100.7` with `ICMP code` 42 will be converted back to UDP packets by the client.

## Subcommand Reference

All subcommands must be run as `root`. Keywords are case-insensitive and can be abbreviated (e.g., `addr` instead of `address`).

### `server`

> Switch to server role.

```text
ktuctl server [max-age SECS]
```

| Parameter | Default | Description |
| ---- | ------ | ---- |
| `max-age` | `60` | UDP session aging time, in seconds |

### `server-add`

> Add a client entry on the server side.

```text
ktuctl server-add [OPTIONS] {uid UID | user USERNAME} address ADDR port PORT [icmp-id ID] [sport PORT] [xor KEY] [comment COMMENT]
```

| Parameter | Default | Description |
| ---- | ------ | ---- |
| `uid` | None | User UID |
| `user` | None | Username |
| `address` | None | Client source address: `IPv4` / `IPv6` / domain name |
| `port` | None | Server destination port |
| `icmp-id` | `0` | Optional: ICMP ID used by the client |
| `sport` | `0` | Optional: Client UDP source port |
| `xor` | None | Optional: XOR obfuscation key (hex format, e.g., `a1b2c3d4`) |
| `comment` | Empty string | Optional: Descriptive comment to identify the client |

Optional parameters:

| Parameter | Description |
| ---- | ---- |
| `-4` | Use only `IPv4` addresses when resolving domain names |
| `-6` | Use only `IPv6` addresses when resolving domain names |
| `-n` | Display UID numbers instead of usernames |

### `server-del`

> Delete a client entry on the server side.

```text
ktuctl server-del [OPTIONS] {uid UID | user USERNAME}
```

| Parameter | Default | Description |
| ---- | ------ | ---- |
| `uid` | None | User UID |
| `user` | None | Username |

Optional parameters:

| Parameter | Description |
| ---- | ---- |
| `-n` | Display UID numbers instead of usernames |

### `client`

> Switch to client role.

```text
ktuctl client [OPTIONS]
```

### `client-add`

> Add a remote server entry to the client configuration.

```text
ktuctl client-add [OPTIONS] address ADDR port PORT {uid UID | user USERNAME} [xor KEY] [comment COMMENT]
```

| Parameter | Default | Description |
| ---- | ------ | ---- |
| `address` | None | Server address: `IPv4` / `IPv6` / domain name |
| `port` | None | Server UDP port |
| `uid` | None | User UID |
| `user` | None | Username |
| `xor` | None | Optional: XOR obfuscation key (hex format, e.g., `a1b2c3d4`) |
| `comment` | Empty string | Optional: Descriptive comment to identify the server |

Optional parameters:

| Parameter | Description |
| ---- | ---- |
| `-4` | Use only `IPv4` addresses when resolving domain names |
| `-6` | Use only `IPv6` addresses when resolving domain names |
| `-n` | Display UID numbers instead of usernames |

> [!IMPORTANT]
> Client configurations with the same UID and host address but different ports are not allowed. If you need the same server to provide service on two or more ports, assign a separate UID for each port.

### `client-del`

> Delete a remote server entry from the client configuration.

```text
ktuctl client-del [OPTIONS] {uid UID | user USERNAME} address ADDRESS
```

| Parameter | Default | Description |
| ---- | ------ | ---- |
| `address` | None | Server address: `IPv4` / `IPv6` / domain name |
| `uid` | None | User UID |
| `user` | None | Username |

The client allows the same UID to use different server addresses, so you must also specify the server address to delete when removing a server entry.

### `status`

> View current configuration and status.

```text
ktuctl status [OPTIONS] [debug]
```

Parameters:

| Parameter | Description |
| ---- | ---- |
| `debug` | Print more debug information |

Optional parameters:

| Parameter | Description |
| ---- | ---- |
| `-n` | Display UID numbers instead of usernames |

### `reaper`

> Server only: clean up expired NAT sessions.

```text
ktuctl reaper
```

> [!WARNING]
> This command is deprecated: `tutuicmptunnel-kmod` automatically cleans up expired NAT sessions.

### `script`

> Batch mode: read multiple commands from a file or `-` (standard input).

```text
ktuctl script FILE
```

Syntax:

- Lines starting with `#` are comments (quoting/escaping supported)
- Empty lines are ignored

### `dump`

> Output the current `tutuicmptunnel-kmod` configuration to stdout; can be redirected to a file and later restored with `ktuctl script`.

Optional parameters:

| Parameter | Description |
| ---- | ---- |
| `-n` | Display UID numbers instead of usernames |

### `version`

> Print the version number.

### `help`

> Print help information.

## XOR Obfuscation

### Overview

`tutuicmptunnel-kmod` supports simple XOR obfuscation for lightweight obfuscation of ICMP payloads. This can make it harder for DPI (Deep Packet Inspection) to identify and filter traffic, but **does not provide true cryptographic security**.

### How It Works

XOR obfuscation works as follows:

1. **Key Generation**: Uses a pre-shared hexadecimal key (e.g., `a1b2c3d4e5f6`).
2. **Payload Processing**: XORs each byte of the ICMP payload with the key.
3. **Dynamic Offset**: Calculates the key starting position using the ICMP sequence and payload length to avoid fixed patterns.
4. **Bidirectional Processing**: Obfuscates on send, de-obfuscates on receive (leveraging the reversibility of XOR).

### Usage Examples

```bash
# Add a client with XOR obfuscation on the server side
sudo ktuctl server-add uid 42 address 10.0.0.2 port 51820 xor a1b2c3d4

# Add a server with XOR obfuscation on the client side
sudo ktuctl client-add address 198.51.100.7 port 51820 uid 42 xor a1b2c3d4
```

### Notes

> [!CAUTION]
> - **Both sides must use the same XOR key**, otherwise communication will fail.
> - **It is not encryption**: XOR obfuscation provides only limited obfuscation and should not be relied upon for security protection.
> - **Recommended to use with encryption tools**: such as WireGuard, Hysteria, Xray, etc., which already provide strong encryption.
> - **Key format**: Must be a hexadecimal string (even length), maximum 64 bytes (128 hexadecimal characters).
