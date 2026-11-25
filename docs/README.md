# wlanscout+ documentation

The [top-level README](../README.md) is the overview. These pages hold
the detail, roughly in the order you need them.

**Getting started**

- [Hardware](hardware.md) — boards, W5500 wiring, memory, Ethernet cabling
- [Installation](install.md) — board package, Arduino settings, vendor
  table, first start
- [Sensor on a phone over USB](usb.md) — without Ethernet, from Chrome
  over WebUSB, and the flasher

**Wi-Fi**

- [Sweep and reception](sweep.md) — channel plan, fast sweep, what the radio can
  and cannot see
- [Wi-Fi views](wifi-views.md) — spectrum, RSSI and waterfall, network
  table, BSS load, anomaly hints, Wi-Fi 7 multi-link APs, the IE catalog
- [Stations](stations.md) — station graph, live tables, association
  history, multi-link stations
- [Capture buffers](capture.md) — the pcap rings and what they keep

**Other radio modes**

- [BLE](ble.md) — advertising scan, passive and active, devices seen
  most, pcap
- [IEEE 802.15.4](ieee802154.md) — frame capture and energy waterfall

**Operation**

- [Settings and persistence](settings.md) — what survives a reboot, the
  clock, snapshots
- [HTTP API and export](api.md) — endpoints, the USB transport, file
  formats, multi-sensor JSON export
- [Tuning and troubleshooting](tuning.md) — limits, memory, common
  problems
- [Tests](testing.md) — tests for the page and the firmware, run
  without a sensor
- [Internals](internals.md) — the sketch's files and design notes, for
  changing the firmware

**Background**

- [Background](background.md) — who it is for, why it exists, what it
  is not, and how it was built
- [Outlook](outlook.md) — ideas that are not built yet
