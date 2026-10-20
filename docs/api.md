# HTTP API and export

## HTTP API

The web interface polls these sequentially, one request at a time: the
device serves a single connection and parallel timers exhausted the
sockets.

Every `GET /api/...` endpoint answers on the chip's USB port as well,
with the same arguments and the same JSON; the framing is described in
[Sensor on a phone over USB](usb.md). `POST /api/ota` and `POST /api/snapshotimport` go over
USB as a `PUT`.

The JSON is valid UTF-8. SSIDs and device names come off the air as
any bytes; a byte that is not part of valid UTF-8 is sent as `\u00XX`,
the character of that value (an SSID `o\xa9\xf7` arrives as `"o©÷"`),
and control characters are left out.

| Endpoint | Purpose |
|----------|---------|
| `GET /` | the web interface |
| `GET /api/status` | mode, clock, timezone, addressing, client count |
| `GET /api/live` | current channel, per-channel RSSI, counters, capture state |
| `GET /api/plan` | the channel plan, so the browser keeps no copy, and the stored default plan (`def`) |
| `GET /api/history?range=ms` | waterfall history |
| `GET /api/changes` | the configuration changes of all BSSs, the last 64 since the start, newest first: `boot`, `seq` (the newest, also in `/api/status` as `cfgseq` with `boot`), `ver` (moves with every entry added or updated, in `/api/status` as `cfgver`), `changes` as `[number, seconds ago, BSSID, SSID, what, from, to]` |
| `GET /api/cfgmute?bssid=…&on=1` | keep a BSS's configuration changes out of the bell, `on=0` lets them in again; stored, up to 16 BSSs, listed in `/api/changes` as `muted` and marked `cfgmute` in `/api/networks` |
| `GET /api/networks` | BSSIDs with full details, plus the 6 GHz list; `clocks` `[frames of one beacon timer, of a second, switches between them, µs apart]` once a second timer is seen; `dtim` `[DTIM period, group traffic buffered, stations with frames waiting]` from the TIM; `ptsf` `[probe responses with timestamp 0, with one]` once one has 0; `mdid` `[Mobility Domain ID, FT Capability and Policy]` with 802.11r (a station: the ID it asked for); `ftm` `[FTM requests to the BSSID, FTM frames from it, [stations ranging, four at most]]` once seen, with `ftmpar` (FTM Parameters, 9 octets), `ftmlci` (LCI field, 16 octets) and `ftmciv` (Location Civic field) in hex; a station: `ftm` `[requests, FTM frames to it, [responders, three at most]]`, `ftmask` (1 position, 2 address asked for), `ftmpar`; `bgap` `[gaps between back-to-back beacons on the BSS's channel, of them under half an interval, the shortest in µs]`; a BSS whose configuration changed has `cfgn`, the changes seen, and `cfgch`, the last eight newest first as `[seconds ago, what, from, to]` (for `restart`, `from` is the uptime in seconds it ended; for `flapping`, the fields and the switches) |
| `GET /api/clients` | stations with capabilities and roaming history |
| `GET /api/assochistory` | stations that were seen associating, kept permanently |
| `GET /api/assocclear` | empty that association history |
| `GET /api/flush?what=networks\|stations` | empty the network table (with the 6 GHz list) or the station table (with the names learned from stations), the Flush buttons; 400 for anything else |
| `GET /api/catalog` | the IE catalog; `?enable=0\|1` switches recording, kept across reboots; `reads` lists what the firmware decodes (element numbers, vendor keys, named AKM and cipher suites) |
| `GET /api/catalog.pcap` | every frame the catalog stored; `?k=kind&id=key` only those carrying one entry's element |
| `GET /api/catalogclear` | empty the catalog and its pcap file |
| `POST /api/snapshotimport?len=…&epoch=…&uptime=…&name=…` | put a snapshot back on the flash: a `multipart/form-data` upload of the file as Export gives it, without its `name`, which goes as an argument; it takes the next number. Answers `{"ok":1,"seq":…}` or `{"ok":0,"error":"…"}`. Over USB a `PUT` of the same URL, see [Sensor on a phone over USB](usb.md#how-it-works) |
| `GET /api/coredump` | the core dump the last crash left in flash, as the raw partition image (`wlanscout-coredump-<ELF>.bin`), 404 without one; `?erase=1` deletes it. Decode it with `esp-coredump --chip esp32c5 info_corefile -t raw -c <file> wlanscout.ino.elf`, see [Tuning and troubleshooting](tuning.md#troubleshooting) |
| `GET /api/snapshot` | the snapshots on flash (`seq`, `epoch`, `uptime`, `bytes`, `name`) and `parts`, what one keeps; `?take=1` writes one now, `?get=seq` downloads one, `?del=seq` deletes it, `?rename=seq&name=…` names it (empty: no name), `?clear=1` deletes them all, `?parts=bits` sets what a snapshot keeps, stored; see [Settings and persistence](settings.md#snapshots) |
| `POST /api/ota` | firmware update: a `multipart/form-data` upload of `wlanscout.ino.bin`, written into the other slot; answers `{"ok":1,"version":"…"}` and restarts, or `{"ok":0,"error":"…"}`. Over USB a `PUT /api/ota?len=…` with the bytes, see [Sensor on a phone over USB](usb.md#how-it-works) |
| `GET /api/otarollback` | restart into the firmware in the other slot |
| `GET /api/ble` | BLE devices |
| `GET /api/zb` | 802.15.4 channels and nodes |
| `GET /api/zbwaterfall?range=ms` | energy detect history |
| `GET /api/system` | memory, allocations, network, counters, NTP detail, why the device last started (`reset`); `cpu` (load and per task `[name, share, stack never used]`, shares in tenths of a percent of the last `win` seconds), `net.lease` (the DHCP client in lwIP: `state`, lease `time` and `left` in seconds, `ack` seconds since the last acknowledgement, `server`, `renew` seconds to the next renewal while bound, `renewals` since start), `crash` while a core dump is stored (`task`, `cause`, `pc`, `ra`, `mtval`, `mcause`, `size`, `elf` the start of the crashed firmware's ELF SHA-256, `same` 1 if that is the running one), `temp` (chip, °C), `loop` (`win` and `boot`: `[longest loop() round µs, longest request µs, its path, highest management queue fill]`, `queue` its capacity, `givenup` `[answers given up since the start, seconds since the last, its client]`) |
| `GET /api/log` | event log, kept across restarts; `boot` per entry, `ago` -1 for an earlier start without a clock |
| `GET /api/export` | everything, for merging across sensors |
| `GET /api/pcap` | capture download |
| `GET /api/pcapclear` | drop captured frames |
| `GET /api/config?channels=1,6,11` | set the channel selection |
| `GET /api/config?defchannels=1,6,11` | store the default channel plan, the scan is not changed |
| `GET /api/mode?...` | all control parameters, see below |
| `GET /api/time?epoch=…` | hand over a wall clock |

Control parameters on `/api/mode`:

```
mode=wifi|ble|zb     switch the radio mode
fast=0|1             standard or fast sweep
capture=0|1          frame capture on or off
capmask=<bits>       1 beacons, 2 probe requests, 4 assoc, 8 action/auth/deauth
active=0|1           BLE active or passive scan
zbscan=0|1           802.15.4 frame capture or energy sweep
zbch=<n>|-1          lock 802.15.4 to a channel, -1 resumes the sweep
tz=<minutes>         display offset from UTC, -720 to 840
name=<text>          sensor name
```

Capture filters:

```
/api/pcap                         everything in the buffers
/api/pcap?src=ring|beacons|assoc  one buffer only
/api/pcap?ta=AA:BB:CC:DD:EE:FF    only frames sent by this address
/api/pcap?limit=1                 only the newest match

/api/pcapclear                    all buffers
/api/pcapclear?what=ring|beacons|assoc
```

`ta` filters on Addr2, the transmitter, so a BSSID selects that AP's
beacons and a station MAC selects its probe and association requests.

---

## Multi-sensor export

> **Status: designed, not proven.** The endpoint works and the document
> is stable, but no collector has been built against it and no
> multi-sensor deployment has been run. The reasoning below is what the
> format was designed for, not experience with it. Expect the schema to
> move once something actually consumes it.

`/api/export` returns one JSON document meant to be merged with the
output of other sensors covering the same area. Three things are
different from the endpoints the web interface uses:

- every timestamp is **absolute UTC epoch seconds**, not an age, because
  ages from different sensors cannot be lined up
- the sensor identifies itself by its Ethernet MAC, which is stable
  across reboots, plus an optional name stored in NVS
- sections and a `since` cut-off keep the document small enough to poll
  often

```
/api/export
/api/export?sections=networks,clients
/api/export?since=1790000000
```

Sizes with a full table of 160 networks and 100 stations:

```
everything                        ~100 kB
sections=networks                  ~59 kB
since=<last poll>, 10 changed       ~8 kB
```

Structure:

```json
{
  "schema": 1,
  "sensor": { "id": "…", "name": "hall-north", "ip": "…", "fw": "2.6.11",
              "uptime": 8421, "time": 1790000000, "clock": 2, "mode": 0 },
  "scan":    { "selected": 33, "plan": 11, "fast": 1, "dwell": 120,
               "sweeps": 4213, "channels": [1, 5, 9, …] },
  "networks": [ … ], "clients": [ … ], "sixghz": [ … ],
  "ble": [ … ], "zb": { "channels": [ … ], "nodes": [ … ] },
  "counters": { … }
}
```

`sensor.clock` says where the time came from: 2 is NTP, 1 the browser,
3 an AP's Time Advertisement element, 0 unset. At anything but 2 the
timestamps should be treated with care when merging; an AP's clock can
be off by a minute or more.

`schema` is raised whenever the shape changes, so a collector does not
have to guess from the firmware version.
