#!/bin/bash
# Build the NVK Vulkan driver for the Nintendo Switch on macOS (arm64 or x86_64) or Linux, in Docker, and lay it out
# as the SDK folder that tools/build_nro.sh expects:
#
#     <OUT>/opt/devkitpro/portlibs/switch/lib/libvulkan.a
#
# Steps: clone danfromtico/mesa-switch at the pinned commit, apply mesa-switch-masseffect.patch and
# mesa-switch-p8-direct-draws.patch (P8_DIRECT_DRAWS=0 leaves it out: draws without the draw macro when their parameters are zero,
# and the memory query cached; project8-sw's patch 0003 as the-simpsons-game-nx ships it, MIT, merged by hand), run the upstream
# build-switch.sh (it builds the devkitpro-mesa-rust image from Docker.rust, then Mesa's host tools, then the Switch
# archives), and merge libnak_rs.a (the Rust runtime of the shader compiler) into libvulkan.a.
#
#   ./build_mesa_docker.sh                       # full build, result in ../../mesa-sdk (next to the repo)
#   OUT=/path/to/mesa-sdk ./build_mesa_docker.sh
#   SRC=/path/to/mesa-switch-src ./build_mesa_docker.sh
#   ./build_mesa_docker.sh --incremental         # after editing SRC: only ninja + merge (needs a previous full build)
#   APPLY_ONLY=1 ./build_mesa_docker.sh          # only clone + apply the patch (no Docker needed)
#   MESA_OPT=1 OUT=../../mesa-sdk-o1 ./build_mesa_docker.sh   # fallback: the old -O1 driver
#
# Optimization: MESA_OPT=2 (default) builds Mesa with -Doptimization=2 (-O2 for C, opt-level=2 for the NAK Rust
# compiler). Upstream build-switch.sh uses -Doptimization=1; the script rewrites that line before building. -O2 was
# measured on the Switch (2026-10-07): +5 % draw throughput in heavy scenes (15.7k -> 16.6k draws/s, 25.8 -> 26.4 fps,
# frames over 60 ms 160 -> 108 per route). MESA_OPT=1 gives the old -O1 build (accepted forms: 1, 2, 3, O1, O2, O3).
#
# Needs: git, docker. First full build: about an hour or more and ~15 GB of disk. The Docker.rust image installs
# the latest Rust nightly; our reference build used rust 1.101.0-nightly (LLVM 23.1.1). If a newer nightly breaks the
# NAK build, install that nightly instead (see README.md).
set -euo pipefail

PIN=d4a00ea0ab3f59afb967cc5d779e4263d237bd77
UPSTREAM=${UPSTREAM:-https://github.com/danfromtico/mesa-switch.git}
HERE=$(cd "$(dirname "$0")" && pwd)
PATCH=$HERE/mesa-switch-masseffect.patch
SRC=${SRC:-$HERE/../../mesa-switch-src}
OUT=${OUT:-$HERE/../../mesa-sdk}
IMAGE=devkitpro-mesa-rust
CONTAINER=mesa-switch-masseffect-build
MESA_OPT=${MESA_OPT:-2}
MESA_OPT=${MESA_OPT#[Oo]}
case "$MESA_OPT" in 1|2|3) ;; *) echo "error: MESA_OPT must be 1, 2 or 3 (got '$MESA_OPT')" >&2; exit 1 ;; esac
INCREMENTAL=0
PATCHES=("$PATCH")
[ "${P8_DIRECT_DRAWS:-1}" = 1 ] && PATCHES+=("$HERE/mesa-switch-p8-direct-draws.patch")
[ "${1:-}" = "--incremental" ] && INCREMENTAL=1

mkdir -p "$(dirname "$SRC")" "$OUT"
SRC=$(cd "$SRC" 2>/dev/null && pwd || echo "$(cd "$(dirname "$SRC")" && pwd)/$(basename "$SRC")")
OUT=$(cd "$OUT" && pwd)

