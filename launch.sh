#!/bin/sh
set -eu
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
# Look for moonlight.conf next to the binary regardless of invocation cwd
cd "$DIR"
KEY_DIR=/mnt/us/.config/moonlight-keys
if [ -f "$DIR/bin/moonlight" ]; then
  BIN_DIR="$DIR/bin"
else
  BIN_DIR="$DIR/extensions/moonlight/bin"
fi
export DISPLAY=${DISPLAY:-:0}
export LD_LIBRARY_PATH="$BIN_DIR:$DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export XDG_CACHE_DIR="$DIR/.cache"
mkdir -p "$KEY_DIR"
for source in /var/local/moonlight-keys "$DIR/.cache/moonlight"; do
  for file in uniqueid.dat client.pem key.pem client.p12; do
    if [ -f "$source/$file" ] && [ ! -f "$KEY_DIR/$file" ]; then
      cp -p "$source/$file" "$KEY_DIR/$file"
    fi
  done
done

# Keep the CPU at full speed while the client runs. Software H.264 decode
# is the bottleneck on a Kindle, and the stock cpufreq governor parks the
# cores at low frequency, which shows up as decoder lag and slow panel
# updates. "performance = false" in moonlight.conf opts out. The previous
# governors are restored when this script exits (UI or CLI path).
GOV_SAVE="$DIR/.cpu-governors"
performance_off() {
  if [ -f "$GOV_SAVE" ]; then
    while read -r gov cur; do
      [ -n "$gov" ] && [ -n "$cur" ] && echo "$cur" > "$gov" 2>/dev/null || true
    done < "$GOV_SAVE"
    rm -f "$GOV_SAVE"
  fi
  return 0
}
performance_on() {
  # "performance = false" in moonlight.conf OR saved from the Settings
  # page (Interface group, written to <keydir>/settings) opts out.
  for conf in "$DIR/moonlight.conf" "$KEY_DIR/settings"; do
    if [ -f "$conf" ] && \
       grep -qiE '^[[:space:]]*performance[[:space:]]*=[[:space:]]*false' "$conf"; then
      return 0
    fi
  done
  : > "$GOV_SAVE" 2>/dev/null || return 0
  for cpu in /sys/devices/system/cpu/cpu[0-9]*; do
    gov="$cpu/cpufreq/scaling_governor"
    if [ -f "$gov" ] && [ -w "$gov" ]; then
      cur=$(cat "$gov" 2>/dev/null || true)
      case "$cur" in
        ""|performance) ;;
        *)
          echo "$gov $cur" >> "$GOV_SAVE"
          echo performance > "$gov" 2>/dev/null || true
          ;;
      esac
    fi
  done
  return 0
}

if [ "$#" -eq 0 ]; then
  LOCK="$DIR/.moonlight-running"
  if ! mkdir "$LOCK" 2>/dev/null; then
    tries=0
    while [ ! -s "$LOCK/pid" ] && [ "$tries" -lt 10 ]; do
      sleep 0.1
      tries=$((tries + 1))
    done
    if [ -r "$LOCK/pid" ]; then
      pid=$(cat "$LOCK/pid")
      case "$pid" in
        ''|*[!0-9]*) ;;
        *)
          if kill -0 "$pid" 2>/dev/null; then
            kill "$pid"
            sleep 0.2
            if [ -d "$LOCK" ] && kill -0 "$pid" 2>/dev/null; then
              kill -KILL "$pid" 2>/dev/null || true
            fi
            echo "Stopping Moonlight (PID $pid)"
            exit 0
          fi
          ;;
      esac
    fi
    rm -f "$LOCK/pid"
    rmdir "$LOCK" 2>/dev/null || exit 0
    mkdir "$LOCK" || exit 0
  fi
  # stdout/stderr go nowhere when started from KUAL or the library, so a
  # failed stream used to leave no trace at all. Keep a small rolling log
  # next to the binary; send it along when reporting a problem.
  LOG="$DIR/moonlight.log"
  if [ -f "$LOG" ] && [ "$(wc -c < "$LOG")" -gt 204800 ]; then
    mv -f "$LOG" "$LOG.old"
  fi
  echo "=== $(date) ===" >> "$LOG"
  performance_on
  "$BIN_DIR/moonlight" -keydir "$KEY_DIR" >> "$LOG" 2>&1 &
  pid=$!
  printf '%s\n' "$pid" > "$LOCK/pid"
  trap 'rm -f "$LOCK/pid"; rmdir "$LOCK" 2>/dev/null || true; performance_off' EXIT
  trap 'rm -f "$LOCK/pid"; rmdir "$LOCK" 2>/dev/null || true; performance_off; exit 130' INT
  trap 'rm -f "$LOCK/pid"; rmdir "$LOCK" 2>/dev/null || true; performance_off; exit 143' TERM
  status=0
  wait "$pid" || status=$?
  exit "$status"
fi
performance_on
trap performance_off EXIT
trap 'performance_off; exit 130' INT
trap 'performance_off; exit 143' TERM
"$BIN_DIR/moonlight" -keydir "$KEY_DIR" "$@"
