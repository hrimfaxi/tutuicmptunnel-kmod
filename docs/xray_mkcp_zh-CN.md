# xray+kcptun

## 概述

`xray-core` `mKCP` 可替代 `xray-core` + `kcptun` 双进程方案。优势主要是:

- 去除双进程间拷贝与上下文切换开销。
- `mKCP` 对 `xray-core` 更友好，调度与实现更高效。
- 原生并发多条 `mKCP` 连接，避免 `KCP` 写头阻塞。

## xray-core配置

服务器配置建议:

```json
"inbounds": [
  {
    "protocol": "vless",
    "port": 1234,
    "settings": {
      "decryption": "none",
      "clients": [
        {
          "id": "your_xray_id",
          "flow": "xtls-rprx-vision"
        }
      ]
    },
    "streamSettings": {
      "network": "kcp",
      "kcpSettings": {
        "mtu": 1450,
        "tti": 100,
        "congestion": true,
        "uplinkCapacity": 5,
        "downlinkCapacity": 100,
        "readBufferSize": 2,
        "writeBufferSize": 3
      },
      "security": "tls",
      "tlsSettings": {
        "minVersion": "1.3",
        "alpn": ["h2","http/1.1"],
        "certificates": [
          {
            "certificateFile": "/etc/xray/xray.crt",
            "keyFile": "/etc/xray/xray.key"
          }
        ]
      }
    }
  }
]
```

客户端配置建议:

```json
"outbounds": [
  {
    "tag": "proxy",
    "protocol": "vless",
    "settings": {
      "vnext": [
        {
          "address": "your_vps_ip",
          "port": 1234,
          "users": [
            {
              "id": "your_vps_id",
              "alterId": 0,
              "security": "auto",
              "encryption": "none",
              "flow": "xtls-rprx-vision"
            }
          ]
        }
      ]
    },
    "streamSettings": {
      "network": "kcp",
      "kcpSettings": {
        "mtu": 1450,
        "tti": 100,
        "congestion": true,
        "uplinkCapacity": 2,
        "downlinkCapacity": 100,
        "readBufferSize": 5,   // ≈ server writeBufferSize(3) × 1.5 取整
        "writeBufferSize": 1
      },
      "security": "tls",
      "tlsSettings": {
        "allowInsecure": false,
        "serverName": "your_vps_domain_name",
        "alpn": ["h2"],
        "fingerprint": "chrome"
      }
    },
    "mux": { "enabled": false, "concurrency": -1 }
  }
]
```

要点：

- 容量：`uplinkCapacity` 贴近实际可用上行；`downlinkCapacity` 设较大（如 100），避免下行瓶颈。
- 服务器 `writeBufferSize` 近似原 `KCP` 的 `sndwnd`，过大可能造成发包洪水，压垮客户端或网络队列。
- 实测经验：客户端 `readBufferSize ≈ ceil(server writeBufferSize × 1.5)`，在吞吐与时延间更均衡。
- 其他：根据链路与硬件微调 `mtu`、`tti`、`congestion`，并观察吞吐、`RTT`、丢包和 `CPU` 占用。
- 建议实际测试 `tti` 值，因为这部分有点反直觉：发送得太快未必会提升性能，反而可能触发 `ICMP` 速率限制，导致效率难以提升。

## tutuicmptunnel-kmod配置

首先检查服务器/客户端两侧是否有共同的`/etc/tutuicmptunnel/uids`配置:

```
123 your_user_name
```

然后检查服务器/客户端两侧是否都已经加载`tutuicmptunnel.ko`，并`ktuctl`可以访问设备

```sh
sudo lsmod | grep tutuicmptunnel
sudo ktuctl -d
```

使用以下简单脚本在客户端运行,会同步配置到服务器/客户端两侧：

```sh
#!/bin/sh

V() {
  echo "$@"
  "$@"
}

TMP=$(mktemp)
export DEV=enp4s0 # 你的客户端的上网接口名

sudo ktuctl dump > $TMP
sudo rmmod tutuicmptunnel
sudo modprobe tutuicmptunnel ifnames=$DEV

export TUTU_UID=your_user_name # 替换为你的服务器上选好的uid
export ADDRESS=yourdomain.com # 替换为你的xray-core服务器域名或IP
export PORT=1234 # 替换为你的xray-core服务器udp端口

sudo ktuctl script - < $TMP
rm -f $TMP
sudo ktuctl client
sudo ktuctl client-del address $ADDRESS user $TUTU_UID
sudo ktuctl client-add address $ADDRESS port $PORT user $TUTU_UID

export COMMENT=your_client_name # 替换为你的客户端的注释，此注释会在服务器的ktuctl命令上显示
export HOST=$ADDRESS
export PSK=yourlongpsk # 替换为你的tuctl_server的PSK口令
export SERVER_PORT=14801 # 替换为你的tuctl_server的端口

# 使用3322.net的ip api服务器获取本机公网IP，你也可以换成其他服务
IP=$(curl -sf ip.3322.net)
echo local ip: $IP

printf "server\nserver-add uid $TUTU_UID address $IP port $PORT comment $COMMENT\n" | V tuctl_client \
psk $PSK \
server $HOST \
server-port $SERVER_PORT

# vim: set sw=2 ts=2 expandtab:
```

启动`xray-core`客户端前运行脚本即可。你也可以在`xray-core`的`systemd`单元文件中添加`ExecStartPre`自动运行这个脚本。
这样就不必每次都运行脚本了。

## 开机自启

参见[hysteria](hysteria.md)，可以定期使用`crontab`或者`systemd-timer`调用以上脚本。

## 参见

- [xray-core mkcp说明](https://xtls.github.io/en/config/transports/mkcp.html)
