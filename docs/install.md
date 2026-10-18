# Installation

## Building

1. Install the board package **esp32 by Espressif Systems**
   (arduino-esp32) in the Boards Manager, version **3.3.0 or later**;
   older releases do not know the ESP32-C5. Tested with 3.3.12. If the
   package does not show up, add this URL under *File → Preferences →
   Additional boards manager URLs*:

```
https://espressif.github.io/arduino-esp32/package_esp32_index.json
```

2. The sketch is `source/wlanscout/` in the repository; open
   `wlanscout.ino` there:

```
source/wlanscout/
  wlanscout.ino            <- the sketch
  index_html.h             <- the web interface
  oui_table.h              <- vendor table, see below
  partitions.csv           <- partition table, see below
```

The folder has to carry the same name as the sketch; that is an Arduino
rule, not a choice here. Built images of the current version are in
`firmware/` as well, with `FLASH.txt`, for flashing without building.

3. The vendor table `oui_table.h` comes with the repository, generated
   from the IEEE MA-L registry. It is a sorted table with a binary
   search over it, with the usual legal suffixes trimmed from company
   names. The full registry is about 40000 entries and lands at
   700–900 kB in flash, which is why the large app partition below is
   needed. To pick up newer assignments, regenerate it — see
   [Updating the vendor table](#updating-the-vendor-table).

4. Select the board **ESP32C5 Dev Module** under *Tools → Board →
   esp32*, then set in the *Tools* menu:

| Setting | Value |
|---------|-------|
| Flash Size | **16MB (128Mb)** |
| Partition Scheme | any, it is ignored (see below) |
| PSRAM | **Enabled** — without it the allocations fail at boot and nothing comes up |

   The partition table is `partitions.csv` next to the sketch, which
   Arduino uses instead of the menu choice. It is the core's 16 MB
   default: two firmware slots of 6.25 MB for updates over the network,
   3.4 MB LittleFS for the IE catalog, NVS and `otadata` where
   every other scheme has them. With arduino-esp32 3.3.12 the sketch
   compiles to about 2.6 MB, roughly 600 kB of it the vendor table and
   170 kB the web interface. The compiler still checks the size against
   the menu's scheme; with "Huge APP" selected that is 3 MB and fits,
   with the 4 MB default (1.2 MB) the build is refused as too large.

The web interface has to stay in `index_html.h` rather than move into
the sketch. The sensor serves it gzip-packed from `index_html_gz.h`,
about a quarter of the size: after a change to `index_html.h` run

```sh
python3 tools/make_page_gz.py
```

The sketch refuses to build while the packed copy is of another page.

### Updating the vendor table

`tools/make_oui.py` turns the IEEE registry into `oui_table.h`. Fetch the
current listing from IEEE, as IEEE asks users to do, and run it:

```sh
curl -o oui.csv https://standards-oui.ieee.org/oui/oui.csv
python3 tools/make_oui.py oui.csv > source/wlanscout/oui_table.h
```

`oui.csv` is only the raw download and is kept out of the repository by
`.gitignore`. Options of the script: `--max-len N` truncates vendor
names (default 20), `--only PREFIX,…` keeps only vendors starting with
one of the given prefixes, to shrink the table.

### Updating over the network

**Settings → Firmware update** takes a new firmware: choose
`wlanscout.ino.bin` (in the Arduino IDE *Sketch → Export Compiled
Binary*, or from `firmware/` in the repository) and
press *Upload and restart*, over Ethernet or, from the phone, over USB
(see [Sensor on a phone over USB](usb.md#using-it)). The sensor writes it into the slot it is
not running from, restarts into it and the page reloads once the new
version answers (over USB it reconnects instead). Settings and the IE catalog stay, except rows of
elements the new firmware decodes, which go together with the frames
kept only for them (see [IE catalog](wifi-views.md#ie-catalog)).

The sensor refuses a file that is not an ESP32-C5 image or does not
carry the wlanscout+ version marker, and checks length and SHA-256
before it boots the new slot. There is no password yet: anyone who can
reach the web interface can replace the firmware.

A new firmware starts out unconfirmed. It confirms itself once
Ethernet has held an IP address for 30 s or it has answered a
request, over HTTP or USB, whichever comes first. If it crashes before that, the
bootloader starts the previous one again; if neither happens within
5 minutes, it rolls back on its own. The event log then says so, and
the failed version is not offered for going back to. *Back to vX* boots
the firmware in the other slot by hand.

---

## First start

1. Wire up the W5500 as in the [pin table](hardware.md).
2. Plug in Ethernet — to a switch or router, not straight into a PC
   unless you have a crossover cable, see [Hardware](hardware.md).
3. Power the board over **USB-C**. The W5500 draws its power from the
   board, so one cable is enough.
4. Read the address off the serial console — it is printed once DHCP
   has finished — and open `http://<address>/` in a browser. (Plain
   HTTP, no TLS: a browser that silently upgrades to `https://` will
   fail, so type the prefix if that happens. Put the device on a lab or
   management network, not on the open internet.)

The device requests an address over DHCP under the hostname
`wlanscout-<sensor name>` (lower case, anything but letters and digits
turned into hyphens), or `wlanscout-<last three MAC bytes>` while no
name is set, so several sensors are told apart in the DHCP server's
lease list. A new name is sent with the next lease, after a restart.
If none arrives it
reinitialises the Ethernet interface after two failed attempts, and
falls back to a static address after six:

```
#define ETH_STATIC_IP   IPAddress(192, 168, 1, 1)
```

Any link change returns it to DHCP. The serial console carries the full
state in every heap line, which is the only place to read it while the
device has no address:

```
[heap] free 122.6 kB  min 100.8 kB  maxblock 104.0 kB  psram 3814.9 kB
       drops mgmt 0 data 0 fcs 0  eth up/dhcp  mode wifi
```

Time comes from NTP once an address is up, re-synced every 15 minutes.
Until then the browser hands over its own clock: the sensor's page when
it opens, the static copy on a phone when it connects over USB (see
[Sensor on a phone over USB](usb.md)). A sensor with neither
— running on its own, without Ethernet — takes the time from an AP that
sends a Time Advertisement element (69): set once two frames of the same
AP agree, renewed after two minutes and then every 15 minutes from the
median of all APs heard. NTP outranks the browser, the browser outranks an AP; the settings page
shows which one the clock came from. Only the offset between
the wall clock and the boot timer is taken from NTP — every captured
frame keeps its own microsecond timestamp, so intervals within a capture
stay exact regardless of how coarse the network time is.
