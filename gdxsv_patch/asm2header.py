#!/usr/bin/env python

import re, time

r_ope = re.compile(r'\s+?([0-9a-f]+):\s*([0-9a-f]+)')
r_symbol = re.compile(r"^([0-9a-f]+) <(\w+)>:")

start = False

f = open('bin/gdxsv_patch.inc', 'w')
for line in open('bin/gdxpatch.asm'):
    line = line.rstrip()
    if 'Disassembly' in line:
        start = True
    if not start:
        continue

    g = r_ope.match(line)
    if g:
        addr = int(g.group(1), 16)
        data = int(g.group(2), 16)
        f.write(f"gdxsv_WriteMem32(0x{addr:08x}u, 0x{data:08x}u);\n")

    g = r_symbol.match(line)
    if g:
        addr = int(g.group(1), 16)
        name = g.group(2)
        f.write(f'symbols["{name}"] = 0x{addr:08x}u;\n')

f.write(f'gdxsv_WriteMem32(0x005548e0u, symbols["gdx_dial_start"]);\n')
f.write(f'symbols[":patch_id"] = {str(int(time.time()) % 100000000)};\n')
f.write(f'gdxsv_WriteMem32(symbols["patch_id"], symbols[":patch_id"]);\n')
f.close()
