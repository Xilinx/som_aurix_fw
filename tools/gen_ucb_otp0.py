#!/usr/bin/env python3
"""gen_ucb_otp0.py enable|disable OUT.hex — full 512-byte UCB_OTP0 ORIG+COPY"""
import struct, sys
from intelhex import IntelHex

OTP0 = (0xAF404000, 0xAF405000)
SWAPEN_ON, UNLOCKED = 0x00030000, 0x43211234
mode = sys.argv[1].lower()

ih = IntelHex()
for base in OTP0:
    block = bytearray(0x200)
    if mode == 'enable':
        struct.pack_into('<II', block, 0x1E8, SWAPEN_ON, 0x00000000)
    struct.pack_into('<II', block, 0x1F0, UNLOCKED, 0x00000000)
    ih.puts(base, bytes(block))

ih.write_hex_file(sys.argv[2])