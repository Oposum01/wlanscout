// SPDX-License-Identifier: GPL-2.0-only WITH AdditionRef-wlanscout-linking-exception
// SPDX-FileCopyrightText: 2026 Dr.-Ing. Simon Wunderlich Systementwicklung & Beratung

// ============================================================
// Client table
// ============================================================

int findOrCreateClient(const uint8_t *mac)
{
    int      freeSlot    = -1;
    uint32_t oldest      = UINT32_MAX;
    int      oldestIndex = 0;

    // Evict randomised, unassociated entries first
    uint32_t oldestTemp      = UINT32_MAX;
    int      oldestTempIndex = -1;

    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        if (clients[i].valid)
        {
            if (memcmp(clients[i].mac, mac, 6) == 0)
                return i;

            if (clients[i].lastSeen < oldest)
            {
                oldest      = clients[i].lastSeen;
                oldestIndex = i;
            }

            if (!clients[i].associated && (clients[i].mac[0] & 0x02) &&
                clients[i].lastSeen < oldestTemp)
            {
                oldestTemp      = clients[i].lastSeen;
                oldestTempIndex = i;
            }
        }
        else if (freeSlot < 0)
        {
            freeSlot = i;
        }
    }

    int idx;

    if (freeSlot >= 0)
        idx = freeSlot;
    else if (oldestTempIndex >= 0)
        idx = oldestTempIndex;
    else
        idx = oldestIndex;

    memset(&clients[idx], 0, sizeof(ClientEntry));

    clients[idx].valid     = true;
    clients[idx].firstSeen = millis();

    memcpy(clients[idx].mac, mac, 6);

    return idx;
}

/*
 * A hidden AP does not put its SSID in the beacon, but a
 * station that wants to join has to name it: both a directed
 * probe request and an association request carry the SSID in
 * clear. Once seen, keep it.
 */
void learnBssName(const uint8_t *bssid, const char *ssid,
                  bool fromAssoc)
{
    if (bssNames == nullptr || ssid == nullptr || ssid[0] == 0)
        return;

    int      freeSlot = -1;
    uint32_t oldest   = UINT32_MAX;
    int      oldestIx = 0;

    for (int i = 0; i < MAX_BSS_NAMES; i++)
    {
        if (!bssNames[i].valid)
        {
            if (freeSlot < 0)
                freeSlot = i;

            continue;
        }

        if (memcmp(bssNames[i].bssid, bssid, 6) == 0)
        {
            // An association request outranks a probe request
            if (fromAssoc || !bssNames[i].fromAssoc)
            {
                memset(bssNames[i].ssid, 0, sizeof(bssNames[i].ssid));
                strncpy(bssNames[i].ssid, ssid, 32);

                bssNames[i].fromAssoc = fromAssoc;
            }

            bssNames[i].lastSeen = millis();

            return;
        }

        if (bssNames[i].lastSeen < oldest)
        {
            oldest   = bssNames[i].lastSeen;
            oldestIx = i;
        }
    }

    int idx = (freeSlot >= 0) ? freeSlot : oldestIx;

    memset(&bssNames[idx], 0, sizeof(BssName));

    bssNames[idx].valid     = true;
    bssNames[idx].fromAssoc = fromAssoc;
    bssNames[idx].lastSeen  = millis();

    memcpy(bssNames[idx].bssid, bssid, 6);
    strncpy(bssNames[idx].ssid, ssid, 32);

    Serial.printf("Learned SSID of %s from a station: %s\r\n",
                  macToString(bssNames[idx].bssid).c_str(),
                  bssNames[idx].ssid);
}

const char *lookupBssName(const uint8_t *bssid)
{
    if (bssNames == nullptr || macIsZero(bssid))
        return "";

    for (int i = 0; i < MAX_BSS_NAMES; i++)
    {
        if (!bssNames[i].valid)
            continue;

        if (memcmp(bssNames[i].bssid, bssid, 6) == 0)
            return bssNames[i].ssid;
    }

    return "";
}

/*
 * Short SSID is the CRC-32 of the SSID, so a name can be
 * recovered by hashing the SSIDs we already know and looking
 * for the same value.
 */
