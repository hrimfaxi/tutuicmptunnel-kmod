#!/usr/bin/env python3

import binascii, socket, sys

def main():
	if len(sys.argv) < 2:
		print ("Usage: hextoip <hex_str>")
		sys.exit(1)
	ip = sys.argv[1]
	if ip.startswith("0x"):
		ip = ip[2:]
	b = binascii.a2b_hex(ip)
	print (socket.inet_ntoa(b))

if __name__ == "__main__":
    main()
