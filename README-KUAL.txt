KUAL and Kindle library launchers
=================================

KUAL extension
--------------

Copy extensions/moonlight to /mnt/us/extensions/moonlight. Restart KUAL to
show the Sniper's Piss Moonshine menu. It provides:

- Start Sniper's Piss Moonshine: opens the touchscreen client.
- Stop Sniper's Piss Moonshine: stops a running client and restores the CPU governor.
- Save log: copies the last 60 log lines to /mnt/us/moonlight-log.txt.

Install the same extension folder under /var/local/moonlight for executable
files. KUAL first checks /var/local/moonlight, then /mnt/us/moonlight and the
extension folder itself. For another location, put the full directory
containing launch.sh on one line in
/mnt/us/extensions/moonlight/bin/moonlight.path.

Kindle library shortcut
-----------------------

Copy documents/moonlight.sh to /mnt/us/documents/moonlight.sh. With Kindle
scriptlet integration enabled, it appears as “Sniper's Piss Moonshine” by
“lmaokasdf123” and launches the same client as KUAL.

The client itself must be installed on an executable filesystem; see
README-INSTALL.txt. The launcher stores pairing keys in
/mnt/us/.config/moonlight-keys.
