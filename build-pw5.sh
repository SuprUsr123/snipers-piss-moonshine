#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
KOXT_ROOT=${KOXT_ROOT:-${HOME}/x-tools/arm-kindlehf-linux-gnueabihf}
KOXT_TRIPLET=${KOXT_TRIPLET:-arm-kindlehf-linux-gnueabihf}
CROSS_BIN=${CROSS_BIN:-$KOXT_ROOT/bin}
CC=$CROSS_BIN/$KOXT_TRIPLET-gcc
CXX=$CROSS_BIN/$KOXT_TRIPLET-g++
AR=$CROSS_BIN/$KOXT_TRIPLET-ar
RANLIB=$CROSS_BIN/$KOXT_TRIPLET-ranlib
STRIP=$CROSS_BIN/$KOXT_TRIPLET-strip
OPENH264_SRC=${OPENH264_SRC:-/tmp/openh264-inspect}
OPENSSL_SRC=${OPENSSL_SRC:-/tmp/openssl-1.0.2u}
FBINK_SRC=${FBINK_SRC-/tmp/FBink}
HOST_HEADERS=$ROOT/build-host-headers
BUILD_DIR=$ROOT/build-koxt

test -x "$CC" || { echo "KOXT Kindle hard-float compiler not found: $CC" >&2; exit 1; }
# Use the compiler-bundled KOXT target sysroot, never a dumped device rootfs.
SYSROOT=$($CC -print-sysroot)
test -d "$SYSROOT/usr/lib" || { echo "KOXT compiler sysroot is missing target libraries: $SYSROOT" >&2; exit 1; }
test -f "$OPENH264_SRC/codec/api/wels/codec_api.h" || { echo "OpenH264 source checkout missing: $OPENH264_SRC" >&2; exit 1; }
make -C "$OPENH264_SRC" clean
make -C "$OPENH264_SRC" libraries ARCH=armv7 \
  CC="$CC" CXX="$CXX" AR="$AR" RANLIB="$RANLIB" STRIP="$STRIP" V=No
test -f "$OPENH264_SRC/libopenh264.a" || { echo "OpenH264 build did not produce $OPENH264_SRC/libopenh264.a" >&2; exit 1; }
test -f "$OPENSSL_SRC/include/openssl/evp.h" || { echo "Configure OpenSSL 1.0.2u first: $OPENSSL_SRC/include/openssl/evp.h is missing" >&2; exit 1; }

# When FBINK_SRC is set, build and link the minimal Kindle FBink library.
# The default path enables both renderer choices in the release package.
FBINK_CMAKE_ARGS=""
if [ -n "$FBINK_SRC" ]; then
  test -f "$FBINK_SRC/fbink.c" || { echo "FBINK_SRC=$FBINK_SRC does not look like an FBink checkout" >&2; exit 1; }
  make -C "$FBINK_SRC" cleanstaticlib >/dev/null 2>&1 || true
  make -C "$FBINK_SRC" staticlib KINDLE=true MINIMAL=true \
    CC="$CC" AR="$AR" \
    EXTRA_CFLAGS="-O2"
  FBINK_LIB="$FBINK_SRC/Release/static/libfbink.a"
  test -f "$FBINK_LIB" || { echo "FBink build did not produce $FBINK_LIB" >&2; exit 1; }
  FBINK_CMAKE_ARGS="-DENABLE_FBINK=ON -DFBINK_INCLUDE_DIR=$FBINK_SRC -DFBINK_LIBRARY=$FBINK_LIB"
fi

mkdir -p "$HOST_HEADERS"
ln -sfn /usr/include/curl "$HOST_HEADERS/curl"
ln -sfn /usr/include/uuid "$HOST_HEADERS/uuid"
ln -sfn /usr/include/expat.h "$HOST_HEADERS/expat.h"
ln -sfn /usr/include/expat_external.h "$HOST_HEADERS/expat_external.h"

