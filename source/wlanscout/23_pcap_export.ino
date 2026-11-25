// SPDX-License-Identifier: GPL-2.0-only WITH AdditionRef-wlanscout-linking-exception
// SPDX-FileCopyrightText: 2026 Dr.-Ing. Simon Wunderlich Systementwicklung & Beratung

// ============================================================
// API: pcap download
//
// Classic libpcap format with a radiotap header per frame, so
// Wireshark shows channel, signal strength, noise floor and the
// rate a frame was sent at alongside the dissected management
// frame. Link type 127 is LINKTYPE_IEEE802_11_RADIOTAP.
//
// The FCS was stripped when the frame was queued, so the
// radiotap flags must not claim one is present.
// ============================================================

// RADIOTAP_LEN is defined next to MGMT_PARSE_MAX

/*
 * A kept rate code (rxRateOf()) in the 500 kbit/s units of the
 * radiotap Rate field, 0 for none. The codes are wifi_phy_rate_t:
 * 0-3 long and 5-7 short preamble 802.11b, 8-15 the L-SIG codes.
 */
uint8_t radiotapRateOf(uint8_t rxRate)
{
    static const uint8_t units[16] = {
        2, 4, 11, 22,                // 1, 2, 5.5, 11, long preamble
        0,
        4, 11, 22,                   // 2, 5.5, 11, short preamble
        96, 48, 24, 12,              // 48, 24, 12, 6
        108, 72, 36, 18              // 54, 36, 18, 9
    };

    return rxRate < 16 ? units[rxRate] : 0;
}

/*
 * Builds the RADIOTAP_LEN octets in front of a frame:
 *
 *    0  version, pad, length (16), present bits
 *    8  Flags             9  Rate (bit 2)
 *   10  Channel: frequency and flags
 *   14  dBm antenna signal
 *   15  dBm antenna noise (bit 6)
 *
 * Rate and noise are left out of the present bits when the frame
 * has none (a frame in an HT or later PPDU has no single legacy
 * rate); the octet stays, as padding, so the length never changes.
 */
void pcapBuildRadiotap(uint8_t *rt, uint8_t channel, int8_t rssi,
                       int8_t noise, uint8_t rxRate)
{
    memset(rt, 0, RADIOTAP_LEN);

    rt[0] = 0;                       // version
    rt[1] = 0;                       // pad
    rt[2] = RADIOTAP_LEN;            // length, little endian
    rt[3] = 0;

    uint8_t rate = radiotapRateOf(rxRate);

    // Flags (bit 1), Channel (bit 3), dBm antenna signal (bit 5)
    uint32_t present = (1u << 1) | (1u << 3) | (1u << 5);

    if (rate)
        present |= 1u << 2;          // Rate

    // The driver says dBm; 0 or above is no reading
    if (noise < 0)
        present |= 1u << 6;          // dBm antenna noise

    rt[4] = present & 0xFF;
    rt[5] = (present >> 8) & 0xFF;
    rt[6] = (present >> 16) & 0xFF;
    rt[7] = (present >> 24) & 0xFF;

    // Flags: no FCS at end; short preamble for codes 5-7
    rt[8] = (rxRate >= 5 && rxRate <= 7) ? 0x02 : 0x00;

    // Rate, or padding for the 2 byte alignment of Channel
    rt[9] = rate;

    uint16_t freq = pcapChannelFreq(channel);

    /*
     * The band the channel sits in, and how the frame was
     * modulated: CCK for 802.11b rates, OFDM for everything else,
     * also when the rate is unknown (HT and later are OFDM).
     */
    bool     cck     = rxRate <= 7;
    uint16_t chFlags = (channel > 14) ? 0x0100 : 0x0080;

    chFlags |= cck ? 0x0020 : 0x0040;

    rt[10] = freq & 0xFF;
    rt[11] = (freq >> 8) & 0xFF;
    rt[12] = chFlags & 0xFF;
    rt[13] = (chFlags >> 8) & 0xFF;

    rt[14] = (uint8_t)rssi;          // signed dBm

    if (noise < 0)
        rt[15] = (uint8_t)noise;     // signed dBm
}

