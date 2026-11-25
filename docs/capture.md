# Capture buffers

| Buffer | Size | Holds |
|--------|------|-------|
| Main ring | 64 kB to 5 MB, growing | probe requests, probe responses, auth, deauth, action |
| Association ring | 64 kB to 1.5 MB, growing | association and reassociation requests |
| Beacon slots | 250 | newest beacon per BSSID, one per network slot |
| Beacon versions | 64 kB to 1 MB, growing | the first beacon of each BSS and each one whose content changed |
| BLE advertisements | 64 kB to 1 MB, growing | every advertisement in BLE mode, see [BLE](ble.md) |

**The two rings grow.** Each starts with one chunk of 64 kB of PSRAM and
adds another whenever it is full, as long as PSRAM keeps a reserve free
for everything else: 1 MB for the main ring, 512 kB for the association
ring, which is rarer and more valuable and so may still grow when the
main ring has stopped. Once a ring may not grow any further, it empties
its oldest chunk and writes there, so the oldest frames go a chunk at a
time. The chunks are given back when the ring is cleared. Frames are
stored back to back at their own length: a 150-octet probe request
takes 168 octets. Settings → Capture shows for each ring how many frames
it holds, the memory it has taken, the most it may take and how many
frames were dropped.

Beacons do not enter the ring: an AP sends about ten a second, all
alike. The newest beacon of each BSSID is held in its own slot instead.
Association requests get a ring of their own because they are the most
informative frames a station sends and the rarest.

**Frame size.** The parser always sees the whole frame, and both rings
keep it whole, up to 2304 octets: a Wi-Fi 7 association request with a
Multi-Link element easily runs past 600, and so does a probe response
with many elements. Only the beacon slots keep the first 1536 octets of
a beacon and note its real length, so Wireshark shows a longer one as
cut off by the capture rather than as malformed; the beacon versions
keep it whole.

**Beacon versions.** The beacon slots hold only the newest beacon of a
BSS. The sensor also keeps the first one it hears, and then each beacon
whose content changed for good, in a log of its own: what an AP changed
can be compared in Wireshark, also where the decoded *Configuration
changes* do not reach (an unknown element, a vendor's). Content means the
elements, the beacon interval and the capabilities, without what changes
with every beacon: the timestamp, the TIM, BSS Load, Extended BSS Load,
the access delays, TWT, and the counts of a channel switch, a colour
change, Quiet, the EDCA and WMM update counts and the DTIM count in a
Multiple BSSID profile. An element that still changes 4 times within a
minute, a vendor's counter say, is left out for that BSS from then on.
A new content counts once a second beacon in a row agrees; one seen
before becomes the current version again and is not stored twice. Up to
8 versions per BSS, the oldest going first; when the log is full its
oldest frames go. They are lost with a restart and are not part of a
snapshot. The tooltip of a pinned BSS lists them in its *Beacons* tab,
each with what changed from the one before.

Each buffer can be downloaded and cleared on its own under Settings →
Capture, *Stored on the sensor*, which also shows how full it is. Downloads are libpcap with a radiotap header per frame, so
Wireshark shows channel, signal strength and the noise floor the radio
reported for the frame, and for 802.11b/a/g frames the rate it was sent
at, with CCK or OFDM and a short preamble marked. A frame in an HT or
later PPDU has no single legacy rate, so it gets none. The FCS is
stripped when the frame is queued, and the radiotap flags say so.

The header is 16 octets from 2.8.27 on, 15 before (no rate, no noise).
The IE catalog's frames on flash may hold both; the firmware and
Wireshark read each frame's own header length, so that does not
matter.
