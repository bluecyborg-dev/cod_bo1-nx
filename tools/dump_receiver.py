#!/usr/bin/env python3
"""Receives the native renderer's trace image dumps sent over TCP (--bo1_debug_dump_host=<this Mac>:<port>) and
saves them as .img files (convert with tools/rgbx_to_png.py). One connection per dump: 64-byte name, 16-byte
header, data.

Usage: python3 tools/dump_receiver.py <folder> [port, default 28772]"""
import os
import socket
import sys

folder = sys.argv[1]
port = int(sys.argv[2]) if len(sys.argv) > 2 else 28772
os.makedirs(folder, exist_ok=True)
server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
server.bind(('0.0.0.0', port))
server.listen(4)
print(f'listening on {port}, saving to {folder}', flush=True)
while True:
    conn, _ = server.accept()
    chunks = []
    while True:
        data = conn.recv(1 << 20)
        if not data:
            break
        chunks.append(data)
    conn.close()
    blob = b''.join(chunks)
    if len(blob) < 80:
        continue
    name = blob[:64].split(b'\0')[0].decode(errors='replace')
    with open(os.path.join(folder, os.path.basename(name)), 'wb') as f:
        f.write(blob[64:])
    print(f'{name}: {len(blob) - 80} bytes', flush=True)