if [ "$INCREMENTAL" = 0 ]; then
  if [ ! -d "$SRC/.git" ]; then
    echo "== clone $UPSTREAM at $PIN"
    git clone --no-checkout "$UPSTREAM" "$SRC"
  fi
  cd "$SRC"
  # The patches, in order. A stamp file holds their hash: when they change, the tree goes back to the pinned commit
  # (tracked files, and the files the patches create) and all are applied again. Build directories stay.
  # PATCHES (set above): mesa-switch-p8-direct-draws.patch is on by default since the console test of 2026-10-16
  # (renderer CPU per draw roughly halved with shadows on); P8_DIRECT_DRAWS=0 builds without it.
  STAMP="$SRC/.bo1-patches"
  WANT=$(cat "${PATCHES[@]}" | shasum | cut -d' ' -f1)
  if [ -f "$STAMP" ] && [ "$(cat "$STAMP")" = "$WANT" ]; then
    echo "== patches already applied"
  else
    git checkout -q -f "$PIN"
    for patch in "${PATCHES[@]}"; do
      # Files the patch creates (--- /dev/null): left over from an earlier apply, they would make it fail.
      awk '/^--- \/dev\/null/{getline; sub(/^\+\+\+ b\//, ""); print}' "$patch" | while read -r created; do
        rm -f "$created"
      done
    done
    for patch in "${PATCHES[@]}"; do
      echo "== apply $(basename "$patch")"
      git apply "$patch"
    done
    echo "$WANT" > "$STAMP"
  fi
  [ "${APPLY_ONLY:-0}" = 1 ] && { echo "patched source: $SRC"; exit 0; }
  # build-switch.sh is upstream's file (not part of the patch): set the optimization level of the cross build there.
  sed -i.orig -E "s/-Doptimization=[0-9sg]+/-Doptimization=$MESA_OPT/" build-switch.sh
  grep -q -- "-Doptimization=$MESA_OPT " build-switch.sh || { echo "error: could not set -Doptimization=$MESA_OPT in build-switch.sh" >&2; exit 1; }
  echo "== full build (build-switch.sh, -Doptimization=$MESA_OPT)"
  ./build-switch.sh
fi

cd "$SRC"
# The build directory whose optimization matches MESA_OPT: builddir-switch (build-switch.sh), or builddir-switch-o<N>
# (mesa/build_mesa_opt.sh). Never merge an archive of another level into OUT.
bd_opt() { python3 -c 'import json,sys;print(next(o["value"] for o in json.load(open(sys.argv[1])) if o["name"]=="optimization"))' \
  "$SRC/$1/meson-info/intro-buildoptions.json" 2>/dev/null || true; }
BD=""
for cand in builddir-switch "builddir-switch-o$MESA_OPT"; do
  [ "$(bd_opt "$cand")" = "$MESA_OPT" ] && { BD=$cand; break; }
done
[ -n "$BD" ] || { echo "error: no build directory in $SRC is configured with -Doptimization=$MESA_OPT" >&2
  echo "       (builddir-switch: -O$(bd_opt builddir-switch)). Run a full build, or OPT=$MESA_OPT mesa/build_mesa_opt.sh" >&2; exit 1; }
if [ "$INCREMENTAL" = 1 ]; then
  echo "== incremental ninja ($BD)"
  docker rm -f "$CONTAINER" >/dev/null 2>&1 || true
  docker run --rm --name "$CONTAINER" -v "$SRC:/project" -w /project "$IMAGE" bash -c "
    export PATH=\"/usr/local/libexec:/project/builddir-native/src/compiler/clc:/project/builddir-native/src/compiler/spirv:\$PATH\"
    ninja -C /project/$BD \
      src/nouveau/vulkan/libnvk.a src/nouveau/vulkan/libvulkan.a src/nouveau/compiler/libnak_rs.a \
      src/nouveau/compiler/libnak.a"
fi

echo "== merge libnak_rs.a into libvulkan.a ($BD)"
LIBDIR=$OUT/opt/devkitpro/portlibs/switch/lib
mkdir -p "$LIBDIR"
docker run --rm -v "$SRC:/project" -w "/project/$BD" "$IMAGE" bash -c "
  AR=/opt/devkitpro/devkitA64/bin/aarch64-none-elf-ar
  printf 'CREATE /project/$BD/libvulkan_merged.a\nADDLIB src/nouveau/vulkan/libvulkan.a\nADDLIB src/nouveau/compiler/libnak_rs.a\nSAVE\nEND\n' | \$AR -M"
mv "$SRC/$BD/libvulkan_merged.a" "$LIBDIR/libvulkan.a"
cat > "$OUT/SOURCE.txt" <<EOT
mesa-switch $PIN, patches: ${PATCHES[*]##*/}
built with build-switch.sh (Docker), -Doptimization=$MESA_OPT ($BD), libvulkan.a = $BD libvulkan.a + libnak_rs.a merged with ar -M
EOT
ls -la "$LIBDIR/libvulkan.a"
echo "Use it with:  MESA_SDK=$OUT tools/build_nro.sh"
echo "(-DREXGLUE_SWITCH_NVK_SDK=$OUT/opt/devkitpro/portlibs/switch). Delete the NRO build dir so it relinks."
