#!/usr/bin/env python3
"""merge_ucb.py APP.hex UCB1.hex [UCB2.hex ...] OUT.hex"""
import sys
from intelhex import IntelHex

app = IntelHex(sys.argv[1])
blocks = set()
for frag in sys.argv[2:-1]:
    ucb = IntelHex(frag)
    for base in {a & ~0x1FF for a in ucb.addresses()}:
        del app[base:base + 0x200]
        blocks.add(base)
    app.merge(ucb, overlap='replace')

app.write_hex_file(sys.argv[-1])
print("UCB blocks in image:", [hex(b) for b in sorted(blocks)])