#!/usr/bin/env python3
"""Compares every file of the disc image with the extracted copy (e.g. on the SD card), in 1 MiB chunks.

Usage: python3 tools/verify_sd.py <iso> <game_root folder>
Run it after remounting the card, so the reads come from the card and not from the Mac page cache."""
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import extract_iso as x
iso, root = sys.argv[1], sys.argv[2]
fh = open(iso, 'rb'); base = x.find_partition(fh); base = base[0] if isinstance(base, tuple) else base
rs, rz = x.read_volume_descriptor(fh, base)
CH = 1 << 20
bad_files = 0; bad_chunks = 0; total = 0
for path, e in x.walk(fh, base, rs, rz):
    if e['is_dir']: continue
    p = os.path.join(root, path)
    if not os.path.exists(p) or os.path.getsize(p) != e['size']:
        print('MISSING/SIZE', path); bad_files += 1; continue
    fh.seek(base + e['sector'] * 2048); bad = []
    with open(p, 'rb') as f:
        off = 0
        while off < e['size']:
            n = min(CH, e['size'] - off)
            if fh.read(n) != f.read(n): bad.append(off)
            off += n
    total += e['size']
    if bad:
        bad_files += 1; bad_chunks += len(bad)
        print(f'BAD {path}: {len(bad)} MiB chunks differ, first at {bad[0]:#x}', flush=True)
print(f'checked {total} bytes; {bad_files} bad files, {bad_chunks} bad 1 MiB chunks')
sys.exit(1 if bad_files else 0)
