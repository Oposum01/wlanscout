// SPDX-License-Identifier: GPL-2.0-only WITH AdditionRef-wlanscout-linking-exception
// SPDX-FileCopyrightText: 2026 Dr.-Ing. Simon Wunderlich Systementwicklung & Beratung

// ============================================================
// BLE capture
//
// Every advertisement the scan reports goes into bleLog as a link
// layer packet, for /api/pcap?src=ble: Wireshark's
// LINKTYPE_BLUETOOTH_LE_LL_WITH_PHDR (256), the pseudo header
// carrying the RSSI.
//
// The controller reports each device about once per scan window
// (BLE_SCAN_DURATION_S): duplicates are filtered, as before. The
// BLE library hands over advertising data and scan response joined;
// they are cut apart again at the last whole AD structure that
// fits 31 octets, the scan response becoming a SCAN_RSP of its own.
// The controller passes on only packets whose CRC was right, but not
// the CRC itself: it is computed again from the PDU, and the pseudo
// header says checked and valid. The channel is not reported; the
// pseudo header's RF channel stays 0, which Wireshark shows as
// advertising channel 37 whichever of the three it was.
// ============================================================

#define BLE_ACCESS_ADDRESS 0x8E89BED6UL

// LL PDU types of the legacy advertisements, by the report's event
// type: ADV_IND, ADV_DIRECT_IND, ADV_SCAN_IND, ADV_NONCONN_IND,
// SCAN_RSP
static const uint8_t blePduType[5] = { 0x0, 0x1, 0x6, 0x2, 0x4 };

// The link layer CRC over a PDU (header and payload), shift register
// preset 0x555555 for the advertising channels, bits least significant
// first (Core spec Vol 6 Part B 3.1.1); sent least significant octet
// first.
uint32_t bleCrc24(const uint8_t *p, int length)
{
    uint32_t state = 0xAAAAAA;           // 0x555555, bit-reversed

    for (int i = 0; i < length; i++)
        for (int bit = 0; bit < 8; bit++)
        {
            uint32_t in = (state ^ (p[i] >> bit)) & 1;

            state >>= 1;
            if (in)
                state ^= 0x800000 | 0x5A6000;
        }

    return state;
}

// One advertising PDU: access address, header, AdvA, data, CRC
static void bleLogPdu(const struct BleAdvInfo &e, uint8_t pduType,
                      const uint8_t *data, uint8_t length)
{
    uint8_t pkt[4 + 2 + 6 + 31 + 3];
    uint32_t aa = BLE_ACCESS_ADDRESS;

    memcpy(pkt, &aa, 4);

    pkt[4] = pduType | ((e.addrType & 1) << 6);   // TxAdd
    pkt[5] = 6 + length;

    // AdvA goes least significant octet first
    for (int i = 0; i < 6; i++)
        pkt[6 + i] = e.addr[5 - i];

    memcpy(&pkt[12], data, length);

    uint32_t crc = bleCrc24(&pkt[4], 8 + length);

    pkt[12 + length]     = crc & 0xFF;
    pkt[12 + length + 1] = (crc >> 8) & 0xFF;
    pkt[12 + length + 2] = (crc >> 16) & 0xFF;

    frameLogAppendBytes(bleLog, e.us, pkt, 12 + length + 3, e.rssi, 0, 0, 0);
}

// Where advertising data ends: the last whole AD structure in 31
uint8_t bleAdvSplit(const uint8_t *p, uint8_t length)
{
    uint8_t at = 0;

    while (at < length)
    {
        uint8_t next = at + 1 + p[at];

        if (p[at] == 0 || next > length || next > 31)
            break;

        at = next;
    }

    return at;
}

void bleLogAdvert(const struct BleAdvInfo &e)
{
    uint8_t type = e.advType < 5 ? blePduType[e.advType] : 0x0;
    uint8_t len  = e.payloadLen;

    if (len <= 31)
    {
        bleLogPdu(e, type, e.payload, len);
        return;
    }

    uint8_t adv = bleAdvSplit(e.payload, len);

    bleLogPdu(e, type, e.payload, adv);

    if (len - adv <= 31)
        bleLogPdu(e, 0x4, e.payload + adv, len - adv);
}

