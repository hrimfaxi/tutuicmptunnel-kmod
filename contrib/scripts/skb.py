#!/usr/bin/env python3

import re
import sys
import binascii
from scapy.all import *
from hexdump import hexdump
from checksum import csum16

def parse_skb_log(lines):
    pattern = re.compile(r'skb\[(\d+)\]=0x([0-9a-fA-F]{2})')
    skb_dict = {}

    for line in lines:
        m = pattern.search(line)
        if not m:
            continue
        idx = int(m.group(1))
        val = int(m.group(2), 16)
        skb_dict[idx] = val

    if not skb_dict:
        raise RuntimeError("No skb log found.")

    # 检查连续性
    indexes = sorted(skb_dict.keys())
    for i in range(1, len(indexes)):
        if indexes[i] != indexes[i-1] + 1:
            raise RuntimeError(f"Discontinuous index found: {indexes[i-1]} -> {indexes[i]}")

    # 拼成二进制
    data = bytes([skb_dict[i] for i in indexes])
    data = binascii.hexlify(data).decode()
    return data

def showIPHdr(pkt):
    ip_bytes = bytes(pkt['IP'])
    ihl = pkt['IP'].ihl * 4  # ihl单位是4字节
    ip_header = ip_bytes[:ihl]
    print ("IP Header")
    hexdump(ip_header)

if __name__ == "__main__":
    if len(sys.argv) < 2:
        fn = "skb.txt"
    else:
        fn = sys.argv[1]
    with open(fn) as f:
        lines = f.readlines()
    hexstr = parse_skb_log(lines)
    print (hexstr)
    pkt = Ether(bytes.fromhex(hexstr))
    pkt.show2()
    if pkt.haslayer(IP):
        print("IP checksum in packet:", hex(pkt[IP].chksum))
    if pkt.haslayer(ICMP):
        print("ICMP checksum in packet:", hex(pkt[ICMP].chksum))
    if pkt.haslayer(UDP):
        print("UDP checksum in packet:", hex(pkt[UDP].chksum))
    print("Recomputing checksums...")
    if pkt.haslayer(IP):
        showIPHdr(pkt)

    # 清除校验和以便自动重算
    if hasattr(pkt, 'chksum'):
        pkt.chksum = None
    if pkt.haslayer(ICMP):
        pkt[ICMP].chksum = None
    if pkt.haslayer(UDP):
        pkt[UDP].chksum = None

    pkt = Ether(raw(pkt))
    pkt.show2()
    if pkt.haslayer(IP):
        print("IP checksum in packet:", hex(pkt[IP].chksum))
    if pkt.haslayer(ICMP):
        print("ICMP checksum in packet:", hex(pkt[ICMP].chksum))
    if pkt.haslayer(UDP):
        print("UDP checksum in packet:", hex(pkt[UDP].chksum))
    if pkt.haslayer(IP):
        showIPHdr(pkt)
    print ("payload")

    if pkt.haslayer(ICMP):
        d = bytes(pkt[ICMP].payload)
        hexdump(d)
        print (hex(csum16(d)))

    if pkt.haslayer(UDP):
        d = bytes(pkt[UDP].payload)
        hexdump(d)
    print (f"payload checksum: {hex(csum16(d))}")
