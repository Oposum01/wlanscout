// SPDX-License-Identifier: GPL-2.0-only WITH AdditionRef-wlanscout-linking-exception
// SPDX-FileCopyrightText: 2026 Dr.-Ing. Simon Wunderlich Systementwicklung & Beratung

// ============================================================
// Beacon versions
//
// The beacon slots keep only the newest beacon of a BSS. Here the
// first one is kept, and then each beacon whose content changed
// for good, whole, in versionLog: what changed can be looked at
// in Wireshark or in the BSS's tooltip, also where the decoded
// fields of "Configuration changes" do not reach.
//
// Content means the elements, the beacon interval and the
// capabilities, not what changes with every beacon: the
// timestamp, the TIM, the load figures, countdowns and counters
// (bvElemHash()). An element that still changes BV_VOLATILE_CHANGES
// times within BV_VOLATILE_MS, a vendor's own counter say, is
// left out from then on for that BSS. A new content counts once a
// second beacon in a row agrees with it, as for the configuration
// changes; one seen before becomes the current version again and
// is not stored twice.
// ============================================================

static uint32_t bvFnv(uint32_t h, uint8_t byte)
{
    return (h ^ byte) * 16777619u;
}

static uint32_t bvFnvBytes(uint32_t h, const uint8_t *p, uint16_t n)
{
    for (uint16_t i = 0; i < n; i++)
        h = bvFnv(h, p[i]);

    return h;
}

// The kind of an element, see BvElem; the page computes the same
uint16_t bvKeyOf(uint8_t id, const uint8_t *body, uint8_t len)
{
    if (id == 255 && len >= 1)
        return 0x100 | body[0];

    if (id == 221 && len >= 4)
    {
        uint32_t oui = ((uint32_t)body[0] << 16) | (body[1] << 8) | body[2];
        uint32_t x   = oui * 31 + body[3];

        return 0x8000 | ((x ^ (x >> 15)) & 0x7FFF);
    }

    return id;
}

// Changes with every beacon or by itself: not content at all
bool bvElemSkipped(uint8_t id)
{
    switch (id)
    {
    case 5:      // TIM
    case 11:     // BSS Load
    case 63:     // BSS Average Access Delay
    case 68:     // BSS AC Access Delay
    case 193:    // Extended BSS Load
    case 216:    // TWT, its target wake times
        return true;
    }

    return false;
}

/*
 * The content hash of one element, with the octets that count down
 * or up by themselves set to 0. Multiple BSSID (71) is walked, its
 * nontransmitted profiles hold elements of their own.
 */
uint32_t bvElemHash(uint8_t id, const uint8_t *body, uint8_t len)
{
    uint8_t b[255];

    memcpy(b, body, len);

    switch (id)
    {
    case 12:                         // EDCA Parameter Set
        if (len >= 1)
            b[0] &= 0xF0;            //   its update count
        break;

    case 37:                         // Channel Switch Announcement
        if (len >= 3)
            b[2] = 0;                //   count
        break;

    case 40:                         // Quiet
        if (len >= 1)
            b[0] = 0;                //   count
        break;

    case 60:                         // Extended Channel Switch
        if (len >= 4)
            b[3] = 0;
        break;

    case 85:                         // Multiple BSSID-Index
        if (len >= 3)
            b[2] = 0;                //   DTIM count
        break;

    case 221:                        // WMM: Microsoft OUI, type 2
        if (len >= 7 && b[0] == 0x00 && b[1] == 0x50 && b[2] == 0xF2 &&
            b[3] == 2)
            b[6] &= 0xF0;            //   its update count
        break;

    case 255:
        if (len >= 2 && b[0] == 42)  // BSS Color Change Announcement
            b[1] = 0;                //   countdown
        break;
    }

    uint32_t h = bvFnv(bvFnv(2166136261u, id), len);

    if (id != 71 || len < 1)
        return bvFnvBytes(h, b, len);

    // Max BSSID Indicator, then the subelements
    h = bvFnv(h, b[0]);

    for (uint16_t pos = 1; pos + 2 <= len;)
    {
        uint8_t sid  = b[pos];
        uint8_t slen = b[pos + 1];

        if (pos + 2 + slen > len)
            break;

        const uint8_t *sb = &b[pos + 2];

        h = bvFnv(bvFnv(h, sid), slen);

        if (sid != 0)
            h = bvFnvBytes(h, sb, slen);
        else
        {
            // Nontransmitted BSSID Profile: elements
            for (uint16_t q = 0; q + 2 <= slen;)
            {
                uint8_t eid  = sb[q];
                uint8_t elen = sb[q + 1];

                if (q + 2 + elen > slen)
                    break;

                if (!bvElemSkipped(eid))
                    h = (h ^ bvElemHash(eid, &sb[q + 2], elen)) *
                        16777619u;

                q += 2 + elen;
            }
        }

        pos += 2 + slen;
    }

    return h;
}