void sendBlePcap()
{
    static FrameCursor c;
    PcapView v;

    uint8_t ta[6];
    bool haveTa = reqHasArg("ta") && parseMacArg(reqArg("ta"), ta);

    // AdvA sits at offset 6 of the packet, reversed
    auto matches = [&](const PcapView &f)
    {
        if (!haveTa)
            return true;

        if (f.length < 12)
            return false;

        for (int i = 0; i < 6; i++)
            if (f.data[6 + i] != ta[5 - i])
                return false;

        return true;
    };

    uint32_t total = 24;
    uint32_t n     = 0;

    frameCursorStart(c, bleLog);

    while (frameCursorNext(c, v))
    {
        if (matches(v))
        {
            total += 16 + 10 + v.length;
            n++;
        }
    }

    if (n == 0)
    {
        reqSend(404, "text/plain",
                bleLog.frameCount
                    ? "No advertisements from this address are kept."
                    : "No BLE advertisements kept: they are recorded "
                      "in BLE mode only.");
        return;
    }

    reqStart(200, "application/vnd.tcpdump.pcap", total,
             haveTa ? "ble-device.pcap" : "ble.pcap");

    uint8_t  hdr[24];
    uint32_t magic    = 0xA1B2C3D4;
    uint32_t snaplen  = 10 + 4 + 2 + 255 + 3;
    uint32_t linktype = 256;     // LINKTYPE_BLUETOOTH_LE_LL_WITH_PHDR

    memcpy(&hdr[0], &magic, 4);
    hdr[4] = 2;  hdr[5] = 0;
    hdr[6] = 4;  hdr[7] = 0;
    memset(&hdr[8], 0, 8);
    memcpy(&hdr[16], &snaplen, 4);
    memcpy(&hdr[20], &linktype, 4);

    reqContent(hdr, sizeof(hdr));

    frameCursorStart(c, bleLog);

    while (frameCursorNext(c, v))
    {
        if (!matches(v))
            continue;

        int64_t  stamp = (int64_t)v.us + clockOffsetUs;
        uint32_t sec   = (uint32_t)(stamp / 1000000LL);
        uint32_t usec  = (uint32_t)(stamp % 1000000LL);
        uint32_t len   = 10 + v.length;

        uint8_t rec[16];

        memcpy(&rec[0], &sec, 4);
        memcpy(&rec[4], &usec, 4);
        memcpy(&rec[8], &len, 4);
        memcpy(&rec[12], &len, 4);

        /*
         * Pseudo header: RF channel, signal, noise, access address
         * offenses, reference access address, flags: dewhitened (1),
         * signal valid (2), reference access address valid (0x10),
         * CRC checked (0x400) and valid (0x800).
         */
        uint8_t  phdr[10] = { 0, (uint8_t)v.rssi, 0, 0 };
        uint32_t aa       = BLE_ACCESS_ADDRESS;
        uint16_t flags    = 0x0001 | 0x0002 | 0x0010 | 0x0400 | 0x0800;

        memcpy(&phdr[4], &aa, 4);
        memcpy(&phdr[8], &flags, 2);

        reqContent(rec, sizeof(rec));
        reqContent(phdr, sizeof(phdr));
        reqContent(v.data, v.length);
    }

    reqDone();
}

// ============================================================
// BLE hits
//
// The devices that came by, and how often: one entry per address
// that stays (public or random static, bleStableAddr()); the ones
// that change every few minutes would only count themselves. A hit
// is a device heard again after BLE_HIT_GAP_MS of silence, counted
// also per hour of the day once the clock is set, so a pattern
// shows: the neighbour's TV every evening, a colleague's watch on
// weekdays. Kept in PSRAM, saved to flash every BLE_HITS_SAVE_MS
// and before a restart, so they add up over days.
// ============================================================

#define BLE_HITS_FILE  "/blehits.bin"
#define BLE_HITS_TMP   "/blehits.tmp"
#define BLE_HITS_MAGIC 0x31484257UL      // "WBH1"

// As stored on flash; no pointers, no runtime state
struct BleHit
{
    uint8_t  addr[6];
    uint8_t  addrType;
    uint8_t  used;
    char     name[25];
    uint8_t  spare;
    uint16_t company;            // 0xFFFF none
    uint32_t hits;
    uint32_t adverts;
    uint32_t firstEpoch;         // 0 without a clock then
    uint32_t lastEpoch;
    uint16_t hours[24];          // hits per hour of the day, local time
};

