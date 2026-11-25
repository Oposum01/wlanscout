# HTTP API and export

## HTTP API

The page asks one request at a time: the sensor serves a single
connection. Every `GET /api/...` endpoint answers on the chip's USB port
as well, with the same arguments and the same JSON; `POST /api/ota` and
`POST /api/snapshotimport` go over USB as a `PUT`, see
[Transport over USB](#transport-over-usb).

The JSON is valid UTF-8. SSIDs and device names come off the air as
any bytes; a byte that is not part of valid UTF-8 is sent as `\u00XX`,
the character of that value (an SSID `o\xa9\xf7` arrives as `"o©÷"`),
and control characters are left out.

| Endpoint | Purpose |
|----------|---------|
| `GET /` | the web interface |
| `GET /api/status` | mode, clock, timezone (`tz`, `dst`; `tzauto` 1 while it comes from the country codes, then `tzcc` the majority country, `tzccn` of `tzccof` BSSs, `tzccok` 0 when it has several timezones), addressing, client count |
| `GET /api/live` | current channel, per-channel RSSI, counters, capture state |
| `GET /api/plan` | the channel plan, so the browser keeps no copy, and the stored default plan (`def`) |
| `GET /api/history?range=ms` | waterfall history |
| `GET /api/changes` | the configuration changes of all BSSs, the last 64 since the start, newest first: `boot`, `seq` (the newest, also in `/api/status` as `cfgseq` with `boot`), `ver` (moves with every entry added or updated, in `/api/status` as `cfgver`), `changes` as `[number, seconds ago, BSSID, SSID, what, from, to]` |
| `GET /api/watch?bssid=…&since=N` | the graphs of one BSS: starts recording it, or renews it, and answers the samples from number N on, `&stop=1` ends it (a kept one only with `&keep=0` as well); `&keep=1` keeps it without renewals and across restarts (in NVS), `&keep=0` ends that; `&csv=1` gives the samples as CSV; one not kept stops by itself 3 min after its last renewal, four at most at once. `{"ok":1,"keep":0,"step":5,"next":…,"first":…,"epoch":…,"up":…,"s":[[rssi min, avg, max, util %, stations, seen, beacons], …]}`: sample k covers the step from `epoch` (0 without a clock; `up`, seconds since boot, as well) + k × `step`; the RSSI 0 when no beacon came, util 255 and stations 65535 without a BSS Load element; `first` the oldest still held (24 h); `{"ok":0,"error":"too many graphs open"}` |
| `GET /api/cfgmute?bssid=…&on=1` | keep a BSS's configuration changes out of the bell, `on=0` lets them in again; stored, up to 16 BSSs, listed in `/api/changes` as `muted` and marked `cfgmute` in `/api/networks` |
| `GET /api/networks` | BSSIDs with full details, plus the 6 GHz list; the further fields under [Fields of /api/networks](#fields-of-apinetworks) |
| `GET /api/clients` | stations with capabilities and roaming history |
| `GET /api/assochistory` | stations that were seen associating, kept permanently |
| `GET /api/assocclear` | empty that association history |
| `GET /api/flush?what=networks\|stations` | empty the network table (with the 6 GHz list) or the station table (with the names learned from stations), the Flush buttons; 400 for anything else |
| `GET /api/catalog` | the IE catalog; `?enable=0\|1` switches recording, kept across reboots; `reads` lists what the firmware decodes (element numbers, vendor keys, named AKM and cipher suites) |
| `GET /api/catalog.pcap` | every frame the catalog stored; `?k=kind&id=key` only those carrying one entry's element |
| `GET /api/catalogclear` | empty the catalog and its pcap file |
| `POST /api/snapshotimport?len=…&epoch=…&uptime=…&name=…` | put a snapshot back on the flash: a `multipart/form-data` upload of the file as Export gives it, without its `name`, which goes as an argument; it takes the next number. Answers `{"ok":1,"seq":…}` or `{"ok":0,"error":"…"}`. Over USB a `PUT` of the same URL, see [Transport over USB](#transport-over-usb) |
| `GET /api/coredump` | the core dump the last crash left in flash, as the raw partition image (`wlanscout-coredump-<ELF>.bin`), 404 without one; `?erase=1` deletes it; decoding it: [Troubleshooting](tuning.md#troubleshooting) |
| `GET /api/snapshot` | the snapshots on flash (`seq`, `epoch`, `uptime`, `bytes`, `name`) and `parts`, what one keeps; `?take=1` writes one now, `?get=seq` downloads one ([Formats](#formats)), `?del=seq` deletes it, `?rename=seq&name=…` names it (empty: no name), `?clear=1` deletes them all, `?parts=bits` sets what a snapshot keeps, stored; see [Settings and persistence](settings.md#snapshots) |
| `POST /api/ota` | firmware update: a `multipart/form-data` upload of `wlanscout.ino.bin`, written into the other slot; answers `{"ok":1,"version":"…"}` and restarts, or `{"ok":0,"error":"…"}`. Over USB a `PUT /api/ota?len=…` with the bytes, see [Transport over USB](#transport-over-usb) |
| `GET /api/otarollback` | restart into the firmware in the other slot |
| `GET /api/reboot` | reboot with the same firmware; the event log names it *reboot on request* |
| `GET /api/ble` | BLE devices |
| `GET /api/zb` | 802.15.4 channels and nodes |
| `GET /api/zbwaterfall?range=ms` | energy detect history |
| `GET /api/system` | memory, allocations, network, counters, CPU, crash and graphs: [Fields of /api/system](#fields-of-apisystem) |
| `GET /api/log` | event log, kept across restarts; `boot` per entry, `ago` -1 for an earlier start without a clock |
| `GET /api/export` | everything, for merging across sensors |
| `GET /api/pcap` | capture download |
| `GET /api/pcapclear` | drop captured frames |
| `GET /api/blehits`, `/api/blehitsclear` | BLE devices with a stable address and their hits; clear them |
| `GET /api/beaconversions?bssid=` | the beacon versions of one BSS, with their frames in base64 |
| `GET /api/config?channels=1,6,11` | set the channel selection |
| `GET /api/config?defchannels=1,6,11` | store the default channel plan, the scan is not changed |
| `GET /api/mode?...` | all control parameters, see below |
| `GET /api/time?epoch=…` | hand over a wall clock |

Control parameters on `/api/mode`:

```
mode=wifi|ble|zb     switch the radio mode
keepmode=0|1         start in the mode picked last, stored
fast=0|1             standard or fast sweep
capture=0|1          frame capture on or off
capmask=<bits>       1 beacons, 2 probe requests, 4 assoc, 8 action/auth/deauth
active=0|1           BLE active or passive scan
zbscan=0|1           802.15.4 frame capture or energy sweep
zbch=<n>|-1          lock 802.15.4 to a channel, -1 resumes the sweep
tz=<minutes>|auto    display offset from UTC, -720 to 840, standard time;
                     auto takes it from the country codes
dst=<rule>           auto daylight saving: 0 off, 1 Europe, 2 North America,
                     3 Australia south-east, 4 New Zealand
name=<text>          sensor name
```

Capture filters:

```
/api/pcap                         everything in the buffers
/api/pcap?src=ring|beacons|assoc  one buffer only
/api/pcap?src=versions            the beacon versions (not in "all")
/api/pcap?src=ble[&ta=..]         BLE advertisements, a file of its own
/api/pcap?ta=AA:BB:CC:DD:EE:FF    only frames sent by this address
/api/pcap?limit=1                 only the newest match

/api/pcapclear                    all buffers
/api/pcapclear?what=ring|beacons|assoc|versions|ble
```

`ta` filters on Addr2, the transmitter, so a BSSID selects that AP's
beacons and a station MAC selects its probe and association requests.

---

## Fields of /api/networks

Beside the BSSIDs' basic fields; "stations too" means `/api/clients`
has the same key.

- `clocks` `[frames of one beacon timer, of a second, switches between them, µs apart]` once a second timer is seen
- `dtim` `[DTIM period, group traffic buffered, stations with frames waiting]` from the TIM
- `ptsf` `[probe responses with timestamp 0, with one]` once one has 0
- `camkit` the status text of a wireless camera kit
- `uuid` the device UUID a LANCOM AP sends (00:A0:57 type 4)
- `center2` the second segment's centre channel of an 80+80 MHz BSS
- `eht` `[width MHz, centre channel, bitmap of the 20 MHz subchannels left out, lowest first]` from the EHT Operation element
- `ehtcap` `[EHT MAC capabilities, first EHT PHY octet, 4096-QAM 0/1]` (stations too)
- `mlrm` `[bit per link ID announced for removal, beacons left]` from the Reconfiguration Multi-Link element
- `twt` TWT support from HE Capabilities, 1 requester, 2 responder, 4 broadcast (stations too)
- `hecap` HE Capabilities detail, 1 1024-QAM, 2 UL MU-MIMO, 4 MU beamformer, 8 SU beamformer, 16 DCM (stations too)
- `heop` HE Operation, 1 TWT required, 2 co-hosted BSS
- `w8080` 1 for a station that offers 80+80 beside 160 MHz (`/api/clients`)
- `csamode`, `csaw` `[width MHz, centre, second centre]` `csacc` and `csatime` with a channel switch announced (`csach`, `csacnt`): its mode (1 stations stop sending), the width after it from the Wide Bandwidth Channel Switch (194, also inside the Channel Switch Wrapper 196), a new country and the Max Channel Switch Time (ext 52) in TU
- `vhtcap` VHT Capabilities detail in 5 GHz, 1 MU beamformer, 2 MU beamformee, 4 SU beamformer, 8 SU beamformee, 16 256-QAM, 32 read (stations too)
- `btwt` `[[ID, recommendation (4 restricted TWT), interval µs, duration µs], …]` the broadcast schedules of the TWT element
- `mdid` `[Mobility Domain ID, FT Capability and Policy]` with 802.11r (a station: the ID it asked for)
- `pdt` `[category, subcategory]` the WSC Primary Device Type from WPS or the P2P Device Info (7/1 a TV, 3 a printer; subcategory 0 when not the Wi-Fi Alliance's)
- `sec` `"Wi-Fi Direct"` for a Wi-Fi Direct device in its Listen state (SSID `DIRECT-`, Privacy bit, no RSN), not WEP
- `vblk` 28 or 36 when `vendor` came from an IEEE MA-M or MA-S/IAB block rather than a 24-bit OUI (stations too, also in the export)
- `xbox` 1 for an Xbox's link to its controllers (no SSID, Privacy without RSN, Microsoft's Network Cost element with 12 octets), `sec` `"Xbox Wireless"`
- `ftm` `[FTM requests to the BSSID, FTM frames from it, [stations ranging, four at most]]` once seen, with `ftmpar` (FTM Parameters, 9 octets), `ftmlci` (LCI field, 16 octets) and `ftmciv` (Location Civic field) in hex
- a station: `ftm` `[requests, FTM frames to it, [responders, three at most]]`, `ftmask` (1 position, 2 address asked for), `ftmpar`
- `bgap` `[gaps between back-to-back beacons on the BSS's channel, of them under half an interval, the shortest in µs]`
- a BSS whose configuration changed has `cfgn`, the changes seen, and `cfgch`, the last eight newest first as `[seconds ago, what, from, to]` (for `restart`, `from` is the uptime in seconds it ended; for `flapping`, the fields and the switches)

## Fields of /api/system

- `board` the board the build is for and `flash` its flash in bytes (with `mem.psramtotal` shown under Settings, *Sensor*)
- memory, allocations, network, counters, NTP detail (`ntpserver` the one asked first, `ntpdhcp` 1 when it came with the DHCP lease, option 42), why the device last started (`reset`)
- `now` the device time in epoch seconds, for the page to count on from (`utc` the same as text)
- `oui` the vendor table's entries, of them `oui28` MA-M and `oui36` MA-S/IAB blocks (also in `/api/status` and `/api/live`)
- `cpu` (load and per task `[name, share, stack never used]`, shares in tenths of a percent of the last `win` seconds)
- `net.eth` (1 with a W5500, 0 on USB alone, where `net.mac` and `sensorid` are only derived from the chip)
- `net.lease` (the DHCP client in lwIP: `state`, lease `time` and `left` in seconds, `ack` seconds since the last acknowledgement, `server`, `renew` seconds to the next renewal while bound, `renewals` since start)
- `crash` while a core dump is stored (`task`, `cause`, `pc`, `ra`, `mtval`, `mcause`, `size`, `elf` the start of the crashed firmware's ELF SHA-256, `same` 1 if that is the running one)
- `temp` (chip, °C)
- `loop` (`win` and `boot`: `[longest loop() round µs, longest request µs, its path, highest management queue fill]`, `queue` its capacity, `givenup` `[answers given up since the start, seconds since the last, its client]`)
- `pcap` the capture stores: the ring's keys bare (`count`, `bytes`, `mem`, `max`, `total`, `dropped`), the association ring's with `assoc`, the beacon versions' with `bv`, the BLE log's with `ble`; `beacons`, `beaconslots`, `blehits`
- `watch` (the graphs recorded: `[bssid, kept, epoch of the first sample or 0, its uptime, samples held]`)

---

## Transport over USB

The firmware answers the same API on the web server and on the chip's
USB Serial/JTAG port; the handlers do not know which side asked. On USB
a request is one line, the answer comes framed:

```
GET /api/networks?range=60000\n

0x02 "200 application/json\n"  <len lo><len hi><bytes> ... 0x00 0x00
```

The header line holds the status code, the content type and, for
downloads, a file name. The body follows in chunks of a two-byte length
(little endian) and the bytes; a chunk of length 0 ends the answer, so
binary pcap data needs no escaping. One request at a time; the page
queues them.

A snapshot goes the other way with a `PUT`: the request line with the
length, then exactly that many bytes, then the answer as above.

```
PUT /api/snapshotimport?len=95321&epoch=1791896972&uptime=27&name=Hall\n
<95321 bytes of the snapshot>

0x02 "200 application/json\n"  ... {"ok":1,"seq":12} ... 0x00 0x00
```

A firmware goes the same way, `PUT /api/ota?len=2548368` and the
image, with the checks of an update over the network. The bytes go in
pieces of 2048; after each piece but the last the client waits for the
byte 0x06 from the sensor, sent once it has written the piece away.
The USB port has no flow control of its own: what does not fit the
sensor's receive queue while it writes the flash is lost, and a file
sent in one go arrived with holes ("USB stalled"). The firmware reads
the bytes before any further request line; if they stop coming for
10 s it gives up and answers with an error.

Two details that matter for anyone writing their own client:

- **No reset on connect.** The page sets DTR and leaves RTS off. RTS on
  resets the chip, which is what a plain serial terminal on Linux does
  when it opens the port: the first answer then comes about 4 s later,
  after the boot, and the tables start empty.
- **Empty packets.** The ESP ends a transfer with a zero-length packet.
  Google's web-serial-polyfill hands those to its stream, which throws
  "chunk is empty"; the page reads the USB endpoint itself and skips
  them.

Measured on a phone: `/api/networks` with 62 networks, 49 kB, in about
100 ms.

---

## Formats

**Snapshot.** `wlanscout_snapshot`, `seq`,
`epoch` (0 without a clock; a snapshot dated later keeps the 0 in the
file and gets the time in its download), `uptime`, `fw`, `sensor`, `sensorid`,
`name` when it has one, `parts` and `answers`, the endpoint answers keyed by the URL the page asks for
(`/api/networks`, `/api/history?range=1800000`, …). With the IE
catalog it carries the catalog's frames as `catalogframes`: `hdr`, the
pcap header in base64, `frames`, each `[base64 of record header and
frame, [kind, key, …]]` with the catalog rows it carries, and `cut`, the
frames that did not fit.

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

Sizes with a table of 160 networks and 100 stations (a full table of
250 networks adds about half to the networks):

```
everything                        ~100 kB
sections=networks                  ~59 kB
since=<last poll>, 10 changed       ~8 kB
```

Structure:

```json
{
  "schema": 1,
  "sensor": { "id": "…", "name": "hall-north", "ip": "…", "fw": "3.0.0",
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