uint32_t crc32ssid(const char *data, size_t length)
{
    uint32_t crc = 0xFFFFFFFF;

    for (size_t i = 0; i < length; i++)
    {
        crc ^= (uint8_t)data[i];

        for (int b = 0; b < 8; b++)
        {
            crc = (crc & 1) ? ((crc >> 1) ^ 0xEDB88320)
                            : (crc >> 1);
        }
    }

    return ~crc;
}

bool isSixGhzOpClass(uint8_t opClass)
{
    // 131 to 137 are the 6 GHz global operating classes
    return (opClass >= 131 && opClass <= 137);
}

uint16_t sixGhzWidth(uint8_t opClass)
{
    switch (opClass)
    {
        case 131: return 20;
        case 132: return 40;
        case 133: return 80;
        case 134: return 160;
        case 135: return 80;      // 80+80
        case 136: return 20;
        case 137: return 320;
    }

    return 0;
}

uint16_t sixGhzFreq(uint8_t channel)
{
    // Channel 2 sits below the regular grid
    if (channel == 2)
        return 5935;

    return 5950 + 5 * (uint16_t)channel;
}

void noteSixGhz(const uint8_t *bssid, uint8_t opClass,
                uint8_t channel, uint32_t shortSsid,
                bool haveShortSsid, uint8_t bssParams,
                bool haveParams, int8_t psd,
                const uint8_t *reporter, const char *reporterSsid,
                uint8_t reporterChannel)
{
    if (sixGhz == nullptr)
        return;

    int      slot   = -1;
    int      freeIx = -1;
    uint32_t oldest = UINT32_MAX;
    int      oldIx  = 0;

    for (int i = 0; i < MAX_SIXG_BSS; i++)
    {
        if (!sixGhz[i].valid)
        {
            if (freeIx < 0)
                freeIx = i;

            continue;
        }

        bool same =
            (bssid != nullptr)
                ? (memcmp(sixGhz[i].bssid, bssid, 6) == 0)
                : (haveShortSsid &&
                   sixGhz[i].shortSsid == shortSsid &&
                   sixGhz[i].channel == channel);

        if (same)
        {
            slot = i;
            break;
        }

        if (sixGhz[i].lastSeen < oldest)
        {
            oldest = sixGhz[i].lastSeen;
            oldIx  = i;
        }
    }

    if (slot < 0)
    {
        slot = (freeIx >= 0) ? freeIx : oldIx;

        memset(&sixGhz[slot], 0, sizeof(SixGhzBss));

        sixGhz[slot].psd = 127;
    }

    SixGhzBss &e = sixGhz[slot];

    bool isNew = !e.valid;

    e.valid    = true;
    e.opClass  = opClass;
    e.channel  = channel;
    e.lastSeen = millis();
    e.reports++;

    if (bssid != nullptr)
        memcpy(e.bssid, bssid, 6);

    if (haveShortSsid)
    {
        e.shortSsid     = shortSsid;
        e.haveShortSsid = true;
    }

    if (haveParams)
    {
        // Bit 1 same SSID, bit 5 unsolicited probe responses,
        // bit 6 co-located AP
        e.sameSsid    = (bssParams & 0x02) != 0;
        e.unsolicited = (bssParams & 0x20) != 0;
        e.coLocated   = (bssParams & 0x40) != 0;
    }

    if (psd != 127)
        e.psd = psd;

    memcpy(e.reporter, reporter, 6);

    e.reporterChannel = reporterChannel;

    if (reporterSsid != nullptr)
    {
        strncpy(e.reporterSsid, reporterSsid, sizeof(e.reporterSsid) - 1);
        e.reporterSsid[sizeof(e.reporterSsid) - 1] = 0;
    }

    /*
     * Name it if we can, on every report, so a renamed network
     * does not keep its old name: the same-SSID bit says it
     * carries the reporting AP's name, otherwise the short SSID
     * may match one we already know. A name the short SSID no
     * longer matches goes.
     */
    if (e.sameSsid && reporterSsid != nullptr &&
        reporterSsid[0] != 0 &&
        strcmp(reporterSsid, "<hidden>") != 0)
    {
        strncpy(e.ssid, reporterSsid, 32);
        e.ssid[32] = 0;
    }
    else if (e.haveShortSsid)
    {
        if (e.ssid[0] != 0 &&
            crc32ssid(e.ssid, strlen(e.ssid)) != e.shortSsid)
            e.ssid[0] = 0;

        for (int i = 0; e.ssid[0] == 0 && networks != nullptr &&
                        i < MAX_NETWORKS; i++)
        {
            if (!networks[i].valid || networks[i].ssid[0] == 0)
                continue;

            if (crc32ssid(networks[i].ssid,
                          strlen(networks[i].ssid)) == e.shortSsid)
            {
                strncpy(e.ssid, networks[i].ssid, 32);
                e.ssid[32] = 0;
            }
        }
    }

    if (isNew)
    {
        Serial.printf("6 GHz BSS reported: %s ch %u (%u MHz) by %s\r\n",
                      macToString(e.bssid).c_str(),
                      (unsigned)channel,
                      (unsigned)sixGhzFreq(channel),
                      macToString(reporter).c_str());
    }
}

