# iperf3

下面示例演示如何用 `ktuctl` 搭建 **UDP-over-ICMP 隧道**，并通过 `iperf3` 进行吞吐测试。
假设：

* 服务器主机名：`a320`（`SSH`配置了免密登陆）
* 隧道 `UDP` 端口：`3322`
* 隧道 `UID`：`99`
* 服务器物理网卡：`enp4s0`
* 客户端物理网卡：`wlan0`

## 部署脚本

把下列脚本保存到客户端，假设文件名 `run_tunnel.sh`，并赋可执行权限。

```bash
#!/bin/sh
set -e

HOST=a320                    # 服务器主机或 IP
PORT=3322                    # 隧道 UDP 端口
HOST_DEV=enp4s0              # 服务器出口网卡名

UID=99
LOCAL=192.168.15.238         # 客户端自己的地址
LOCAL_DEV=wlan0              # 客户端出口网卡名
COMMENT=r7735h               # 备注，可随意

# -------- 服务器端 --------
ssh $HOST sudo rmmod tutuicmptunnel
ssh $HOST sudo modprobe tutuicmptunnel
ssh $HOST sudo ktuctl server
ssh $HOST sudo ktuctl server-add uid $UID address $LOCAL port $PORT comment $COMMENT

# -------- 客户端 --------
sudo rmmod tutuicmptunnel
sudo modprobe tutuicmptunnel
cat << EOF | sudo ktuctl script -
client
client-add uid $UID address $HOST port $PORT
EOF
```

执行：

```bash
chmod +x run_tunnel.sh
./run_tunnel.sh
```

使用 iperf3 进行测速
--------------------

服务器侧启动：

```bash
ssh a320 "iperf3 -s -p 3322"
```

客户端侧启动下行`UDP`测试 1 小时，报文长度`1472 B`，目标带宽 `1 Gbps`：

```bash
iperf3 -c a320 -p 3322 -u -b 1000m -t 3600 -l 1472 -R
```

观察结果：

* `iperf3` 客户端/服务器输出即为隧道实测带宽、丢包率、抖动等。
* 在两端另开终端执行 `sudo ktuctl status -d` 可查看隧道处理 / 丢弃 / GSO 等计数。

## 清理

```bash
# 客户端
sudo rmmod tutuicmptunnel

# 服务器
ssh a320 sudo rmmod tutuicmptunnel
```

至此完成一次基于`ICMP`隧道的 `iperf3` 吞吐测试。
