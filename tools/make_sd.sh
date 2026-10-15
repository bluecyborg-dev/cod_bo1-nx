#!/usr/bin/env bash
# Lays out the port on a Switch SD card (or any folder that you copy to the SD card root later):
#
#   <dest>/switch/bo1-nx/
#     bo1-nx.nro        out/nx/bo1-nx.nro (tools/build_nro.sh)
#     bo1.toml          app/bo1.toml (only copied if not already there, so your edits are kept)
#     game_root/        the files of your disc (extracted from your ISO; skipped if already present)
#
#   tools/make_sd.sh <dest> [path/to/your.iso]
#     e.g. tools/make_sd.sh /Volumes/SWITCH cod-bo.iso      (SD card mounted on the Mac)
#          tools/make_sd.sh out/sd cod-bo.iso               (a folder; then copy out/sd/switch to the SD card)
#
# Set FORCE_GAME=1 to extract the disc again over an existing game_root.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="${1:?usage: tools/make_sd.sh <dest> [iso]}"
ISO="${2:-$ROOT/cod-bo.iso}"
NRO="$ROOT/out/nx/bo1-nx.nro"
APPDIR="$DEST/switch/bo1-nx"

[[ -f "$NRO" ]] || { echo "error: $NRO not found; run tools/build_nro.sh first" >&2; exit 1; }
mkdir -p "$APPDIR"

cp -f "$NRO" "$APPDIR/bo1-nx.nro"
echo "copied bo1-nx.nro"
if [[ ! -f "$APPDIR/bo1.toml" ]]; then
  cp "$ROOT/app/bo1.toml" "$APPDIR/bo1.toml"
  echo "copied bo1.toml"
else
  echo "kept existing bo1.toml"
fi

if [[ -f "$APPDIR/game_root/default.xex" && "${FORCE_GAME:-0}" != 1 ]]; then
  echo "kept existing game_root/ (FORCE_GAME=1 to extract again)"
else
  [[ -f "$ISO" ]] || { echo "error: ISO $ISO not found" >&2; exit 1; }
  echo "extracting $ISO into $APPDIR/game_root (about 7.3 GB, this takes a while on an SD card)"
  python3 "$ROOT/tools/extract_iso.py" "$ISO" -o "$APPDIR/game_root"
fi
# macOS litters FAT/exFAT volumes with AppleDouble files; they are harmless but clutter the game folder.
find "$APPDIR" -name '._*' -delete 2>/dev/null || true

# Verify what really reached the card. SD cards can silently drop writes (seen on the development card: a few
# 128 KB blocks kept their old contents), and reads right after writing come from the Mac's page cache, so the
# volume is unmounted and mounted again first. SKIP_VERIFY=1 skips this.
if [[ "${SKIP_VERIFY:-0}" != 1 ]]; then
  if [[ "$DEST" == /Volumes/* ]]; then
    vol="/Volumes/$(echo "${DEST#/Volumes/}" | cut -d/ -f1)"
    dev="$(diskutil info "$vol" | awk -F': *' '/Device Identifier/ {print $2}')"
    sync; diskutil unmount "$vol" >/dev/null && diskutil mount "$dev" >/dev/null
  fi
  cmp -s "$NRO" "$APPDIR/bo1-nx.nro" || { echo "error: bo1-nx.nro on the card differs from the build (bad card?)" >&2; exit 1; }
  echo "bo1-nx.nro verified"
  if [[ -f "$ISO" ]]; then
    python3 "$ROOT/tools/verify_sd.py" "$ISO" "$APPDIR/game_root" || {
      echo "error: some game files on the card differ from the disc image (see above)" >&2; exit 1; }
  fi
fi
echo "done: $APPDIR"
ls -la "$APPDIR"
