#!/bin/sh
# KUAL scriptlet: start the Moonlight UI.
#
# KUAL runs actions in the background, so this script may block: it stays
# alive for as long as the UI runs and gets the panel ready for the menu
# again when the user quits (KUAL repaints underneath). The install dir
# is resolved by moonlight-common.sh; launch.sh handles DISPLAY, the
# pairing keys, the CPU governor and the rolling log.

DIR=$(dirname -- "$0")
. "$DIR/moonlight-common.sh"

if ! find_moonlight; then
  report_not_found
  exit 1
fi

# Hide this script's diagnostics from a previous failed run.
rm -f "$EXT_DIR/moonlight-not-found.txt" 2>/dev/null

exec sh "$MOONLIGHT_DIR/launch.sh"
