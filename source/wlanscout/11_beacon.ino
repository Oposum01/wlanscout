// SPDX-License-Identifier: GPL-2.0-only WITH AdditionRef-wlanscout-linking-exception
// SPDX-FileCopyrightText: 2026 Dr.-Ing. Simon Wunderlich Systementwicklung & Beratung

// ============================================================
// Parse beacon / probe response
// ============================================================

const char *const cfgFieldNames[] = {
    "restart", "channel", "width", "standard", "spatial streams",
    "lowest basic rate", "beacon interval", "country", "PMF",
    "802.11k", "802.11v", "802.11r", "WPS", "BSS colour", "security",
    "AKM", "cipher", "group cipher", "flapping", "DTIM period",
    "two clocks", "SSID", "RSNE override", "override AKM",
    "override cipher", "override PMF", "override H2E",
    "association", "power constraint", "Tx power envelope",
    "WPS state", "WPS locked", "WPS methods", "OWE transition",
    "OWE partner", "OWE partner SSID", "Hotspot 2.0", "interworking",
    "MLD address", "MLO links", "basic rates", "pre-authentication",
    "admission control", "40 MHz intolerant", "mesh gate",
    "accepting peerings", "mesh authentication", "mobility domain",
    "FT over the DS", "punctured", "link removal", "broadcast TWT",
    "TWT required"
};

bool cfgIsMuted(const uint8_t *bssid)
{
    for (uint8_t i = 0; i < cfgMutedCount; i++)
        if (memcmp(cfgMuted[i], bssid, 6) == 0)
            return true;

    return false;
}

void cfgMuteLoad()
{
    prefs.begin("analyzer", true);

    size_t len = prefs.getBytesLength("cfgmute");

    if (len > 0 && len <= sizeof(cfgMuted) && len % 6 == 0)
    {
        prefs.getBytes("cfgmute", cfgMuted, len);
        cfgMutedCount = len / 6;
    }

    prefs.end();
}

void cfgMuteSave()
{
    prefs.begin("analyzer", false);

    if (cfgMutedCount)
        prefs.putBytes("cfgmute", cfgMuted, cfgMutedCount * 6);
    else
        prefs.remove("cfgmute");

    prefs.end();
}

void cfgRecord(WiFiNetwork &n, uint8_t field, const char *from,
               const char *to)
{
    CfgChange &c = n.cfgChanges[n.cfgHead];

    c.at    = millis();
    c.field = field;

    strlcpy(c.from, from, sizeof(c.from));
    strlcpy(c.to, to, sizeof(c.to));

    n.cfgHead = (n.cfgHead + 1) % CFG_CHANGES;

    if (n.cfgTotal < 65535)
        n.cfgTotal++;

    if (cfgEvents == nullptr || cfgIsMuted(n.bssid))
        return;

    CfgEvent &e = cfgEvents[cfgSeq % CFG_EVENTS];

    cfgVersion++;

    e.seq   = ++cfgSeq;
    e.at    = c.at;
    e.field = field;

    memcpy(e.bssid, n.bssid, 6);
    strlcpy(e.ssid, n.ssid, sizeof(e.ssid));
    strlcpy(e.from, from, sizeof(e.from));
    strlcpy(e.to, to, sizeof(e.to));
}

/*
 * GET /api/changes: the configuration changes of all BSSs, newest
 * first, [number, seconds ago, BSSID, SSID, what, from, to].
 */
void sendChangesJSON()
{
    outBegin();

    outStr("{\"boot\":");
    outNum(bootCount);
    outStr(",\"seq\":");
    outNum(cfgSeq);
    outStr(",\"ver\":");
    outNum(cfgVersion);

    outStr(",\"muted\":[");

    for (uint8_t i = 0; i < cfgMutedCount; i++)
    {
        char mac[18];

        snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
                 cfgMuted[i][0], cfgMuted[i][1], cfgMuted[i][2],
                 cfgMuted[i][3], cfgMuted[i][4], cfgMuted[i][5]);

        outStr(i ? "," : "");
        outJSON(mac);
    }

    outStr("],\"changes\":[");

    uint32_t now   = millis();
    uint32_t kept  = cfgSeq < CFG_EVENTS ? cfgSeq : CFG_EVENTS;

    for (uint32_t i = 0; i < kept && cfgEvents; i++)
    {
        const CfgEvent &e = cfgEvents[(cfgSeq - 1 - i) % CFG_EVENTS];

        char mac[18];

        snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
                 e.bssid[0], e.bssid[1], e.bssid[2],
                 e.bssid[3], e.bssid[4], e.bssid[5]);

        outStr(i ? ",[" : "[");
        outNum(e.seq);
        outStr(",");
        outNum((now - e.at) / 1000);
        outStr(",");
        outJSON(mac);
        outStr(",");
        outJSON(e.ssid);
        outStr(",");
        outJSON(cfgFieldNames[e.field]);
        outStr(",");
        outJSON(e.from);
        outStr(",");
        outJSON(e.to);
        outStr("]");
    }

    outStr("]}");

    outEnd();
}

/*
 * The basic rates a beacon may mark, in 500 kbit/s units: one bit
 * each in CfgSnap.basicRates.
 */
const uint8_t cfgRates[] = { 2, 4, 11, 22, 12, 18, 24, 36, 48, 72, 96, 108 };

// What a beacon says beyond the network entry's fields, see CfgSnap
void cfgExtrasOf(const uint8_t *ies, int ieLength, CfgSnap &c)
{
    int len = 0;
    const uint8_t *p;

    // SSID, or the Mesh ID of a mesh point; nothing but zeros is hidden
    p = findIE(ies, ieLength, 0, len);

    if (p == nullptr || len == 0)
        p = findIE(ies, ieLength, 114, len);

    if (p != nullptr && len > 0 && len <= 32)
        for (int i = 0; i < len; i++)
            if (p[i] != 0)
            {
                memcpy(c.ssid, p, len);
                break;
            }

    // RSN Capabilities: pre-authentication
    {
        RsnInfo rsn;

        memset(&rsn, 0, sizeof(rsn));

        p = findIE(ies, ieLength, 48, len);
        parseSuites(p, len, false, rsn);

        c.preAuth = rsn.preAuth;
    }

    // RSNE Override and Override 2 together, the RSNXE Override's H2E bit
    c.ovr = rsnOverridesOf(ies, ieLength, c.ovrAkm, sizeof(c.ovrAkm),
                           c.ovrPairwise, sizeof(c.ovrPairwise), c.ovrPmf);

    p = findVendorIE(ies, ieLength, 0x50, 0x6F, 0x9A, 0x2B, len);

    c.ovrH2E = p != nullptr && len >= 1 && (p[0] & 0x20) != 0;

    // MBO-OCE attributes: 4 is Association Disallowed with its reason
    p = findVendorIE(ies, ieLength, 0x50, 0x6F, 0x9A, 0x16, len);

    for (int i = 0; p != nullptr && i + 2 <= len; )
    {
        uint8_t id = p[i];
        uint8_t l  = p[i + 1];

        if (i + 2 + l > len)
            break;

        if (id == 4)
            c.mboDisallowed = l >= 1 && p[i + 2] ? p[i + 2] : 1;

        i += 2 + l;
    }

    // Power Constraint (32) and the first Transmit Power Envelope (195)
    p = findIE(ies, ieLength, 32, len);

    c.powerConstraint = p != nullptr && len >= 1 ? p[0] : 255;

    p = findIE(ies, ieLength, 195, len);

    if (p != nullptr && len >= 2)
    {
        c.tpeLen = len > (int)sizeof(c.tpe) ? sizeof(c.tpe) : len;
        memcpy(c.tpe, p, c.tpeLen);
    }

    // WPS: state, AP Setup Locked, Config Methods, as the beacon has them
    p = findVendorIE(ies, ieLength, 0x00, 0x50, 0xF2, 0x04, len);

    for (int i = 0; p != nullptr && i + 4 <= len; )
    {
        uint16_t attr = (p[i] << 8) | p[i + 1];
        uint16_t l    = (p[i + 2] << 8) | p[i + 3];

        if (i + 4 + l > len)
            break;

        const uint8_t *v = p + i + 4;

        if (attr == 0x1044 && l >= 1)
            c.wpsState = v[0];
        else if (attr == 0x1057 && l >= 1)
            c.wpsLocked = v[0] != 0;
        else if (attr == 0x1008 && l >= 2)
            c.wpsMethods = (v[0] << 8) | v[1];

        i += 4 + l;
    }

    // OWE Transition Mode: the partner's BSSID and SSID
    p = findVendorIE(ies, ieLength, 0x50, 0x6F, 0x9A, 0x1C, len);

    if (p != nullptr && len >= 7 && p[6] <= 32 && 7 + p[6] <= len)
    {
        c.oweTrans = true;

        memcpy(c.oweBssid, p, 6);
        memcpy(c.oweSsid, p + 7, p[6]);
    }

    // Hotspot 2.0 Indication, Interworking (107) with its network type
    c.hs20 = findVendorIE(ies, ieLength, 0x50, 0x6F, 0x9A, 0x10,
                          len) != nullptr;

    p = findIE(ies, ieLength, 107, len);

    c.iwType = p != nullptr && len >= 1 ? (p[0] & 0x0F) : 255;

    /*
     * Multi-Link: the MLD address and the links of this MLD, its
     * own from the Multi-Link element and the others from the
     * Reduced Neighbor Report, whose entries with MLD parameters
     * (TBTT information of 16 octets or more) and MLD ID 0 belong
     * to the reporting AP's MLD.
     */
    {
        MloInfo mlo;

        memset(&mlo, 0, sizeof(mlo));

        if (parseMultiLink(ies, ieLength, mlo))
        {
            memcpy(c.mldMac, mlo.mldMac, 6);

            if (mlo.linkId >= 0 && mlo.linkId < 16)
                c.mloLinks |= 1 << mlo.linkId;
        }

        p = findIE(ies, ieLength, 201, len);

        for (int pos = 0; p != nullptr && pos + 4 <= len; )
        {
            uint8_t count   = ((p[pos] >> 4) & 0x0F) + 1;
            uint8_t infoLen = p[pos + 1];

            pos += 4;

            if (infoLen == 0)
                break;

            for (uint8_t i = 0; i < count && pos + infoLen <= len; i++)
            {
                if (infoLen >= 16 && p[pos + 13] == 0)
                    c.mloLinks |= 1 << (p[pos + 14] & 0x0F);

                pos += infoLen;
            }
        }
    }

    // The basic rates, marked by the top bit in either rate element
    for (int pass = 0; pass < 2; pass++)
    {
        p = findIE(ies, ieLength, pass == 0 ? 1 : 50, len);

        for (int i = 0; p != nullptr && i < len; i++)
            if (p[i] & 0x80)
                for (size_t k = 0; k < sizeof(cfgRates); k++)
                    if ((p[i] & 0x7F) == cfgRates[k])
                        c.basicRates |= 1 << k;
    }

    // WMM Parameter element: the ACM bit of each AC record
    p = findVendorIE(ies, ieLength, 0x00, 0x50, 0xF2, 0x02, len);

    c.acm = 255;

    if (p != nullptr && len >= 20 && p[0] == 1)
    {
        c.acm = 0;

        for (int i = 0; i < 4; i++)
            if (p[4 + i * 4] & 0x10)
                c.acm |= 1 << ((p[4 + i * 4] >> 5) & 3);
    }

    // HT Capabilities: 40 MHz Intolerant, bit 14
    p = findIE(ies, ieLength, 45, len);

    c.intolerant = p != nullptr && len >= 2 && (p[1] & 0x40) != 0;

    // EHT Operation: preamble puncturing
    ehtOperationOf(ies, ieLength, c.ehtWidth, c.ehtCenter, c.punctured);

    // Broadcast TWT, the first schedule
    {
        uint8_t  ids[2], recs[2];
        uint32_t iv[2], du[2];

        c.btwtCount = broadcastTwtOf(ies, ieLength, ids, recs, iv, du, 2);

        if (c.btwtCount)
        {
            c.btwtInterval = iv[0];
            c.btwtDuration = du[0];
        }
    }

    // HE Operation Parameters, bit 3: stations have to use TWT
    p = findExtendedIE(ies, ieLength, 36, len);
    c.twtRequired = p != nullptr && len >= 3 && (p[0] & 0x08);

    // Links announced for removal, without the countdown
    {
        uint16_t tbtts = 0;

        mlRemovalOf(ies, ieLength, c.mlRemoving, tbtts);
    }

    // Mobility Domain (54): its ID and FT over the DS
    p = findIE(ies, ieLength, 54, len);

    if (p != nullptr && len >= 3)
    {
        c.mde      = true;
        c.mdid     = p[0] | (p[1] << 8);
        c.ftOverDs = (p[2] & 0x01) != 0;
    }

    // Mesh Configuration (113), exactly 7 octets as in parseBeacon()
    p = findIE(ies, ieLength, 113, len);

    c.meshAuth = 255;

    if (p != nullptr && len == 7)
    {
        c.meshAuth      = p[4];
        c.meshGate      = (p[5] & 0x01) != 0;
        c.meshAccepting = (p[6] & 0x01) != 0;
    }
}

