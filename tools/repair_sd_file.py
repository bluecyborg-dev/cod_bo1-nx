#!/usr/bin/env python3
"""Replaces damaged game files on the (failing) SD card with fresh copies from the disc image, and verifies them.

Usage: python3 tools/repair_sd_file.py <volume> <file> [<file> ...] [--iso cod-bo.iso] [--force]
  e.g. python3 tools/repair_sd_file.py /Volumes/SWITCH common_zombie.ff

For each file (a path inside game_root):
  1. Compares the copy on the card with the disc, per MiB (after a remount, so the reads come from the card), and
     prints which MiB differ.
  2. Moves the damaged copy into switch/bo1-nx/bad_blocks_do_not_delete/ (never overwritten in place: its bad
     clusters stay allocated, so no new file lands on them).
  3. Writes a fresh copy from the disc, unmounts and mounts the card again, and compares every byte. Retries (moving
     each failed copy to the quarantine folder too) up to 4 times.
--force rewrites a file even when the Mac reads it correctly: seen on the development card, where the Switch read 3
MiB of common_zombie.ff wrong while the Mac read the same file right, so the copy has to move to other clusters.
"""
import argparse
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import extract_iso as x

CH = 1 << 20


def remount(volume):
    dev = subprocess.run(['diskutil', 'info', volume], capture_output=True, text=True, check=True).stdout
    dev = next(l.split(':', 1)[1].strip() for l in dev.splitlines() if 'Device Identifier' in l)
    subprocess.run(['sync'], check=True)
    subprocess.run(['diskutil', 'unmount', volume], check=True, stdout=subprocess.DEVNULL)
    subprocess.run(['diskutil', 'mount', dev], check=True, stdout=subprocess.DEVNULL)


def bad_chunks(fh, start, size, path):
    """MiB indexes where the file at path differs from the disc bytes at start."""
    if not os.path.exists(path) or os.path.getsize(path) != size:
        return None
    fh.seek(start)
    bad = []
    with open(path, 'rb') as f:
        for i in range((size + CH - 1) // CH):
            n = min(CH, size - i * CH)
            if fh.read(n) != f.read(n):
                bad.append(i)
    return bad


def main():
    p = argparse.ArgumentParser()
    p.add_argument('volume')
    p.add_argument('files', nargs='+')
    p.add_argument('--force', action='store_true', help='rewrite even files that read back correctly here')
    p.add_argument('--iso', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'cod-bo.iso'))
    a = p.parse_args()

    app = os.path.join(a.volume, 'switch', 'bo1-nx')
    root = os.path.join(app, 'game_root')
    quarantine = os.path.join(app, 'bad_blocks_do_not_delete')
    os.makedirs(quarantine, exist_ok=True)

    fh = open(a.iso, 'rb')
    base = x.find_partition(fh)
    base = base[0] if isinstance(base, tuple) else base
    rs, rz = x.read_volume_descriptor(fh, base)
    entries = {path.lower(): e for path, e in x.walk(fh, base, rs, rz) if not e['is_dir']}

    remount(a.volume)
    failed = 0
    for name in a.files:
        e = entries.get(name.lower())
        if not e:
            print(f'{name}: not on the disc'); failed += 1; continue
        start, size = base + e['sector'] * 2048, e['size']
        dest = os.path.join(root, name)
        bad = bad_chunks(fh, start, size, dest)
        if bad == [] and not a.force:
            print(f'{name}: already matches the disc, nothing to do'); continue
        print(f'{name}: ' + ('missing or wrong size' if bad is None else f'{len(bad)} MiB differ: {bad}'))
        for attempt in range(1, 5):
            if os.path.exists(dest):
                q = os.path.join(quarantine, f'{os.path.basename(name)}.{time.strftime("%Y%m%d-%H%M%S")}.{attempt}')
                os.rename(dest, q)
                print(f'  moved the damaged copy to {os.path.relpath(q, a.volume)}')
            fh.seek(start)
            with open(dest, 'wb') as out:
                left = size
                while left:
                    buf = fh.read(min(CH, left))
                    out.write(buf)
                    left -= len(buf)
                out.flush()
                os.fsync(out.fileno())
            remount(a.volume)
            bad = bad_chunks(fh, start, size, dest)
            if bad == []:
                print(f'  attempt {attempt}: written and verified after remount')
                break
            print(f'  attempt {attempt}: still wrong after remount: {bad}')
        else:
            failed += 1
    for junk in subprocess.run(['find', app, '-name', '._*'], capture_output=True, text=True).stdout.splitlines():
        os.remove(junk)
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
