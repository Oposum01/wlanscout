// SPDX-License-Identifier: GPL-2.0-only WITH AdditionRef-wlanscout-linking-exception
// SPDX-FileCopyrightText: 2026 Dr.-Ing. Simon Wunderlich Systementwicklung & Beratung

// ============================================================
// AWDL action frames
//
// After the 24 byte header: category 127, OUI 00:17:F2, type 8,
// version, subtype (0 periodic synchronisation, 3 master
// indication), a reserved octet, PHY and target transmit time
// (4 octets each). Then TLVs of type, 2-octet length (little
// endian), value:
//
//   21 Version: version (major << 4 | minor), device class
//      (1 macOS, 2 iOS or watchOS, 8 tvOS)
//   12 Data Path State: 2 octets of flags (little endian), then
//      only the fields the flags announce, in this order:
//      0x0100 country (3), 0x0200 social channel map (2),
//      0x0001 infra BSSID (6) and channel (2), 0x0002 infra
//      address (6), 0x0004 AWDL address (6)
//
// The AWDL address has to be the sender, and a country two
// capital letters; otherwise the TLV was not read as laid out
// above and nothing of it is kept.
// ============================================================

struct AwdlEntry *findAwdl(const uint8_t *mac)
{
    int freeSlot = -1;
    int oldest   = 0;

    for (int i = 0; i < MAX_AWDL; i++)
    {
        if (!awdlDevices[i].valid)
        {
            if (freeSlot < 0)
                freeSlot = i;
            continue;
        }

        if (memcmp(awdlDevices[i].mac, mac, 6) == 0)
            return &awdlDevices[i];

        if (awdlDevices[i].lastSeen < awdlDevices[oldest].lastSeen ||
            !awdlDevices[oldest].valid)
            oldest = i;
    }

    // Full: the one heard longest ago makes room
    AwdlEntry &d = awdlDevices[freeSlot >= 0 ? freeSlot : oldest];

    memset(&d, 0, sizeof(d));

    d.valid     = true;
    d.devClass  = -1;
    d.firstSeen = millis();

    memcpy(d.mac, mac, 6);

    return &d;
}

void parseAwdlPath(const uint8_t *v, int l, const uint8_t *sender,
                   struct AwdlEntry &d)
{
    if (l < 2)
        return;

    uint16_t flags = v[0] | (v[1] << 8);
    int at = 2;

    char    country[3] = { 0, 0, 0 };
    bool    infra = false, sta = false;
    uint8_t bssid[6], addr[6], channel = 0;

    if (flags & 0x0100)
    {
        if (at + 3 > l || v[at] < 'A' || v[at] > 'Z' ||
            v[at + 1] < 'A' || v[at + 1] > 'Z')
            return;

        country[0] = v[at];
        country[1] = v[at + 1];
        at += 3;
    }

    if (flags & 0x0200)
        at += 2;

    if (flags & 0x0001)
    {
        if (at + 8 > l)
            return;

        memcpy(bssid, v + at, 6);
        channel = v[at + 6];
        infra   = !(bssid[0] & 0x01);
        at += 8;
    }

    if (flags & 0x0002)
    {
        if (at + 6 > l)
            return;

        memcpy(addr, v + at, 6);
        sta = !(addr[0] & 0x01);
        at += 6;
    }

    if (flags & 0x0004)
    {
        if (at + 6 > l || memcmp(v + at, sender, 6) != 0)
            return;
    }

    if (country[0])
        memcpy(d.country, country, 3);

    if (infra)
    {
        d.infraSeen    = true;
        d.infraChannel = channel;
        memcpy(d.infraBssid, bssid, 6);
    }

    if (sta)
    {
        d.staSeen = true;
        memcpy(d.sta, addr, 6);
    }
}

// Fine Timing Measurement elements into an FtmInfo, see there
void ftmElementsOf(const uint8_t *ies, int len, FtmInfo &f)
{
    for (int pos = 0; pos + 2 <= len; )
    {
        uint8_t id = ies[pos];
        uint8_t l  = ies[pos + 1];

        if (pos + 2 + l > len)
            break;

        const uint8_t *v = ies + pos + 2;

        // Measurement token, mode, type, then the request or report
        if (id == 206 && l >= 9)
        {
            f.paramSeen = true;
            memcpy(f.param, v, 9);
        }
        else if (id == 38 && l >= 3)
        {
            if (v[2] == 8)
                f.askLci = true;
            else if (v[2] == 11)
                f.askCivic = true;
        }
        else if (id == 39 && l >= 3 && (v[1] & 0x07) == 0)
        {
            // Subelements: ID, length, value; 0 carries the field
            int from = v[2] == 11 ? 4 : 3;

            // Location Civic: type 0 is the IETF RFC 4776 format
            if (v[2] == 11 && (l < 4 || v[3] != 0))
                from = l;

            for (int i = from; i + 2 <= l; )
            {
                uint8_t sid = v[i];
                uint8_t sl  = v[i + 1];

                if (i + 2 + sl > l)
                    break;

                if (sid == 0 && v[2] == 8 && sl >= 16)
                {
                    f.lciSeen = true;
                    memcpy(f.lci, v + i + 2, 16);
                }
                else if (sid == 0 && v[2] == 11)
                {
                    f.civicLen = sl > sizeof(f.civic) ? sizeof(f.civic) : sl;
                    memcpy(f.civic, v + i + 2, f.civicLen);
                }

                i += 2 + sl;
            }
        }

        pos += 2 + l;
    }
}