CfgSnap cfgSnapOf(const WiFiNetwork &n, const uint8_t *ies, int ieLength)
{
    CfgSnap c;

    // Zeroed first: the snapshot is compared and hashed whole
    memset(&c, 0, sizeof(c));

    c.channel        = n.channel;
    c.standard       = n.standard;
    c.bandwidth      = n.bandwidth;
    c.streams        = n.streams;
    c.basicRate      = n.basicRate;
    c.beaconInterval = n.beaconInterval;
    c.pmf            = n.mfpRequired ? 2 : (n.mfpCapable ? 1 : 0);
    c.rm11k          = n.rm11k;
    c.bss11v         = n.bss11v;
    c.ft11r          = n.ft11r;
    c.wps            = n.wps;
    c.bssColor       = n.bssColor;
    c.dtimPeriod     = n.dtimPeriod;

    strlcpy(c.country, n.country, sizeof(c.country));
    strlcpy(c.security, n.security, sizeof(c.security));
    strlcpy(c.akm, n.akm, sizeof(c.akm));
    strlcpy(c.pairwise, n.pairwise, sizeof(c.pairwise));
    strlcpy(c.group, n.groupCipher, sizeof(c.group));

    cfgExtrasOf(ies, ieLength, c);

    return c;
}

uint32_t cfgHash(const CfgSnap &c)
{
    const uint8_t *p = (const uint8_t *)&c;
    uint32_t h = 2166136261u;

    for (size_t i = 0; i < sizeof(c); i++)
        h = (h ^ p[i]) * 16777619u;

    return h;
}

