# Capture buffers

For a sense of scale: in a busy office environment over 24 hours the
sensor saw **more than 160 networks** and recorded **over 300
association requests** from roughly a hundred distinct stations. That is
the load the defaults below are sized against — 160 network slots is
exactly where that environment pushed the table into eviction, so raise
`MAX_NETWORKS` if yours is comparable.

| Buffer | Size | Holds |
|--------|------|-------|
| Main ring | 6000 frames | probe requests, probe responses, auth, deauth, action |
| Association ring | 300 frames | association and reassociation requests |
| Beacon slots | 160 | newest beacon per BSSID |

Beacons do not enter the ring. One TU is 1024 µs, so the usual beacon
interval of 100 TU is 102.4 ms and an AP sends a little under ten
beacons per second — and they are all alike, so a shared ring filled up
within minutes. The
newest beacon of each BSSID is held in its own slot and overwritten
instead. Association requests get a ring of their own because they are
the most informative frames a station ever sends and the rarest — in the
shared ring they were pushed out by probe requests while the table still
marked the station green.

**Frame size.** The parser always sees the whole frame. What is kept
for the download differs: the association ring stores requests whole,
up to 2304 octets, since a Wi-Fi 7 request with a Multi-Link element
easily runs past 600. The main ring and the beacon slots keep the first
512 octets of a frame and note its real length, so Wireshark shows a
longer frame as cut off by the capture rather than as malformed.

Each buffer can be downloaded and cleared on its own from the settings
page. Downloads are libpcap with a radiotap header per frame, so
Wireshark shows channel and signal strength. The FCS is stripped when
the frame is queued, and the radiotap flags say so.