/*
 * Reduced Neighbor Report, element 201.
 *
 * A sequence of neighbour AP information fields, each with a
 * header, an operating class, a channel and one or more TBTT
 * information entries whose layout is given by the length in
 * the header.
 */
void parseRNR(const uint8_t *rnr, int len,
              const uint8_t *reporter, const char *reporterSsid,
              uint8_t reporterChannel)
{
    int pos = 0;

    while (pos + 4 <= len)
    {
        uint16_t hdr = rnr[pos] | (rnr[pos + 1] << 8);

        uint8_t count   = ((hdr >> 4) & 0x0F) + 1;
        uint8_t infoLen = (hdr >> 8) & 0xFF;

        uint8_t opClass = rnr[pos + 2];
        uint8_t channel = rnr[pos + 3];

        pos += 4;

        if (infoLen == 0)
            return;

        for (uint8_t i = 0; i < count; i++)
        {
            if (pos + infoLen > len)
                return;

            const uint8_t *f = &rnr[pos];

            pos += infoLen;

            if (!isSixGhzOpClass(opClass))
                continue;

            const uint8_t *bssid = nullptr;

            uint32_t shortSsid     = 0;
            bool     haveShortSsid = false;

            uint8_t bssParams  = 0;
            bool    haveParams = false;

            int8_t psd = 127;

            // f[0] is the TBTT offset, the rest depends on the
            // declared length
            if (infoLen >= 7)
                bssid = &f[1];

            if (infoLen == 5 || infoLen == 6)
            {
                shortSsid = (uint32_t)f[1] | ((uint32_t)f[2] << 8) |
                            ((uint32_t)f[3] << 16) |
                            ((uint32_t)f[4] << 24);

                haveShortSsid = true;
            }
            else if (infoLen >= 11)
            {
                shortSsid = (uint32_t)f[7] | ((uint32_t)f[8] << 8) |
                            ((uint32_t)f[9] << 16) |
                            ((uint32_t)f[10] << 24);

                haveShortSsid = true;
            }

            if (infoLen == 2)
            {
                bssParams  = f[1];
                haveParams = true;
            }
            else if (infoLen == 6)
            {
                bssParams  = f[5];
                haveParams = true;
            }
            else if (infoLen == 8)
            {
                bssParams  = f[7];
                haveParams = true;
            }
            else if (infoLen >= 12)
            {
                bssParams  = f[11];
                haveParams = true;
            }

            if (infoLen >= 13)
                psd = (int8_t)f[12];

            noteSixGhz(bssid, opClass, channel, shortSsid,
                       haveShortSsid, bssParams, haveParams, psd,
                       reporter, reporterSsid, reporterChannel);
        }
    }
}