// Compares the agreed state with a new one and records what differs
void cfgDiff(WiFiNetwork &n, const CfgSnap &a, const CfgSnap &b,
             char *names, size_t size)
{
    char x[40], y[40];

    // With names: only list what differs, "width, standard, ..."
    auto cfgRecord = [&](WiFiNetwork &n, uint8_t field, const char *from,
                         const char *to)
    {
        if (names == nullptr)
        {
            ::cfgRecord(n, field, from, to);
            return;
        }

        if (names[0])
            strlcat(names, ", ", size);

        strlcat(names, cfgFieldNames[field], size);
    };

    auto num = [&](uint8_t field, long from, long to, const char *unit)
    {
        if (from == to)
            return;

        snprintf(x, sizeof(x), "%ld%s", from, unit);
        snprintf(y, sizeof(y), "%ld%s", to, unit);
        cfgRecord(n, field, x, y);
    };

    auto flag = [&](uint8_t field, bool from, bool to)
    {
        if (from != to)
            cfgRecord(n, field, from ? "on" : "off", to ? "on" : "off");
    };

    auto text = [&](uint8_t field, const char *from, const char *to)
    {
        if (strcmp(from, to) != 0)
            cfgRecord(n, field, from[0] ? from : "-", to[0] ? to : "-");
    };

    num(CFG_CHANNEL, a.channel, b.channel, "");
    num(CFG_WIDTH, a.bandwidth, b.bandwidth, " MHz");
    num(CFG_STANDARD, a.standard, b.standard, "");
    num(CFG_STREAMS, a.streams, b.streams, "");

    if (a.basicRate != b.basicRate)
    {
        auto rate = [](char *out, size_t size, uint8_t r)
        {
            if (r == 0)
                strlcpy(out, "none", size);
            else
                snprintf(out, size, "%u%s Mbit/s", r / 2, r & 1 ? ".5" : "");
        };

        rate(x, sizeof(x), a.basicRate);
        rate(y, sizeof(y), b.basicRate);
        cfgRecord(n, CFG_BASIC_RATE, x, y);
    }

    num(CFG_BEACON_INTERVAL, a.beaconInterval, b.beaconInterval, " TU");
    text(CFG_COUNTRY, a.country, b.country);

    if (a.pmf != b.pmf)
    {
        static const char *const pmf[] = { "off", "capable", "required" };
        cfgRecord(n, CFG_PMF, pmf[a.pmf], pmf[b.pmf]);
    }

    flag(CFG_11K, a.rm11k, b.rm11k);
    flag(CFG_11V, a.bss11v, b.bss11v);
    flag(CFG_11R, a.ft11r, b.ft11r);
    flag(CFG_WPS, a.wps, b.wps);
    num(CFG_COLOR, a.bssColor, b.bssColor, "");
    num(CFG_DTIM, a.dtimPeriod, b.dtimPeriod, "");
    text(CFG_SECURITY, a.security, b.security);
    text(CFG_AKM, a.akm, b.akm);
    text(CFG_PAIRWISE, a.pairwise, b.pairwise);
    text(CFG_GROUP, a.group, b.group);

    // From the beacon's elements, see cfgExtrasOf()
    static const char *const pmf[] = { "off", "capable", "required" };

    auto mac = [](char *out, size_t size, const uint8_t *m)
    {
        static const uint8_t none[6] = { 0 };

        if (memcmp(m, none, 6) == 0)
            strlcpy(out, "-", size);
        else
            snprintf(out, size, "%02X:%02X:%02X:%02X:%02X:%02X",
                     m[0], m[1], m[2], m[3], m[4], m[5]);
    };

    if (strcmp(a.ssid, b.ssid) != 0)
        cfgRecord(n, CFG_SSID, a.ssid[0] ? a.ssid : "hidden",
                  b.ssid[0] ? b.ssid : "hidden");

    // Switched on or off: one entry with the AKM, the rest only between two
    if (a.ovr != b.ovr)
        cfgRecord(n, CFG_OVR, a.ovr ? (a.ovrAkm[0] ? a.ovrAkm : "on") : "off",
                  b.ovr ? (b.ovrAkm[0] ? b.ovrAkm : "on") : "off");
    else if (a.ovr)
    {
        text(CFG_OVR_AKM, a.ovrAkm, b.ovrAkm);
        text(CFG_OVR_CIPHER, a.ovrPairwise, b.ovrPairwise);

        if (a.ovrPmf != b.ovrPmf)
            cfgRecord(n, CFG_OVR_PMF, pmf[a.ovrPmf], pmf[b.ovrPmf]);
    }

    flag(CFG_OVR_H2E, a.ovrH2E, b.ovrH2E);

    if (a.mboDisallowed != b.mboDisallowed)
    {
        static const char *const why[] = {
            "allowed", "disallowed", "disallowed, stations at maximum",
            "disallowed, air interface overloaded",
            "disallowed, authentication server overloaded",
            "disallowed, RSSI too low"
        };

        auto reason = [&](char *out, size_t size, uint8_t r)
        {
            if (r < sizeof(why) / sizeof(why[0]))
                strlcpy(out, why[r], size);
            else
                snprintf(out, size, "disallowed, reason %u", r);
        };

        reason(x, sizeof(x), a.mboDisallowed);
        reason(y, sizeof(y), b.mboDisallowed);
        cfgRecord(n, CFG_MBO, x, y);
    }

    if (a.powerConstraint != b.powerConstraint)
    {
        auto db = [](char *out, size_t size, uint8_t v)
        {
            if (v == 255)
                strlcpy(out, "-", size);
            else
                snprintf(out, size, "%u dB", v);
        };

        db(x, sizeof(x), a.powerConstraint);
        db(y, sizeof(y), b.powerConstraint);
        cfgRecord(n, CFG_PWR_CONSTRAINT, x, y);
    }

    /*
     * Transmit Power Envelope: the first value, 0.5 dB steps. Even
     * interpretations are an EIRP per width, odd ones a PSD per MHz.
     */
    if (a.tpeLen != b.tpeLen || memcmp(a.tpe, b.tpe, a.tpeLen) != 0)
    {
        auto tpe = [](char *out, size_t size, const uint8_t *t, uint8_t l)
        {
            if (l < 2)
            {
                strlcpy(out, "-", size);
                return;
            }

            int8_t v = (int8_t)t[1];

            snprintf(out, size, "%d%s dBm%s", v / 2, v % 2 ? ".5" : "",
                     ((t[0] >> 3) & 7) % 2 ? "/MHz" : "");
        };

        tpe(x, sizeof(x), a.tpe, a.tpeLen);
        tpe(y, sizeof(y), b.tpe, b.tpeLen);

        // The same first value: something further on changed
        if (strcmp(x, y) == 0)
            strlcat(y, ", other values", sizeof(y));

        cfgRecord(n, CFG_TPE, x, y);
    }

    // The detail only while WPS stays on, its switch is above
    if (a.wps == b.wps && a.wpsState != b.wpsState)
    {
        static const char *const state[] = { "-", "not configured",
                                             "configured" };

        cfgRecord(n, CFG_WPS_STATE, a.wpsState < 3 ? state[a.wpsState] : "?",
                  b.wpsState < 3 ? state[b.wpsState] : "?");
    }

    if (a.wps == b.wps)
        flag(CFG_WPS_LOCK, a.wpsLocked, b.wpsLocked);

    // The page names the methods
    if (a.wps == b.wps && a.wpsMethods != b.wpsMethods)
    {
        snprintf(x, sizeof(x), "0x%04X", a.wpsMethods);
        snprintf(y, sizeof(y), "0x%04X", b.wpsMethods);
        cfgRecord(n, CFG_WPS_METHODS, a.wpsMethods ? x : "-",
                  b.wpsMethods ? y : "-");
    }

    if (a.oweTrans != b.oweTrans)
        flag(CFG_OWE, a.oweTrans, b.oweTrans);
    else if (a.oweTrans)
    {
        mac(x, sizeof(x), a.oweBssid);
        mac(y, sizeof(y), b.oweBssid);
        text(CFG_OWE_BSSID, x, y);
        text(CFG_OWE_SSID, a.oweSsid, b.oweSsid);
    }

    flag(CFG_HS20, a.hs20, b.hs20);

    if (a.iwType != b.iwType)
    {
        static const char *const type[16] = {
            "private", "private with guest", "chargeable public",
            "free public", "personal device", "emergency services only",
            "type 6", "type 7", "type 8", "type 9", "type 10", "type 11",
            "type 12", "type 13", "test", "wildcard"
        };

        cfgRecord(n, CFG_INTERWORKING, a.iwType < 16 ? type[a.iwType] : "-",
                  b.iwType < 16 ? type[b.iwType] : "-");
    }

    mac(x, sizeof(x), a.mldMac);
    mac(y, sizeof(y), b.mldMac);
    text(CFG_MLD, x, y);

    if (a.mloLinks != b.mloLinks)
    {
        auto links = [](char *out, size_t size, uint16_t m)
        {
            out[0] = 0;

            for (int i = 0; i < 16; i++)
                if (m & (1 << i))
                {
                    char id[6];

                    snprintf(id, sizeof(id), "%s%d", out[0] ? ", " : "", i);
                    strlcat(out, id, size);
                }

            if (!out[0])
                strlcpy(out, "-", size);
        };

        links(x, sizeof(x), a.mloLinks);
        links(y, sizeof(y), b.mloLinks);
        cfgRecord(n, CFG_MLO_LINKS, x, y);
    }

    // The whole set, unless the lowest rate above says it already
    if (a.basicRates != b.basicRates && a.basicRate == b.basicRate)
    {
        auto rates = [](char *out, size_t size, uint16_t m)
        {
            out[0] = 0;

            for (size_t k = 0; k < sizeof(cfgRates); k++)
                if (m & (1 << k))
                {
                    char r[10];

                    snprintf(r, sizeof(r), "%s%u%s", out[0] ? ", " : "",
                             cfgRates[k] / 2, cfgRates[k] & 1 ? ".5" : "");
                    strlcat(out, r, size);
                }

            if (!out[0])
                strlcpy(out, "none", size);
        };

        rates(x, sizeof(x), a.basicRates);
        rates(y, sizeof(y), b.basicRates);
        cfgRecord(n, CFG_BASIC_RATES, x, y);
    }

    flag(CFG_PREAUTH, a.preAuth, b.preAuth);

    if (a.acm != b.acm)
    {
        // AC index order: best effort, background, video, voice
        auto acm = [](char *out, size_t size, uint8_t m)
        {
            static const char *const ac[] = { "BE", "BK", "VI", "VO" };

            out[0] = 0;

            if (m == 255)
            {
                strlcpy(out, "no WMM", size);
                return;
            }

            for (int i = 3; i >= 0; i--)
                if (m & (1 << i))
                {
                    if (out[0])
                        strlcat(out, ", ", size);

                    strlcat(out, ac[i], size);
                }

            if (!out[0])
                strlcpy(out, "none", size);
        };

        acm(x, sizeof(x), a.acm);
        acm(y, sizeof(y), b.acm);
        cfgRecord(n, CFG_ACM, x, y);
    }

    flag(CFG_INTOLERANT, a.intolerant, b.intolerant);
    flag(CFG_TWT_REQUIRED, a.twtRequired, b.twtRequired);

    // Broadcast TWT: none, or the first schedule's interval and duration
    if (a.btwtCount != b.btwtCount || a.btwtInterval != b.btwtInterval ||
        a.btwtDuration != b.btwtDuration)
    {
        auto twt = [](char *out, size_t size, const CfgSnap &s)
        {
            if (!s.btwtCount)
                strlcpy(out, "-", size);
            else
                snprintf(out, size, "every %.1f ms for %.1f ms%s",
                         s.btwtInterval / 1000.0, s.btwtDuration / 1000.0,
                         s.btwtCount > 1 ? " +more" : "");
        };

        twt(x, sizeof(x), a);
        twt(y, sizeof(y), b);
        cfgRecord(n, CFG_TWT, x, y);
    }

    // Links the AP MLD announces to remove, by link ID
    if (a.mlRemoving != b.mlRemoving)
    {
        auto ids = [](char *out, size_t size, uint16_t m)
        {
            out[0] = 0;

            for (int i = 0; i < 16; i++)
                if (m & (1 << i))
                {
                    char id[6];

                    snprintf(id, sizeof(id), "%s%d", out[0] ? ", " : "", i);
                    strlcat(out, id, size);
                }

            if (!out[0])
                strlcpy(out, "-", size);
        };

        ids(x, sizeof(x), a.mlRemoving);
        ids(y, sizeof(y), b.mlRemoving);
        cfgRecord(n, CFG_ML_REMOVE, x, y);
    }

    // Subchannels left out, by their channel numbers
    if (a.punctured != b.punctured)
    {
        puncturedText(x, sizeof(x), a.ehtWidth, a.ehtCenter, a.punctured);
        puncturedText(y, sizeof(y), b.ehtWidth, b.ehtCenter, b.punctured);
        cfgRecord(n, CFG_PUNCT, x, y);
    }

    // 802.11r detail only while it stays on, its switch is above
    if (a.mde && b.mde)
    {
        if (a.mdid != b.mdid)
        {
            snprintf(x, sizeof(x), "0x%04X", a.mdid);
            snprintf(y, sizeof(y), "0x%04X", b.mdid);
            cfgRecord(n, CFG_MDID, x, y);
        }

        flag(CFG_FT_DS, a.ftOverDs, b.ftOverDs);
    }

    // Mesh settings only while it stays a mesh point
    if (a.meshAuth != 255 && b.meshAuth != 255)
    {
        flag(CFG_MESH_GATE, a.meshGate, b.meshGate);
        flag(CFG_MESH_ACCEPT, a.meshAccepting, b.meshAccepting);
        num(CFG_MESH_AUTH, a.meshAuth, b.meshAuth, "");
    }
}

