#!/usr/bin/env bash
# Translates the game's default.xex to C++ with the host code generator. It builds nothing: build the generator
# first with tools/build_host.sh.
#
#   tools/codegen.sh
#
# Input:   assets/default.xex        (python3 tools/extract_iso.py <your iso> --xex-only -o assets)
#          app/bo1_manifest.toml     (includes app/overrides.toml)
# Output:  app/generated/            (C++ of the game and generated/rexglue.cmake; never committed)
#          out/codegen.log
#
# Environment: REXGLUE (default sdk/out/host/rexglue).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APP="$ROOT/app"
REXGLUE="${REXGLUE:-$ROOT/sdk/out/host/rexglue}"
XEX="$ROOT/assets/default.xex"
EXPECTED_SHA256=6027bd4fa4edc750fc60f544ef51feca766290f8a2785e91ebdda6368f281851

[[ -x "$REXGLUE" ]] || { echo "error: $REXGLUE not found; run tools/build_host.sh (or set REXGLUE)" >&2; exit 1; }
[[ -f "$XEX" ]] || { echo "error: $XEX not found; extract it from your disc with tools/extract_iso.py --xex-only -o assets" >&2; exit 1; }
actual="$(shasum -a 256 "$XEX" | cut -d' ' -f1)"
if [[ "$actual" != "$EXPECTED_SHA256" ]]; then
  echo "warning: $XEX has SHA-256 $actual; app/overrides.toml was written for $EXPECTED_SHA256" >&2
fi

mkdir -p "$ROOT/out"
echo "== codegen (app/bo1_manifest.toml)"
if ! ( cd "$APP" && REX_MAX_JUMP_TABLE_ENTRIES=1024 "$REXGLUE" codegen bo1_manifest.toml ) 2>&1 | tee "$ROOT/out/codegen.log"; then
  echo "codegen failed, see out/codegen.log" >&2
  exit 1
fi
[[ -f "$APP/generated/default/sources.cmake" ]] || { echo "error: the generator produced no app/generated/default" >&2; exit 1; }
echo "ok: $(ls "$APP"/generated/default/*.cpp | wc -l | tr -d ' ') C++ files in app/generated/default"
