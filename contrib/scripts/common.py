#!/usr/bin/python3

import subprocess, re, socket, struct, ctypes

class PrettyBigEndianStructure(ctypes.BigEndianStructure):
    def __str__(self):
        print_byte = lambda x: bytearray(x) if isinstance(x, (bytes, bytearray, ctypes.Array)) else x
        return "{}: {{{}}}".format(self.__class__.__name__, ", ".join(
            [
                "{}: {}".format(field[0], print_byte(getattr(self, field[0])))
                for field in self._fields_
            ]
        ))

def get_bpf_map_id(map_name):
    # 获取所有 map 的信息
    result = subprocess.run(
        ["bpftool", "map", "show"],
        stdout=subprocess.PIPE,
        text=True,
        check=True
    )
    # 逐行查找名字匹配的行
    for line in result.stdout.splitlines():
        if f"name {map_name}" in line:
            # id: 在行首，格式如 "49: hash  name config_map  flags 0x0"
            m = re.match(r"(\d+):", line)
            if m:
                id_ = int(m.group(1))
                print (f"{map_name} id: {id_}")
                return id_
    raise RuntimeError(f"Map {map_name} not found.")

# 转换ip字符为主机字序的整数
def to_ip(ip_str):
    packed_ip = socket.inet_aton(ip_str)
    ip_uint32 = struct.unpack(">I", packed_ip)[0]
    return ip_uint32

def call_verbose(cmd):
    print (' '.join(cmd))
    return subprocess.run(cmd, check=True)

def bpf_ntohs(x):
    # x 是一个 16bit int，表示网络字节序
    # 转成2字节，再用主机序解析
    return int.from_bytes(x.to_bytes(2, 'big'), byteorder='little')

def bpf_htons(x):
    return bpf_ntohs(x)