/*
 * A list "a, b, c" into size: as many whole names as fit, then an
 * ellipsis instead of a name cut in two
 */
void shortList(char *out, size_t size, const char *all)
{
    if (strlen(all) < size)
    {
        strlcpy(out, all, size);
        return;
    }

    size_t cut = 0;

    for (const char *q = strstr(all, ", "); q != nullptr;
         q = strstr(q + 2, ", "))
        if ((size_t)(q - all) + sizeof(", \xE2\x80\xA6") <= size)
            cut = q - all;

    memcpy(out, all, cut);
    strlcpy(out + cut, cut ? ", \xE2\x80\xA6" : "\xE2\x80\xA6", size - cut);
}

/*
 * Back to the state before the last change: one entry for the
 * flapping, "width, standard, ..." and the switches so far,
 * updated in place while it goes on.
 */
void cfgFlap(WiFiNetwork &n, const CfgSnap &now)
{
    char all[200] = "";
    char names[40];
    char count[24];

    cfgDiff(n, n.cfg, now, all, sizeof(all));
    shortList(names, sizeof(names), all);

    // The first change and this one back
    n.cfgFlaps = n.cfgFlaps ? n.cfgFlaps + 1 : 2;

    snprintf(count, sizeof(count), "%u switches", (unsigned)n.cfgFlaps);

    CfgChange *own = n.cfgFlapSlot >= 0 ? &n.cfgChanges[n.cfgFlapSlot]
                                        : nullptr;

    if (own == nullptr || own->field != CFG_FLAP)
    {
        cfgRecord(n, CFG_FLAP, names, count);

        n.cfgFlapSlot  = (n.cfgHead + CFG_CHANGES - 1) % CFG_CHANGES;
        n.cfgFlapEvent = cfgSeq;
        return;
    }

    own->at = millis();
    strlcpy(own->to, count, sizeof(own->to));

    // The bell's entry too, if it is still in the list
    if (cfgEvents != nullptr && n.cfgFlapEvent != 0)
    {
        CfgEvent &e = cfgEvents[(n.cfgFlapEvent - 1) % CFG_EVENTS];

        if (e.seq == n.cfgFlapEvent)
        {
            e.at = own->at;
            strlcpy(e.to, count, sizeof(e.to));
            cfgVersion++;
        }
    }
}

// Two devices clocking as one: alternating and both seen a few times
bool clockTwin(const WiFiNetwork &n)
{
    return n.clkSwitches >= 4 && n.clkHits[0] >= 3 && n.clkHits[1] >= 3;
}

/*
 * Sorts a beacon or probe response by its clock, see WiFiNetwork.
 * True when it came from the other of two clocks seen alternating,
 * so the jump in the timer is no restart.
 */
bool clockTrack(WiFiNetwork &n, uint64_t tsf)
{
    /*
     * µs. Drift between frames is far less, but a device that writes
     * the timestamp of beacons it sends itself can be off by up to an
     * interval (an ESP32 beside its SoftAP, 0.1 s); a cloned address
     * on another device has another uptime, seconds to days apart.
     */
    const int64_t SAME = 1000000;

    int64_t off = (int64_t)tsf - (int64_t)mgmtFrameUs;

    int slot = -1;

    for (int i = 0; i < 2; i++)
        if (n.clkHits[i] && llabs(off - n.clkOff[i]) < SAME)
            slot = i;

    if (slot < 0)
    {
        // A new clock: an empty slot, else the one seen less often
        // (after a restart the old one stops coming)
        slot = n.clkHits[0] == 0 ? 0
             : n.clkHits[1] == 0 ? 1
             : (n.clkHits[0] < n.clkHits[1] ? 0 : 1);

        n.clkHits[slot] = 0;

        if (n.clkHits[slot ^ 1] == 0)
            n.clkSwitches = 0;
    }

    bool switched = n.clkHits[0] && n.clkHits[1] && slot != n.clkLast;

    // Following the drift of both crystals
    n.clkOff[slot] = off;

    if (n.clkHits[slot] < 65535)
        n.clkHits[slot]++;

    if (switched && n.clkSwitches < 65535)
        n.clkSwitches++;

    n.clkLast = slot;

    if (clockTwin(n) && !n.clkReported)
    {
        char apart[24];

        snprintf(apart, sizeof(apart), "%.1f s apart",
                 llabs(n.clkOff[0] - n.clkOff[1]) / 1e6);

        n.clkReported = true;
        cfgRecord(n, CFG_CLONE, "one timer", apart);
    }

    return switched && clockTwin(n);
}

// After a beacon: the gap to the one before, see WiFiNetwork
void beaconGap(WiFiNetwork &n, uint8_t heardOn)
{
    // On 2.4 GHz a BSS is heard on its neighbours' channels too;
    // only its own channel gives back-to-back beacons
    if (heardOn != n.channel || n.beaconInterval == 0)
    {
        n.beaconUs = 0;
        return;
    }

    uint64_t interval = (uint64_t)n.beaconInterval * 1024;
    uint64_t gap      = mgmtFrameUs - n.beaconUs;

    // Within one visit to the channel only
    if (n.beaconUs != 0 && mgmtFrameUs > n.beaconUs && gap < 3 * interval)
    {
        // Halved now and then, so the share follows what it does now
        if (n.gapCount >= 1000)
        {
            n.gapCount /= 2;
            n.gapShort /= 2;
        }

        n.gapCount++;

        if (gap < interval / 2)
            n.gapShort++;

        if (n.gapMinUs == 0 || gap < n.gapMinUs)
            n.gapMinUs = (uint32_t)gap;
    }

    n.beaconUs = mgmtFrameUs;
}

// After a beacon has been read into n, with the beacon's elements
void configWatch(WiFiNetwork &n, const uint8_t *ies, int ieLength)
{
    CfgSnap now = cfgSnapOf(n, ies, ieLength);

    if (!n.cfgKnown)
    {
        n.cfg      = now;
        n.cfgKnown = true;
        return;
    }

    /*
     * The DTIM period is 0 until a beacon with a TIM came by (a
     * device sending beacons of its own without one): learnt, not
     * changed.
     */
    if (now.dtimPeriod != 0)
    {
        if (n.cfg.dtimPeriod == 0)
            n.cfg.dtimPeriod = now.dtimPeriod;

        if (n.cfgPrevKnown && n.cfgPrev.dtimPeriod == 0)
            n.cfgPrev.dtimPeriod = now.dtimPeriod;
    }

    if (memcmp(&now, &n.cfg, sizeof(now)) == 0)
    {
        n.cfgPendingCount = 0;
        return;
    }

    // A new state counts once a second beacon agrees with it
    uint32_t h = cfgHash(now);

    if (n.cfgPendingCount == 0 || h != n.cfgPending)
    {
        n.cfgPending      = h;
        n.cfgPendingCount = 1;
        return;
    }

    bool back = n.cfgPrevKnown &&
                memcmp(&now, &n.cfgPrev, sizeof(now)) == 0 &&
                millis() - n.cfgChangedAt < CFG_FLAP_MS;

    if (back)
        cfgFlap(n, now);
    else
    {
        cfgDiff(n, n.cfg, now, nullptr, 0);

        n.cfgFlaps    = 0;
        n.cfgFlapSlot = -1;
    }

    n.cfgPrev         = n.cfg;
    n.cfgPrevKnown    = true;
    n.cfg             = now;
    n.cfgChangedAt    = millis();
    n.cfgPendingCount = 0;
}

// The timestamp of a beacon or probe response: restarts, uptime
void tsfWatch(WiFiNetwork &n, uint64_t tsf)
{
    /*
     * 0 is no time: some APs leave it empty in every probe response
     * (an OpenWrt AP) and keep it running in the beacons.
     * Taken for one, each answer to a probe was a restart.
     */
    if (tsf == 0)
        return;

    /*
     * When the frame was received, not when it is read here: a
     * frame can wait in the queue for a second or two while a
     * long answer goes out, and measured from now the AP's timer
     * looked as if it had gone back, a restart of every AP at
     * once.
     */
    uint32_t now = (uint32_t)(mgmtFrameUs / 1000);

    bool otherClock = clockTrack(n, tsf);

    // Where the timer should be now, give or take a second; a
    // frame of the second clock is no restart
    if (n.tsfAt != 0 && !otherClock)
    {
        uint64_t expected = n.tsf + (uint64_t)(now - n.tsfAt) * 1000;

        if (tsf + 1000000ULL < expected)
        {
            if (n.tsfResets < 255)
                n.tsfResets++;

            n.tsfResetAt = now;

            // How long it had been up: the page writes it out
            char up[24];
            snprintf(up, sizeof(up), "%lu",
                     (unsigned long)(expected / 1000000ULL));
            cfgRecord(n, CFG_RESTART, up, "");
        }
    }

    n.tsf   = tsf;
    n.tsfAt = now ? now : 1;
}

/*
 * EHT Operation (ext 106): parameters (bit 0 EHT Operation
 * Information present, bit 1 Disabled Subchannel Bitmap present),
 * four octets of basic EHT-MCS, then the information: the width
 * (0 20, 1 40, 2 80, 3 160, 4 320 MHz), CCFS0 and CCFS1, and the
 * bitmap of 20 MHz subchannels the BSS leaves out (preamble
 * puncturing), bit 0 the lowest. CCFS1 is the centre of 160 and 320,
 * CCFS0 of 80 and less. False without the information.
 */
