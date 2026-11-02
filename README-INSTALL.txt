Sniper's Piss Moonshine for Kindle
====================

Targets: Kindle Paperwhite 4 and 5, firmware 5.16.4 or newer, hard-float.
The supplied binary is built with KOXT kindlehf hard-float tools and target
libraries bundled with KOXT; no device rootfs dump is required. PW4 real-device
compatibility should still be verified.

Install the app
---------------

1. Copy the generated package folder to the Kindle USB storage as
   /mnt/us/moonlight.
2. Put the app files on an executable filesystem. For example, from a root
   shell on the Kindle:

     cp -R /mnt/us/moonlight/extensions/moonlight /var/local/
     chmod +x /var/local/moonlight/launch.sh \
        /var/local/moonlight/bin/moonlight
3. Launch the UI:

     /var/local/moonlight/launch.sh

USB storage may be mounted noexec, so the executable and libraries belong on
an executable filesystem. The launcher saves pairing keys in
/mnt/us/.config/moonlight-keys and migrates keys from older locations.

Optional launchers
------------------

For KUAL, copy package/extensions/moonlight to
/mnt/us/extensions/moonlight. For the Kindle library shortcut, copy
package/documents/moonlight.sh to /mnt/us/documents/moonlight.sh. Details are
in README-KUAL.txt.

Using the app
-------------

Connect to the PC by IP address or hostname, pair if prompted, choose a host
app, then tap Stream. Settings are available from the bottom bar. The default
stream is 1280×720 at 15 fps; requests above 1280×720 are reduced to fit that
limit. The full monitor stays visible, centered with its aspect ratio preserved.
OpenH264 uses up to three available CPU cores; frame rate is adjustable up to
15 fps in Settings. The X11 and
FBInk renderer choices appear when using the FBInk-enabled package. If FBink
cannot initialize, it remains selected and the stream reports the problem;
tap Render to switch to X11.

During a stream, the options bar changes renderer and e-ink mode, rotates the
video, forces a full refresh, toggles touch input, or exits. Exiting requests
a fresh host launch next time instead of resuming the previous session. The touchscreen
exit gesture is a two-second hold in the top-left corner. A keyboard Escape
key also exits.

Configuration and troubleshooting
---------------------------------

On-device settings are saved under
/mnt/us/.config/moonlight-keys/settings and take precedence over
moonlight.conf next to the executable. See README.md for configuration
options. For a black picture or missing windows with Sunshine, see
HOST-SETUP.md.

The app log is moonlight.log next to the executable. KUAL can copy its last
60 lines to /mnt/us/moonlight-log.txt.

License
-------

GPL-3.0; see LICENSE.