PKG_CONFIG_SYSROOT_DIR=$SYSROOT \
PKG_CONFIG_LIBDIR="$SYSROOT/usr/lib/pkgconfig:$SYSROOT/usr/share/pkgconfig:$SYSROOT/lib/pkgconfig" \
PKG_CONFIG_PATH= cmake -S "$ROOT/embedded" -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER="$CC" \
  -DCMAKE_CXX_COMPILER="$CXX" \
  -DCMAKE_SYSROOT="$SYSROOT" \
  -DCMAKE_FIND_ROOT_PATH="$SYSROOT" \
  -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY \
  -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=BOTH \
  -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH \
  -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
  -DPKG_CONFIG_EXECUTABLE="$ROOT/pkg-config" \
  -DENABLE_KINDLE=ON -DENABLE_AUDIO=OFF -DENABLE_SDL=OFF \
  -DENABLE_FFMPEG=OFF -DENABLE_X11=OFF -DENABLE_CEC=OFF -DENABLE_PULSE=OFF \
  -DWELS_INCLUDE_DIR="$OPENH264_SRC/codec/api" \
  -DWELS_LIBRARY="$OPENH264_SRC/libopenh264.a" \
  -DOPENSSL_INCLUDE_DIR="$OPENSSL_SRC/include" \
  -DOPENSSL_SSL_LIBRARY="$SYSROOT/usr/lib/libssl.so" \
  -DOPENSSL_CRYPTO_LIBRARY="$SYSROOT/usr/lib/libcrypto.so" \
  -DCURL_INCLUDE_DIR="$HOST_HEADERS" \
  -DCURL_LIBRARY="$SYSROOT/usr/lib/libcurl.so" \
  -DEXPAT_INCLUDE_DIR="$HOST_HEADERS" \
  -DEXPAT_LIBRARY="$SYSROOT/usr/lib/libexpat.so" \
  -DLibUUID_INCLUDE_DIR="$HOST_HEADERS" \
  -DLibUUID_LIBRARY="$SYSROOT/lib/libuuid.so" \
  $FBINK_CMAKE_ARGS

cmake --build "$BUILD_DIR" --parallel
mkdir -p "$ROOT/package"
rm -rf "$ROOT/package/extensions/bin"
rm -f "$ROOT/package/moonlight" "$ROOT/package/libgamestream.so.4" "$ROOT/package/libmoonlight-common.so.4"
cp "$ROOT/launch.sh" "$ROOT/package/launch.sh"
chmod +x "$ROOT/package/launch.sh"
# Ship the config template once; rebuilds keep existing user edits.
[ -f "$ROOT/package/moonlight.conf" ] || \
  cp "$ROOT/embedded/moonlight.conf" "$ROOT/package/moonlight.conf"
# Optional style overrides, same keep-user-edits policy.
[ -f "$ROOT/package/ink.rc" ] || \
  cp "$ROOT/ink.rc" "$ROOT/package/ink.rc"
cp "$ROOT/README.md" "$ROOT/BUILD.md" "$ROOT/README-INSTALL.txt" "$ROOT/README-KUAL.txt" "$ROOT/HOST-SETUP.md" "$ROOT/LICENSE" "$ROOT/package/"
# Refresh launchers from source while preserving a local install-path override.
if [ -f "$ROOT/package/extensions/moonlight/bin/moonlight.path" ]; then
  cp "$ROOT/package/extensions/moonlight/bin/moonlight.path" "$ROOT/package/.moonlight.path.keep"
fi
rm -rf "$ROOT/package/extensions/moonlight" "$ROOT/package/documents"
mkdir -p "$ROOT/package/extensions" "$ROOT/package/documents"
cp -R "$ROOT/extensions/moonlight" "$ROOT/package/extensions/"
mkdir -p "$ROOT/package/extensions/moonlight/bin"
cp "$BUILD_DIR/moonlight" "$ROOT/package/extensions/moonlight/bin/moonlight"
cp "$BUILD_DIR/libgamestream/libgamestream.so.2.7.1" "$ROOT/package/extensions/moonlight/bin/libgamestream.so.4"
cp "$BUILD_DIR/libgamestream/libmoonlight-common.so.2.7.1" "$ROOT/package/extensions/moonlight/bin/libmoonlight-common.so.4"
cp "$ROOT/package/launch.sh" "$ROOT/package/extensions/moonlight/launch.sh"
cp "$ROOT/package/moonlight.conf" "$ROOT/package/extensions/moonlight/moonlight.conf"
cp "$ROOT/package/ink.rc" "$ROOT/package/extensions/moonlight/ink.rc"
"$STRIP" "$ROOT/package/extensions/moonlight/bin/moonlight" "$ROOT/package/extensions/moonlight/bin/libgamestream.so.4" "$ROOT/package/extensions/moonlight/bin/libmoonlight-common.so.4"
cp "$ROOT/documents/moonlight.sh" "$ROOT/package/documents/moonlight.sh"
if [ -f "$ROOT/package/.moonlight.path.keep" ]; then
  cp "$ROOT/package/.moonlight.path.keep" "$ROOT/package/extensions/moonlight/bin/moonlight.path"
fi
chmod +x "$ROOT/package/launch.sh" "$ROOT/package/extensions/moonlight/launch.sh" "$ROOT/package/documents/moonlight.sh" "$ROOT/package"/extensions/moonlight/bin/*.sh "$ROOT/package/extensions/moonlight/bin/moonlight"
