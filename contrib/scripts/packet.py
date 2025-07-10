#!/usr/bin/env python3

import re
import sys
import binascii
from scapy.all import *
from hexdump import hexdump
from checksum import csum16

if __name__ == "__main__":
    input_ = sys.argv[1].replace(" ", "").replace("\t", "").replace('\n', '')
    hexstr = binascii.a2b_hex(input_).hex()
    print (hexstr)
    pkt = IP(bytes.fromhex(hexstr))
    pkt.show2()

    # 清除校验和以便自动重算
    if hasattr(pkt, 'chksum'):
        pkt.chksum = None
    if pkt.haslayer(ICMP):
        pkt[ICMP].chksum = None

    # show2() 会触发重算
    pkt.show2()

    # 获得重算过校验和的原始包字节
    rebuilt_bytes = bytes(pkt)
    print("Recalculated packet(hex):", rebuilt_bytes.hex())
