# hysteria

[English](./hysteria.md) | [简体中文](./hysteria_zh-CN.md)

---

## Example Demonstration

> Local -(SOCKS5)→ Hysteria-UDP -(ICMP)→ Server \
> All parameters have been abstracted as placeholders. Please replace them according to your actual situation, especially domains/keys. \
> ⚠️ The documentation intentionally omits real sensitive information like SNI, passwords, IPs, etc. Do not leave plaintext credentials in public spaces.

We assume you currently have a working `hysteria` configuration with `UDP` port `3322`.
Now you need to use `tutuicmptunnel-kmod` to transform the traffic into the `ICMP` protocol.

## Dependencies

`Ubuntu`:

```bash
sudo apt-get install curl
```

`Archlinux`:

```bash
sudo pacman -S curl
```

## `tutuicmptunnel-kmod` Settings

First, you need to select a `UID` for your client device on the server. For example, let's say the hostname is `a320` and the `UID` is 100.

`/etc/tutuicmptunnel/uids`:

```
100 a320 # a320
```

Add the above record to `/etc/tutuicmptunnel/uids` on the client side as well.

## Modify Systemd Unit File

Note that `tutuicmptunnel-kmod` cannot handle `GSO` packets, so `hysteria`'s related functionality needs to be disabled.
Set the environment variable `QUIC_GO_DISABLE_GSO=1` as shown below:

`/etc/systemd/system/hysteria-server@.service`:

```ini
[Unit]
Description=Hysteria Server (%i.yaml)
After=network.target tutuicmptunnel.service

[Service]
Type=simple
ExecStart=/usr/local/bin/hysteria server --config /etc/hysteria/%i.yaml
DynamicUser=yes
Environment=HYSTERIA_LOG_LEVEL=info QU