bool ehtOperationOf(const uint8_t *ies, int ieLength, uint16_t &width,
                    uint8_t &centre, uint16_t &punctured)
{
    int len = 0;

    const uint8_t *p = findExtendedIE(ies, ieLength, 106, len);

    if (p == nullptr || len < 8 || !(p[0] & 0x01))
        return false;

    static const uint16_t widths[] = { 20, 40, 80, 160, 320 };

    uint8_t w = p[5] & 0x07;

    if (w > 4)
        return false;

    width     = widths[w];
    centre    = w >= 3 ? p[7] : p[6];
    punctured = (p[0] & 0x02) && len >= 10 ? (p[8] | (p[9] << 8)) : 0;

    return true;
}

/*
 * The punctured 20 MHz channels as text, "56, 60", or "-": the
 * lowest channel of the width is the centre less two per further
 * 20 MHz.
 */
void puncturedText(char *out, size_t size, uint16_t width, uint8_t centre,
                   uint16_t punctured)
{
    out[0] = 0;

    int lowest = (int)centre - (width / 20 - 1) * 2;

    for (int i = 0; i < 16 && i < width / 20; i++)
        if (punctured & (1 << i))
        {
            char ch[8];

            snprintf(ch, sizeof(ch), "%s%d", out[0] ? ", " : "",
                     lowest + i * 4);
            strlcat(out, ch, size);
        }

    if (!out[0])
        strlcpy(out, "-", size);
}

/*
 * Max Channel Switch Time (extension 52), sent beside a channel
 * switch: the longest gap from the last beacon on the old channel to
 * the first on the new one, 3 octets in TU, little endian. A Ruckus
 * AP moving from 1 to 6 sent 97. 0 without it.
 */
uint32_t csaTimeOf(const uint8_t *ies, int ieLength)
{
    int len = 0;
    const uint8_t *t = findExtendedIE(ies, ieLength, 52, len);

    if (t == nullptr || len < 3)
        return 0;

    return t[0] | ((uint32_t)t[1] << 8) | ((uint32_t)t[2] << 16);
}

/*
 * The width after a channel switch: the Wide Bandwidth Channel Switch
 * (194), on its own or inside the Channel Switch Wrapper (196), which
 * may also carry a New Country (7) and a New Transmit Power Envelope
 * (195). 194 has the fields of the VHT Operation, see vhtWidthOf();
 * width 0 is 20 or 40 MHz and gives no width here. False without 194.
 */
bool csaWideOf(const uint8_t *ies, int ieLength, uint16_t &width,
               uint8_t &centre, uint8_t &centre2, char *country)
{
    int len = 0;

    country[0] = 0;

    const uint8_t *wrap = findIE(ies, ieLength, 196, len);
    const uint8_t *wide = nullptr;
    int wideLen = 0;

    if (wrap != nullptr)
    {
        // Subelements laid out as elements
        wide = findIE(wrap, len, 194, wideLen);

        int ccLen = 0;
        const uint8_t *cc = findIE(wrap, len, 7, ccLen);

        if (cc != nullptr && ccLen >= 2 && cc[0] >= 'A' && cc[0] <= 'Z' &&
            cc[1] >= 'A' && cc[1] <= 'Z')
        {
            country[0] = cc[0];
            country[1] = cc[1];
            country[2] = 0;
        }
    }

    if (wide == nullptr)
        wide = findIE(ies, ieLength, 194, wideLen);

    width   = 0;
    centre  = 0;
    centre2 = 0;

    return wide != nullptr && wideLen >= 3 &&
           vhtWidthOf(wide, width, centre, centre2);
}

/*
 * VHT Operation (192): channel width, then the two centre frequency
 * segments CCFS0 and CCFS1. Today's APs give 160 MHz as width 1 with
 * CCFS0 the centre of the primary 80 MHz and CCFS1 that of the whole
 * 160, eight channels apart (802.11-2020 Table 9-274): 36/160 is 42
 * and 50, centre 50. CCFS1 further off is 80+80. The deprecated
 * widths 2 (160) and 3 (80+80) have the centre in CCFS0. With 80+80
 * the second segment's centre is in CCFS1, else centre2 is 0. False
 * for width 0, 20 or 40 MHz, which the HT Operation element tells.
 */
bool vhtWidthOf(const uint8_t *vht, uint16_t &bandwidth, uint8_t &centre,
                uint8_t &centre2)
{
    uint8_t width = vht[0];
    uint8_t ccfs0 = vht[1];
    uint8_t ccfs1 = vht[2];

    if (width == 1)
    {
        int diff = (int)ccfs1 - (int)ccfs0;

        if (ccfs1 == 0)
        {
            bandwidth = 80;
            centre    = ccfs0;
        }
        else if (diff == 8 || diff == -8)
        {
            bandwidth = 160;
            centre    = ccfs1;
        }
        else
        {
            bandwidth = 8080;
            centre    = ccfs0;
            centre2   = ccfs1;
        }

        return true;
    }

    if (width == 2)
    {
        bandwidth = 160;
        centre    = ccfs0;
        return true;
    }

    if (width == 3)
    {
        bandwidth = 8080;
        centre    = ccfs0 ? ccfs0 : ccfs1;
        centre2   = ccfs0 ? ccfs1 : 0;
        return true;
    }

    return false;
}

