#!/usr/bin/env python3
"""Post-codegen pass over app/generated/default (run by tools/codegen.sh; safe to run again).

1. Guest memory accesses: the REX_LOAD_* / REX_STORE_* macros of bo1_pch.h access memory through REX_GUEST_MEM,
   which is `volatile` unless the build defines it (CMake option BO1_GUEST_PLAIN_MEMORY defines it empty). With plain
   accesses GCC keeps guest values in registers across neighbouring instructions; masseffect-nx measured +15-20 %.
2. Loop barriers: REX_LOOP_BARRIER() after every label a backward goto reaches (a loop header). Without volatile, GCC
   -O3 hoists the load of a polling loop (`while (flag == 0) {}` waiting for another thread) out of the loop and turns
   it into a branch to itself: futari-nx had 172 such hangs. The barrier is an empty asm with a memory clobber, so
   the loop reads memory again on every pass. It expands to nothing in the default (volatile) build.

The idea comes from futari-nx's tools/perf_codegen_post.py (passes 2 and 3); this is our own implementation.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GEN = ROOT / "app" / "generated" / "default"

PCH_MARKER = "// bo1-nx codegen_post: REX_GUEST_MEM"
PCH_BLOCK = """// bo1-nx codegen_post: REX_GUEST_MEM
// Qualifier of guest memory accesses: volatile unless the build defines it (BO1_GUEST_PLAIN_MEMORY defines it empty).
// REX_LOOP_BARRIER() sits at every loop header of the generated code: with plain accesses it keeps polling loops
// reading memory; with volatile accesses it is not needed and expands to nothing.
#ifndef REX_GUEST_MEM
#define REX_GUEST_MEM volatile
#define REX_LOOP_BARRIER() ((void)0)
#else
#define REX_LOOP_BARRIER() __asm__ __volatile__("" ::: "memory")
#endif
"""

LABEL = re.compile(r"^(loc_[0-9A-F]{8}):\s*$")
GOTO = re.compile(r"goto (loc_[0-9A-F]{8});")
FUNC_START = re.compile(r"^(DEFINE_REX_FUNC|PPC_FUNC_IMPL|PPC_FUNC)\b")
BARRIER = "\tREX_LOOP_BARRIER();"


def patch_pch(path: Path) -> bool:
    text = path.read_text()
    if PCH_MARKER in text:
        return False
    # Only the plain load/store macros: the MMIO helpers keep volatile.
    plain = re.compile(r"^#define REX_(LOAD|STORE)(_D)?_U(8|16|32|64)\(")
    new = "\n".join(
        re.sub(r"\(\*\(volatile (u8|u16|u32|u64)\*\)", r"(*(REX_GUEST_MEM \1*)", line) if plain.match(line) else line
        for line in text.split("\n"))
    anchor = "#define REX_RAW_ADDR(x)"
    if anchor not in new:
        sys.exit(f"error: {path} has no '{anchor}' line to put the qualifier before")
    new = new.replace(anchor, PCH_BLOCK + "\n" + anchor, 1)
    path.write_text(new)
    return True


def patch_source(path: Path) -> int:
    lines = path.read_text().split("\n")
    out = []
    added = 0
    i = 0
    n = len(lines)
    while i < n:
        # One function at a time: from a function start to the line before the next one.
        if not FUNC_START.match(lines[i]):
            out.append(lines[i])
            i += 1
            continue
        j = i + 1
        while j < n and not FUNC_START.match(lines[j]):
            j += 1
        body = lines[i:j]
        label_line = {}
        for k, line in enumerate(body):
            m = LABEL.match(line)
            if m:
                label_line[m.group(1)] = k
        headers = set()
        for k, line in enumerate(body):
            for target in GOTO.findall(line):
                at = label_line.get(target)
                if at is not None and at <= k:
                    headers.add(at)
        for k, line in enumerate(body):
            out.append(line)
            if k in headers and not (k + 1 < len(body) and body[k + 1].strip() == BARRIER.strip()):
                out.append(BARRIER)
                added += 1
        i = j
    if added:
        path.write_text("\n".join(out))
    return added


def main() -> None:
    pch = GEN / "bo1_pch.h"
    if not pch.exists():
        sys.exit(f"error: {pch} not found; run tools/codegen.sh first")
    pch_changed = patch_pch(pch)
    total = 0
    files = sorted(GEN.glob("bo1_recomp.*.cpp"))
    for src in files:
        total += patch_source(src)
    print(f"codegen_post: pch {'patched' if pch_changed else 'already patched'}; "
          f"{total} loop barriers added in {len(files)} files")


if __name__ == "__main__":
    main()
