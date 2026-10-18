# Outlook

Ideas that are not built, in no particular order. None of them is
promised; they are written down so they are not lost, and because
someone else may want one of them badly enough to do it.

**A spectrum front end.** A second C5 running
[esp-sdr](https://github.com/ESPARGOS/esp-sdr) as a dedicated SDR,
connected over UART, to cover the one thing this cannot do: see energy
that is not an 802.11 frame. It cannot live in the same chip, as the
[Wi-Fi views](wifi-views.md#the-four-wi-fi-views) explain, but two of them would be a complete picture for
about forty euros.

**Something that consumes the export.** `/api/export` was designed for
merging several sensors covering one area, and nothing has ever been
written against it. Until something does, the schema is a guess.

**Which security clients really use, per BSS.** The association
history already shows the AKM each client picked in its association
request. Counting them per BSS, together with the algorithm in the
authentication frames (Open System, SAE, FT), would show what a network
is actually used with rather than what its beacon offers: whether a
WPA3 transition AP really gets SAE clients or whether everyone falls
back to PSK.

**More anomaly hints.** The list reflects one person's idea of what
matters in the field. See the [feedback note](background.md#how-this-was-built).

**Decoding past Wi-Fi 7**, and **verifying UNII-4**, both described in
[Sweep and reception](sweep.md).
