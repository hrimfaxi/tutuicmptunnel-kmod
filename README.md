[English](./README.md) | [简体中文](./README_zh-CN.md)

---

# tutuicmptunnel-kmod

An `nftables`-based tool for tunneling UDP traffic over ICMP, serving as an alternative to `udp2raw`'s ICMP mode.

It is recommended to use this tool together with `kcptun`, `hysteria`, `wireguard`, and similar tools. This combination helps to effectively bypass increasingly strict UDP QOS and packet loss policies imposed by the `GFW` or ISPs, greatly improving penetration capability and connection stability.

## Advantages and Features

1.  Several times faster than `udp2raw` in maximum throughput under the same CPU load, while consuming significantly less CPU resources. See [Performance Benchmarks](https://github.com/hrimfaxi/tutuicmptunnel/blob/master/docs/benchmark.md).
2.  Secure by design and implementation.
3.  Compared to the `bpf`-based `tutuicmptunnel`, it is about 22% faster. It supports `openwrt` without recompiling the kernel.
4.  Supports `ICMP`/`ICMPv6` for both `IPv4`/`IPv6`.
5.  Allows for secure and rapid synchronization of server/client configurations using `tuctl_server`.

## Overview

`tutuicmptunnel-kmod` consists of two parts: a server and a client. Each host can only act as one role, not both.
To distinguish packets from different clients, each client connected to the server is assigned a unique `UID` ranging from 0 to 255.
This `UID` corresponds to the `code` field in the ICMP protocol, meaning each server can support up to 256 clients.

A client can also map to different servers. For instance, traffic to host `1.2.3.4` on port `3322` can be mapped to `UID` 100,
while traffic to `2.3.4.5` on port `2233` can be mapped to `UID` 101.
This allows a client to flexibly manage access to multiple servers using different `UID`s.

Clients are allowed to use the same `UID` on different servers, as the `UID` only identifies the client's identity on a specific server.
In other words, the `UID` is unique only within the scope of each server and can be reused across different servers.

*   The client uses a 3-tuple [`UID`, Server `IP`, Destination Port (`port`)] to identify which UDP packets need to be converted to ICMP.
*   The server uses a 3-tuple [`UID`, Client `IP`, Destination Port (`port`)] to identify which ICMP packets need to be restored and forwarded as UDP.
*   The `IP` address can be `IPv4` or `IPv6`. `tutuicmptunnel-kmod` will automatically select `ICMP` or `ICMPv6` for encapsulation and forwarding based on the IP type.

`tutuicmptunnel-kmod` can be paired with tools like `WireGuard`, `xray-core`+`kcptun`, and `hysteria`.
Since these applications already provide encryption and integrity checks, `tutuicmptunnel-kmod` does not handle data encryption, obfuscation, or validation, focusing solely on data encapsulation and forwarding.

`tutuicmptunnel-kmod` does not modify the payload of data packets, nor does it add extra `IP` headers.
Forwarding rules on the server are configured entirely by the user manually adding the aforementioned 3-tuples via commands.

The client can invoke server commands via `SSH`: using the [ktuctl](ktuctl/README.md) command to manually modify the 3-tuples (including updating the client's own `IP` address).
To facilitate dynamic updates, the `tuctl_client` tool can be used to synchronize configuration via the UDP protocol, allowing the client to notify the server of its new IP and port information.

`tuctl_server` and `tuctl_client` communicate over UDP, using a timestamp-based mechanism combined with the `XChaCha20-Poly1305` encryption algorithm, `Argon2id` key derivation function, and a Pre-Shared Key (`PSK`) for secure authentication. This scheme enables clients to securely and efficiently notify the server of new configuration information in real-time.

`tutuicmptunnel-kmod` is based on the `nftables` kernel module mechanism. It can communicate interchangeably with `tutuicmptunnel` (one as server, the other as client). It has the following advantages:
1. About 22% faster than `tutuicmptunnel`.
2. It does not require recompiling the kernel to support `openwrt`.

## OS Requirements and Dependencies

### `Ubuntu`

Version: At least 20.04, with version 24.04 LTS or newer recommended.

Dependencies:

```sh
sudo apt install -y git libsodium-dev dkms build-essential linux-headers-$(uname -r) flex bison libmnl-dev
```

### `Arch Linux`

Version: Latest is sufficient

Dependency Preparation:

```sh
sudo pacman -S git libsodium dkms base-devel linux-headers flex bison libmnl
```
### `Openwrt`

Version: At least 24.10.1, please see the [OpenWrt Guide](docs/openwrt.md)

## Installation Method

1.  Check out the code and install

    ```sh
    git clone https://github.com/hrimfaxi/tutuicmptunnel-kmod
    cd tutuicmptunnel-kmod
    cmake -DCMAKE_BUILD_TYPE=Release -DENABLE_HARDEN_MODE=1 .
    ```

    ```sh
    make
    sudo make install
    ```

2.  Kernel Module

    Both the server and client need to install the [tutuicmptunnel.ko](kmod/README.md) kernel module. This is the main program of `tutuicmptunnel-kmod`.

    ```sh
    cd kmod
    sudo dkms remove tutuicmptunnel/x.x --all # If an older version was previously installed, use dkms status to check past versions
    sudo make dkms
    sudo tee -a /etc/modules-load.d/modules.conf <<< tutuicmptunnel
    sudo modprobe tutuicmptunnel
    ```

    Some systems require setting the `force_sw_checksum` parameter. For details, see [kmod](kmod/README.md#force_sw_checksum).

3.  Server: Set up the system service and enable the optional `tuctl_server`

    `tuctl_server` allows clients to control the `tutuicmptunnel-kmod` configuration on the server side.

    Please meet the following requirements:
    *   To prevent brute-force attacks, remember to choose a sufficiently strong `PSK` password before use. It is best to generate one using the `uuidgen -r` command.
    *   Since timestamps are used for validation, both the server and client require accurate system time.

    ```sh
    # Automatically load the tutuicmptunnel-kmod service and restore configuration on boot
    sudo cp contrib/etc/systemd/system/tutuicmptunnel-kmod-server@.service /etc/systemd/system/
    sudo systemctl enable --now tutuicmptunnel-kmod-server@eth0.service # where eth0 is the server's network interface

    # Optional tuctl_server
    sudo cp contrib/etc/systemd/system/tutuicmptunnel-tuctl-server.service /etc/systemd/system/
    # Modify psk, port, etc.
    sudo vim /etc/systemd/system/tutuicmptunnel-tuctl-server.service
    timedatectl | grep "System clock synchronized:" # Check if the system time is synchronized with NTP
    # Reload configuration
    sudo systemctl daemon-reload
    # Enable the tuctl_server service
    sudo systemctl enable --now tutuicmptunnel-tuctl-server
    ```

    At this point, you can use the `ktuctl` command to check the server status:
    ```sh
    sudo ktuctl
    tutuicmptunnel-kmod: Role: Server, BPF build type: Release

    Peers:
    ....
    ```

    Alternatively, you can manually start `tutuicmptunnel-kmod` in server mode without using the `systemd` service:

    ```sh
    sudo modprobe -r tutuicmptunnel # Unload tutuicmptunnel module
    sudo modprobe tutuicmptunnel # Load tutuicmptunnel module
    sudo ktuctl server # Set to server mode
    sudo ktuctl server-add uid 123 address 1.2.3.4 port 1234 # Add a client (id 123) with IP 1.2.3.4 and destination UDP port 1234
    sudo ktuctl server-del uid 123 # Delete the previous client
    ```

4.  Optional: Set up a `UID` and hostname mapping table

    To facilitate `UID` management, `tutuicmptunnel-kmod` supports mapping client hostnames to `UID`s via the `/etc/tutuicmptunnel/uids` mapping file. You can create and edit this file as follows:

    ```sh
    sudo mkdir -p /etc/tutuicmptunnel
    sudo vim /etc/tutuicmptunnel/uids
    ```

    The format is as follows:

    ```
    #
    # Format: UID hostname # Optional comment
    #

    0 alice # alice's laptop
    1 bob   # bob's laptop
    ```

    After configuration, in the `tuctl` (or `ktuctl`) command, any place that requires specifying a `UID` (e.g., `uid 0`) can be replaced with the hostname (e.g., `user alice`), making management more intuitive and convenient.

5.  Client: Set up the system service and enable `tutuicmptunnel-kmod`

    ```sh
    # Set tutuicmptunnel-kmod to start on boot
    sudo cp contrib/etc/systemd/system/tutuicmptunnel-kmod-client@.service /etc/systemd/system/
    sudo systemctl enable --now tutuicmptunnel-kmod-client@enp4s0 # Assuming your internet interface is enp4s0
    ```

    Now you can try out `tutuicmptunnel-kmod`:

    ```sh
    export ADDRESS=yourserver.com # Server domain name or IP
    export PORT=3322 # UDP port to be converted to ICMP
    export TUTU_UID=123 # tutuicmptunnel user ID
    export PSK=yourlongpsk # PSK for tuctl_server
    export SERVER_PORT=14801 # Port for tuctl_server
    export COMMENT=yourname # A description of your client's identity, the comment can be viewed in the tuctl command output on the server after the command succeeds

    # Set to client mode
    sudo ktuctl client
    # Set the server's endpoint configuration
    sudo ktuctl client-add uid $TUTU_UID address $ADDRESS port $PORT
    # Verify if correct
    sudo ktuctl status
    # Use 3322.net to get the client's public IP
    IP=$(curl -s ip.3322.net)
    # Use tuctl_client to notify the server of the new client settings
    tuctl_client psk $PSK server $ADDRESS server-port $SERVER_PORT <<< "server-add uid $TUTU_UID address $IP port $PORT comment $COMMENT"
    ```

    At this point, you can go to the server and use the `sudo ktuctl` command to view the rules added by the client:

    ```sh
    tutuicmptunnel-kmod: Role: Server, BPF build type: Release

    Peers:
      User: xxxx, Address: xxx.xxx.xxx.xxx, Sport: 37926, Dport: 3322, ICMP: 11403, Comment: yourname
    ```

    If everything is configured correctly, the `UDP` communication between the client and the server, originally with the client as the source address and 3322 as the destination port, will be automatically converted into `ICMP` packets by `tutuicmptunnel-kmod` on the client side. After the server receives the `ICMP` packet, it will restore it to a `UDP` packet and continue forwarding. Throughout the transmission process, intermediate network nodes can only see `ICMP Echo`/`Reply` packets.

## Main Application Scenarios

| Name                                      | Introduction                                                                                             |
| :---------------------------------------- | :------------------------------------------------------------------------------------------------------- |
| [iperf3](docs/iperf3.md)                  | A powerful network performance testing tool used to measure bandwidth, jitter, and packet loss.            |
| [hysteria](docs/hysteria.md)              | A proxy tool based on the QUIC protocol, optimized for unstable and high-loss networks.                  |
| [xray+kcptun](docs/xray_kcptun.md)        | A combination of the Xray core and the KCPtun protocol, used to accelerate and stabilize network connections. |
| [xray+mkcp](docs/xray_mkcp.md)            | Xray core with its native mKCP implementation, used to accelerate and stabilize network connections. |
| [wireguard](docs/wireguard.md)            | A modern, high-performance, and easy-to-configure secure VPN tunnel.                                     |
| [openwrt](docs/openwrt.md)                | A highly customizable Linux operating system for embedded devices, especially routers.                   |

## Acknowledgements

During its design, implementation, and performance tuning, `tutuicmptunnel-kmod` has referenced and benefited from numerous excellent open-source projects and technical articles. We extend our sincere gratitude to their authors and community contributors!

* [hysteria](https://github.com/apernet/hysteria)
* [kcptun](https://github.com/xtaci/kcptun)
* [xray-core](https://github.com/XTLS/Xray-core)
* [udp2raw](https://github.com/wangyu-/udp2raw)
* [mimic](https://github.com/hack3ric/mimic)
* [libbpf-bootstrap](https://github.com/libbpf/libbpf-bootstrap)

Special thanks to [@hack3ric](https://github.com/hack3ric) and all contributors for their continuous maintenance, making `UDP`→`fakeTCP` obfuscation on `eBPF` possible.

## License

This project as a whole adheres to the **GNU General Public License v2.0**.
The `libbpf` and `bpftool` submodules retain their respective original licenses (`LGPL-2.1` / `BSD-2-Clause`).
