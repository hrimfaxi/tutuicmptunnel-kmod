# iperf3

[English](./iperf3.md) | [简体中文](./iperf3_zh-CN.md)

---

The following example demonstrates how to set up a **UDP-over-ICMP tunnel** using `ktuctl` and perform throughput testing with `iperf3`.
Assumptions:

* Server hostname: `a320` (configured with SSH key-based authentication)
* Tunnel UDP port: `3322`
* Tunnel UID: `99`
* Server physical network interface: `enp4s0`
* Client physical network interface: `wlan0`

## Deployment Script

Save the following script to the client, assuming the filename is `run_tunnel.sh`, and grant it executable permission.

```bash
#!/bin/sh
set -e

HOST=a320                    # Server hostname or IP
PORT=3322                    # Tunnel UDP port
HOST_DEV=enp4s0              # Server outbound network interface name

UID=99
LOCAL=192.168.15.238         # Client's own address
LOCAL_DEV=wlan0              # Client outbound network interface name
COMMENT=r7735h               # Comment, optional

# -------- Server Side --------
ssh $HOST sudo rmmod tutuicmptunnel
ssh $HOST sudo modprobe tutuicmptunnel
ssh $HOST sudo ktuctl server
ssh $HOST sudo ktuctl server-add uid $UID address $LOCAL port $PORT comment $COMMENT

# -------- Client Side --------
sudo rmmod tutuicmptunnel
sudo modprobe tutuicmptunnel
cat << EOF | sudo ktuctl script -
client
client-add uid $UID address $HOST port $PORT
EOF
```

Execute:

```bash
chmod +x run_tunnel.sh
./run_tunnel.sh
```

Speed Test Using iperf3
--------------------

Start on the server side:

```bash
ssh a320 "iperf3 -s -p 3322"
```

Start downlink `UDP` test on the client side for 1 hour, with packet length `1472 B`, and target bandwidth `1 Gbps`:

```bash
iperf3 -c a320 -p 3322 -u -b 1000m -t 3600 -l 1472 -R
```

Observe results:

* The `iperf3` client/server output shows the tunnel's measured bandwidth, packet loss rate, jitter, etc.
* Open another terminal on both ends and run `sudo ktuctl status -d` to view tunnel processing / drop / GSO counters.

## Cleanup

```bash
# Client
sudo rmmod tutuicmptunnel

# Server
ssh a320 sudo rmmod tutuicmptunnel
```

This completes an `iperf3` throughput test based on the `ICMP` tunnel.