BleHit   *bleHits      = nullptr;   // PSRAM, BLE_HITS_MAX
uint32_t *bleHitHeard  = nullptr;   // millis() last heard, this start
bool      bleHitsDirty = false;
uint32_t  bleHitsSaved = 0;         // millis()

bool bleHitsAllocate()
{
    if (bleHits != nullptr)
        return true;

    bleHits = (BleHit *)allocateTable("BLE hits",
                                      sizeof(BleHit) * BLE_HITS_MAX);
    bleHitHeard = (uint32_t *)allocateTable("BLE hits, heard",
                                            sizeof(uint32_t) * BLE_HITS_MAX);

    if (bleHits == nullptr || bleHitHeard == nullptr)
    {
        bleHits = nullptr;
        return false;
    }

    memset(bleHits, 0, sizeof(BleHit) * BLE_HITS_MAX);
    memset(bleHitHeard, 0, sizeof(uint32_t) * BLE_HITS_MAX);

    return true;
}

void bleHitsLoad()
{
    if (!bleHitsAllocate() || !catalogFsOk)
        return;

    File f = LittleFS.open(BLE_HITS_FILE, "r");

    if (!f)
        return;

    uint32_t magic = 0, count = 0;

    if (f.read((uint8_t *)&magic, 4) == 4 && magic == BLE_HITS_MAGIC &&
        f.read((uint8_t *)&count, 4) == 4 && count <= BLE_HITS_MAX)
    {
        size_t bytes = count * sizeof(BleHit);

        if (f.read((uint8_t *)bleHits, bytes) != bytes)
            memset(bleHits, 0, sizeof(BleHit) * BLE_HITS_MAX);
    }

    f.close();
}

void bleHitsSave()
{
    if (bleHits == nullptr || !bleHitsDirty || !catalogFsOk)
        return;

    // The used entries first, so the file holds only those
    uint32_t count = 0;

    for (uint32_t i = 0; i < BLE_HITS_MAX; i++)
    {
        if (!bleHits[i].used)
            continue;

        if (i != count)
        {
            BleHit   h = bleHits[count];
            uint32_t t = bleHitHeard[count];

            bleHits[count] = bleHits[i];
            bleHitHeard[count] = bleHitHeard[i];
            bleHits[i] = h;
            bleHitHeard[i] = t;
        }

        count++;
    }

    File f = LittleFS.open(BLE_HITS_TMP, "w");

    if (!f)
        return;

    uint32_t magic = BLE_HITS_MAGIC;
    size_t   bytes = count * sizeof(BleHit);

    bool ok = f.write((uint8_t *)&magic, 4) == 4 &&
              f.write((uint8_t *)&count, 4) == 4 &&
              f.write((uint8_t *)bleHits, bytes) == bytes;

    f.close();

    if (ok && LittleFS.rename(BLE_HITS_TMP, BLE_HITS_FILE))
        bleHitsDirty = false;
    else
        LittleFS.remove(BLE_HITS_TMP);

    bleHitsSaved = millis();
}

// From loop(): now and then while something changed
void bleHitsLoop()
{
    if (bleHitsDirty && millis() - bleHitsSaved > BLE_HITS_SAVE_MS)
        bleHitsSave();
}

// A new entry: a free one, else the one with the fewest hits
static int bleHitSlot()
{
    int pick = -1;

    for (int i = 0; i < BLE_HITS_MAX; i++)
    {
        if (!bleHits[i].used)
            return i;

        if (pick < 0 || bleHits[i].hits < bleHits[pick].hits ||
            (bleHits[i].hits == bleHits[pick].hits &&
             bleHits[i].lastEpoch < bleHits[pick].lastEpoch))
            pick = i;
    }

    return pick;
}

