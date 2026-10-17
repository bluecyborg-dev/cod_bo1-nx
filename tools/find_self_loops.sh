#!/usr/bin/env bash
# Lists the branches to themselves in a Switch ELF (an instruction like `b .`, `cbz w0, .` or `b.ne .`). GCC writes
# one when it hoists the load of a polling loop out of the loop, which hangs the thread forever: check every build
# made with BO1_GUEST_PLAIN_MEMORY=ON against the default build (genuine guest halts appear in both).
#
#   tools/find_self_loops.sh [elf]        (default out/nx-fast/bo1)
#
# Runs objdump in the devkitpro/devkita64 Docker image that tools/build_nro.sh uses.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ELF="${1:-$ROOT/out/nx-fast/bo1}"
IMAGE="${IMAGE:-devkitpro/devkita64:latest}"
[[ -f "$ELF" ]] || { echo "error: $ELF not found" >&2; exit 1; }
ELF="$(cd "$(dirname "$ELF")" && pwd)/$(basename "$ELF")"
docker run --rm -v "$ROOT:$ROOT" -v "$(dirname "$ELF"):$(dirname "$ELF")" "$IMAGE" \
  /opt/devkitpro/devkitA64/bin/aarch64-none-elf-objdump -d --no-show-raw-insn "$ELF" |
  awk '
    /^[0-9a-f]+ <.*>:$/ { fn = $2 }
    $1 ~ /^[0-9a-f]+:$/ {
      addr = substr($1, 1, length($1) - 1)
      op = $2
      if (op ~ /^(b|b\..*|cbz|cbnz|tbz|tbnz)$/) {
        target = $NF ~ /^<.*>$/ ? $(NF - 1) : $NF
        n = split(target, parts, ",")
        target = parts[n]
        if (target == addr) { print fn, addr, op; count++ }
      }
    }
    END { printf "%d branch(es) to self\n", count + 0 > "/dev/stderr" }'
