#!/usr/bin/env python3
"""dual_bank.py A.hex B.hex OUT.hex — A as-is, B's PFlash code relocated to Bank B (+0x400000)"""
import sys
from intelhex import IntelHex

BANK_OFF, BANK_SIZE = 0x400000, 0x400000
SEGMENTS = (0x80000000, 0xA0000000)               # cached and non-cached PFlash aliases

a, b, out = IntelHex(sys.argv[1]), IntelHex(sys.argv[2]), IntelHex()
out.merge(a, overlap='replace')

moved = 0
for addr in b.addresses():
    for seg in SEGMENTS:
        if seg <= addr < seg + BANK_SIZE:
            out[addr + BANK_OFF] = b[addr]
            moved += 1
out.write_hex_file(sys.argv[3])
print(f"relocated {moved} bytes of B; span {hex(out.minaddr())}..{hex(out.maxaddr())}")