// From processBleQueue(), for every advertisement
void bleHitsNote(const struct BleAdvInfo &e)
{
    if (bleHits == nullptr || !bleStableAddr(e.addr, e.addrType))
        return;

    uint32_t now   = millis();
    uint32_t epoch = deviceEpoch();
    bool     clock = epoch >= 1600000000UL;

    int idx = -1;

    for (int i = 0; i < BLE_HITS_MAX; i++)
    {
        if (bleHits[i].used && memcmp(bleHits[i].addr, e.addr, 6) == 0 &&
            (bleHits[i].addrType & 1) == (e.addrType & 1))
        {
            idx = i;
            break;
        }
    }

    bool hit;

    if (idx < 0)
    {
        idx = bleHitSlot();

        BleHit &n = bleHits[idx];

        memset(&n, 0, sizeof(n));
        memcpy(n.addr, e.addr, 6);
        n.addrType   = e.addrType & 1;
        n.used       = 1;
        n.company    = 0xFFFF;
        n.firstEpoch = clock ? epoch : 0;

        hit = true;
    }
    else
        hit = bleHitHeard[idx] == 0 || now - bleHitHeard[idx] > BLE_HIT_GAP_MS;

    BleHit &h = bleHits[idx];

    if (hit)
    {
        h.hits++;

        if (clock)
        {
            uint32_t local = epoch + tzOffsetAt(epoch) * 60;
            uint16_t &bin  = h.hours[(local / 3600) % 24];

            if (bin < 0xFFFF)
                bin++;
        }
    }

    h.adverts++;
    bleHitHeard[idx] = now ? now : 1;

    if (clock)
    {
        h.lastEpoch = epoch;

        if (h.firstEpoch == 0)
            h.firstEpoch = epoch;
    }

    if (e.name[0])
        strlcpy(h.name, e.name, sizeof(h.name));

    if (e.company != 0xFFFF)
        h.company = e.company;

    bleHitsDirty = true;
}

uint32_t countBleHits()
{
    uint32_t n = 0;

    for (int i = 0; bleHits != nullptr && i < BLE_HITS_MAX; i++)
        n += bleHits[i].used;

    return n;
}

void clearBleHits()
{
    if (bleHits == nullptr)
        return;

    memset(bleHits, 0, sizeof(BleHit) * BLE_HITS_MAX);
    memset(bleHitHeard, 0, sizeof(uint32_t) * BLE_HITS_MAX);

    if (catalogFsOk)
        LittleFS.remove(BLE_HITS_FILE);

    bleHitsDirty = false;
}

/*
 * /api/blehits: every device counted, unsorted. "ago" is seconds
 * since last heard in this start, -1 before; times are epochs, 0
 * when the clock was not set.
 *
 *   {"max":512,"gap":300,"devices":[{"addr":"..","type":"public",
 *    "name":"..","company":76,"companyName":"Apple","vendor":"..",
 *    "hits":12,"adv":340,"first":1762400000,"last":1762430000,
 *    "ago":5,"hours":[0,..]},..]}
 */
void sendBleHitsJSON()
{
    uint32_t now = millis();

    outBegin();

    outStr("{\"max\":");
    outNum(BLE_HITS_MAX);
    outStr(",\"gap\":");
    outNum(BLE_HIT_GAP_MS / 1000);
    outStr(",\"devices\":[");

    bool first = true;

    for (int i = 0; bleHits != nullptr && i < BLE_HITS_MAX; i++)
    {
        const BleHit &h = bleHits[i];

        if (!h.used)
            continue;

        outStr(first ? "{\"addr\":" : ",{\"addr\":");
        first = false;

        outMac(h.addr);
        outStr(",\"type\":");
        outJSON(bleAddrType(h.addr, h.addrType));
        outStr(",\"name\":");
        outJSON(h.name);
        outStr(",\"company\":");
        outNum(h.company);
        outStr(",\"companyName\":");
        outJSON(h.company == 0xFFFF ? "" : lookupCompany(h.company));
        outStr(",\"vendor\":");
        outJSON(h.addrType & 1 ? "" : lookupVendor(h.addr));
        outStr(",\"hits\":");
        outNum(h.hits);
        outStr(",\"adv\":");
        outNum(h.adverts);
        outStr(",\"first\":");
        outNum(h.firstEpoch);
        outStr(",\"last\":");
        outNum(h.lastEpoch);
        outStr(",\"ago\":");
        outNum(bleHitHeard[i] ? (long)((now - bleHitHeard[i]) / 1000) : -1);
        outStr(",\"hours\":[");

        for (int k = 0; k < 24; k++)
        {
            if (k)
                outStr(",");

            outNum(h.hours[k]);
        }

        outStr("]}");
    }

    outStr("]}");
    outEnd();
}

void handleBleHitsClear()
{
    logRequest("BLE hits cleared");

    clearBleHits();

    reqSend(200, "application/json", "{\"ok\":1}");
}
