# Background

## Who this is for

Wi-Fi professionals working in environments where **not every AP is
theirs**: apartment blocks, shared office floors, event venues,
campuses, industrial sites — say, a building with three tenants and four
unrelated networks in it. A WLAN management platform shows the APs it
manages in detail and everything else as a rogue entry with an RSSI
value. This fills in the rest.

**Where the clients actually are.** The station view answers questions
your own controller cannot, because the APs in question are not on it:
which station sits on which BSS, which BSS is
carrying the crowd while its neighbour is idle, which client keeps
bouncing between two APs, which ones probe constantly without ever
associating. In a hall with several vendors' equipment in it, that is
often the only way to see the load distribution at all. It only knows
the stations whose frames it hears: a client far away, or one whose
traffic goes out while the sweep is on another channel, stays
invisible or shows fewer frames than it sent.

**Capabilities in plain words, on both sides.** For an AP everything is
in the beacon, repeated every hundred milliseconds: the PHY generations
it announces, its basic rate set, 802.11k/v/r, PMF, WPS, the security
suites. A client is harder. An association request states what it can do
— channel width, spatial streams, 802.11k/v/r, PMF, which PHY
generations — and it says it once, at the moment of joining, and then
never again. The site <https://clients.mikealbano.com/> catalogues those
capabilities per device model from captures people contributed; this
does the same for the clients that are actually in the room right now,
and keeps the record after they have gone quiet. When a roaming problem
comes down to "does this handset even support 802.11r", that is the
difference between an afternoon of guessing and a glance at a table.

Waiting for a station to join by itself can take a while, so force it:
click the BSS in the spectrum view to lock the sweep to its channel,
then make the client reconnect — toggle its Wi-Fi, or walk it out of
range and back. The association request lands straight away, and with
the sweep parked there it will not be missed.

**Context rather than raw frames.** Wireshark will show you everything
and tell you nothing. The point here is the arrangement: the anomaly
hints, the PHY mode sets, the association history, the reported airtime
drawn where it belongs in the spectrum. Each of those is a thing you
could read out of a capture by hand; the value is that you do not have
to.

**Why it exists.** Range and throughput tests in the office
at simonwunderlich.de kept coming out wrong — a run that should have
been reproducible drifted from one day to the next. The
cause turned out to be colleagues bringing up APs for their own testing,
which on its own is harmless. What was not harmless was that some of
those APs had around a hundred clients associated for load tests. That
is a serious noise floor to measure against, and nothing in the test
setup showed it. An AP count alone would not have been enough either:
the number of APs had barely changed, only what was hanging off them.
Seeing stations per BSS is what made it obvious.

**A sensor, not a laptop.** It runs unattended on Ethernet with no
Wi-Fi NIC of its own, from a fixed position, for days. A survey from a
walking laptop and a long observation from one point answer different
questions, and the second one is harder to arrange with normal tools.

**Low barrier on purpose.** The intended deployment is simply to leave
one running permanently in the office: nothing to install, no client
software, no account, no agent on anyone's machine.
You open the address in a browser and it is there. That matters more
than it sounds — a diagnostic that takes twenty minutes to set up
is one you reach for after the problem, not before it, and by then the
thing you wanted to see has usually stopped happening.

It works as a portable tool too: hang it off a laptop's Ethernet port
and carry both to wherever the complaint is. Nothing to configure — with
no DHCP server answering it falls back to a static address by itself, it
only takes a moment.

**Why build it at all.** Nothing comparable turned up for the ESP32-C5
— there are scanners and sniffers for the older ESP32 parts, but none
that covers 5 GHz on this chip and presents the result in a form you
would want to look at for an hour. That was the gap worth filling.

If you want more and are willing to carry more, look at
[WLAN Pi](https://github.com/wlan-pi) — a community project building a
multi-purpose Wi-Fi analysis tool on single-board computers, and a
considerably more powerful instrument than this one. Full Linux, real
adapters, so 6 GHz and Wi-Fi 7 are within reach with the right module,
plus Kismet and throughput testing. Its `wlanpi-profiler` covers client
capabilities from the other side: it brings up an AP of its own so
clients associate on demand.

What this offers instead is a twenty-euro board that only listens, has
no operating system to keep current, and can be plugged in and
forgotten. If you do serious Wi-Fi work and do not know WLAN Pi, go and
look at it.

The inspiration was [WiFiAnalyzer by
VREM](https://play.google.com/store/apps/details?id=com.vrem.wifianalyzer),
which does the band overview on a phone extremely well.
[Aruba Utilities](https://play.google.com/store/apps/details?id=com.arubanetworks.arubautilities)
is a Wi-Fi monitor for professionals: the AP the phone is on with
channel, RSSI and PHY rates, the other APs it hears, handovers as
they happen, AP names where configured, logged to a text file and CSV
reports. It also shows much of the beacons' raw content, but leaves
putting it into context to the reader.
Both stop where Android stops: an app gets the elements of the beacons
and probe responses the phone's own scan picks up, no other frames,
and the radio cannot go promiscuous. Stations, association requests,
traffic per BSS and everything else that depends on hearing the whole
channel need different hardware.

### What it is not

**This is not a hacker tool and does not contain the functions of one.**
There is no deauthentication, no injection, no frame forging, no
handshake capture aimed at cracking, no WPS attack, no evil twin, no
client isolation probing. In scanner mode the radio only receives.

Everything shown is something a device within range broadcast into the
open air on its own initiative. The work is putting those broadcasts
into the right context — not extracting anything that was not already
being announced.

The security hints exist for the same reason the rest does: a BSS still
offering TKIP, or WPS with an unlocked PIN method, is a finding you want
in your own survey report. The tool names the condition and stops there.

---

## How this was built

**The firmware, the web interface and this documentation were written with
[Claude](https://claude.ai), Anthropic's AI assistant**, over a long
series of sessions: describing what the tool should do, reading the
output, testing it on real hardware, and feeding the results back. The
decoder work was driven by captures taken from actual access points, and
several rounds went into correcting things the model had got wrong.

That is worth stating plainly for two reasons. If you are reading the
code to learn from it, you should know where it came from. And if you
find something that is subtly wrong, that is the likeliest kind of
defect here: it compiles, it runs, it looks right, and the standard says
something slightly different.

**Feedback is very welcome** — issues, corrections, captures that are
decoded wrongly. Especially welcome are **cases worth flagging as an
anomaly that are not in the list yet**: this is a working Wi-Fi
engineer's tool, and the set of hints reflects one person's experience
of what matters. If something regularly costs you time in the field and
could be read out of a beacon or a management frame, say so and it can
go in.
