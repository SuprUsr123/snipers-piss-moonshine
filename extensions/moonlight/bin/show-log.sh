#!/bin/sh
# KUAL scriptlet: copy the tail of the client's rolling log to the USB
# partition, where it can be opened in the reader or pulled over USB.
# KUAL runs actions in the background (output is not shown), so writing
# a file is the honest way to present it; the item name says where.

DIR=$(dirname -- "$0")
. "$DIR/moonlight-common.sh"

OUT=/mnt/us/moonlight-log.txt

if ! find_moonlight; then
  {
    echo "$(date) KUAL could not find Moonlight to read its log."
    echo "Write the install path into the extension's bin/moonlight.path."
  } > "$OUT" 2>/dev/null
  exit 1
fi

LOG="$MOONLIGHT_DIR/moonlight.log"
if [ -f "$LOG" ]; then
  {
    echo "Moonlight log (last 60 lines of $LOG)"
    echo "=================================================="
    tail -n 60 "$LOG"
  } > "$OUT" 2>/dev/null
else
  echo "$(date) No moonlight.log yet in $MOONLIGHT_DIR." > "$OUT" 2>/dev/null
fi
exit 0
