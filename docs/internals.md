# Internals

For changing the firmware rather than using it.

## The sketch's files

Arduino joins the files into one before compiling: `wlanscout.ino`
first, then the numbered ones in name order. The numbers keep that
order, so a definition always comes before the code that uses it; a new
file goes in where its definitions are needed. `wlanscout.ino` keeps
`setup()` and `loop()` as one-line calls into `35_setup_loop.ino`,
because Arduino puts the declarations it generates in front of the main
file's first function.

| Files | What |
|-------|------|
| `01_heap_probe` | the heap watchdog |
| `02_model` | channels, OUI lookup, the network and station tables |
| `03_frame_log`, `23_pcap_export` | the capture rings, the pcap download |
| `04_requests`, `20_json_out`, `33_api_routes`, `34_usb` | requests over Ethernet or USB, chunked JSON, the routes, the USB protocol |
| `05_settings` | NVS settings, sensor name, time zone, web clients |
| `06_network` | W5500, DHCP, NTP, Ethernet events (`ENABLE_ETHERNET`) |
| `07_utility` | helpers, channel helpers |
| `08_ie_helpers` to `13_rx_path` | element parsers, stations, beacons, AWDL, the receive path |
| `14_catalog`, `24_api_catalog` | the IE catalog and its API |
| `15_eventlog` | the event log on flash and its API |
| `16_scanner`, `19_mode_switch` | sweeps and history, switching between Wi-Fi, BLE and 802.15.4 |
| `17_ble`, `18_zigbee` | BLE and IEEE 802.15.4 with their API |
| `21_api_wifi`, `22_api_status`, `28_api_system`, `29_api_export` | the JSON API |
| `25_ota`, `26_coredump` | updates from the page (`ENABLE_OTA`), the core dump |
| `27_watch` | graphs of one BSS |
| `30_web_page` | the page and the web server's handlers (`ENABLE_ETHERNET`) |
| `31_allocation` | PSRAM tables, Wi-Fi setup |
| `32_snapshots` | snapshots and their import |
| `35_setup_loop` | setup() and the main loop |
| `36_beacon_versions` | beacon versions of each BSS and their API |
| `37_ble_capture` | BLE advertisements for the pcap, the hit counter |

## Requests

The web server serves one connection at a time, so the page asks one
request at a time; parallel timers exhausted the sockets. While loop()
answers a request it reads no frames, so long answers fill the
management queue (see [Settings → Diagnostics](settings.md#diagnostics)).
The same handlers answer over USB, see
[Transport over USB](api.md#transport-over-usb).

## Settings in NVS

The settings are written only when one of them actually changes, never
on a poll, so the flash sees a handful of writes a session. A stored
channel mask of zero is ignored and the default kept, otherwise a sensor
on which every channel was once deselected would come up with a stalled
sweep.

One key lives apart, in the namespace `wlsboot`: `early`, the mark of
RESET pressed twice ([Settings](settings.md)), set at a start after
power-on and cleared 10 s later, two writes per such start.