// Adds mac to a short list unless it is there; full lists stay as they are
void ftmAddPeer(uint8_t list[][6], uint8_t &count, uint8_t size,
                const uint8_t *mac)
{
    for (uint8_t i = 0; i < count; i++)
        if (memcmp(list[i], mac, 6) == 0)
            return;

    if (count < size)
        memcpy(list[count++], mac, 6);
}

void parseFtm(const uint8_t *frame, uint16_t length, int8_t rssi,
              uint8_t channel)
{
    if (length < 24 + 3 || frame[24] != 4 ||
        (frame[25] != 32 && frame[25] != 33))
        return;

    const uint8_t *da = frame + 4;
    const uint8_t *sa = frame + 10;

    if ((sa[0] & 0x01) || (da[0] & 0x01))
        return;

    bool request = frame[25] == 32;

    // Request: category, action, trigger. FTM: category, action, dialog
    // token, follow up token, TOD, TOA, TOD error, TOA error
    int fixed = request ? 3 : 2 + 1 + 1 + 6 + 6 + 2 + 2;

    if (length < 24 + fixed)
        return;

    FtmInfo f;

    memset(&f, 0, sizeof(f));
    ftmElementsOf(frame + 24 + fixed, length - 24 - fixed, f);

    const uint8_t *initiator = request ? sa : da;
    const uint8_t *responder = request ? da : sa;

    WiFiNetwork *n = nullptr;

    for (int i = 0; i < MAX_NETWORKS; i++)
        if (networks[i].valid &&
            memcmp(networks[i].bssid, responder, 6) == 0)
            n = &networks[i];

    if (request)
    {
        ClientEntry &c = clients[findOrCreateClient(initiator)];

        c.rssi     = rssi;
        c.channel  = channel;
        c.lastSeen = millis();

        if (c.ftmRequests < 65535)
            c.ftmRequests++;

        ftmAddPeer(c.ftmTargets, c.ftmTargetCount, 3, responder);

        c.ftmAskLci   |= f.askLci;
        c.ftmAskCivic |= f.askCivic;

        if (f.paramSeen)
        {
            c.ftmParamSeen = true;
            memcpy(c.ftmParam, f.param, 9);
        }

        if (n != nullptr && n->ftmRequests < 65535)
            n->ftmRequests++;
    }
    else
    {
        // Only a station seen asking: the answers alone make none
        for (int i = 0; i < MAX_CLIENTS; i++)
            if (clients[i].valid && memcmp(clients[i].mac, initiator, 6) == 0 &&
                clients[i].ftmAnswers < 65535)
                clients[i].ftmAnswers++;

        if (n == nullptr)
            return;

        if (n->ftmFrames < 65535)
            n->ftmFrames++;

        if (f.paramSeen)
        {
            n->ftmParamSeen = true;
            memcpy(n->ftmParam, f.param, 9);
        }

        if (f.lciSeen)
        {
            n->ftmLciSeen = true;
            memcpy(n->ftmLci, f.lci, 16);
        }

        if (f.civicLen)
        {
            n->ftmCivicLen = f.civicLen;
            memcpy(n->ftmCivic, f.civic, f.civicLen);
        }
    }

    if (n != nullptr)
        ftmAddPeer(n->ftmPeers, n->ftmPeerCount, 4, initiator);
}

void parseAwdl(const uint8_t *frame, uint16_t length, int8_t rssi,
               uint8_t channel)
{
    if (length < 24 + 16)
        return;

    const uint8_t *body = frame + 24;

    if (body[0] != 0x7F || body[1] != 0x00 || body[2] != 0x17 ||
        body[3] != 0xF2 || body[4] != 0x08)
        return;

    const uint8_t *sender = frame + 10;

    if (sender[0] & 0x01)
        return;

    AwdlEntry &d = *findAwdl(sender);

    d.rssi     = rssi;
    d.channel  = channel;
    d.lastSeen = millis();
    d.frames++;

    const uint8_t *p = body + 16;
    int len = length - 24 - 16;

    for (int i = 0; i + 3 <= len; )
    {
        uint8_t  type = p[i];
        uint16_t l    = p[i + 1] | (p[i + 2] << 8);

        if (i + 3 + l > len)
            break;

        const uint8_t *v = p + i + 3;

        if (type == 21 && l >= 2)
        {
            d.version  = v[0];
            d.devClass = v[1];
        }

        if (type == 12)
            parseAwdlPath(v, l, sender, d);

        i += 3 + l;
    }
}

void cleanupAwdl()
{
    uint32_t now = millis();

    for (int i = 0; i < MAX_AWDL; i++)
        if (awdlDevices[i].valid &&
            now - awdlDevices[i].lastSeen > CLIENT_TIMEOUT_MS)
            awdlDevices[i].valid = false;
}