void resolveHiddenSSID(const uint8_t *bssid, const char *ssid,
                       bool fromAssoc)
{
    if (ssid == nullptr || ssid[0] == 0)
        return;

    if (bssid == nullptr || (bssid[0] & 0x01) || macIsZero(bssid))
        return;

    // Remember it regardless of whether the AP was ever heard
    learnBssName(bssid, ssid, fromAssoc);

    for (int i = 0; i < MAX_NETWORKS; i++)
    {
        if (!networks[i].valid)
            continue;

        if (memcmp(networks[i].bssid, bssid, 6) != 0)
            continue;

        bool hidden =
            (networks[i].ssid[0] == 0 ||
             strcmp(networks[i].ssid, "<hidden>") == 0);

        if (hidden)
        {
            memset(networks[i].ssid, 0, sizeof(networks[i].ssid));
            strncpy(networks[i].ssid, ssid, 32);

            networks[i].ssidResolved = true;

            Serial.printf("Resolved hidden SSID of %s: %s\r\n",
                          macToString(networks[i].bssid).c_str(),
                          networks[i].ssid);
        }

        return;
    }
}

/*
 * Name a hidden BSS from its OWE Transition partner.
 *
 * In Enhanced Open the open BSS carries the BSSID and the
 * SSID of the OWE one, which does not broadcast its name. The
 * pair belongs to the same AP, so this is not inference from
 * station traffic but a statement by the operator, and it
 * outranks anything learned from a probe request.
 */
void resolveOweSSID(const uint8_t *bssid, const char *ssid)
{
    if (ssid == nullptr || ssid[0] == 0)
        return;

    if (bssid == nullptr || macIsZero(bssid))
        return;

    learnBssName(bssid, ssid, true);

    for (int i = 0; i < MAX_NETWORKS; i++)
    {
        if (!networks[i].valid)
            continue;

        if (memcmp(networks[i].bssid, bssid, 6) != 0)
            continue;

        bool hidden =
            (networks[i].ssid[0] == 0 ||
             strcmp(networks[i].ssid, "<hidden>") == 0);

        if (hidden)
        {
            memset(networks[i].ssid, 0, sizeof(networks[i].ssid));
            strncpy(networks[i].ssid, ssid, 32);

            networks[i].ssidResolved = true;
            networks[i].ssidFromOwe  = true;

            Serial.printf(
                "Named OWE BSS %s from its transition partner: %s\r\n",
                macToString(networks[i].bssid).c_str(),
                networks[i].ssid);
        }

        return;
    }
}


/*
 * Track BSSID changes so roaming stays visible.
 *
 * Debounced on purpose: a station that appears to jump
 * between two BSSIDs which differ in a single bit is almost
 * always a corrupted frame, not a roaming client. An
 * association request is authoritative and skips the
 * debounce.
 */
#define BSSID_CHANGE_CONFIRMATIONS 3

void noteBssidChange(struct ClientEntry &c, const uint8_t *bssid,
                     bool authoritative, bool ft = false)
{
    // Never accept the station's own address as its BSSID
    if (memcmp(c.mac, bssid, 6) == 0)
        return;

    if (macIsZero(c.bssid))
    {
        memcpy(c.bssid, bssid, 6);

        c.bssidCandidateCount = 0;

        return;
    }

    if (memcmp(c.bssid, bssid, 6) == 0)
    {
        c.bssidCandidateCount = 0;
        return;
    }

    if (!authoritative)
    {
        if (memcmp(c.bssidCandidate, bssid, 6) != 0)
        {
            memcpy(c.bssidCandidate, bssid, 6);

            c.bssidCandidateCount = 1;

            return;
        }

        c.bssidCandidateCount++;

        if (c.bssidCandidateCount < BSSID_CHANGE_CONFIRMATIONS)
            return;
    }

    memcpy(c.prevBssid, c.bssid, 6);
    memcpy(c.bssid, bssid, 6);

    c.bssidCandidateCount = 0;

    c.roams++;

    if (authoritative)
        c.roamsConfirmed++;

    if (ft)
        c.roamsFt++;

    // Keep the recent history so a ping-pong between two APs
    // can be told apart from a burst of bad frames
    c.roamLog[c.roamLogWrite].at = millis();
    c.roamLog[c.roamLogWrite].confirmed = authoritative;
    c.roamLog[c.roamLogWrite].ft        = ft;

    memcpy(c.roamLog[c.roamLogWrite].bssid, bssid, 6);

    c.roamLogWrite = (c.roamLogWrite + 1) % MAX_ROAM_EVENTS;

    if (c.roamLogCount < MAX_ROAM_EVENTS)
        c.roamLogCount++;
}