/*
 * The elements of a beacon as kinds and hashes, the fixed fields
 * first (key 0xFFFF). Elements of a kind already listed fold into
 * its entry, so do those beyond BV_ELEMS into the last one.
 */
uint8_t bvElements(const uint8_t *frame, uint16_t length,
                   struct BvElem *out)
{
    uint8_t count = 0;

    out[count++] = { 0xFFFF, 0, 0, bvFnvBytes(2166136261u, &frame[32], 4),
                     0 };

    for (uint16_t pos = 36; pos + 2 <= length;)
    {
        uint8_t        id   = frame[pos];
        uint8_t        len  = frame[pos + 1];
        const uint8_t *body = &frame[pos + 2];

        if (pos + 2 + len > length)
            break;

        pos += 2 + len;

        if (bvElemSkipped(id))
            continue;

        uint16_t key = bvKeyOf(id, body, len);
        uint32_t h   = bvElemHash(id, body, len);

        int at = -1;

        for (uint8_t i = 0; i < count; i++)
        {
            if (out[i].key == key)
            {
                at = i;
                break;
            }
        }

        if (at < 0 && count < BV_ELEMS)
        {
            out[count++] = { key, 0, 0, h, 0 };
            continue;
        }

        if (at < 0)
            at = count - 1;

        out[at].hash = (out[at].hash ^ h) * 16777619u;
    }

    return count;
}

bool bvVolatile(const struct BeaconTrack &t, uint16_t key)
{
    for (uint8_t i = 0; i < t.volatileCount; i++)
        if (t.volatileKeys[i] == key)
            return true;

    return false;
}

// One hash for the content, the volatile elements left out
uint32_t bvContentHash(const struct BeaconTrack &t, const struct BvElem *list,
                       uint8_t count)
{
    uint32_t h = 2166136261u;

    for (uint8_t i = 0; i < count; i++)
    {
        if (bvVolatile(t, list[i].key))
            continue;

        h = bvFnv(bvFnv(h, list[i].key & 0xFF), list[i].key >> 8);
        h = bvFnvBytes(h, (const uint8_t *)&list[i].hash, 4);
    }

    return h;
}

/*
 * Counts each element's changes against the last beacon; one that
 * changes BV_VOLATILE_CHANGES times within BV_VOLATILE_MS becomes
 * volatile. True when one did.
 */
bool bvLearn(struct BeaconTrack &t, struct BvElem *cur, uint8_t count,
             uint32_t now)
{
    bool learned = false;

    for (uint8_t i = 0; i < count; i++)
    {
        for (uint8_t j = 0; j < t.prevCount; j++)
        {
            const BvElem &p = t.prev[j];

            if (p.key != cur[i].key)
                continue;

            cur[i].changes     = p.changes;
            cur[i].windowStart = p.windowStart;

            if (p.hash != cur[i].hash)
            {
                if (cur[i].changes == 0 ||
                    now - cur[i].windowStart > BV_VOLATILE_MS)
                {
                    cur[i].changes     = 1;
                    cur[i].windowStart = now;
                }
                else if (cur[i].changes < 255)
                    cur[i].changes++;

                if (cur[i].changes >= BV_VOLATILE_CHANGES &&
                    !bvVolatile(t, cur[i].key) &&
                    t.volatileCount < BV_VOLATILE)
                {
                    t.volatileKeys[t.volatileCount++] = cur[i].key;
                    learned = true;
                }
            }

            break;
        }
    }

    return learned;
}

