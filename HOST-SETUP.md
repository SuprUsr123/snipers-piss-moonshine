# Fixing the PC side (Sunshine), especially on Wayland

If the Kindle shows a black picture, or the streamed windows vanish,
flicker, come back and disappear again, the problem is almost always the
**capture method** Sunshine uses on the PC - not the Kindle client and not
the network. Wayland does not let applications grab other applications'
windows the way X11 did, so the capture method has to match your desktop,
and per-window capture is off the table entirely.

The Kindle client helps you notice this: if nothing but black frames
arrive for a few seconds, a hint appears under the video area, and the
Settings page has a "PC help" group with the short version of this file.

## 1. Set the capture method

1. On the PC, open Sunshine's web page: `https://PC-IP:47990`
   (accept the self-signed certificate warning) and sign in.
2. Go to **Configuration** and find **Capture**
   ("Force a specific capture method").
3. Pick the value for your desktop:

| Desktop on the PC              | Capture value | Notes |
| ------------------------------ | ------------- | ----- |
| GNOME Wayland                  | `portal`      | The reliable choice on GNOME. |
| KDE Plasma Wayland             | `portal` or `kwin` | `kwin` is KDE-only; on Flatpak use `portal`. |
| KDE Plasma 6.5+ with `kms`     | `portal`      | KWin's overlay support interferes with KMS; `export KWIN_USE_OVERLAYS=0` is the alternative. |
| wlroots (Sway, Hyprland, ...)  | `wlr`         | Uses wlr-screencopy; virtual outputs work too. |
| Any Wayland, native package    | `kms`         | Kernel/DRM capture; needs `CAP_SYS_ADMIN`, see below. |
| X11 session                    | `kms` or `x11`| `x11` is the slowest; only as fallback. |

4. **Save** and **restart Sunshine** (quit it fully, start it again).

The equivalent manual edit is one line in Sunshine's `sunshine.conf`:

```ini
capture = portal      # or kms / kwin / wlr
```

## 2. KMS capture requirements

`kms` is the lowest-latency method, but it needs privileges:

- It **does not work from the Flatpak or AppImage** packages - install
  Sunshine from your distribution's native package instead.
- Sunshine needs the `CAP_SYS_ADMIN` capability; the native packages set
  this up, otherwise: `sudo setcap cap_sys_admin+p /usr/bin/sunshine`.
- On **NVIDIA**, KMS capture shows a black screen unless the DRM modeset
  is enabled: add `nvidia_drm.modeset=1` to your kernel command line
  (or `/etc/modprobe.d/nvidia-drm.conf`: `options nvidia-drm modeset=1`)
  and reboot. Check with `cat /sys/module/nvidia_drm/parameters/modeset`
  - it must print `Y`.

If a recent Sunshine release suddenly prefers portal capture over KMS on
your system, that is intentional: newer versions take the XDG portal grab
first. Force `capture = kms` in the config if you specifically want KMS.

## 3. Symptoms and what they mean

- **Everything is black from the start** - wrong capture method, or
  NVIDIA+KMS without modeset. Fix per sections 1-2.
- **Windows disappear, turn black, reappear, flicker** - the capture
  method is fighting the compositor (often `kms` on KDE 6.5+, or portal
  hiccups on GNOME). Switch to the recommended method from the table.
- **One specific app is black, the rest is fine** - that app renders with
  hardware acceleration / direct scanout, which the capture cannot see.
  Stream the whole **Desktop** app instead of a single window (Sunshine's
  per-window capture does not work on Wayland at all), or disable
  hardware acceleration in that app.
- **Mouse works but the picture freezes** - the stream is alive, the
  capture is not producing frames; switch capture method.
- **"No picture" but audio connects** - Sunshine is up but its encoder
  has no source; same fix.

## 4. Sunshine settings that matter for a slow client

The Kindle decodes H.264 in software, so keep the stream gentle (the
client's Settings page controls these from the device side - resolution,
fps, bitrate - you do not need to edit `sunshine.conf` for them):

- Add a **Desktop** app in Sunshine's *Applications* tab so the app list
  on the Kindle has something to stream.
- Leave the encoder on auto (nvenc/vaapi/vulkan/software - whatever your
  GPU offers). Software encoding is fine at e-ink frame rates.
- If you use **Flatpak Sunshine on Wayland**, prefer `portal` capture;
  `kms`/`kwin` cannot work from inside the sandbox.

## 5. Checking it worked

- Restart Sunshine, then start a stream from the Kindle.
- The first picture should appear within a couple of seconds; the
  client's "waiting for video" line hides itself.
- The client's log (`moonlight.log` next to the binary) shows one line
  per second groups of decoded frames when `-debug`/verbose log is on
  (Settings > Interface > Verbose log): `decoded N frames, N changed`
  tells you the PC is actually sending changing pictures.

## 6. Firewall / ports (if Connect itself fails)

- TCP 47984 and 47989 (HTTPS/HTTP control), TCP 47990 (web UI),
  UDP 47998-48000, 48010 (video/control/audio). On Wayland-agnostic
  distros with `firewalld`/`ufw`, allow the Sunshine service or those
  ports for the LAN.
- The Kindle connects to the address you type on the Connect page -
  the PC's LAN IP (for example `192.168.1.20`) is the safest choice;
  `.local` hostnames work via the ABC keypad or a USB keyboard.
