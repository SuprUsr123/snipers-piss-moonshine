#!/bin/sh
# Create an optional source-and-package archive after build-pw5.sh.
# Usage: ./make-patch.sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
OUT="$ROOT/package"
STAMP=$(date +%Y%m%d)
NAME="moonlight-kindle-pw5-patch-$STAMP.tar.gz"

mkdir -p "$OUT"

# Stage the package skeleton so the archive always carries a complete,
# runnable package folder: launcher, settings template, install notes.
# User-edited files in package/ are kept (same policy as build-pw5.sh).
cp "$ROOT/launch.sh" "$OUT/launch.sh"
chmod +x "$OUT/launch.sh"
[ -f "$OUT/moonlight.conf" ] || \
  cp "$ROOT/embedded/moonlight.conf" "$OUT/moonlight.conf"
cp "$ROOT/README.md" "$ROOT/README-INSTALL.txt" "$OUT/"
cp "$ROOT/HOST-SETUP.md" "$OUT/HOST-SETUP.md"
[ -f "$OUT/ink.rc" ] || cp "$ROOT/ink.rc" "$OUT/ink.rc"

# KUAL extension and library scriptlet. Preserve a locally configured install path.
rm -rf "$OUT/extensions" "$OUT/documents"
mkdir -p "$OUT/extensions" "$OUT/documents"
cp -r "$ROOT/extensions/moonlight" "$OUT/extensions/moonlight"
cp "$ROOT/documents/moonlight.sh" "$OUT/documents/moonlight.sh"
cp "$ROOT/README-KUAL.txt" "$OUT/README-KUAL.txt"
cp "$ROOT/LICENSE" "$OUT/LICENSE"
if [ -f "$OUT/.moonlight.path.keep" ]; then
  cp "$OUT/.moonlight.path.keep" "$OUT/extensions/moonlight/bin/moonlight.path"
fi
if [ -f "$OUT/extensions/moonlight/bin/moonlight.path" ]; then
  cp "$OUT/extensions/moonlight/bin/moonlight.path" "$OUT/.moonlight.path.keep"
fi
chmod +x "$OUT"/extensions/moonlight/bin/*.sh "$OUT/documents/moonlight.sh"

# Ship source, build scripts and the package skeleton (including any
# binaries already built). Leave build directories and older patch archives
# out of the archive.
tar -czf "$OUT/$NAME.tmp" -C "$ROOT" \
  --exclude='./.git' \
  --exclude='./build-pw5' \
  --exclude='./build-koxt' \
  --exclude='./build-host-headers' \
  --exclude='./embedded/build' \
  --exclude='./embedded/build-*' \
  --exclude='./package/*.tar.gz' \
  --exclude='./package/*.tar.gz.tmp' \
  .
mv "$OUT/$NAME.tmp" "$OUT/$NAME"
chmod 644 "$OUT/$NAME"

echo "Patch written to $OUT/$NAME"
echo "Contents:"
tar -tzf "$OUT/$NAME" | head -n 8
echo "  ..."