struct BeaconTrack *bvTrackOf(const uint8_t *bssid)
{
    if (beaconTracks == nullptr)
        return nullptr;

    for (int i = 0; i < MAX_BEACON_SLOTS; i++)
        if (beaconTracks[i].used &&
            memcmp(beaconTracks[i].bssid, bssid, 6) == 0)
            return &beaconTracks[i];

    return nullptr;
}

// A slot for a new BSS: a free one, else the one heard longest ago
struct BeaconTrack *bvNewTrack(const uint8_t *bssid, uint32_t now)
{
    int pick = -1;
    uint32_t idle = 0;

    for (int i = 0; i < MAX_BEACON_SLOTS; i++)
    {
        if (!beaconTracks[i].used)
        {
            pick = i;
            break;
        }

        if (pick < 0 || now - beaconTracks[i].lastSeen > idle)
        {
            pick = i;
            idle = now - beaconTracks[i].lastSeen;
        }
    }

    BeaconTrack &t = beaconTracks[pick];

    memset(&t, 0, sizeof(t));

    t.used = true;
    memcpy(t.bssid, bssid, 6);
    t.nextSeq = 1;

    return &t;
}

void bvStore(struct BeaconTrack &t, const struct MgmtFrameCopy &entry,
             uint32_t hash)
{
    frameLogAppend(versionLog, entry);

    // Full: the oldest version goes, unless it is the current one
    if (t.versionCount == BV_KEEP)
    {
        uint8_t drop = t.current == 0 ? 1 : 0;

        memmove(&t.versions[drop], &t.versions[drop + 1],
                (BV_KEEP - 1 - drop) * sizeof(BvVersion));

        t.versionCount--;

        if (t.current > drop)
            t.current--;
    }

    BvVersion &v = t.versions[t.versionCount];

    v.us         = entry.us;
    v.sinceUs    = entry.us;
    v.hash       = hash;
    v.seq        = t.nextSeq++;
    v.times      = 1;
    v.origLength = entry.length;

    t.current = t.versionCount++;
}

// From processManagementQueue(), for every management frame
void beaconVersionFrame(const struct MgmtFrameCopy &entry)
{
    if (beaconTracks == nullptr || entry.length < 36)
        return;

    const uint8_t *f = entry.data;

    // Beacons only, sent by the BSS itself: Addr2 is Addr3
    if ((f[0] & 0xFC) != 0x80 || memcmp(&f[10], &f[16], 6) != 0)
        return;

    uint32_t now = millis();

    static BvElem cur[BV_ELEMS];     // too large for the loop's stack

    uint8_t count = bvElements(f, entry.length, cur);

    BeaconTrack *tp = bvTrackOf(&f[10]);

    if (tp == nullptr)
    {
        tp = bvNewTrack(&f[10], now);

        memcpy(tp->prev, cur, count * sizeof(BvElem));
        memcpy(tp->agreed, cur, count * sizeof(BvElem));
        tp->prevCount   = count;
        tp->agreedCount = count;
        tp->lastSeen    = now;

        bvStore(*tp, entry, bvContentHash(*tp, cur, count));
        return;
    }

    BeaconTrack &t = *tp;

    t.lastSeen = now;

    // A newly volatile element no longer makes the current version
    // differ from itself
    if (bvLearn(t, cur, count, now))
        t.versions[t.current].hash =
            bvContentHash(t, t.agreed, t.agreedCount);

    memcpy(t.prev, cur, count * sizeof(BvElem));
    t.prevCount = count;

    uint32_t h = bvContentHash(t, cur, count);

    if (h == bvContentHash(t, t.agreed, t.agreedCount))
    {
        t.pendingCount = 0;
        return;
    }

    // Counts once a second beacon in a row agrees
    if (t.pendingCount == 0 || h != t.pendingHash)
    {
        t.pendingHash  = h;
        t.pendingCount = 1;
        return;
    }

    t.pendingCount = 0;
    t.changes++;

    memcpy(t.agreed, cur, count * sizeof(BvElem));
    t.agreedCount = count;

    for (uint8_t i = 0; i < t.versionCount; i++)
    {
        if (t.versions[i].hash == h)
        {
            t.current = i;
            t.versions[i].sinceUs = entry.us;

            if (t.versions[i].times < 0xFFFF)
                t.versions[i].times++;

            return;
        }
    }

    bvStore(t, entry, h);
}