void parseBeacon(const uint8_t *frame, uint16_t length,
                 int8_t rssi, uint8_t channelHint)
{
    if (length < 37)
        return;

    const uint8_t *bssid = &frame[16];

    uint16_t beaconInterval = frame[32] | (frame[33] << 8);
    uint16_t capability     = frame[34] | (frame[35] << 8);

    bool privacy = (capability & 0x0010) != 0;

    const uint8_t *ies = &frame[36];

    int ieLength = (int)length - 36;

    if (ieLength <= 0)
        return;

    // --------------------------------------------------------
    // SSID
    // --------------------------------------------------------

    int ssidLength = 0;

    const uint8_t *ssidData = findIE(ies, ieLength, 0, ssidLength);

    char ssid[33];
    memset(ssid, 0, sizeof(ssid));

    bool hidden = true;

    if (ssidData != nullptr && ssidLength > 0)
    {
        if (ssidLength > 32)
            ssidLength = 32;

        for (int i = 0; i < ssidLength; i++)
        {
            if (ssidData[i] != 0)
                hidden = false;
        }

        if (!hidden)
            memcpy(ssid, ssidData, ssidLength);
    }

    /*
     * 802.11s mesh points send an empty SSID element and carry
     * their Mesh ID in element 114 instead. Showing that is a
     * lot more useful than calling the network hidden.
     */
    bool mesh = false;

    int meshIdLen = 0;

    const uint8_t *meshId =
        findIE(ies, ieLength, 114, meshIdLen);

    if (meshId != nullptr && meshIdLen > 0)
    {
        mesh = true;

        if (hidden)
        {
            int copy = meshIdLen > 32 ? 32 : meshIdLen;

            memset(ssid, 0, sizeof(ssid));
            memcpy(ssid, meshId, copy);

            hidden = false;
        }
    }

    if (hidden)
        strcpy(ssid, "<hidden>");

    /*
     * Mesh Configuration, element 113, 7 octets:
     * path selection protocol, path selection metric,
     * congestion control, synchronisation method,
     * authentication protocol, mesh formation info,
     * mesh capability.
     */
    uint8_t meshPeerings   = 0;
    bool    meshGate       = false;
    bool    meshAccepting  = false;
    bool    meshForwarding = false;
    uint8_t meshPathProto  = 0;
    uint8_t meshMetric     = 0;
    uint8_t meshAuth       = 0;

    int meshCfgLen = 0;

    const uint8_t *meshCfg =
        findIE(ies, ieLength, 113, meshCfgLen);

    // Exactly 7: some Tenda/Realtek APs put 18 octets of their own
    // under this number, which is no mesh at all
    if (meshCfg != nullptr && meshCfgLen == 7)
    {
        mesh = true;

        meshPathProto = meshCfg[0];
        meshMetric    = meshCfg[1];
        meshAuth      = meshCfg[4];

        // Formation info: bit 0 gate, bits 1..6 peerings
        meshGate     = (meshCfg[5] & 0x01) != 0;
        meshPeerings = (meshCfg[5] >> 1) & 0x3F;

        // Capability: bit 0 accepting peerings, bit 3 forwarding
        meshAccepting  = (meshCfg[6] & 0x01) != 0;
        meshForwarding = (meshCfg[6] & 0x08) != 0;
    }

    // --------------------------------------------------------
    // Channel (DS Parameter Set, ID 3)
    // --------------------------------------------------------

    /*
     * channelHint is rx_ctrl.channel, which is the channel the
     * radio sits on right now, not the one the frame arrived
     * on. Right after a sweep step the driver still hands over
     * frames from the previous dwell, and those carry the new
     * number. On 2.4 GHz the DS Parameter Set corrects that,
     * but 5 GHz beacons do not carry element 3 at all, so a
     * 5 GHz BSS could end up stamped with channel 1 for a
     * sweep or two. The HT Operation element names the primary
     * channel explicitly and is present in every HT, VHT, HE
     * and EHT beacon, so it is the better source; the radio is
     * only a last resort.
     */
    uint8_t channel = channelHint;

    bool channelFromBeacon = false;

    int len = 0;

    const uint8_t *ds = findIE(ies, ieLength, 3, len);

    if (ds != nullptr && len >= 1)
    {
        channel = ds[0];
        channelFromBeacon = true;
    }

    // --------------------------------------------------------
    // Country (ID 7)
    // --------------------------------------------------------

    char country[4];
    memset(country, 0, sizeof(country));

    int ccLen = 0;

    const uint8_t *cc = findIE(ies, ieLength, 7, ccLen);

    if (cc != nullptr && ccLen >= 2)
    {
        country[0] = (char)cc[0];
        country[1] = (char)cc[1];
    }

    // --------------------------------------------------------
    // BSS Load (ID 11)
    // --------------------------------------------------------

    int16_t stations    = -1;
    int16_t channelUtil = -1;

    int loadLen = 0;

    const uint8_t *load = findIE(ies, ieLength, 11, loadLen);

    if (load != nullptr && loadLen >= 5)
    {
        stations    = (int16_t)(load[0] | (load[1] << 8));
        channelUtil = (int16_t)((load[2] * 100) / 255);
    }

    watchFrame(bssid, rssi, stations, channelUtil);

    // --------------------------------------------------------
    // HT Operation (ID 61)
    // --------------------------------------------------------

    bool   ht40        = false;
    int8_t htSecOffset = 0;

    bool htGreenfield    = false;
    bool fortyIntolerant = false;

    int htCapLen = 0;

    const uint8_t *htCap = findIE(ies, ieLength, 45, htCapLen);

    if (htCap != nullptr && htCapLen >= 2)
    {
        uint16_t htInfo = htCap[0] | (htCap[1] << 8);

        htGreenfield    = (htInfo & 0x0010) != 0;
        fortyIntolerant = (htInfo & 0x4000) != 0;
    }

    int htLen = 0;

    const uint8_t *ht = findIE(ies, ieLength, 61, htLen);

    if (ht != nullptr && htLen >= 2)
    {
        uint8_t secondary = ht[1] & 0x03;

        if (secondary == 1)
        {
            ht40        = true;
            htSecOffset = +1;
        }
        else if (secondary == 3)
        {
            ht40        = true;
            htSecOffset = -1;
        }

        if (!channelFromBeacon && ht[0] != 0)
        {
            channel = ht[0];
            channelFromBeacon = true;
        }
    }

    // --------------------------------------------------------
    // VHT Operation (ID 192)
    // --------------------------------------------------------

    uint16_t bandwidth      = 20;
    uint8_t  centerChannel  = channel;
    uint8_t  centerChannel2 = 0;

    int vhtLen = 0;

    const uint8_t *vht = findIE(ies, ieLength, 192, vhtLen);

    bool vhtHandled = false;

    if (vht != nullptr && vhtLen >= 3)
        vhtHandled = vhtWidthOf(vht, bandwidth, centerChannel, centerChannel2);

    if (!vhtHandled)
    {
        bandwidth = ht40 ? 40 : 20;

        centerChannel = ht40
                            ? (uint8_t)(channel + htSecOffset * 2)
                            : channel;
    }

    // --------------------------------------------------------
    // PHY-Generation
    // --------------------------------------------------------

    uint8_t standard = PHY_UNKNOWN;

    int tmpLen = 0;

    bool vht24 = false;

    if (findExtendedIE(ies, ieLength, 108, tmpLen) != nullptr)
    {
        standard = PHY_EHT;
    }
    else if (findExtendedIE(ies, ieLength, 35, tmpLen) != nullptr)
    {
        standard = PHY_HE;
    }
    else if (findIE(ies, ieLength, 191, tmpLen) != nullptr)
    {
        /*
         * 802.11ac is defined for 5 GHz only. Several vendors
         * put VHT elements into 2.4 GHz beacons anyway to
         * announce proprietary 256-QAM rates, so taking them
         * at face value there would report Wi-Fi 5 for a plain
         * HT network. In 2.4 GHz the honest answer is HT.
         */
        if (channel > 14)
        {
            standard = PHY_VHT;
        }
        else
        {
            vht24 = true;

            standard =
                (findIE(ies, ieLength, 45, tmpLen) != nullptr)
                    ? PHY_HT
                    : PHY_UNKNOWN;
        }
    }
    else if (findIE(ies, ieLength, 45, tmpLen) != nullptr)
    {
        standard = PHY_HT;
    }

    /*
     * Walk the rate sets once, for everybody. Rates are in
     * units of 500 kbit/s with the top bit marking a basic
     * rate, so 1/2/5.5/11 Mbit/s are 2/4/11/22 and the OFDM
     * rates of 802.11a and g are 12/18/24/36/48/72/96/108.
     */
    bool dsss = false;
    bool ofdm = false;

    uint8_t basicRate = 0;

    uint8_t basicRates[12];
    uint8_t basicRateCount = 0;

    memset(basicRates, 0, sizeof(basicRates));

    for (int pass = 0; pass < 2; pass++)
    {
        int rateLen = 0;

        const uint8_t *rates =
            findIE(ies, ieLength, pass == 0 ? 1 : 50, rateLen);

        if (rates == nullptr)
            continue;

        for (int i = 0; i < rateLen; i++)
        {
            uint8_t r = rates[i] & 0x7F;

            /*
             * 122 to 127 are BSS membership selectors (EHT,
             * HE, SAE hash-to-element, VHT, HT), not rates.
             */
            if ((rates[i] & 0x80) && r < 122)
            {
                if (basicRate == 0 || r < basicRate)
                    basicRate = r;

                if (basicRateCount < 12)
                    basicRates[basicRateCount++] = r;
            }

            if (r == 2 || r == 4 || r == 11 || r == 22)
                dsss = true;

            if (r == 12 || r == 18 || r == 24 || r == 36 ||
                r == 48 || r == 72 || r == 96 || r == 108)
            {
                ofdm = true;
            }
        }
    }

    bool erp = findIE(ies, ieLength, 42, tmpLen) != nullptr;

    if (standard == PHY_UNKNOWN)
    {
        if (channel > 14)
            standard = PHY_11A;
        else if (erp || ofdm)
            standard = PHY_11G;
        else
            standard = PHY_11B;
    }

    // --------------------------------------------------------
    // Every generation the BSS announces
    // --------------------------------------------------------

    uint8_t phyModes = 0;

    if (channel > 14)
    {
        /*
         * No DSSS above channel 14, so any OFDM rate set there
         * is the 802.11a one that 11n and later still build on.
         */
        if (ofdm)
            phyModes |= PHY_MODE_A;
    }
    else
    {
        if (dsss)
            phyModes |= PHY_MODE_B;

        if (ofdm || erp)
            phyModes |= PHY_MODE_G;
    }

    if (findIE(ies, ieLength, 45, tmpLen) != nullptr)
        phyModes |= PHY_MODE_N;

    // Same 2.4 GHz caveat as above: VHT there is not 802.11ac
    if (channel > 14 && findIE(ies, ieLength, 191, tmpLen) != nullptr)
        phyModes |= PHY_MODE_AC;

    if (findExtendedIE(ies, ieLength, 35, tmpLen) != nullptr)
        phyModes |= PHY_MODE_AX;

    if (findExtendedIE(ies, ieLength, 108, tmpLen) != nullptr)
        phyModes |= PHY_MODE_BE;

    // --------------------------------------------------------
    // Roaming support, co-hosted BSSs, pending channel switch
    // --------------------------------------------------------

    int capLen = 0;

    // RM enabled capabilities -> 802.11k
    bool rm11k = findIE(ies, ieLength, 70, capLen) != nullptr;

    // Extended capabilities, bit 19 = BSS transition -> 802.11v
    const uint8_t *extCap = findIE(ies, ieLength, 127, capLen);

    bool bss11v = (extCap != nullptr && capLen >= 3 &&
                   (extCap[2] & 0x08) != 0);

    // Mobility domain -> 802.11r: the ID little endian, then the policy
    const uint8_t *md = findIE(ies, ieLength, 54, capLen);

    bool     ft11r    = md != nullptr;
    uint16_t mdid     = md != nullptr && capLen >= 2 ? md[0] | (md[1] << 8) : 0;
    uint8_t  ftPolicy = md != nullptr && capLen >= 3 ? md[2] : 0;

    bool multiBssid = findIE(ies, ieLength, 71, capLen) != nullptr;

    /*
     * A pending channel switch is announced either by the
     * Channel Switch Announcement (37: mode, new channel,
     * count) or, when the new channel is in another operating
     * class, by the Extended CSA (60: mode, class, channel,
     * count). Either way the last two fields are what matters.
     */
    uint8_t csaChannel = 0;
    uint8_t csaCount   = 0;
    uint8_t csaMode    = 0;

    const uint8_t *csa = findIE(ies, ieLength, 37, capLen);

    if (csa != nullptr && capLen >= 3)
    {
        csaMode    = csa[0];
        csaChannel = csa[1];
        csaCount   = csa[2];
    }
    else
    {
        csa = findIE(ies, ieLength, 60, capLen);

        if (csa != nullptr && capLen >= 4)
        {
            csaMode    = csa[0];
            csaChannel = csa[2];
            csaCount   = csa[3];
        }
    }

    // And the width it moves to, with a new country if any
    uint16_t csaWidth = 0;
    uint8_t  csaCenter = 0, csaCenter2 = 0;
    char     csaCountry[3];

    if (csaChannel)
        csaWideOf(ies, ieLength, csaWidth, csaCenter, csaCenter2, csaCountry);
    else
        csaCountry[0] = 0;

    uint32_t csaTime = csaChannel ? csaTimeOf(ies, ieLength) : 0;

    // --------------------------------------------------------
    // Security
    // --------------------------------------------------------

    RsnInfo rsn;
    memset(&rsn, 0, sizeof(rsn));

    int rsnLen = 0;

    const uint8_t *rsnIE = findIE(ies, ieLength, 48, rsnLen);

    parseSuites(rsnIE, rsnLen, false, rsn);

    RsnInfo wpa;
    memset(&wpa, 0, sizeof(wpa));

    int wpaLen = 0;

    const uint8_t *wpaIE = findVendorIE(ies, ieLength,
                                        0x00, 0x50, 0xF2, 0x01,
                                        wpaLen);

    parseSuites(wpaIE, wpaLen, true, wpa);

    int wpsLen = 0;

    const uint8_t *wpsIE = findVendorIE(ies, ieLength,
                                        0x00, 0x50, 0xF2, 0x04,
                                        wpsLen);

    bool wps = (wpsIE != nullptr);

    /*
     * WPS attributes are 2-byte big-endian ID, 2-byte
     * big-endian length, then the value. Only three of them
     * say anything about how exposed the AP is.
     */
    uint16_t wpsMethods = 0;
    bool     wpsLocked  = false;
    uint8_t  wpsState   = 0;

    // Beacons often carry only version and state; the methods and
    // the lock come with probe responses. A frame without them
    // must not clear what an earlier one said.
    bool     haveMethods = false;
    bool     haveLocked  = false;

    for (int pos = 0; wpsIE != nullptr && pos + 4 <= wpsLen; )
    {
        uint16_t attr = (wpsIE[pos] << 8) | wpsIE[pos + 1];
        uint16_t alen = (wpsIE[pos + 2] << 8) | wpsIE[pos + 3];

        pos += 4;

        if (pos + alen > wpsLen)
            break;

        if (attr == 0x1008 && alen >= 2)          // Config Methods
        {
            wpsMethods  = (wpsIE[pos] << 8) | wpsIE[pos + 1];
            haveMethods = true;
        }
        else if (attr == 0x1057 && alen >= 1)     // AP Setup Locked
        {
            wpsLocked  = (wpsIE[pos] != 0);
            haveLocked = true;
        }
        else if (attr == 0x1044 && alen >= 1)     // WPS State
            wpsState = wpsIE[pos];

        pos += alen;
    }

    /*
     * OWE Transition Mode element: BSSID of the partner, one
     * byte SSID length, then the SSID. Anything after that is
     * optional band and channel info we do not need.
     */
    bool    oweTrans = false;
    uint8_t oweBssid[6];
    char    oweSSID[33];

    memset(oweBssid, 0, sizeof(oweBssid));
    memset(oweSSID, 0, sizeof(oweSSID));

    int oweLen = 0;

    const uint8_t *owe = findVendorIE(ies, ieLength,
                                      0x50, 0x6F, 0x9A, 0x1C,
                                      oweLen);

    if (owe != nullptr && oweLen >= 7)
    {
        uint8_t ssidLen = owe[6];

        if (ssidLen <= 32 && 7 + ssidLen <= oweLen)
        {
            oweTrans = true;

            memcpy(oweBssid, owe, 6);
            memcpy(oweSSID, &owe[7], ssidLen);

            resolveOweSSID(oweBssid, oweSSID);
        }
    }

    RsnInfo osen;
    memset(&osen, 0, sizeof(osen));

    int osenLen = 0;

    parseOsen(findVendorIE(ies, ieLength,
                           0x50, 0x6F, 0x9A, 0x12, osenLen),
              osenLen, osen);

    // Hotspot 2.0 Indication
    int hs20Len = 0;

    bool hs20 = findVendorIE(ies, ieLength,
                             0x50, 0x6F, 0x9A, 0x10,
                             hs20Len) != nullptr;

    /*
     * A Wi-Fi Direct device in its Listen state answers probe
     * requests with the SSID "DIRECT-" and the Privacy bit, but no
     * RSN element: it is no group yet, and once it is one it uses
     * WPA2. Not WEP.
     */
    int p2pLen = 0;

    bool p2pListen = ssidLength == 7 && memcmp(ssid, "DIRECT-", 7) == 0 &&
                     findVendorIE(ies, ieLength, 0x50, 0x6F, 0x9A, 0x09,
                                  p2pLen) != nullptr;

    /*
     * An Xbox (One on, also switched off in standby) beacons for its
     * controllers on 5 GHz: no SSID, the Privacy bit without RSN, and
     * Microsoft's Network Cost element with 12 octets where the
     * documented one has 4. One console seen (October 2026).
     */
    int costLen = 0;

    const uint8_t *cost = findVendorIE(ies, ieLength, 0x00, 0x50, 0xF2,
                                       0x11, costLen);

    bool xbox = privacy && hidden && !rsn.present && !wpa.present &&
                xboxWirelessCost(cost, costLen);

    char security[16];

    // A WPA element counts only with something to log in with; an
    // empty one beside SAE does not make it a transition network
    bool wpaOffered = wpa.present && (wpa.psk || wpa.eap);

    // WPA1 beside WPA3 (a LANCOM set to WPA1/WPA2/WPA3): its own
    // element, TKIP as the group cipher
    if (rsn.sae && wpaOffered)
        strcpy(security, rsn.psk ? "WPA/WPA2/WPA3" : "WPA/WPA3");
    else if (rsn.sae && rsn.psk)
        strcpy(security, "WPA2/WPA3");
    else if (rsn.sae)
        strcpy(security, "WPA3");
    else if (rsn.owe)
        strcpy(security, "OWE");
    else if (rsn.present && rsn.eap)
        strcpy(security, "WPA2-EAP");
    else if (rsn.present && rsn.psk && wpaOffered)
        strcpy(security, "WPA/WPA2");
    else if (rsn.present && rsn.psk)
        strcpy(security, "WPA2");
    else if (rsn.present)
        strcpy(security, "RSN");
    else if (wpa.present && wpa.eap)
        strcpy(security, "WPA-EAP");
    else if (wpa.present)
        strcpy(security, "WPA");
    else if (osen.present)
        strcpy(security, "OSEN");
    else if (privacy && p2pListen)
        strcpy(security, "Wi-Fi Direct");
    else if (xbox)
        strcpy(security, "Xbox Wireless");
    else if (privacy)
        strcpy(security, "WEP");
    else
        strcpy(security, "Open");

    const RsnInfo &detail =
        rsn.present ? rsn : (wpa.present ? wpa : osen);

    // --------------------------------------------------------
    // Update network database
    // --------------------------------------------------------

    int existing = -1;

    for (int i = 0; i < MAX_NETWORKS; i++)
    {
        if (!networks[i].valid)
            continue;

        if (memcmp(networks[i].bssid, bssid, 6) == 0)
        {
            existing = i;
            break;
        }
    }

    bool isNew = false;

    if (existing < 0)
    {
        for (int i = 0; i < MAX_NETWORKS; i++)
        {
            if (!networks[i].valid)
            {
                existing = i;
                isNew    = true;
                break;
            }
        }
    }

    if (existing < 0)
    {
        uint32_t oldest      = UINT32_MAX;
        int      oldestIndex = 0;

        for (int i = 0; i < MAX_NETWORKS; i++)
        {
            if (networks[i].lastSeen < oldest)
            {
                oldest      = networks[i].lastSeen;
                oldestIndex = i;
            }
        }

        existing = oldestIndex;
        isNew    = true;
    }

    WiFiNetwork &n = networks[existing];

    if (isNew)
    {
        memset(&n, 0, sizeof(n));
        n.firstSeen = millis();

        // 0 is a value for these, so "not seen" is -1
        n.mptInterval = -1;
        n.atimWindow  = -1;
        n.bssColor    = -1;
        n.colorNext   = -1;
        n.acmMask     = -1;
        n.wfdInfo     = -1;
        n.oceCtrl     = -1;
        n.omn         = -1;
        n.nanPref     = -1;
        n.nanAmPref   = -1;
    }

    n.valid = true;

    memcpy(n.bssid, bssid, 6);

    // Do not fall back to <hidden> once the name is known
    if (!(hidden && n.ssidResolved && n.ssid[0] != 0))
    {
        memset(n.ssid, 0, sizeof(n.ssid));
        strncpy(n.ssid, ssid, 32);
    }

    n.vht24          = vht24;
    n.mesh           = mesh;

    // Out-of-band announcement of co-located 6 GHz BSSs
    {
        int rnrLen = 0;

        const uint8_t *rnr = findIE(ies, ieLength, 201, rnrLen);

        if (rnr != nullptr && rnrLen >= 4)
            parseRNR(rnr, rnrLen, n.bssid, n.ssid, n.channel);
    }
    n.meshPeerings   = meshPeerings;
    n.meshGate       = meshGate;
    n.meshAccepting  = meshAccepting;
    n.meshForwarding = meshForwarding;
    n.meshPathProto  = meshPathProto;
    n.meshMetric     = meshMetric;
    n.meshAuth       = meshAuth;

    n.channel        = channel;
    n.bandwidth      = bandwidth;
    n.centerChannel  = centerChannel;
    n.centerChannel2 = centerChannel2;

    n.ehtWidth  = 0;
    n.punctured = 0;
    ehtOperationOf(ies, ieLength, n.ehtWidth, n.ehtCenter, n.punctured);
    n.rssi           = rssi;
    n.standard       = standard;
    n.phyModes       = phyModes;
    n.htGreenfield   = htGreenfield;
    n.fortyIntolerant = fortyIntolerant;
    n.rm11k          = rm11k;
    n.bss11v         = bss11v;
    n.rrmCaps        = rrmCapsOf(ies, ieLength);
    n.streams        = maxStreamsOf(ies, ieLength, channel);

    n.ehtMac = 0;
    ehtCapsOf(ies, ieLength, false, n.ehtMac, n.ehtPhy, n.ehtQam4k);

    mlRemovalOf(ies, ieLength, n.mlRemoving, n.mlRemoveIn);

    n.heTwt     = heTwtOf(ies, ieLength);
    n.heCaps    = heCapsOf(ies, ieLength);
    n.vhtCaps   = vhtCapsOf(ies, ieLength, channel <= 14);
    n.btwtCount = broadcastTwtOf(ies, ieLength, n.btwtId, n.btwtRec,
                                 n.btwtInterval, n.btwtDuration, 2);

    parseMultiLink(ies, ieLength, n.mlo);

    tpeOf(ies, ieLength, n.tpe, n.tpeLen);

    int pwrLen = 0;

    const uint8_t *tpc = findIE(ies, ieLength, 35, pwrLen);

    n.tpcPower = (tpc != nullptr && pwrLen >= 1) ? (int8_t)tpc[0] : 127;

    const uint8_t *pc = findIE(ies, ieLength, 32, pwrLen);

    n.powerConstraint = (pc != nullptr && pwrLen >= 1) ? pc[0] : 255;

    const uint8_t *erpIe = findIE(ies, ieLength, 42, pwrLen);

    n.erp = (erpIe != nullptr && pwrLen >= 1) ? erpIe[0] : 255;

    const uint8_t *xl = findIE(ies, ieLength, 193, pwrLen);

    n.xloadPresent = xl != nullptr && pwrLen >= 6;

    if (n.xloadPresent)
    {
        n.xloadMu = xl[0] | (xl[1] << 8);
        memcpy(n.xload, xl + 2, 4);
    }

    // Time Advertisement and Time Zone, kept when absent
    const uint8_t *ta = findIE(ies, ieLength, 69, pwrLen);

    if (ta != nullptr)
    {
        int64_t offset = apClockOffsetOf(ta, pwrLen, &frame[24],
                                         mgmtFrameUs);

        if (offset != 0)
        {
            int64_t prev = n.apClockOffsetUs;

            n.apClockOffsetUs = offset;
            noteApClock(n.bssid, offset, prev);
        }
    }

    const uint8_t *tz = findIE(ies, ieLength, 98, pwrLen);

    if (tz != nullptr && pwrLen > 0)
    {
        int k = 0;

        for (int i = 0; i < pwrLen && k < (int)sizeof(n.apTz) - 1; i++)
            if (tz[i] >= 0x20 && tz[i] < 0x7f)
                n.apTz[k++] = (char)tz[i];

        n.apTz[k] = 0;
    }

    collectVendorOuis(ies, ieLength, n.vendorOui, n.vendorOuiCount);

    /*
     * Interworking (107): access network type, venue and HESSID.
     * Kept from the last frame that had it, an AP may send it in
     * its probe responses only.
     */
    int iwLen = 0;

    const uint8_t *iw = findIE(ies, ieLength, 107, iwLen);

    if (iw != nullptr && iwLen >= 1)
    {
        n.iwLen = iwLen > 9 ? 9 : iwLen;

        memcpy(n.iw, iw, n.iwLen);
    }

    int accLen = 0;

    const uint8_t *acc = findVendorIE(ies, ieLength,
                                      0x00, 0xA0, 0x40, 0x00, accLen);

    if (acc != nullptr)
        parseAppleDevice(acc, accLen, n);

    parseApExtras(ies, ieLength, n);
    parseWfaElements(ies, ieLength, n);
    parseRemoteId(ies, ieLength, n);

    // Beacon and probe response timestamp, little endian
    {
        uint64_t tsf = 0;

        for (int i = 7; i >= 0; i--)
            tsf = (tsf << 8) | frame[24 + i];

        // An AP that leaves it empty in every answer gets a note
        if (((frame[0] >> 4) & 0x0F) == 5)
        {
            uint8_t &count = tsf == 0 ? n.probeTsfZero : n.probeTsfSet;

            if (count < 255)
                count++;
        }

        tsfWatch(n, tsf);
    }

    collectUnknownElements(ies, ieLength, n.unknownIes,
                           n.unknownSeen, n.unknownCount, 6,
                           ((frame[0] >> 4) & 0x0F) == 8
                               ? SEEN_BEACON : SEEN_PROBE_RESP);
    n.wnmCaps        = wnmCapsOf(ies, ieLength);
    n.ft11r          = ft11r;
    n.mdid           = mdid;
    n.ftPolicy       = ftPolicy;
    n.multiBssid     = multiBssid;
    n.csaChannel     = csaChannel;
    n.csaCount       = csaCount;
    n.csaMode        = csaMode;
    n.csaWidth       = csaWidth;
    n.csaCenter      = csaCenter;
    n.csaCenter2     = csaCenter2;
    memcpy(n.csaCountry, csaCountry, sizeof(n.csaCountry));
    n.csaTime        = csaTime;
    n.basicRate      = basicRate;
    n.basicRateCount = basicRateCount;

    memcpy(n.basicRates, basicRates, sizeof(n.basicRates));
    // Without WPS nothing is kept; with it, what this frame leaves
    // out stays from the last frame that had it
    if (!wps || haveMethods)
        n.wpsMethods = wpsMethods;

    if (!wps || haveLocked)
        n.wpsLocked = wpsLocked;

    n.wpsState       = wpsState;
    n.preAuth        = detail.preAuth;
    n.badSuiteIn     = rsn.badSeen ? 1 : (wpa.badSeen ? 2 : 0);
    n.badSuite       = rsn.badSeen ? rsn.badSuite : wpa.badSuite;
    n.wpaNoAkm       = wpa.present && !wpaOffered;
    n.hs20           = hs20;
    n.xbox           = xbox;
    n.oweTrans       = oweTrans;

    /*
     * Only clear the OWE flag when this beacon carries a real
     * name: the partner element may have filled it in before
     * the BSS itself was ever heard.
     */
    if (!hidden)
        n.ssidFromOwe = false;

    memcpy(n.oweBssid, oweBssid, 6);
    strncpy(n.oweSSID, oweSSID, sizeof(n.oweSSID) - 1);
    n.oweSSID[sizeof(n.oweSSID) - 1] = 0;
    n.mfpCapable     = rsn.mfpCapable;
    n.mfpRequired    = rsn.mfpRequired;
    n.wps            = wps;
    n.stations       = stations;
    n.channelUtil    = channelUtil;
    n.beaconInterval = beaconInterval;
    n.ibss           = (capability & 0x0002) != 0;

    strncpy(n.security, security, sizeof(n.security) - 1);
    n.security[sizeof(n.security) - 1] = 0;

    memset(n.pairwise, 0, sizeof(n.pairwise));
    memset(n.groupCipher, 0, sizeof(n.groupCipher));
    memset(n.mgmtCipher, 0, sizeof(n.mgmtCipher));
    memset(n.akm, 0, sizeof(n.akm));

    strncpy(n.pairwise, detail.pairwise, sizeof(n.pairwise) - 1);
    strncpy(n.groupCipher, detail.group, sizeof(n.groupCipher) - 1);
    strncpy(n.mgmtCipher, detail.mgmt, sizeof(n.mgmtCipher) - 1);
    strncpy(n.akm, detail.akm, sizeof(n.akm) - 1);

    memcpy(n.country, country, sizeof(n.country));

    if (((frame[0] >> 4) & 0x0F) == 8)
    {
        configWatch(n, ies, ieLength);
        beaconGap(n, channelHint);
    }

    n.lastSeen = millis();
    n.beacons++;
}

