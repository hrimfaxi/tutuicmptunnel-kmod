#!/usr/bin/env python3

import binascii, sys

def csum16_add(sum, add):
    """
    16位校验和加法
    :param sum: 当前校验和
    :param add: 要加的值
    :return: 计算后的校验和
    """
    sum += add
    return ((sum & 0xFFFF) + (sum >> 16)) & 0xFFFF

def csum16_sub(sum, sub):
    return csum16_add(sum, ~sub & 0xFFFF)

def csum16(data):
    """
    计算16位校验和
    :param data: 要计算校验和的字节序列
    :return: 16位校验和
    """
    checksum = 0
    # 按16位分段并求和
    for i in range(0, len(data), 2):
        # 每次取2个字节（16位）
        chunk = data[i:i+2]
        # 如果不足2个字节，用0填充
        if len(chunk) < 2:
            chunk += b'\x00'
        # 将16位数据转换为整数并加到校验和中
        checksum = csum16_add(checksum, int.from_bytes(chunk, byteorder='big'))
    return checksum & 0xFFFF

if __name__ == "__main__":
    if len(sys.argv) != 2:
        print("Usage: python3 script.py <hex_string>")
        sys.exit(1)

    b = sys.argv[1]
    b = b.replace(' ', '')
    b = binascii.a2b_hex(b)

    checksum = csum16(b)
    print(f"16位校验和: {checksum:04x}")
