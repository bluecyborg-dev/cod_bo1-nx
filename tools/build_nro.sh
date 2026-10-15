#!/usr/bin/env bash
# Builds the Switch executable (out/nx/bo1-nx.nro) inside the official devkitpro/devkita64 Docker image
# (devkitA64 GCC + libnx + portlibs). Adapted from masseffect-nx's tools/build_nro.sh.
#
#   tools/build_nro.sh
#
# Prerequisites, in order:
#   1. tools/build_host.sh            the code generator (fetches sdk/thirdparty too)
#   2. tools/codegen.sh               app/generated/ (the game translated to C++, from your own default.xex)
#   3. mesa/build_mesa_docker.sh      the NVK Vulkan driver: <MESA_SDK>/opt/devkitpro/portlibs/switch/lib/libvulkan.a
#
# Environment (all optional):
#   MESA_SDK   folder produced by mesa/build_mesa_docker.sh (default out/mesa-sdk)
#   NRO_OUT    build folder, relative to the repository (default out/nx)
#   JOBS       parallel compile jobs inside the container (default 4; each needs 1-2 GB of Docker memory)
#   IMAGE      Docker image (default devkitpro/devkita64:latest)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SDK_DIR="$ROOT/sdk"
NRO_OUT="${NRO_OUT:-out/nx}"
JOBS="${JOBS:-4}"
IMAGE="${IMAGE:-devkitpro/devkita64:latest}"
MESA_SDK="${MESA_SDK:-$ROOT/out/mesa-sdk}"

command -v docker >/dev/null 2>&1 || { echo "error: docker not found in PATH" >&2; exit 1; }
docker info >/dev/null 2>&1 || { echo "error: the Docker daemon is not running" >&2; exit 1; }
[[ -f "$MESA_SDK/opt/devkitpro/portlibs/switch/lib/libvulkan.a" ]] || {
  echo "error: no NVK driver at $MESA_SDK/opt/devkitpro/portlibs/switch/lib/libvulkan.a (run mesa/build_mesa_docker.sh)" >&2
  exit 1
}
MESA_SDK="$(cd "$MESA_SDK" && pwd)"
[[ -f "$ROOT/app/generated/rexglue.cmake" && -f "$ROOT/app/generated/default/sources.cmake" ]] || {
  echo "error: app/generated is missing. Run tools/codegen.sh first" >&2
  exit 1
}
[[ -f "$SDK_DIR/thirdparty/fmt/include/fmt/format.h" ]] || {
  echo "error: $SDK_DIR/thirdparty is incomplete. Run python3 tools/fetch_thirdparty.py" >&2
  exit 1
}

# The repository is mounted at the same absolute path inside the container, so paths in generated files stay valid.
mounts=(-v "$ROOT:$ROOT")
case "$MESA_SDK/" in "$ROOT"/*) ;; *) mounts+=(-v "$MESA_SDK:$MESA_SDK:ro") ;; esac

echo "== configure and build ($NRO_OUT, -j$JOBS, image $IMAGE)"
docker run --rm "${mounts[@]}" -w "$ROOT" \
  -e JOBS="$JOBS" -e ROOT="$ROOT" -e SDK_DIR="$SDK_DIR" -e MESA_SDK="$MESA_SDK" -e NRO_OUT="$NRO_OUT" \
  "$IMAGE" bash -euo pipefail -c '
    cmake -S app -B "$NRO_OUT" -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE="$ROOT/tools/switch/cmake/switch-devkitA64.cmake" \
      -DDEVKITPRO=/opt/devkitpro -DREXSDK_DIR="$SDK_DIR" \
      -DREXGLUE_SWITCH_NVK_SDK="$MESA_SDK/opt/devkitpro/portlibs/switch"
    cmake --build "$NRO_OUT" -j "$JOBS"
    # crt0 must be first in .text or the NRO does not boot
    /opt/devkitpro/devkitA64/bin/aarch64-none-elf-nm "$NRO_OUT/bo1" | grep "^0000000000000000 T _start$" >/dev/null
    echo "OK: _start at 0"
  '

# The app looks for game_root/ and bo1.toml next to itself (SD card: switch/bo1-nx/). Keep the unstripped ELF
# (out/nx/bo1) of every build that leaves this machine: crash logs are resolved against it with addr2line.
cp -f "$ROOT/$NRO_OUT/bo1.nro" "$ROOT/$NRO_OUT/bo1-nx.nro"
ls -la "$ROOT/$NRO_OUT/bo1-nx.nro"
echo "ELF with symbols: $ROOT/$NRO_OUT/bo1"
