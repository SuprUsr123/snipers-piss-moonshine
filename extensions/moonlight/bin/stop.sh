#!/bin/sh
# KUAL scriptlet: stop a running Moonlight (UI or stream).
#
# launch.sh with no arguments is a toggle: while a client is running it
# kills it, restores the CPU governors and returns at once, so this
# scriptlet is quick and the KUAL menu stays usable. When nothing runs,
# it must NOT start anything - the lock file decides.

DIR=$(dirname -- "$0")
. "$DIR/moonlight-common.sh"

if ! find_moonlight; then
  report_not_found
  exit 1
fi

LOCK="$MOONLIGHT_DIR/.moonlight-running"
pid=""
if [ -f "$LOCK/pid" ]; then
  pid=$(cat "$LOCK/pid" 2>/dev/null)
fi

if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
  sh "$MOONLIGHT_DIR/launch.sh"
  # e-ink housekeeping: one full-clear so no ghost of the client stays
  # on the panel. Unsupported firmware just prints a usage line.
  eips -c 2>/dev/null || true
else
  # Stale lock without a live process: clean it so the next start works.
  rm -f "$LOCK/pid" 2>/dev/null
  rmdir "$LOCK" 2>/dev/null || true
fi
exit 0
