#!/usr/bin/env python3
"""gen_meta_clear.py OUT.hex — zero the SOTA metadata magic so BootValid skips BIST"""
import sys
from intelhex import IntelHex
META = 0xAF000000                     # <- DFLASH_SOTA_META address from DFlash.h
ih = IntelHex(); ih.puts(META, bytes(8)); ih.write_hex_file(sys.argv[1])