# Hardware

Built for the **Waveshare ESP32-C5-WIFI6-KIT-NXRX** with a W5500
Ethernet module on SPI ([this board](https://www.amazon.de/dp/B0GDFY22Q1)
and [this W5500 module](https://www.amazon.de/dp/B0FB94KPVT) were used).

| Signal | GPIO |
|--------|------|
| SCLK   | 23   |
| MOSI   | 24   |
| MISO   | 25   |
| CS     | 26   |
| RST    | 4    |
| INT    | not connected (`-1`, the driver polls the link over SPI) |

The pins are defines at the top of the sketch.

> **Flash and PSRAM.** The kit carries two kinds of memory. The
> **16 MB flash** keeps what survives a restart: two firmware slots of
> 6.25 MB, so an update goes into the slot not running, 3.4 MB for
> the IE catalog, the snapshots and the event log, and a small area for
> the settings ([Installation](install.md)). The **8 MB PSRAM** is working memory:
> roughly 5 MB of it are allocated at boot, the capture ring alone takes
> 3.1 MB.
>
> **PSRAM is mandatory.** A board without PSRAM, such as the
> **ESP32-C5-Mini**, will not run this firmware — the allocations fail
> at startup and nothing comes up. Check the part number before
> ordering: the Waveshare kit above carries 8 MB of PSRAM, several
> otherwise identical C5 boards carry none.

> **Direct connection to a PC:** the W5500 does not support auto-MDI/MDI-X
> (datasheet 5.5.6). A straight cable works to a switch or router; for a
> PC or another W5500 you need a crossover cable, unless the other end
> has working auto-MDI/MDI-X. Not every laptop does.

## USB ports

The kit has two USB-C ports:

| Port   | Goes to | Used for |
|--------|---------|----------|
| **UART** | CH343 USB-to-serial bridge, GPIO 11/12 | the serial log (115200 baud), uploading from the Arduino IDE |
| **USB**  | the chip's own USB Serial/JTAG, GPIO 13/14 | the sensor on a phone over WebUSB and flashing from the phone ([Sensor on a phone over USB](usb.md)); uploading from a PC works here too |

Next to them, **BOOT** (GPIO 28) takes a snapshot when pressed while
the sensor runs, and the RGB LED (GPIO 27) answers, see
[Snapshots](settings.md#snapshots); held while the board starts it
enters the download mode instead.

Either one powers the board. A plain serial terminal on the **USB**
port resets the chip when it opens it (RTS); the page sets DTR only
and leaves it running.

## Enclosure

[`3d-printed-housing/`](../3d-printed-housing/) holds two cases, each as
OpenSCAD source and the printable STLs.

**Ethernet sensor** (board and W5500): `esp32c5_w5500_v31.scad` with
`esp32c5_w5500_v31_tray.stl` and `esp32c5_w5500_v31_hood.stl`. The
boards sit in pockets that follow their outlines, the ESP32-C5 with its
components down, the USB-C ports and the RJ45 at the front. They are
wired directly, with the wires kept at least 15 mm from the antenna.
The hood snaps over the tray without screws. RESET and BOOT are pressed
through flexure tongues in the floor, engraved underneath. The comment
block at the top of the `.scad` file describes the layout and the
measurements to check before printing.

![The Ethernet sensor in its case, closed and open](images/02-hardware.jpg)

**Phone sensor** (board alone, see [Sensor on a phone over
USB](usb.md)): `esp32-c5-magsafe-case-v6.scad` with
`esp32-c5-magsafe-case-base-v6.stl` and
`esp32-c5-magsafe-case-lid-v6.stl`, a MagSafe case for the back of a
phone. The metal ring is glued into the recess on the underside; these
[MagSafe metal rings](https://www.amazon.de/dp/B0DH217VB7) were used.
The lid snaps on and has flexure buttons for RESET and the snapshot
(BOOT), *USB* and *Serial* stand below the ports.

![The MagSafe case on the back of a phone: closed, lid off, and the underside with the ring and a short angled USB-C cable](images/23-phone-sensor.jpg)

This case is a compromise for the radio. Espressif recommends at least
15 mm clearance around the PCB antenna in all directions
([ESP32-C5 PCB layout design](https://docs.espressif.com/projects/esp-hardware-design-guidelines/en/latest/esp32c5/pcb-layout-design.html)).
Here the board lies about 1.5 mm above the phone's back, and the steel
ring runs right under the antenna. Expect weaker signals and fewer
networks than from the Ethernet sensor or the bare board held away
from the phone.

It is adapted from
[Biscuit – DIY MagSafe case, ESP32-C5 Waveshare](https://makerworld.com/de/models/2590419-biscuit-diy-magsafe-case-esp32-c5-waveshare)
by [Dtmcnamara](https://makerworld.com/de/@Dtmcnamara), licensed under
[CC BY-NC-SA 4.0](https://creativecommons.org/licenses/by-nc-sa/4.0/).
These three files are under the same licence, not under the GPL of the
rest of wlanscout: free to share and adapt, with credit, not for
commercial use, and changes under the same licence. The `.scad` file
lists what was changed.

Without the W5500 the bare board also fits the
[Waveshare ESP32-C5 case](https://makerworld.com/de/models/1974552-waveshare-esp32-c5-case)
on MakerWorld.