void registerDataFrame(const uint8_t *mac, const uint8_t *bssid,
                       const uint8_t *peer, int8_t rssi,
                       uint8_t channel, bool uplink, uint16_t length)
{
    ClientEntry &c = clients[findOrCreateClient(mac)];

    c.associated = true;

    noteBssidChange(c, bssid, false);

    memcpy(c.peer, peer, 6);

    c.rssi     = rssi;
    c.channel  = channel;
    c.lastSeen = millis();

    if (uplink)
    {
        c.toAp++;
        c.toApBytes += length;
    }
    else
    {
        c.fromAp++;
        c.fromApBytes += length;
    }
}

void registerProbeRequest(const uint8_t *mac, const char *ssid,
                          const uint8_t *bssid, int8_t rssi,
                          uint8_t channel,
                          const uint8_t *ies, int ieLength)
{
    ClientEntry &c = clients[findOrCreateClient(mac)];

    c.rssi     = rssi;
    c.channel  = channel;
    c.lastSeen = millis();
    c.probes++;

    // Directed probe requests carry the BSSID
    if (bssid != nullptr && !(bssid[0] & 0x01) && !macIsZero(bssid))
    {
        if (!c.associated)
            memcpy(c.bssid, bssid, 6);

        // A directed probe names the network it is looking for
        resolveHiddenSSID(bssid, ssid, false);
    }

    parseStationCapabilities(ies, ieLength, c, SEEN_PROBE_REQ);

    if (ssid == nullptr || ssid[0] == 0)
        return;

    for (int i = 0; i < MAX_PROBE_SSIDS; i++)
    {
        if (strcmp(c.probeSSID[i], ssid) == 0)
            return;
    }

    // Push out the oldest entry
    for (int i = MAX_PROBE_SSIDS - 1; i > 0; i--)
    {
        memcpy(c.probeSSID[i], c.probeSSID[i - 1],
               sizeof(c.probeSSID[i]));
    }

    memset(c.probeSSID[0], 0, sizeof(c.probeSSID[0]));
    strncpy(c.probeSSID[0], ssid, 32);
}

/*
 * Record one association request.
 *
 * Takes a snapshot of the station as it stands after the
 * frame has been parsed, which is the most complete view of
 * it that will ever exist: an association request carries the
 * full capability set, a probe request only a subset.
 */
void noteAssocHistory(const ClientEntry &c)
{
    if (assocHistory == nullptr)
        return;

    int slot   = -1;
    int oldest = 0;

    for (int i = 0; i < MAX_ASSOC_HISTORY; i++)
    {
        if (!assocHistory[i].valid)
        {
            if (slot < 0)
                slot = i;

            continue;
        }

        if (memcmp(assocHistory[i].sta.mac, c.mac, 6) == 0)
        {
            slot = i;
            break;
        }

        if (assocHistory[i].sta.lastSeen <
            assocHistory[oldest].sta.lastSeen)
            oldest = i;
    }

    if (slot < 0)
    {
        slot = oldest;
        assocHistoryDropped++;
        assocHistory[slot].valid = false;
    }

    AssocHistory &h = assocHistory[slot];

    if (!h.valid)
    {
        h.valid      = true;
        h.firstEpoch = deviceEpoch();
    }

    h.sta       = c;
    h.lastEpoch = deviceEpoch();
}

// ============================================================
// (Re)Association Request
//
// Subtype 0: capability 2, listen interval 2 -> IEs at 28
// Subtype 2: plus current AP address 6 -> IEs at 34
//
// Addr1 = AP, Addr2 = station, Addr3 = BSSID
// ============================================================

