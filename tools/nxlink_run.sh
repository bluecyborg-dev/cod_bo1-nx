#!/usr/bin/env bash
# Sends out/nx/bo1-nx.nro to the Switch over Wi-Fi and records the game's log live, without moving the SD card.
#
# On the Switch: hold R while starting any game (full memory), then in the Homebrew Menu press Y ("NetLoader").
# Then, on the Mac:
#
#   tools/nxlink_run.sh [switch-ip]
#
# What happens:
#   1. A TCP listener starts on this Mac (port 28771) and writes everything it receives to out/net_logs/<time>.log.
#   2. nxlink uploads the NRO to sdmc:/switch/bo1-nx/bo1-nx.nro (next to game_root/, which stays on the card) and the
#      Homebrew Menu starts it.
#   3. The NRO gets --log_network=<this Mac>:28771 (the SDK sends every log line, and stdout/stderr, here) and
#      --game_data_root=sdmc:/switch/bo1-nx/game_root (launched by the netloader, argv[0] is not the usual path).
#   The script ends when the game closes its connection (the game exited or crashed). Logs and crash files are also
#   still written to the SD card as usual.
#
# Without an IP, nxlink looks for the Switch with a broadcast (works on most home networks).
# Environment: NRO (default out/nx/bo1-nx.nro), PORT (default 28771), HOST_IP (default: this Mac's Wi-Fi address),
#              EXTRA_ARGS (more arguments for the game, e.g. "--bo1_debug_exec=devmap+zombie_theater").
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
NRO="${NRO:-$ROOT/out/nx/bo1-nx.nro}"
PORT="${PORT:-28771}"
NXLINK="${NXLINK:-/opt/devkitpro/tools/bin/nxlink}"
SWITCH_IP="${1:-}"
HOST_IP="${HOST_IP:-$(ipconfig getifaddr en0 2>/dev/null || true)}"

[[ -f "$NRO" ]] || { echo "error: $NRO not found; run tools/build_nro.sh" >&2; exit 1; }
[[ -x "$NXLINK" ]] || { echo "error: nxlink not found at $NXLINK (devkitPro tools)" >&2; exit 1; }
[[ -n "$HOST_IP" ]] || { echo "error: could not find this Mac's IP address; set HOST_IP" >&2; exit 1; }

mkdir -p "$ROOT/out/net_logs"
LOG="$ROOT/out/net_logs/$(date +%Y%m%d-%H%M%S).log"
ln -sfn "$(basename "$LOG")" "$ROOT/out/net_logs/latest.log"

# A listener left over from an earlier run (its game connection half-closed) still answers on the port: the next game
# connected to it, nobody read, and the game blocked on its own stdout at a black screen (2026-10-16). Stop it first.
stale=$(lsof -nP -t -iTCP:"$PORT" -sTCP:LISTEN 2>/dev/null || true)
if [[ -n "$stale" ]]; then
  echo "== stopping an old log listener on port $PORT (pid $stale)"
  kill $stale 2>/dev/null || true
  sleep 0.5
fi

# Listener first: the game connects to it while it starts its logging.
nc -l "$PORT" > "$LOG" &
NC_PID=$!
trap 'kill $NC_PID 2>/dev/null || true' EXIT
sleep 0.5

ARGS="--log_network=$HOST_IP:$PORT --game_data_root=sdmc:/switch/bo1-nx/game_root${EXTRA_ARGS:+ $EXTRA_ARGS}"
# The netloader passes only about 256 characters of arguments: anything after that is silently dropped.
if (( ${#ARGS} > 250 )); then
  echo "error: the arguments are ${#ARGS} characters; the netloader keeps only ~256 and drops the rest:" >&2
  echo "       ...${ARGS:240}" >&2
  exit 1
fi

echo "== sending $(basename "$NRO") ($(du -h "$NRO" | cut -f1)) to ${SWITCH_IP:-the Switch (broadcast search)}"
addr=()
[[ -n "$SWITCH_IP" ]] && addr=(-a "$SWITCH_IP")
# Keep trying for about ten minutes: the Homebrew Menu only accepts the upload while its NetLoader screen (Y) is open.
sent=0
for attempt in $(seq 1 200); do
  if "$NXLINK" "${addr[@]+"${addr[@]}"}" -r 5 -p switch/bo1-nx/bo1-nx.nro \
       --args "$ARGS" "$NRO"; then
    sent=1
    break
  fi
  echo "   (attempt $attempt: the Switch is not accepting yet; is the NetLoader screen open? retrying)"
  sleep 3
done
[[ $sent == 1 ]] || { echo "error: could not send the NRO; open the NetLoader (Y) in the Homebrew Menu and try again" >&2; exit 1; }

echo "== game started; live log: $LOG (Ctrl+C to stop)"
wait $NC_PID || true
echo "== connection closed (game exited or crashed). Log: $LOG ($(wc -l < "$LOG") lines)"
