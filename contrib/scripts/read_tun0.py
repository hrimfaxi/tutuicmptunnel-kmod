#!/usr/bin/env python3

import os, fcntl, struct

TUNSETIFF = 0x400454ca
IFF_TUN   = 0x0001

fd = os.open('/dev/net/tun', os.O_RDWR)
ifr = struct.pack('16sH', b'tun0', IFF_TUN)
fcntl.ioctl(fd, TUNSETIFF, ifr)

while True:
    pkt = os.read(fd, 2048)
    print("Got packet:", pkt.hex())
