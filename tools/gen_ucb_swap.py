#!/usr/bin/env python3
"""gen_ucb_swap.py a|b OUT.hex — full 512-byte UCB_SWAP ORIG+COPY blocks"""
import struct, sys
from intelhex import IntelHex

SWAP_ORIG, SWAP_COPY = 0xAF402E00, 0xAF403E00
UCB_SIZE, CONFIRM = 0x200, 0x57B5327F
UNLOCKED = 0x43211234
marker = {'a': 0x55, 'b': 0xAA}[sys.argv[1].lower()]

ih = IntelHex()
for base in (SWAP_ORIG, SWAP_COPY):
    block = bytearray(UCB_SIZE)                                    # 512 bytes, all 0x00
    struct.pack_into('<IIII', block, 0x000, marker, base, CONFIRM, base + 8)   # entry 0
    struct.pack_into('<II',   block, 0x1F0, UNLOCKED, 0x00000000)              # block confirmation
    ih.puts(base, bytes(block))

ih.write_hex_file(sys.argv[2])