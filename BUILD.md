# Build and release

Sniper's Piss Moonshine is a Moonlight Embedded client for jailbroken PW4s (I just say "theoretically" since I haven't tested on older devices, just a PW5) and newer devices running firmware 5.16.4 or later. It targets
Kindle's ARM hard-float ABI (`kindlehf`) and uses the KOXT toolchain's bundled
target libraries. A dump of a Kindle system image is not needed.

## Build on your computer

The build script has been tested on Linux. Install Git, CMake, Make, curl,
Perl, zstd, and the host development headers for curl, expat, and uuid. On
Ubuntu or Debian:

```sh
sudo apt-get update
sudo apt-get install -y build-essential cmake curl git libcurl4-openssl-dev \
  libarchive-dev libexpat1-dev make nettle-dev perl uuid-dev zlib1g-dev zstd
```

Download and unpack the KOXT `kindlehf` toolchain under your home directory:

```sh
mkdir -p "$HOME/x-tools"
curl -fL https://github.com/koreader/koxtoolchain/releases/download/2026.08/kindlehf.tar.zst \
  | tar --zstd -xf - -C "$HOME"
```

The build workflow installs the Kindle SDK over KOXT to supply GTK2 headers,
libraries, and pkg-config metadata absent from the bare compiler archive. The
SDK obtains its Kindle libraries from Amazon firmware packages; you do not need
to dump or provide a device filesystem. Locally, install it after unpacking
KOXT:

```sh
git clone --depth 1 --recurse-submodules \
  https://github.com/KindleModding/kindle-sdk.git /tmp/kindle-sdk
bash /tmp/kindle-sdk/gen-sdk.sh kindlehf "$HOME/x-tools/arm-kindlehf-linux-gnueabihf"
```

Fetch the source dependencies. OpenSSL 1.0.2u is needed for compatibility
headers; the target OpenSSL libraries are provided by KOXT.

```sh
git clone --depth 1 --branch v2.7.0 \
  https://github.com/cisco/openh264.git /tmp/openh264-inspect
git clone --depth 1 --recurse-submodules \
  https://github.com/NiLuJe/FBInk /tmp/FBink
curl -fL https://www.openssl.org/source/old/1.0.2/openssl-1.0.2u.tar.gz \
  | tar -xz -C /tmp
cd /tmp/openssl-1.0.2u
./Configure linux-armv4 no-shared
```

From the project directory, build and stage the installable bundle:

```sh
./build-pw5.sh
```

The finished package is in `package/`. It contains the KUAL extension under
`extensions/moonlight/`, including the executable and shared libraries in its
`bin/` directory, plus the Kindle library shortcut and installation notes.
Zip the contents of `package/` to make a bundle for USB transfer:

```sh
cd package
zip -qr ../snipers-piss-moonshine-kindlehf.zip .
```

The build uses `/tmp/openh264-inspect`, `/tmp/openssl-1.0.2u`, and `/tmp/FBink`
by default. Set `OPENH264_SRC`, `OPENSSL_SRC`, or `FBINK_SRC` to use other
locations. Set `FBINK_SRC=` to build X11-only. Set `KOXT_ROOT` if the toolchain
is not in `$HOME/x-tools/arm-kindlehf-linux-gnueabihf`; `KOXT_TRIPLET` and
`CROSS_BIN` can also override its compiler paths.

## Build with GitHub Actions

The workflow in `.github/workflows/build-release.yml` builds on pushes to
branches and can also be started manually from the repository's **Actions**
tab. These runs attach the package ZIP as a downloadable workflow artifact.

To publish a GitHub Release, push a version tag beginning with `v`:

```sh
git tag v1.0.0
git push origin v1.0.0
```

The workflow builds the Kindle package and attaches the ZIP to the Release.
Publishing uses GitHub Actions' built-in `GITHUB_TOKEN`; no custom secret is
required. The repository must allow workflows to create releases.
