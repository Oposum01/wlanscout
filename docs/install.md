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
   `wlanscout.ino` there. The IDE opens the numbered files next to it
   as tabs of the same sketch:

```
source/wlanscout/
  wlanscout.ino            <- the sketch: includes, settings, setup() and loop()
  01_heap_probe.ino ...    <- the rest, one file per part, see below
  37_ble_capture.ino
  config.h                 <- the board and what is built in, see below
  index_html.h             <- the web interface
  oui_table.h              <- vendor table, see below
  partitions.csv           <- partition table, see below
  partitions_xiao.csv      <- the same for 8 MB flash
  partitions_xiao_1slot.csv <- 8 MB flash, one firmware slot (option)
```

Which file holds what, and why they are numbered:
[Internals](internals.md#the-sketchs-files).

The folder has to carry the same name as the sketch; that is an Arduino
rule, not a choice here. Ready-built images come with each
[release](https://github.com/Oposum01/wlanscout/releases/latest), with
`FLASH.txt` saying which file goes where.

3. The vendor table `oui_table.h` comes with the repository, generated
   from the IEEE registries: MA-L (24-bit OUIs) and the small blocks
   inside OUIs IEEE holds itself, MA-M (28 bit), MA-S and IAB (36 bit),
   so an address from such a block is named after its holder, not
   after "IEEE Registration Authority". They are sorted tables with a
   binary search over them, with the usual legal suffixes trimmed from
   company names. Together about 59000 entries, they take about 1 MB
   of the firmware. To pick up newer assignments, regenerate it — see
   [Updating the vendor table](#updating-the-vendor-table).

4. Select the board **ESP32C5 Dev Module** under *Tools → Board →
   esp32*, then set in the *Tools* menu:

| Setting | Value |
|---------|-------|
| Flash Size | **16MB (128Mb)** |
| Partition Scheme | **Custom** (see below) |
| PSRAM | **Enabled** — without it the allocations fail at boot and nothing comes up |

   The partition table is `partitions.csv` next to the sketch, which
   Arduino uses instead of the menu choice; its layout is under
   [Memory](hardware.md#memory). With arduino-esp32 3.3.12 the sketch
   compiles to about 3.1 MB, about 1 MB of it the vendor table and
   170 kB the web interface. The IDE still checks the size against the
   menu's scheme: "Custom" allows up to 16 MB, "Huge APP" (3 MB) refuses
   the build as too large.

5. **`config.h`** says what the board has and what is built in. Its
   defaults are for the Waveshare kit with a W5500; nothing there
   depends on the board picked in the IDE.

| Define | Default | What |
|--------|---------|------|
| `BOARD_NAME` | Waveshare ESP32-C5-WIFI6-KIT | shown in Settings → Sensor |
| `BOARD_ID` | kit | carried in the image for the flasher's check, and the release folder |
| `ENABLE_ETHERNET` | 1 | W5500, DHCP, NTP, the web server and the page; 0: USB only |
| `ENABLE_OTA` | 1 | updates from the page; needs two firmware slots (`partitions.csv`, `partitions_xiao.csv`) |
| `ENABLE_IEEE802154` | 1 | 0 if the link step misses `esp_ieee802154_*` |
| `W5500_SCLK` ... `W5500_RST` | 23, 24, 25, 26, 4 | the W5500 pins |
| `BOOT_BUTTON` | 28 | the snapshot button |
| `STATUS_LED`, `STATUS_LED_TYPE` | 27, `LED_RGB` | `LED_PLAIN_HIGH`, `LED_PLAIN_LOW` for a single-colour LED, `LED_NONE` |

   Each can be changed in the file or given as an extra compiler flag
   (`-DENABLE_OTA=0`). A build with PSRAM disabled or USB CDC On Boot
   enabled stops with an error saying which setting to change.

   **Seeed Studio XIAO ESP32-C5:** `tools/build_xiao.sh` builds it, see
   below. In the IDE: put the XIAO values from the comment in
   `config.h` in front of its defaults, copy `partitions_xiao.csv` over
   `partitions.csv`, and pick the board **XIAO_ESP32C5** with PSRAM
   **Enabled** and USB CDC On Boot **Disabled** (its menu starts on
   Enabled; `Serial` would then share the USB port with the API). With
   the 16 MB table the bootloader rejects the partitions ("exceeds
   flash chip size") and nothing starts. The build is about 2.75 MB;
   the IDE checks it against its own 8 MB scheme (3.2 MB), close to the
   slot of 3.25 MB. Its pins 23 to 26 carry I2C and the battery
   divider: leaving `ENABLE_ETHERNET` on would drive them as the
   W5500's. What differs on the XIAO, the option with one slot
   included, is under [XIAO build](hardware.md#xiao-build).

The web interface has to stay in `index_html.h` rather than move into
the sketch. The sensor serves it gzip-packed from `index_html_gz.h`,
about a quarter of the size: after a change to `index_html.h` run

```sh
python3 tools/make_page_gz.py
```

The sketch refuses to build while the packed copy is of another page.

### arduino-cli and releases

```sh
MAP="-ffile-prefix-map=$HOME/.arduino15=~/.arduino15"
arduino-cli compile \
    --fqbn esp32:esp32:esp32c5:FlashSize=16M,PartitionScheme=custom,PSRAM=enabled \
    --build-property "upload.maximum_size=6553600" \
    --build-property "compiler.c.extra_flags=$MAP" \
    --build-property "compiler.cpp.extra_flags=$MAP" \
    --build-path build source/wlanscout

# The XIAO: the sketch with the values from config.h's comment and
# partitions_xiao.csv
tools/build_xiao.sh build-xiao

python3 tools/make_release.py build --xiao build-xiao
```

`upload.maximum_size` checks the image against the real slot of
6.25 MB. The prefix map keeps the builder's home directory out of the
images, where the core's source file names end up in its log messages.
`make_release.py` writes the release site at the top of the checkout,
or with `--out DIR` elsewhere for publishing: `firmware/` with the
images, `FLASH.txt` and `version.json`, `firmware/xiao/` the same for
the XIAO, and next to them the static page and the flasher
([Sensor on a phone over USB](usb.md)). These files are generated but
kept in git, so `firmware/` and the page at the top always hold the
version in `wlanscout.ino`.

### Updating the vendor table

`tools/make_oui.py` turns the IEEE registries into `oui_table.h`. Fetch
the current listings from IEEE, as IEEE asks users to do, and run it:

```sh
curl -o oui.csv https://standards-oui.ieee.org/oui/oui.csv
curl -o mam.csv https://standards-oui.ieee.org/oui28/mam.csv
curl -o oui36.csv https://standards-oui.ieee.org/oui36/oui36.csv
curl -o iab.csv https://standards-oui.ieee.org/iab/iab.csv
python3 tools/make_oui.py oui.csv mam.csv oui36.csv iab.csv \
    > source/wlanscout/oui_table.h
```

The script tells the registries apart by the length of the assignment,
so any of the last three may be left out. IEEE's server rejects some
browser user agents; curl's own goes through. The CSV files are only
the raw downloads and are kept out of the repository by
`.gitignore`. Options of the script: `--max-len N` truncates vendor
names (default 20), `--only PREFIX,…` keeps only vendors starting with
one of the given prefixes, to shrink the table.

### Updating over the network

**Settings → Sensor → Firmware** takes a new firmware: choose
`wlanscout.ino.bin` (in the Arduino IDE *Sketch → Export Compiled
Binary*, or from `firmware/` in the repository) and
press *Upload and restart*, over Ethernet or, from the phone, over USB
(see [Sensor on a phone over USB](usb.md#using-it)). The sensor writes it into the slot it is
not running from, restarts into it and the page reloads once the new
version answers (over USB it reconnects instead). Settings and the IE catalog stay, except rows of
elements the new firmware decodes, which go together with the frames
kept only for them (see [IE catalog](wifi-views.md#ie-catalog)).

**New firmware from GitHub.** Once a session, when Settings opens,
and with *Check for new firmware*, the page asks the public repository
on GitHub Pages for `firmware/version.json` (version, size and SHA-256
of the newest release). The browser asks, not the sensor, so a sensor
without internet access is no obstacle; a browser without one shows
nothing, or says so after the button. For a newer release *Download
vX* fetches `wlanscout.ino.bin` into the browser and checks its size,
and its SHA-256 where the browser allows it (the static copy over https
does, the sensor's own http page does not); *Install vX* then writes
it like a chosen file, after the same question. Pages may serve the
old files for some minutes after a release, a size that does not match
is refused with "try again in a minute".

The sensor refuses an image the flasher would refuse too (another chip,
no wlanscout+ marker, another board, more flash than it has; see
[Flashing from the phone](usb.md#flashing-from-the-phone)) and checks
length and SHA-256 before it boots the new slot. There is no password
yet: anyone who can reach the web interface can replace the firmware.

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

Time comes from NTP once an address is up; where else it can come
from is under [Time](settings.md#time).
