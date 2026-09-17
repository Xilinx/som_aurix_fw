#!/usr/bin/env python3
"""gen_ucb_otp0.py enable|disable OUT.hex — SWAPEN in UCB_OTP0 ORIG+COPY, block left UNLOCKED (reversible)"""
import struct, sys
from intelhex import IntelHex

OTP0 = (0xAF404000, 0xAF405000)      # UCB32 ORIG, UCB40 COPY
PROCONTP_OFF, CONFIRM_OFF = 0x1E8, 0x1F0
SWAPEN_ON, UNLOCKED = 0x00030000, 0x43211234
mode = sys.argv[1].lower()

ih = IntelHex()
for base in OTP0:
    if mode == 'enable':
        ih.puts(base + PROCONTP_OFF, struct.pack('<II', SWAPEN_ON, 0))
    ih.puts(base + CONFIRM_OFF, struct.pack('<II', UNLOCKED, 0))   # never 0x57B5327F here
ih.write_hex_file(sys.argv[2])