/*
 * Association and reassociation responses.
 *
 * The fixed part is two bytes of capability, two bytes of
 * status code, two bytes of association ID. Addr1 is the
 * station the AP is answering, Addr3 the BSS.
 *
 * Nothing is created from this frame: a response to a station
 * that was never seen requesting says little, and inventing a
 * table entry from it would show stations that are not there.
 */
void parseAssocResponse(const uint8_t *frame, uint16_t length)
{
    if (length < 30)
        return;

    const uint8_t *dest  = &frame[4];
    const uint8_t *bssid = &frame[16];

    uint16_t status = frame[26] | (frame[27] << 8);

    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        if (!clients[i].valid)
            continue;

        if (memcmp(clients[i].mac, dest, 6) != 0)
            continue;

        ClientEntry &c = clients[i];

        c.assocStatus   = status;
        c.assocStatusAt = millis();
        c.assocComeback = 0;

        memcpy(c.assocStatusBssid, bssid, 6);

        /*
         * Status 30, rejected temporarily: the AP still holds a
         * PMF protected association of this station and checks it
         * with an SA Query first. The Timeout Interval element
         * (type 3) says after how many TU to try again.
         */
        int len = 0;

        const uint8_t *idle = findIE(&frame[30], length - 30, 90, len);

        if (status == 0 && idle != nullptr && len >= 3)
        {
            c.maxIdleGranted    = idle[0] | (idle[1] << 8);
            c.maxIdleGrantedOpt = idle[2];
        }

        const uint8_t *ti = findIE(&frame[30], length - 30, 56, len);

        if (status == 30 && ti != nullptr && len >= 5 && ti[0] == 3)
            c.assocComeback = ti[1] | (ti[2] << 8) | (ti[3] << 16) |
                              ((uint32_t)ti[4] << 24);

        if (status != 0)
            c.assocFailures++;

        return;
    }
}
