# wireguard

`WireGuard`是一个强大的基于`UDP`的`VPN`协议。为防止流量受到`ISP`针对`UDP`的`QoS`或限速规则影响，可以借助`tutuicmptunnel`对`WireGuard`流量进行封装和保护。
本文假设你已经有一个配置成功的`/etc/wireguard/myserver.conf`，现在需要对其进行调整，以启用`tutuicmptunnel`。
并且你的`wireguard`服务器端口为`9000`。服务器域名为`myserver.ip`。
你的`tuctl_server`端口为9010, `PSK`口令为`verylongpsk`。

## 部署

首先为你的客户端设备选择一个`uid`和主机名，在我们的例子里是`uid`=`100`，主机名为`a320`。

修改服务器和客户端的
`/etc/tutuicmptunnel/uids`:

```
100 a320
```

创建环境变量文件: `/etc/wireguard/tutuicmptunnel.myserver`

```sh
#!/bin/sh

TUTU_UID=a320
ADDR=myserver.ip
PORT=9000
SERVER_PORT=9010
PSK=verylongpsk
COMMENT=myserver-wgserver
```

在你的客户端`wireguard`配置(`/etc/wireguard/myserver.conf`)上添加：

```ini
[Interface]
PreUp = env_file=$(dirname $CONFIG_FILE)/tutuicmptunnel.myserver; source $env_file && tuctl client-add user $TUTU_UID address $ADDR port $PORT comment $COMMENT || true
PreUp = env_file=$(dirname $CONFIG_FILE)/tutuicmptunnel.myserver; source $env_file && IP=$(curl -s ip.3322.net) && tuctl_client server $ADDR server-port $SERVER_PORT psk $PSK <<< "server-add user $TUTU_UID comment $COMMENT address $IP port $PORT" || true
PostDown = env_file=$(dirname $CONFIG_FILE)/tutuicmptunnel.myserver; source $env_file && tuctl client-del user $TUTU_UID address $ADDR || true
PostDown = env_file=$(dirname $CONFIG_FILE)/tutuicmptunnel.myserver; source $env_file && tuctl_client server $ADDR server-port $SERVER_PORT psk $PSK <<< "server-del user $TUTU_UID" || true
```

使用`wg-quick`重启接口即可：

```bash
sudo wg-quick down myserver
sudo wg-quick up myserver
```

现在可以使用`tcpdump`抓包看看是否有`ICMP`流量

```bash
sudo tcpdump -i any -n icmp -v
```

窍门：使用`tcp_bbr`拥塞算法可以大大提高`wireguard`的性能。
