#!/usr/bin/env python3
"""gen_ucb_swap.py a|b OUT.hex — clean UCB_SWAP (ORIG+COPY, entry 0) and restore stray blocks"""
import struct, sys
from intelhex import IntelHex

SWAP_ORIG, SWAP_COPY = 0xAF402E00, 0xAF403E00
STRAY = (0xAF405000, 0xAF405E00)          # OTP0_COPY, OTP7_COPY — restore to factory unlocked
CONFIRM, UNLOCKED = 0x57B5327F, 0x43211234
marker = {'a': 0x55, 'b': 0xAA}[sys.argv[1].lower()]

ih = IntelHex()
for base in (SWAP_ORIG, SWAP_COPY):
    ih.puts(base, struct.pack('<IIII', marker, base, CONFIRM, base + 8))   # entry 0
    ih.puts(base + 0x1F0, struct.pack('<II', UNLOCKED, 0))               # block confirmation, as factory
for base in STRAY:
    ih.puts(base + 0x1F0, struct.pack('<II', UNLOCKED, 0))               # erase + unlocked only
ih.write_hex_file(sys.argv[2])