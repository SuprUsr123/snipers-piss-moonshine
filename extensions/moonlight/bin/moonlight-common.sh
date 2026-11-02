#!/bin/sh
# Shared lookup for the Moonlight install folder, sourced by the KUAL
# scriptlets. Sets MOONLIGHT_DIR (and EXT_DIR for diagnostics) or leaves
# it empty and writes the reason to moonlight-not-found.txt in the
# extension folder, where it survives USB mount and is easy to spot.
#
# KUAL runs menu.json actions as background processes with the working
# directory set to the folder that contains menu.json, so $0-based
# resolution plus that cwd covers every launcher (KUAL 2.7, KUAL Next).

EXT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." 2>/dev/null && pwd)

find_moonlight() {
  MOONLIGHT_DIR=""

  # 1. One-line override file next to the scripts: the full path to the
  #    folder that holds launch.sh (everything after # is a comment).
  if [ -f "$EXT_DIR/bin/moonlight.path" ]; then
    while IFS= read -r line || [ -n "$line" ]; do
      line=${line%%\#*}
      line=$(printf '%s' "$line" | tr -d ' \t\r\n')
      if [ -n "$line" ] && [ -f "$line/launch.sh" ]; then
        MOONLIGHT_DIR="$line"
        return 0
      fi
    done < "$EXT_DIR/bin/moonlight.path"
  fi

  # 2. Environment override, 3. the standard executable install locations.
  for candidate in \
      "$MOONLIGHT_HOME" \
      /var/local/moonlight \
      /mnt/us/moonlight \
      "$EXT_DIR"; do
    if [ -n "$candidate" ] && [ -f "$candidate/launch.sh" ]; then
      MOONLIGHT_DIR=$candidate
      return 0
    fi
  done
  return 1
}

report_not_found() {
  if [ -n "$EXT_DIR" ]; then
    {
      echo "$(date) KUAL could not find Moonlight."
      echo "Looked in /var/local/moonlight, /mnt/us/moonlight, and this extension."
      echo "Fix: put the package folder on an executable filesystem"
      echo "(see README-INSTALL.txt) or write its full path into"
      echo "$EXT_DIR/bin/moonlight.path"
    } > "$EXT_DIR/moonlight-not-found.txt" 2>/dev/null
  fi
}
