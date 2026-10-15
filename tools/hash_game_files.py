#!/usr/bin/env python3
"""Reference hashes for bo1_debug_hash_files: FNV-1a 64 per MiB and of the whole file, the same as the game computes.

Usage: python3 tools/hash_game_files.py <game folder> file1 [file2 ...]
Compare with the "bo1: hash" lines of a Switch log: tools/hash_game_files.py game common_zombie.ff > ref.txt
"""
import sys

def fnv(data, h=1469598103934665603):
    for b in data:
        h = ((h ^ b) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h

root = sys.argv[1]
for name in sys.argv[2:]:
    whole = 1469598103934665603
    with open(f"{root}/{name}", "rb") as f:
        index = 0
        while True:
            chunk = f.read(1 << 20)
            if not chunk:
                break
            print(f"{name} MiB {index} {fnv(chunk):016x}")
            whole = fnv(chunk, whole)
            index += 1
    print(f"{name} whole {whole:016x}")