/*
 * Length of the radiotap header in front of a frame read back from
 * a pcap file, 0 when it cannot be one of ours. Files written before
 * 2.8.27 have 15 octets, later ones RADIOTAP_LEN.
 */
uint16_t radiotapLenOf(const uint8_t *rt, uint32_t len)
{
    if (len < 8)
        return 0;

    uint16_t n = rt[2] | (rt[3] << 8);

    if (n < 8 || n > RADIOTAP_LEN || n > len)
        return 0;

    return n;
}

bool parseMacArg(const String &text, uint8_t *mac)
{
    unsigned int b[6];

    if (sscanf(text.c_str(), "%x:%x:%x:%x:%x:%x",
               &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6)
    {
        return false;
    }

    for (int i = 0; i < 6; i++)
        mac[i] = (uint8_t)b[i];

    return true;
}

/*
 * Optional filters:
 *
 *   ta=aa:bb:cc:dd:ee:ff   only frames sent by this address
 *   limit=n                only the n newest matches
 *
 * Addr2 is the transmitter, so a BSSID selects the beacons of
 * that AP and a station MAC selects its probe and association
 * requests. Without filters the whole buffer is returned.
 */
/*
 * Recount what is in the capture buffer, per AP and per
 * station.
 *
 * Counted fresh instead of kept as a running total: a ring
 * drops its oldest entry silently, and a counter maintained
 * alongside would slowly drift away from the truth. One pass
 * over the ring is cheap enough at this interval and is
 * always exact.
 *
 * The pass used to hold every frame against every entry of the
 * three tables, all in PSRAM: 6300 frames by 380 entries, a
 * quarter to most of a second before /api/networks sent its
 * first byte, growing as the ring filled, and loop() read no
 * frames meanwhile. The addresses now go into a small index in
 * internal RAM first, by their last octet; a frame is held only
 * against the one or two entries in its bucket.
 */
uint32_t pcapCountsAt = 0;

#define TA_INDEX_SIZE (MAX_NETWORKS + MAX_CLIENTS + MAX_ASSOC_HISTORY)

struct TaIndexEntry
{
    uint8_t  mac[6];
    uint8_t  kind;          // 0 network, 1 station, 2 history
    int16_t  next;          // next in the bucket, -1 none
    uint16_t idx;           // into its table
};

static TaIndexEntry taIndex[TA_INDEX_SIZE];
static int16_t        taBucket[256];
static uint16_t       taIndexCount = 0;

static void taIndexAdd(const uint8_t *mac, uint8_t kind, uint16_t idx)
{
    if (taIndexCount >= TA_INDEX_SIZE)
        return;

    TaIndexEntry &e = taIndex[taIndexCount];

    memcpy(e.mac, mac, 6);
    e.kind = kind;
    e.idx  = idx;
    e.next = taBucket[mac[5]];

    taBucket[mac[5]] = taIndexCount++;
}

void refreshPcapCounts()
{
    if (networks == nullptr || clients == nullptr)
        return;

    // At most a few times per second, several endpoints ask
    if (pcapCountsAt != 0 && millis() - pcapCountsAt < 1500)
        return;

    pcapCountsAt = millis();

    for (int i = 0; i < 256; i++)
        taBucket[i] = -1;

    taIndexCount = 0;

    /*
     * Added from the end, so that each bucket lists them first to
     * last: the first match of each table counts, as before
     */
    if (assocHistory != nullptr)
    {
        for (int i = MAX_ASSOC_HISTORY - 1; i >= 0; i--)
        {
            assocHistory[i].sta.pcapFrames = 0;

            if (assocHistory[i].valid)
                taIndexAdd(assocHistory[i].sta.mac, 2, i);
        }
    }

    for (int i = MAX_CLIENTS - 1; i >= 0; i--)
    {
        clients[i].pcapFrames = 0;

        if (clients[i].valid)
            taIndexAdd(clients[i].mac, 1, i);
    }

    for (int i = MAX_NETWORKS - 1; i >= 0; i--)
    {
        networks[i].pcapFrames = 0;
        networks[i].pcapBeacon = false;

        if (networks[i].valid)
            taIndexAdd(networks[i].bssid, 0, i);
    }

    // Both logs, one after the other
    FrameCursor cur;
    PcapView    v;

    frameCursorStart(cur, ringLog);

    bool inRing = true;

    for (;;)
    {
        if (!frameCursorNext(cur, v))
        {
            if (!inRing)
                break;

            inRing = false;

            frameCursorStart(cur, assocLog);

            continue;
        }

        const uint8_t *data = v.data;

        if (v.length < 16)
            continue;

        uint8_t ta[6];

        memcpy(ta, &data[10], 6);

        /*
         * One count per table: the network, the station, and the
         * history, which keeps its own. A station can sit in there
         * long after it left the live table, and its association
         * request usually still sits in the assoc ring, so the
         * pcap link stays useful.
         */
        uint8_t counted = 0;

        for (int16_t j = taBucket[ta[5]]; j >= 0; j = taIndex[j].next)
        {
            const TaIndexEntry &e = taIndex[j];

            if ((counted & (1 << e.kind)) || memcmp(e.mac, ta, 6) != 0)
                continue;

            counted |= 1 << e.kind;

            if (e.kind == 0)
                networks[e.idx].pcapFrames++;
            else if (e.kind == 1)
                clients[e.idx].pcapFrames++;
            else
                assocHistory[e.idx].sta.pcapFrames++;
        }
    }

    if (beaconSlots == nullptr)
        return;

    for (int b = 0; b < MAX_BEACON_SLOTS; b++)
    {
        if (!beaconSlots[b].valid)
            continue;

        const uint8_t *bssid = beaconSlots[b].bssid;

        for (int16_t j = taBucket[bssid[5]]; j >= 0; j = taIndex[j].next)
        {
            const TaIndexEntry &e = taIndex[j];

            if (e.kind == 0 && memcmp(e.mac, bssid, 6) == 0)
            {
                networks[e.idx].pcapBeacon = true;
                break;
            }
        }
    }
}

uint8_t countBeaconSlots()
{
    uint8_t n = 0;

    if (beaconSlots == nullptr)
        return 0;

    for (int i = 0; i < MAX_BEACON_SLOTS; i++)
    {
        if (beaconSlots[i].valid)
            n++;
    }

    return n;
}

// The elaborated "struct" is needed again: the generated
// prototype sits above the struct definition
template <typename R>
struct PcapView pcapViewOf(const R &r)
{
    return { r.us, r.length, r.origLength, r.rssi, r.channel,
             r.noise, r.rxRate, r.data };
}

/*
 * The frames of an export in time order. The two logs are each in
 * order already, the beacon slots are few and sorted first, and the
 * three are merged while they are read: no index of every frame,
 * which a log of any length would need.
 */
struct PcapMerge
{
    bool        haveTa;
    uint8_t     ta[6];

    FrameCursor ring;
    FrameCursor assoc;
    FrameCursor versions;
    PcapView    ringNext;
    PcapView    assocNext;
    PcapView    versionsNext;
    bool        ringHas;
    bool        assocHas;
    bool        versionsHas;

    uint8_t     beacons[MAX_BEACON_SLOTS];   // slot numbers, by time
    uint8_t     beaconCount;
    uint8_t     beaconPos;
};

// The next frame of a log that passes the address filter
bool pcapMergeFetch(struct PcapMerge &m, struct FrameCursor &c,
                    struct PcapView &v)
{
    while (frameCursorNext(c, v))
    {
        // Addr2 starts at offset 10 of the MAC header
        if (!m.haveTa ||
            (v.length >= 16 && memcmp(&v.data[10], m.ta, 6) == 0))
        {
            return true;
        }
    }

    return false;
}

void pcapMergeStart(struct PcapMerge &m, bool wantRing, bool wantAssoc,
                    bool wantBeacons, bool wantVersions)
{
    frameCursorStart(m.ring, ringLog);
    frameCursorStart(m.assoc, assocLog);
    frameCursorStart(m.versions, versionLog);

    m.ringHas  = wantRing && pcapMergeFetch(m, m.ring, m.ringNext);
    m.assocHas = wantAssoc && pcapMergeFetch(m, m.assoc, m.assocNext);
    m.versionsHas =
        wantVersions && pcapMergeFetch(m, m.versions, m.versionsNext);

    m.beaconCount = 0;
    m.beaconPos   = 0;

    for (int i = 0; wantBeacons && i < MAX_BEACON_SLOTS; i++)
    {
        if (!beaconSlots[i].valid)
            continue;

        if (m.haveTa && memcmp(beaconSlots[i].bssid, m.ta, 6) != 0)
            continue;

        // Insertion sort, at most MAX_BEACON_SLOTS
        uint64_t us = beaconSlots[i].rec.us;
        int      j  = m.beaconCount++;

        while (j > 0 && beaconSlots[m.beacons[j - 1]].rec.us > us)
        {
            m.beacons[j] = m.beacons[j - 1];
            j--;
        }

        m.beacons[j] = (uint8_t)i;
    }
}

// The oldest of the next frames, false at the end
bool pcapMergeNext(struct PcapMerge &m, struct PcapView &v)
{
    bool     haveBeacon = m.beaconPos < m.beaconCount;
    uint64_t beaconUs   = haveBeacon
        ? beaconSlots[m.beacons[m.beaconPos]].rec.us : UINT64_MAX;
    uint64_t ringUs     = m.ringHas ? m.ringNext.us : UINT64_MAX;
    uint64_t assocUs    = m.assocHas ? m.assocNext.us : UINT64_MAX;
    uint64_t versionsUs = m.versionsHas ? m.versionsNext.us : UINT64_MAX;

    if (!haveBeacon && !m.ringHas && !m.assocHas && !m.versionsHas)
        return false;

    // Beacon versions only ever come alone (src=versions)
    if (m.versionsHas && versionsUs <= ringUs && versionsUs <= assocUs &&
        versionsUs <= beaconUs)
    {
        v             = m.versionsNext;
        m.versionsHas = pcapMergeFetch(m, m.versions, m.versionsNext);
    }
    else if (m.ringHas && ringUs <= assocUs && ringUs <= beaconUs)
    {
        v         = m.ringNext;
        m.ringHas = pcapMergeFetch(m, m.ring, m.ringNext);
    }
    else if (m.assocHas && assocUs <= beaconUs)
    {
        v          = m.assocNext;
        m.assocHas = pcapMergeFetch(m, m.assoc, m.assocNext);
    }
    else
    {
        v = pcapViewOf(beaconSlots[m.beacons[m.beaconPos++]].rec);
    }

    return true;
}

// Size of one frame in the file: record header, radiotap, frame
uint32_t pcapFileBytes(const struct PcapView &v)
{
    return 16 + RADIOTAP_LEN + v.length;
}

void sendPcap()
{
    // Bluetooth has a link type of its own, so a file of its own
    if (reqHasArg("src") && reqArg("src") == "ble")
    {
        sendBlePcap();
        return;
    }

    if (beaconSlots == nullptr)
    {
        reqSend(503, "text/plain", "capture buffers unavailable");
        return;
    }

    uint8_t beaconCount = countBeaconSlots();

    // Static: two cursors and the beacon list are too much for the
    // loop task's stack
    static PcapMerge m;

    m.haveTa = reqHasArg("ta") && parseMacArg(reqArg("ta"), m.ta);

    /*
     * Which of the three buffers to export. The settings page
     * offers one download per buffer; without the argument
     * everything is merged, as before.
     */
    String src = reqHasArg("src") ? reqArg("src")
                                      : String("all");

    bool wantRing     = (src == "all" || src == "ring");
    bool wantAssoc    = (src == "all" || src == "assoc");
    bool wantBeacons  = (src == "all" || src == "beacons");
    bool wantVersions = (src == "versions");   // not in "all": old beacons

    uint32_t selected =
        (wantRing ? ringLog.frameCount : 0) +
        (wantAssoc ? assocLog.frameCount : 0) +
        (wantBeacons ? beaconCount : 0) +
        (wantVersions ? versionLog.frameCount : 0);

    if (selected == 0)
    {
        reqSend(404, "text/plain",
                    pcapEnabled
                        ? "This buffer is empty, nothing has been "
                          "recorded into it yet."
                        : "This buffer is empty and capturing is "
                          "switched off.");
        return;
    }

    uint32_t limit = 0;              // 0 = no limit

    if (reqHasArg("limit"))
    {
        long n = reqArg("limit").toInt();

        if (n > 0)
            limit = (uint32_t)n;
    }

    PcapView v;

    // ---- count the matches ----

    uint32_t n = 0;

    pcapMergeStart(m, wantRing, wantAssoc, wantBeacons, wantVersions);

    while (pcapMergeNext(m, v))
        n++;

    if (n == 0)
    {
        /*
         * The buffer holds frames, just none from this address
         * any more. Say how far back it reaches so it is clear
         * whether they aged out or were never captured.
         */
        String msg =
            "No frames from this address are in the capture.\n\n";

        uint64_t nowUs  = esp_timer_get_time();
        uint32_t uptime = (uint32_t)(nowUs / 1000000ULL);
        uint64_t oldest = frameLogOldest(ringLog);
        uint32_t span   =
            oldest ? (uint32_t)((nowUs - oldest) / 1000000ULL) : 0;

        if (ringLog.framesDropped == 0)
        {
            /*
             * Nothing was evicted, so the frames were never
             * recorded rather than aged out. Say why that can
             * happen, it is not obvious.
             */
            msg += "Nothing has been dropped from the ring yet: it "
                   "holds ";
            msg += ringLog.frameCount;
            msg += " frames in ";
            msg += (ringLog.chunkCount * FRAME_CHUNK_BYTES) / 1024;
            msg += " kB and reaches back ";
            msg += span;
            msg += " of the ";
            msg += uptime;
            msg += " seconds since start. So these frames were never "
                   "captured.\n\n";

            msg += "Stations are also discovered from data frames, "
                   "and those are not recorded: only management "
                   "frames go into the capture. A station that "
                   "simply carries traffic has nothing to export "
                   "until it sends a probe or association "
                   "request, and those are only heard while the "
                   "sweep dwells on its channel.\n";
        }
        else
        {
            msg += "The ring has reached the memory it may take and "
                   "drops its oldest frames: it holds ";
            msg += ringLog.frameCount;
            msg += " frames reaching back ";
            msg += span;
            msg += " seconds, ";
            msg += ringLog.framesDropped;
            msg += " older ones are gone, and these may be among "
                   "them.\n";
        }

        msg += "\n";
        msg += beaconCount;
        msg += " beacons are held separately, one per AP, and ";
        msg += assocLog.frameCount;
        msg += " association requests in a ring of their own.\n";

        msg += "\nCapture is currently ";
        msg += pcapEnabled ? "on" : "off";
        msg += ", frame types 0x";
        msg += String(pcapMask, HEX);
        msg += ".";

        if (!(pcapMask & PCAP_PROBEREQ))
            msg += "\nProbe requests are not being captured.";

        if (!(pcapMask & PCAP_ASSOC))
            msg += "\nAssociation requests are not being captured.";

        if (!(pcapMask & PCAP_OTHER))
        {
            msg += "\nAction, authentication and deauthentication "
                   "frames are not being captured.";
        }

        reqSend(404, "text/plain", msg);

        return;
    }

    // Keep the newest entries when a limit was given
    uint32_t first = 0;

    if (limit > 0 && n > limit)
        first = n - limit;

    // ---- exact length first, that avoids chunked binary ----

    uint32_t total = 24;             // global header
    uint32_t i     = 0;

    pcapMergeStart(m, wantRing, wantAssoc, wantBeacons, wantVersions);

    while (pcapMergeNext(m, v))
    {
        if (i++ >= first)
            total += pcapFileBytes(v);
    }

    String filename = wantVersions ? "beacon-versions" : "capture";

    if (m.haveTa)
    {
        char suffix[16];

        snprintf(suffix, sizeof(suffix), "-%02X%02X%02X%02X%02X%02X",
                 m.ta[0], m.ta[1], m.ta[2], m.ta[3], m.ta[4], m.ta[5]);

        filename += suffix;
    }

    filename += ".pcap";

    reqStart(200, "application/vnd.tcpdump.pcap", total, filename.c_str());

    // ---- global header ----

    uint8_t hdr[24];

    uint32_t magic    = 0xA1B2C3D4;
    uint32_t snaplen  = MGMT_PARSE_MAX + RADIOTAP_LEN;
    uint32_t linktype = 127;

    memcpy(&hdr[0], &magic, 4);

    hdr[4] = 2;  hdr[5] = 0;         // version major 2
    hdr[6] = 4;  hdr[7] = 0;         // version minor 4

    memset(&hdr[8], 0, 8);           // thiszone, sigfigs

    memcpy(&hdr[16], &snaplen, 4);
    memcpy(&hdr[20], &linktype, 4);

    reqContent(hdr, sizeof(hdr));

    // ---- records, oldest first ----

    uint8_t rec[16];
    uint8_t rt[RADIOTAP_LEN];

    i = 0;

    pcapMergeStart(m, wantRing, wantAssoc, wantBeacons, wantVersions);

    /*
     * Nothing is stored between the passes: all three run in this
     * one request, and frames only arrive from loop(), which waits
     * for it.
     */
    while (pcapMergeNext(m, v))
    {
        if (i++ < first)
            continue;

        int64_t  stamp = (int64_t)v.us + clockOffsetUs;

        uint32_t sec  = (uint32_t)(stamp / 1000000LL);
        uint32_t usec = (uint32_t)(stamp % 1000000LL);

        // Captured and on-air length differ for a cut-off frame,
        // which Wireshark then marks as truncated, not malformed
        uint32_t len  = RADIOTAP_LEN + v.length;
        uint32_t orig = RADIOTAP_LEN + max(v.length, v.origLength);

        memcpy(&rec[0], &sec, 4);
        memcpy(&rec[4], &usec, 4);
        memcpy(&rec[8], &len, 4);
        memcpy(&rec[12], &orig, 4);

        pcapBuildRadiotap(rt, v.channel, v.rssi, v.noise, v.rxRate);

        reqContent(rec, sizeof(rec));
        reqContent(rt, RADIOTAP_LEN);
        reqContent(v.data, v.length);
    }

    reqDone();

    lastRequestServed = millis();
}

void clearPcapRing()
{
    frameLogClear(ringLog);
}

void clearBeaconSlots()
{
    beaconTotal = 0;

    if (beaconSlots == nullptr)
        return;

    for (int i = 0; i < MAX_BEACON_SLOTS; i++)
        beaconSlots[i].valid = false;
}

void clearAssocRing()
{
    frameLogClear(assocLog);
}

/*
 * The three capture buffers hold different things and fill at
 * very different rates, so they can be emptied one at a time.
 * Without an argument everything goes, which is what the
 * button on the capture panel has always done.
 */
void handlePcapClear()
{
    String what = reqHasArg("what") ? reqArg("what")
                                        : String("all");

    bool all = (what == "all");

    if (all || what == "ring")
    {
        logRequest("capture ring cleared, %u frames discarded",
                   (unsigned)ringLog.frameCount);

        clearPcapRing();
    }

    if (all || what == "beacons")
    {
        logRequest("beacon slots cleared, %u beacons discarded",
                   (unsigned)countBeaconSlots());

        clearBeaconSlots();
    }

    if (all || what == "ble")
    {
        logRequest("BLE log cleared, %u advertisements discarded",
                   (unsigned)bleLog.frameCount);

        frameLogClear(bleLog);
    }

    if (all || what == "versions")
    {
        logRequest("beacon versions cleared, %u frames discarded",
                   (unsigned)versionLog.frameCount);

        clearBeaconVersions();
    }

    if (all || what == "assoc")
    {
        logRequest("assoc ring cleared, %u requests discarded",
                   (unsigned)assocLog.frameCount);

        clearAssocRing();
    }

    reqSend(200, "application/json", "{\"ok\":1}");
}