// Versions kept for a BSS, 0 when none (for the network list)
uint8_t bvCountOf(const uint8_t *bssid)
{
    const BeaconTrack *t = bvTrackOf(bssid);

    return t ? t->versionCount : 0;
}

void clearBeaconVersions()
{
    frameLogClear(versionLog);

    if (beaconTracks != nullptr)
        memset(beaconTracks, 0, sizeof(BeaconTrack) * MAX_BEACON_SLOTS);
}

void outBase64(const uint8_t *data, size_t length)
{
    static const char digits[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    char out[65];
    size_t n = 0;

    for (size_t i = 0; i < length; i += 3)
    {
        uint32_t v = (uint32_t)data[i] << 16;

        if (i + 1 < length)
            v |= (uint32_t)data[i + 1] << 8;

        if (i + 2 < length)
            v |= data[i + 2];

        out[n++] = digits[(v >> 18) & 63];
        out[n++] = digits[(v >> 12) & 63];
        out[n++] = i + 1 < length ? digits[(v >> 6) & 63] : '=';
        out[n++] = i + 2 < length ? digits[v & 63] : '=';

        if (n == sizeof(out) - 1)
        {
            out[n] = 0;
            outStr(out);
            n = 0;
        }
    }

    out[n] = 0;
    outStr(out);
}

/*
 * /api/beaconversions?bssid=..: the versions of one BSS, oldest
 * first, each with its frame in base64 while versionLog still holds
 * it. Times are seconds ago; "volatile" the keys left out.
 *
 *   {"bssid":"..","changes":3,"current":2,"volatile":[33025],
 *    "versions":[{"seq":1,"ago":600,"since":20,"times":2,
 *                 "len":412,"frame":"gAAAAP..."},..]}
 */
void sendBeaconVersions()
{
    uint8_t bssid[6];

    if (!reqHasArg("bssid") || !parseMacArg(reqArg("bssid"), bssid))
    {
        reqSend(400, "text/plain", "bssid missing");
        return;
    }

    const BeaconTrack *t = bvTrackOf(bssid);

    outBegin();

    outStr("{\"bssid\":");
    outMac(bssid);

    if (t == nullptr)
    {
        outStr(",\"changes\":0,\"versions\":[]}");
        outEnd();
        return;
    }

    uint64_t nowUs = esp_timer_get_time();

    outStr(",\"changes\":");
    outNum(t->changes);

    outStr(",\"current\":");
    outNum(t->versions[t->current].seq);

    outStr(",\"volatile\":[");

    for (uint8_t i = 0; i < t->volatileCount; i++)
    {
        if (i)
            outStr(",");

        outNum(t->volatileKeys[i]);
    }

    outStr("],\"versions\":[");

    for (uint8_t i = 0; i < t->versionCount; i++)
    {
        const BvVersion &v = t->versions[i];

        outStr(i ? ",{\"seq\":" : "{\"seq\":");
        outNum(v.seq);

        outStr(",\"ago\":");
        outNum((long)((nowUs - v.us) / 1000000ULL));

        outStr(",\"since\":");
        outNum((long)((nowUs - v.sinceUs) / 1000000ULL));

        outStr(",\"times\":");
        outNum(v.times);

        outStr(",\"len\":");
        outNum(v.origLength);

        // Its frame, unless the log has dropped it since
        FrameCursor c;
        PcapView    f;

        frameCursorStart(c, versionLog);

        while (frameCursorNext(c, f))
        {
            if (f.us == v.us && f.length >= 16 &&
                memcmp(&f.data[10], bssid, 6) == 0)
            {
                outStr(",\"frame\":\"");
                outBase64(f.data, f.length);
                outStr("\"");
                break;
            }
        }

        outStr("}");
    }

    outStr("]}");
    outEnd();
}