void parseAssocRequest(const uint8_t *frame, uint16_t length,
                       int8_t rssi, uint8_t channel, bool reassoc)
{
    uint16_t fixed = reassoc ? 10 : 4;

    if (length < 24 + fixed)
        return;

    const uint8_t *bssid = &frame[4];
    const uint8_t *sta   = &frame[10];

    if (sta[0] & 0x01)
        return;

    const uint8_t *ies = &frame[24 + fixed];

    int ieLength = (int)length - (24 + fixed);

    ClientEntry &c = clients[findOrCreateClient(sta)];

    /*
     * Fast BSS Transition (802.11r): a reassociation request whose
     * FT element (55) carries a MIC, i.e. a MIC Control with an
     * element count. The first association in a mobility domain
     * has the element without one.
     */
    int  fteLength = 0;

    const uint8_t *fte = ieLength > 0
        ? findIE(ies, ieLength, 55, fteLength) : nullptr;

    bool ft = reassoc && fte != nullptr && fteLength >= 2 && fte[1] != 0;

    // An association request states the BSS explicitly
    if (!(bssid[0] & 0x01) && !macIsZero(bssid))
        noteBssidChange(c, bssid, true, ft);

    c.associated = true;
    c.sawAssoc   = true;
    c.assocFrames++;

    c.rssi     = rssi;
    c.channel  = channel;
    c.lastSeen = millis();

    if (ieLength > 0)
    {
        int ssidLength = 0;

        const uint8_t *ssidData =
            findIE(ies, ieLength, 0, ssidLength);

        if (ssidData != nullptr && ssidLength > 0)
        {
            if (ssidLength > 32)
                ssidLength = 32;

            memset(c.assocSSID, 0, sizeof(c.assocSSID));
            memcpy(c.assocSSID, ssidData, ssidLength);

            // The association request names the network even
            // when its beacons hide the SSID
            resolveHiddenSSID(bssid, c.assocSSID, true);
        }

        parseStationCapabilities(ies, ieLength, c, SEEN_ASSOC_REQ);

        int selLength = 0;

        const uint8_t *sel = findVendorIE(ies, ieLength, 0x50, 0x6F,
                                          0x9A, 0x2C, selLength);

        if (sel != nullptr && selLength >= 1)
        {
            c.rsnSelSeen = true;
            c.rsnSel     = sel[0];
        }

        int idleLength = 0;

        const uint8_t *idle = findIE(ies, ieLength, 90, idleLength);

        if (idle != nullptr && idleLength >= 3)
        {
            c.maxIdleAsked    = idle[0] | (idle[1] << 8);
            c.maxIdleAskedOpt = idle[2];
        }
    }

    noteAssocHistory(c);
}

uint16_t countClients(const uint8_t *bssid)
{
    uint16_t count = 0;

    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        if (!clients[i].valid || !clients[i].associated)
            continue;

        if (memcmp(clients[i].bssid, bssid, 6) == 0)
            count++;
    }

    return count;
}

void cleanupClients()
{
    uint32_t now = millis();

    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        if (!clients[i].valid)
            continue;

        uint32_t timeout = CLIENT_TIMEOUT_MS;

        // Randomised MAC without association: short hold time
        if (!clients[i].associated && (clients[i].mac[0] & 0x02))
            timeout = PROBE_ONLY_TIMEOUT_MS;

        if (now - clients[i].lastSeen > timeout)
            clients[i].valid = false;
    }
}

// ============================================================
// Probe Request
//
// 24 byte header followed directly by the IEs.
// Addr1 = DA (usually broadcast), Addr2 = station,
// Addr3 = BSSID (broadcast for a wildcard probe)
// ============================================================

void parseProbeRequest(const uint8_t *frame, uint16_t length,
                       int8_t rssi, uint8_t channel)
{
    if (length < 24)
        return;

    const uint8_t *sta   = &frame[10];
    const uint8_t *bssid = &frame[16];

    if (sta[0] & 0x01)
        return;

    const uint8_t *ies = &frame[24];

    int ieLength = (int)length - 24;

    char ssid[33];
    memset(ssid, 0, sizeof(ssid));

    if (ieLength > 0)
    {
        int ssidLength = 0;

        const uint8_t *ssidData =
            findIE(ies, ieLength, 0, ssidLength);

        if (ssidData != nullptr && ssidLength > 0)
        {
            if (ssidLength > 32)
                ssidLength = 32;

            memcpy(ssid, ssidData, ssidLength);
        }
    }

    registerProbeRequest(sta, ssid, bssid, rssi, channel,
                         ies, ieLength);
}
