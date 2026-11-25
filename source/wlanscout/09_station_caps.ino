// SPDX-License-Identifier: GPL-2.0-only WITH AdditionRef-wlanscout-linking-exception
// SPDX-FileCopyrightText: 2026 Dr.-Ing. Simon Wunderlich Systementwicklung & Beratung

// ============================================================
// Station Capabilities
//
// Fed from the IEs of probe requests and (re)association
// requests. The association request is the complete one,
// probe requests carry only a subset depending on the
// device.
// ============================================================

/*
 * Time Advertisement (69), timing capabilities 2: the Time
 * Value is the UTC time at which the AP's TSF timer was 0, so
 * UTC at the moment a frame left the AP is that plus the TSF
 * in the frame's Timestamp field. Value 1 (an offset without
 * the time itself) and 0 (no time source) carry nothing to set
 * a clock from.
 *
 * Returns UTC minus esp_timer at reception, in microseconds,
 * the same form as clockOffsetUs, or 0 when the element holds
 * no usable time.
 */
int64_t apClockOffsetOf(const uint8_t *ta, int len,
                        const uint8_t *tsfField, uint64_t rxUs)
{
    if (len < 11 || ta[0] != 2 || rxUs == 0)
        return 0;

    int year = ta[1] | (ta[2] << 8);

    unsigned month = ta[3], day = ta[4];
    unsigned hour  = ta[5], minute = ta[6], second = ta[7];
    unsigned ms    = ta[8] | (ta[9] << 8);

    if (year < 2024 || year > 2099 || month < 1 || month > 12 ||
        day < 1 || day > 31 || hour > 23 || minute > 59 ||
        second > 60 || ms > 999)
    {
        return 0;
    }

    uint64_t tsf = 0;

    for (int i = 7; i >= 0; i--)
        tsf = (tsf << 8) | tsfField[i];

    int64_t zeroUs =
        ((int64_t)daysFromCivil(year, month, day) * 86400LL +
         hour * 3600LL + minute * 60LL + second) * 1000000LL +
        ms * 1000LL;

    return zeroUs + (int64_t)tsf - (int64_t)rxUs;
}

/*
 * Without NTP and without a browser (a sensor running on its
 * own) the clock is taken from an AP's Time Advertisement, so
 * the event log and the capture still get real timestamps.
 *
 * Two frames of the same BSS must agree within two seconds;
 * that throws out a single garbled frame and an AP whose TSF
 * and time value do not fit together. Each frame is held against
 * the one before of its own BSS (prevUs, 0 for none): with one
 * candidate for all, two APs heard in turn, on two channels with
 * a long beacon interval, kept pushing each other out.
 *
 * One AP is not very exact: a TP-Link EAP ran 15 s behind NTP on
 * its 2.4 GHz radio and 84 s on its 5 GHz one, apparently each
 * radio fixes the time at which its TSF was 0 once and never
 * again. So the renewals take the median over every AP heard.
 */
void noteApClock(const uint8_t *bssid, int64_t offsetUs, int64_t prevUs)
{
    if (pcapClockSource == 1 || pcapClockSource == 2)
        return;

    if (pcapClockSource == 3)
    {
        if (millis() - wifiClockLastSet < wifiClockWaitMs)
            return;

        static int64_t offsets[MAX_NETWORKS];

        int count = 0;

        for (int i = 0; i < MAX_NETWORKS; i++)
            if (networks[i].valid && networks[i].apClockOffsetUs != 0)
                offsets[count++] = networks[i].apClockOffsetUs;

        if (count == 0)
            return;

        std::sort(offsets, offsets + count);

        clockOffsetUs    = offsets[count / 2];
        wifiClockLastSet = millis();
        wifiClockApCount = (uint8_t)count;
        wifiClockWaitMs  = WIFI_CLOCK_INTERVAL_MS;

        Serial.printf("Clock renewed from the median of %d APs\r\n",
                      count);
        return;
    }

    int64_t diff = offsetUs - prevUs;

    if (prevUs == 0 || diff <= -2000000LL || diff >= 2000000LL)
        return;

    clockOffsetUs    = offsetUs;
    wifiClockLastSet = millis();

    memcpy(wifiClockBssid, bssid, 6);

    if (pcapClockSource != 3)
    {
        pcapClockSource = 3;

        char mac[18];

        snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
                 bssid[0], bssid[1], bssid[2],
                 bssid[3], bssid[4], bssid[5]);

        Serial.printf("Clock taken from the time advertisement "
                      "of %s\r\n", mac);

        logEventFrom("system", "clock set from the time advertisement "
                     "of %s", mac);
    }
}

/*
 * Elements this sensor knows: those it decodes, and those it
 * deliberately passes over because they say nothing it would
 * show. Anything else in a frame is recorded as unknown, so that
 * something new in the field shows up instead of being skipped
 * without a trace. Vendor specific elements (221) are proprietary
 * by definition and not counted.
 *
 *   decoded:  0 SSID, 1/50 rates, 3 DS, 5 TIM (DTIM period,
 *             buffered traffic), 6 IBSS Parameter Set,
 *             7 Country, 11 BSS Load,
 *             36 Supported Channels, 37/60 channel switch, 194 Wide
 *             Bandwidth Channel Switch and 196 Channel Switch Wrapper
 *             (the width and country after it), 42 ERP,
 *             45/61 HT, 48 RSN, 54 Mobility Domain, 59 Operating
 *             Classes, 70 RM Capabilities, 71 Multiple BSSID,
 *             113/114 Mesh, 127 Extended Capabilities, 191/192 VHT,
 *             32 Power Constraint, 35 TPC Report, 69 Time
 *             Advertisement, 98 Time Zone, 193 Extended BSS Load,
 *             195 Tx Power Envelope, 201 Reduced Neighbor Report,
 *             107 Interworking,
 *             40 Quiet, 66 Measurement Pilot Transmission, 67 BSS
 *             Available Admission Capacity, 76 Management MIC (beacon
 *             protection), 108 Advertisement Protocol, 133 Cisco
 *             CCX device name, 199 Operating Mode Notification,
 *             55 Fast BSS Transition (whether it has a MIC),
 *             90 BSS Max Idle Period, 56 Timeout Interval (the
 *             comeback time of a temporary association reject);
 *             ext 35 HE Capabilities, 36 HE Operation (BSS colour),
 *             42 BSS Color Change Announcement, 52 Max Channel
 *             Switch Time, 59 HE 6 GHz, 107 Multi-Link, 108 EHT
 *             Capabilities
 *   passed:   4 CF Parameter Set (point coordination, never used;
 *             an old Linksys sends it), 33 Power Capability, 46 QoS Capability,
 *             47 ERP (the 802.11g draft's number, Broadcom sends
 *             it next to 42 with the same content),
 *             51 AP Channel Report, 52 Neighbor Report (in probe
 *             responses of FRITZ!Boxes, devolo and Extreme: 22 octets,
 *             a few small values and zeros, no neighbour in them),
 *             62 Secondary Channel Offset, 72 20/40 BSS
 *             Coexistence, 74 OBSS Scan Parameters, 244 RSN
 *             Extension, 10 Request (what
 *             a station asks for), 128 Lucent/Agere proprietary,
 *             150 (Cisco's number, empty in probe requests of
 *             some phones), 168 Relay Transfer Parameter (60 GHz,
 *             one zero octet in a camera chip's probe requests), 200 and 231 as Tenda/Realtek fill them, 240 FILS
 *             Indication (FILS itself shows in the AKM list);
 *             ext 2 FILS Request Parameters, 37 UORA, 38 MU EDCA, 39 Spatial Reuse, 41 NDP
 *             Feedback Report, 56 Non-Inheritance,
 *             110 Multi-Link Traffic Indication,
 *             137 Non-AP STA Regulatory Connectivity
 *
 * Unknown ones are kept as their ID, extension IDs as 256 + ID.
 */
/*
 * The lists above, also sent to the page (sendCatalogReads()) so
 * the IE catalog view can show what the sensor reads. 221 vendor
 * specific is handled on its own, by OUI and type.
 */
static const uint8_t decodedIes[] = {
    0, 1, 3, 5, 6, 7, 11, 32, 35, 36, 37, 40, 42, 45, 48, 50, 54, 55, 56,
    59, 60, 61, 66, 67, 69, 70, 71, 76, 90, 98, 107, 108, 113, 114, 127,
    133, 191, 192, 193, 194, 195, 196, 199, 201, 216
};

static const uint8_t decodedExt[] = { 35, 36, 42, 52, 59, 106, 107, 108 };

static const uint8_t passedIes[] = {
    4, 10, 33, 46, 47, 51, 52, 62, 72, 74, 128, 150, 168, 200, 231, 240, 244
};

static const uint8_t passedExt[] = {
    2, 37, 38, 39, 41, 56, 110, 137
};

/*
 * Decoded only in part, so they stay in the catalog. Empty for
 * now: Interworking (107) is decoded in full since 1.60.0.
 */
static const uint8_t partialIes[] = { };

static bool inList(const uint8_t *list, size_t n, int v)
{
    for (size_t i = 0; i < n; i++)
        if (list[i] == v)
            return true;

    return false;
}

#define IN_LIST(list, v) inList(list, sizeof(list), v)

bool elementKnown(uint8_t id, int ext)
{
    if (id == 255)
        return IN_LIST(decodedExt, ext) || IN_LIST(passedExt, ext);

    return id == 221 || IN_LIST(decodedIes, id) || IN_LIST(passedIes, id);
}

bool elementPartial(uint8_t id)
{
    return IN_LIST(partialIes, id);
}

/*
 * Copies one text field of the Apple Device IE, printable
 * characters only, cut to the buffer.
 */
void appleDeviceText(const uint8_t *v, int len, char *out, size_t size)
{
    size_t k = 0;

    for (int i = 0; i < len && k < size - 1; i++)
        if (v[i] >= 0x20 && v[i] < 0x7f)
            out[k++] = (char)v[i];

    out[k] = 0;
}

/*
 * Apple Device IE (00:A0:40 type 0), the body after the type:
 * type / length / value fields. 1 is the name, 2 the
 * manufacturer, 3 the model; the others (flags, an OUI, MAC
 * addresses) are not shown. Seen in the field from car head
 * units: "My SEAT 4624", "SEAT", "SE3821".
 */
void parseAppleDevice(const uint8_t *p, int len, struct WiFiNetwork &n)
{
    n.acc = true;

    for (int pos = 0; pos + 2 <= len; )
    {
        uint8_t t = p[pos];
        uint8_t l = p[pos + 1];

        if (pos + 2 + l > len)
            break;

        const uint8_t *v = p + pos + 2;

        if (t == 1)
            appleDeviceText(v, l, n.accName, sizeof(n.accName));
        else if (t == 2)
            appleDeviceText(v, l, n.accMaker, sizeof(n.accMaker));
        else if (t == 3)
            appleDeviceText(v, l, n.accModel, sizeof(n.accModel));

        pos += 2 + l;
    }
}

/*
 * A name in a fixed field, NUL padded: printable characters up to
 * the first NUL, cut to the buffer, without spaces at either end
 * (many devices send a lone space as their WPS name).
 */
void fixedName(const uint8_t *v, int len, char *out, size_t size)
{
    size_t k = 0;

    for (int i = 0; i < len && v[i] != 0 && k < size - 1; i++)
        if ((v[i] > 0x20 && v[i] < 0x7f) || (v[i] == 0x20 && k > 0))
            out[k++] = (char)v[i];

    while (k > 0 && out[k - 1] == ' ')
        k--;

    out[k] = 0;
}

/*
 * Microsoft's Network Cost element (00:50:F2 type 0x11) as an Xbox
 * sends it for its controllers: 12 octets after the type, cost level
 * 1 and 0x10 beside it ("01 10 06 2c 01 28 00 …"), where a Windows
 * hotspot sends the documented 4.
 */
bool xboxWirelessCost(const uint8_t *cost, int len)
{
    return cost != nullptr && len == 12 && cost[0] == 0x01 &&
           cost[1] == 0x10;
}

/*
 * WSC Primary Device Type, 8 octets: category, the OUI, subcategory.
 * In the WPS element (attribute 0x1054) and the P2P Device Info. The
 * subcategories are the Wi-Fi Alliance's (OUI 00:50:F2:04) only, so
 * another OUI keeps the category alone: 7/1 a TV, 3 a printer.
 */
static void primaryDeviceType(const uint8_t *t, WiFiNetwork &n)
{
    uint16_t cat = (t[0] << 8) | t[1];

    if (cat == 0)
        return;

    bool wfa = t[2] == 0x00 && t[3] == 0x50 && t[4] == 0xF2 && t[5] == 0x04;

    n.pdtCat = cat;
    n.pdtSub = wfa ? (t[6] << 8) | t[7] : 0;
}

/*
 * The smaller elements of an AP, each kept from the last frame
 * carrying it:
 *
 *   40 Quiet: count, period (beacon intervals), duration and
 *      offset (TU), the count is a snapshot and left out
 *   66 Measurement Pilot Transmission: the interval in TU, 0 for
 *      none; a Broadcom AP seen sends one stray octet after it
 *   67 BSS Available Admission Capacity: kept raw, the page
 *      decodes the bitmask and the values
 *   76 Management MIC (MME) or extended capability bit 84: the
 *      AP signs its beacons (beacon protection)
 *   108 Advertisement Protocol: the protocol IDs, ANQP and so on
 *   133 Cisco CCX "device name": the AP's name at offset 10,
 *      LANCOM and others send it too
 *   MikroTik 00:0C:42 type 0: subelement 1 carries the radio name
 *      at offset 10, DD-WRT sends it as well
 *   00:30:1A type 1: 01 01, then the firmware version as text up
 *      to the end of the element
 *   WMM Parameter element: the ACM bit of each access category
 *   199 Operating Mode Notification: channel width and streams
 *   WPS Device Name, Manufacturer, Model Name and Model Number
 *      (attributes 0x1011, 0x1021, 0x1023, 0x1024); probe
 *      responses carry them, beacons often only the name. Never
 *      the serial number.
 */
static uint16_t le16(const uint8_t *p)
{
    return p[0] | (p[1] << 8);
}

static int32_t le32(const uint8_t *p)
{
    return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                     ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}

// Printable ASCII only, cut at the first NUL
static void ridText(char *out, size_t size, const uint8_t *p, int len)
{
    size_t k = 0;

    for (int i = 0; i < len && p[i] && k + 1 < size; i++)
        out[k++] = (p[i] >= 0x20 && p[i] < 0x7F) ? (char)p[i] : '?';

    out[k] = 0;
}

// One 25-octet Remote ID message
void parseRidMessage(const uint8_t *m, struct DroneInfo &d)
{
    switch (m[0] >> 4)
    {
        case 0:     // Basic ID
        {
            uint8_t idType = m[1] >> 4;

            d.uaType = m[1] & 0x0F;

            // Serial number: only the manufacturer code
            if (idType == 1)
                ridText(d.maker, sizeof(d.maker), &m[2], 4);

            break;
        }

        case 1:     // Location/Vector
        {
            d.haveLocation = true;
            d.status       = m[1] >> 4;
            d.heightAgl    = (m[1] & 0x04) != 0;

            d.track = m[2] <= 179
                ? m[2] + ((m[1] & 0x02) ? 180 : 0) : 361;

            if (m[3] == 255)
                d.speed = -1;
            else
                d.speed = (m[1] & 0x01) ? m[3] * 75 + 6375 : m[3] * 25;

            d.vspeed  = (int16_t)((int8_t)m[4]) * 50;
            d.lat     = le32(&m[5]);
            d.lon     = le32(&m[9]);
            d.altBaro = le16(&m[13]);
            d.altGeo  = le16(&m[15]);
            d.height  = le16(&m[17]);

            break;
        }

        case 3:     // Self-ID: free text
            ridText(d.selfId, sizeof(d.selfId), &m[2], 23);
            break;

        case 4:     // System: the pilot's position
            d.haveOperator = (m[1] & 0x03) != 0 || le32(&m[2]) != 0;
            d.opLat        = le32(&m[2]);
            d.opLon        = le32(&m[6]);
            d.opAlt        = le16(&m[18]);

            // Classification type 1 is the EU's: category and class
            d.euClass = ((m[1] >> 2) & 0x07) == 1 ? m[17] : 0;
            break;

        default:    // Authentication, Operator ID: not read
            break;
    }
}

void parseRemoteId(const uint8_t *ies, int ieLength,
                   struct WiFiNetwork &n)
{
    int len = 0;

    const uint8_t *p = findVendorIE(ies, ieLength,
                                    0xFA, 0x0B, 0xBC, 0x0D, len);

    // Message counter, then the pack: header, size, count
    if (p == nullptr || len < 4 || (p[1] >> 4) != 0x0F || p[2] != 25)
        return;

    for (int i = 0; i < p[3] && 4 + (i + 1) * 25 <= len; i++)
        parseRidMessage(&p[4 + i * 25], n.drone);

    n.drone.seen = true;
    n.drone.at   = millis();
}

void parseApExtras(const uint8_t *ies, int ieLength,
                   struct WiFiNetwork &n)
{
    int len = 0;

    const uint8_t *p = findIE(ies, ieLength, 40, len);

    if (p != nullptr && len >= 6)
    {
        n.quietSeen     = true;
        n.quietPeriod   = p[1];
        n.quietDuration = p[2] | (p[3] << 8);
        n.quietOffset   = p[4] | (p[5] << 8);
    }

    p = findIE(ies, ieLength, 66, len);

    if (p != nullptr && len >= 1)
        n.mptInterval = p[0];

    // BSS colour, HE Operation: three octets of parameters, then
    // the colour information (colour, bit 7 disabled)
    p = findExtendedIE(ies, ieLength, 36, len);

    if (p != nullptr && len >= 4)
    {
        n.bssColor    = p[3] & 0x3F;
        n.bssColorOff = (p[3] & 0x80) != 0;

        // HE Operation Parameters: bit 3 TWT required, 15 co-hosted BSS
        n.heOpFlags   = ((p[0] & 0x08) ? 1 : 0) | ((p[1] & 0x80) ? 2 : 0);
    }

    /*
     * BSS Color Change Announcement: countdown, new colour. The AP
     * sends it in every beacon until the switch; a new colour, or
     * the same one after half a minute, is the next change.
     */
    p = findExtendedIE(ies, ieLength, 42, len);

    if (p != nullptr && len >= 2)
    {
        int8_t next = p[1] & 0x3F;

        if (next != n.colorNext || millis() - n.colorChangeAt > 30000)
        {
            if (n.colorChanges < 255)
                n.colorChanges++;
        }

        n.colorNext     = next;
        n.colorChangeAt = millis();
    }

    /*
     * TIM (5): DTIM count, DTIM period, bitmap control (bit 0:
     * group-addressed frames buffered), then the partial virtual
     * bitmap, one bit per station with frames waiting while it
     * dozes. Beacons only; a probe response leaves it as it was.
     */
    p = findIE(ies, ieLength, 5, len);

    if (p != nullptr && len >= 4)
    {
        n.dtimPeriod = p[1];
        n.timMcast   = (p[2] & 0x01) != 0;

        uint8_t waiting = 0;

        for (int i = 3; i < len; i++)
            waiting += __builtin_popcount(p[i]);

        // Bit 0 of the bitmap's first octet is AID 0, the group bit
        if ((p[2] & 0xFE) == 0 && (p[3] & 0x01))
            waiting--;

        n.timWaiting = waiting;
    }

    // How long the AP keeps a silent station associated
    p = findIE(ies, ieLength, 90, len);

    if (p != nullptr && len >= 3)
    {
        n.maxIdle    = le16(p);
        n.maxIdleOpt = p[2];
    }

    // ATIM window of an ad-hoc network: how long its stations stay
    // awake after each beacon to announce buffered frames, 0 when
    // none of them sleeps
    p = findIE(ies, ieLength, 6, len);

    if (p != nullptr && len >= 2)
        n.atimWindow = p[0] | (p[1] << 8);

    p = findIE(ies, ieLength, 67, len);

    if (p != nullptr && len >= 2)
    {
        n.aacLen = len > (int)sizeof(n.aac) ? sizeof(n.aac) : len;

        memcpy(n.aac, p, n.aacLen);
    }

    if (findIE(ies, ieLength, 76, len) != nullptr)
        n.beaconProt = true;

    p = findIE(ies, ieLength, 127, len);

    if (p != nullptr && len >= 11 && (p[10] & 0x10))
        n.beaconProt = true;

    p = findIE(ies, ieLength, 108, len);

    if (p != nullptr)
    {
        n.advProtoCount = 0;

        // Tuples: info, protocol ID; 221 is followed by a vendor
        // element of its own (length, OUI ...)
        for (int i = 0; i + 2 <= len && n.advProtoCount < 4; )
        {
            uint8_t id = p[i + 1];

            n.advProto[n.advProtoCount++] = id;

            i += (id == 221 && i + 2 < len) ? 3 + p[i + 2] : 2;
        }
    }

    p = findIE(ies, ieLength, 133, len);

    if (p != nullptr && len > 10)
        fixedName(p + 10, len - 10 > 16 ? 16 : len - 10,
                  n.apName, sizeof(n.apName));

    p = findVendorIE(ies, ieLength, 0x00, 0x0C, 0x42, 0x00, len);

    for (int i = 2; p != nullptr && i + 2 <= len; )
    {
        uint8_t id = p[i];
        uint8_t l  = p[i + 1];

        if (i + 2 + l > len)
            break;

        if (id == 1 && l > 10)
            fixedName(p + i + 12, l - 10, n.apName, sizeof(n.apName));

        i += 2 + l;
    }

    // Mist (5C:5B:35 type 1): the AP's name as plain text
    p = findVendorIE(ies, ieLength, 0x5C, 0x5B, 0x35, 0x01, len);

    if (p != nullptr && len > 0)
        fixedName(p, len, n.apName, sizeof(n.apName));

    // LANCOM (00:A0:57 type 8): 01 00, then the AP's name as text
    p = findVendorIE(ies, ieLength, 0x00, 0xA0, 0x57, 0x08, len);

    if (p != nullptr && len > 2)
        fixedName(p + 2, len - 2, n.apName, sizeof(n.apName));

    // and type 4: 01 00, then the UUID set on the device
    p = findVendorIE(ies, ieLength, 0x00, 0xA0, 0x57, 0x04, len);

    if (p != nullptr && len >= 18)
        memcpy(n.lancomUuid, p + 2, 16);

    /*
     * 30:FF:83 type 104, an OUI not registered with the IEEE: wireless
     * camera kits (AltoBeam chips, Xiongmai recorders) put plain text
     * into it, padded with zeros. A camera's bridge its link to the
     * recorder, "Signal:063", the recorder "XM722 pairing disable".
     */
    p = findVendorIE(ies, ieLength, 0x30, 0xFF, 0x83, 0x68, len);

    if (p != nullptr && len > 0)
        fixedName(p, len, n.camKit, sizeof(n.camKit));

    // 00:30:1A type 1: the firmware version
    p = findVendorIE(ies, ieLength, 0x00, 0x30, 0x1A, 0x01, len);

    if (p != nullptr && len > 2 && p[0] == 0x01 && p[1] == 0x01)
        fixedName(p + 2, len - 2, n.vendorFirmware,
                  sizeof(n.vendorFirmware));

    // Operating Mode Notification (199): channel width, streams
    p = findIE(ies, ieLength, 199, len);

    if (p != nullptr && len == 1)
        n.omn = p[0];

    // WPS Device Name, attribute 0x1011: what the device calls
    // itself, cars and printers often fill it in
    p = findVendorIE(ies, ieLength, 0x00, 0x50, 0xF2, 0x04, len);

    for (int i = 0; p != nullptr && i + 4 <= len; )
    {
        uint16_t attr = (p[i] << 8) | p[i + 1];
        uint16_t l    = (p[i + 2] << 8) | p[i + 3];

        if (i + 4 + l > len)
            break;

        // Never the serial number (0x1042)
        if (attr == 0x1011 && l > 0)
            fixedName(p + i + 4, l, n.wpsName, sizeof(n.wpsName));
        else if (attr == 0x1021 && l > 0)
            fixedName(p + i + 4, l, n.wpsMaker, sizeof(n.wpsMaker));
        else if (attr == 0x1023 && l > 0)
            fixedName(p + i + 4, l, n.wpsModel, sizeof(n.wpsModel));
        else if (attr == 0x1024 && l > 0)
            fixedName(p + i + 4, l, n.wpsModelNo, sizeof(n.wpsModelNo));
        else if (attr == 0x1054 && l == 8)
            primaryDeviceType(p + i + 4, n);

        i += 4 + l;
    }

    // WMM Parameter element: subtype 1, then version, QoS info,
    // a reserved octet and four AC records of 4 octets
    p = findVendorIE(ies, ieLength, 0x00, 0x50, 0xF2, 0x02, len);

    if (p != nullptr && len >= 20 && p[0] == 1)
    {
        n.acmMask = 0;

        for (int i = 0; i < 4; i++)
        {
            uint8_t aci = p[4 + i * 4];

            if (aci & 0x10)
                n.acmMask |= 1 << ((aci >> 5) & 3);
        }
    }
}

/*
 * Wi-Fi Alliance elements and HP's, each kept from the last frame
 * that had it:
 *
 *   50:6F:9A 0x29 / 0x2A RSNE Override (2): an RSN element for
 *      clients that understand it, Wi-Fi 7 ones; the AP can offer
 *      them WPA3 with GCMP-256 while older ones see WPA2/WPA3
 *   50:6F:9A 0x2B RSNXE Override: bit 5, SAE hash-to-element
 *   50:6F:9A 0x23 Wi-Fi Alliance Capabilities: a length octet, then
 *      capability octets; the first holds the QoS Management bits
 *      (WFA_CAPA_*), stations send it as well
 *   50:6F:9A 0x2C RSN Selection, from stations only: one octet, 0
 *      RSNE, 1 RSNE Override, 2 RSNE Override 2, see
 *      parseAssocRequest()
 *   50:6F:9A 0x09 P2P: attributes of ID, 2-octet length (little
 *      endian), value; 2 is the capability (device, group), 13
 *      the device info with the name as WPS attribute 0x1011
 *   50:6F:9A 0x0A Wi-Fi Display: subelements of ID, 2-octet length
 *      (big endian), value; 0 is the device information
 *   50:6F:9A 0x16 MBO-OCE: attributes of ID, length, value; 1 AP
 *      capability, 2 non-preferred channels, 4 association
 *      disallowed with its reason, 101 OCE capability
 *   50:6F:9A 0x13 NAN (Wi-Fi Aware), in the beacons of a cluster:
 *      attributes of ID, 2-octet length (little endian), value;
 *      0 Master Indication (master preference, random factor),
 *      1 Cluster: the anchor master rank, 8 octets little endian
 *      with preference and random factor in the top two, then the
 *      hop count to the anchor master
 *   08:00:09 type 0, HP printers: 5 octets, then type / length /
 *      value; 3 the model name, 4 the model number. 5 is the
 *      serial number and deliberately left alone.
 */
// Wi-Fi QoS Management, first capability octet of 50:6F:9A 0x23
#define WFA_CAPA_DSCP_POLICY     0x01
#define WFA_CAPA_UNSOLICITED     0x02    // unsolicited DSCP policy
#define WFA_CAPA_SCS_NON_EHT     0x04    // SCS traffic description

// The capability octet of a Wi-Fi Alliance Capabilities element
bool wfaCapaOf(const uint8_t *ies, int ieLength, uint8_t &capa)
{
    int len = 0;

    const uint8_t *p = findVendorIE(ies, ieLength,
                                    0x50, 0x6F, 0x9A, 0x23, len);

    if (p == nullptr || len < 1)
        return false;

    capa = (p[0] >= 1 && len >= 2) ? p[1] : 0;

    return true;
}

/*
 * The RSNE Override (0x29) and Override 2 (0x2A) together: an AP can
 * offer WPA3 with GCMP-256 in one and SAE-EXT-KEY in the other, as
 * OpenWrt does for multi-link, and a client takes either. AKMs and
 * ciphers of both, each once; PMF the stricter. False with neither.
 */
void mergeNames(char *dst, size_t size, const char *src)
{
    char item[24];

    for (const char *q = src; *q; )
    {
        const char *e = strstr(q, ", ");
        size_t l = e ? (size_t)(e - q) : strlen(q);

        if (l > 0 && l < sizeof(item))
        {
            memcpy(item, q, l);
            item[l] = 0;

            // Already there as a whole name
            bool have = false;

            for (const char *d = dst; *d && !have; )
            {
                const char *de = strstr(d, ", ");
                size_t dl = de ? (size_t)(de - d) : strlen(d);

                have = dl == l && memcmp(d, item, l) == 0;
                d += dl + (de ? 2 : 0);
            }

            if (!have)
            {
                if (dst[0])
                    strlcat(dst, ", ", size);

                strlcat(dst, item, size);
            }
        }

        q += l + (e ? 2 : 0);
    }
}

bool rsnOverridesOf(const uint8_t *ies, int ieLength, char *akm,
                    size_t akmSize, char *pairwise, size_t pairSize,
                    uint8_t &pmf)
{
    bool seen = false;

    akm[0] = pairwise[0] = 0;
    pmf    = 0;

    for (uint8_t type = 0x29; type <= 0x2A; type++)
    {
        int len = 0;

        const uint8_t *p = findVendorIE(ies, ieLength, 0x50, 0x6F, 0x9A,
                                        type, len);

        if (p == nullptr)
            continue;

        RsnInfo o;

        memset(&o, 0, sizeof(o));
        parseSuites(p, len, false, o);

        mergeNames(akm, akmSize, o.akm);
        mergeNames(pairwise, pairSize, o.pairwise);

        uint8_t m = o.mfpRequired ? 2 : (o.mfpCapable ? 1 : 0);

        if (m > pmf)
            pmf = m;

        seen = true;
    }

    return seen;
}

void parseWfaElements(const uint8_t *ies, int ieLength,
                      struct WiFiNetwork &n)
{
    int len = 0;

    if (wfaCapaOf(ies, ieLength, n.wfaCapa))
        n.wfaCapaSeen = true;

    const uint8_t *p;

    {
        char    akm[sizeof(n.ovrAkm)], pairwise[sizeof(n.ovrPairwise)];
        uint8_t pmf = 0;

        if (rsnOverridesOf(ies, ieLength, akm, sizeof(akm), pairwise,
                           sizeof(pairwise), pmf))
        {
            n.ovrSeen = true;
            n.ovrPmf  = pmf;

            strlcpy(n.ovrAkm, akm, sizeof(n.ovrAkm));
            strlcpy(n.ovrPairwise, pairwise, sizeof(n.ovrPairwise));
        }
    }

    p = findVendorIE(ies, ieLength, 0x50, 0x6F, 0x9A, 0x2B, len);

    if (p != nullptr && len >= 1)
        n.ovrH2E = (p[0] & 0x20) != 0;

    p = findVendorIE(ies, ieLength, 0x50, 0x6F, 0x9A, 0x09, len);

    for (int i = 0; p != nullptr && i + 3 <= len; )
    {
        uint8_t  id = p[i];
        uint16_t l  = p[i + 1] | (p[i + 2] << 8);

        if (i + 3 + l > len)
            break;

        const uint8_t *v = p + i + 3;

        n.p2pSeen = true;

        if (id == 2 && l >= 2)
        {
            n.p2pDevCap   = v[0];
            n.p2pGroupCap = v[1];
        }

        // Device info: address, config methods, primary type,
        // secondary types, then the WPS device name attribute
        if (id == 13 && l >= 17)
        {
            primaryDeviceType(v + 8, n);

            int at = 17 + v[16] * 8;

            if (at + 4 <= l && v[at] == 0x10 && v[at + 1] == 0x11)
            {
                int nl = (v[at + 2] << 8) | v[at + 3];

                if (at + 4 + nl <= l)
                    fixedName(v + at + 4, nl, n.p2pName,
                              sizeof(n.p2pName));
            }
        }

        i += 3 + l;
    }

    p = findVendorIE(ies, ieLength, 0x50, 0x6F, 0x9A, 0x13, len);

    for (int i = 0; p != nullptr && i + 3 <= len; )
    {
        uint8_t  id = p[i];
        uint16_t l  = p[i + 1] | (p[i + 2] << 8);

        if (i + 3 + l > len)
            break;

        const uint8_t *v = p + i + 3;

        if (id == 0 && l >= 2)
        {
            n.nanPref   = v[0];
            n.nanRandom = v[1];
        }

        if (id == 1 && l >= 9)
        {
            n.nanAmPref   = v[7];
            n.nanAmRandom = v[6];
            n.nanHops     = v[8];
        }

        i += 3 + l;
    }

    p = findVendorIE(ies, ieLength, 0x50, 0x6F, 0x9A, 0x0A, len);

    for (int i = 0; p != nullptr && i + 3 <= len; )
    {
        uint8_t  id = p[i];
        uint16_t l  = (p[i + 1] << 8) | p[i + 2];

        if (i + 3 + l > len)
            break;

        if (id == 0 && l >= 2)
            n.wfdInfo = (p[i + 3] << 8) | p[i + 4];

        i += 3 + l;
    }

    p = findVendorIE(ies, ieLength, 0x50, 0x6F, 0x9A, 0x16, len);

    if (p != nullptr)
    {
        n.mboSeen       = true;
        n.mboDisallowed = 0;
        n.mboNpcCount   = 0;

        for (int i = 0; i + 2 <= len; )
        {
            uint8_t id = p[i];
            uint8_t l  = p[i + 1];

            if (i + 2 + l > len)
                break;

            const uint8_t *v = p + i + 2;

            if (id == 1 && l >= 1)
                n.mboCap = v[0];

            // Operating class, channels, preference, reason
            if (id == 2 && l >= 3)
                for (int c = 1; c < l - 2 && n.mboNpcCount < 8; c++)
                    n.mboNpc[n.mboNpcCount++] = v[c];

            if (id == 4)
                n.mboDisallowed = l >= 1 && v[0] ? v[0] : 1;

            if (id == 101 && l >= 1)
                n.oceCtrl = v[0];

            i += 2 + l;
        }
    }

    p = findVendorIE(ies, ieLength, 0x08, 0x00, 0x09, 0x00, len);

    for (int i = 5; p != nullptr && i + 2 <= len; )
    {
        uint8_t t = p[i];
        uint8_t l = p[i + 1];

        if (i + 2 + l > len)
            break;

        if (t == 3)
            fixedName(p + i + 2, l, n.hpModel, sizeof(n.hpModel));
        else if (t == 4)
            fixedName(p + i + 2, l, n.hpNumber, sizeof(n.hpNumber));

        i += 2 + l;
    }
}

/*
 * Transmit Power Envelope (195). An AP can send several, one per
 * interpretation: local or regulatory-client limit, as EIRP per
 * channel width or as PSD per 20 MHz subchannel. The first octet
 * says which, the rest are signed values in 0.5 dB steps. Kept
 * raw, two at most, and decoded in the browser.
 */
void tpeOf(const uint8_t *ies, int ieLength,
           uint8_t tpe[2][12], uint8_t tpeLen[2])
{
    tpeLen[0] = tpeLen[1] = 0;

    uint8_t found = 0;

    for (int pos = 0; pos + 2 <= ieLength && found < 2; )
    {
        uint8_t id = ies[pos];
        uint8_t l  = ies[pos + 1];

        if (pos + 2 + l > ieLength)
            break;

        if (id == 195 && l >= 2)
        {
            uint8_t n = l > 12 ? 12 : l;

            memcpy(tpe[found], &ies[pos + 2], n);

            tpeLen[found++] = n;
        }

        pos += 2 + l;
    }
}

/*
 * Where an unknown element was seen, one bit per frame type; an
 * AP can put an element into its probe responses but not into
 * its beacons, which is easy to miss when looking at one frame.
 */
#define SEEN_BEACON     0x01
#define SEEN_PROBE_RESP 0x02
#define SEEN_PROBE_REQ  0x04
#define SEEN_ASSOC_REQ  0x08

/*
 * OUI and type of all vendor elements (221) in a frame, added to
 * the ones already known for the BSS. They tell who made the AP or
 * its Wi-Fi chip: Broadcom drivers send 00:90:4C (Epigram) or
 * 00:10:18, Qualcomm ones 8C:FD:F0 or 00:03:7F (Atheros),
 * MediaTek 00:0C:43 or 00:0C:E7, and many vendors add their own.
 * Microsoft (WPA, WMM, WPS), the Wi-Fi Alliance and the IEEE
 * are left out, everybody sends those.
 */
void collectVendorOuis(const uint8_t *ies, int ieLength,
                       uint8_t (*list)[4], uint8_t &count)
{
    for (int pos = 0; pos + 2 <= ieLength; )
    {
        uint8_t id = ies[pos];
        uint8_t l  = ies[pos + 1];

        if (pos + 2 + l > ieLength)
            break;

        const uint8_t *o = &ies[pos + 2];

        pos += 2 + l;

        if (id != 221 || l < 4)
            continue;

        uint32_t oui = ((uint32_t)o[0] << 16) |
                       ((uint32_t)o[1] << 8) | o[2];

        if (oui == 0x0050F2 || oui == 0x506F9A || oui == 0x000FAC)
            continue;

        bool seen = false;

        for (uint8_t i = 0; i < count && !seen; i++)
            seen = memcmp(list[i], o, 4) == 0;

        if (!seen && count < VENDOR_OUI_MAX)
            memcpy(list[count++], o, 4);
    }
}

/*
 * The element numbers the sensor does not know, for the notes.
 * The same as catalogScan() takes: nothing from an element of a
 * length the standard does not allow on, nor from a second SSID,
 * so a frame broken past the FCS check (a "beacon" with element
 * 24) makes no note the catalog has no row for.
 */
void collectUnknownElements(const uint8_t *ies, int ieLength,
                            uint16_t *list, uint8_t *seenIn,
                            uint8_t &count, uint8_t capacity,
                            uint8_t source)
{
    bool ssidSeen = false;

    for (int pos = 0; pos + 2 <= ieLength; )
    {
        uint8_t id = ies[pos];
        uint8_t l  = ies[pos + 1];

        if (pos + 2 + l > ieLength)
            break;

        if (!elementLengthOk(id, l) || (id == 0 && ssidSeen))
            break;

        if (id == 0)
            ssidSeen = true;

        int ext = (id == 255 && l >= 1) ? ies[pos + 2] : -1;

        // Fragment elements (242) continue the one before them
        if (id != 242 && !(id == 255 && ext < 0) &&
            !elementKnown(id, ext))
        {
            uint16_t code = (id == 255) ? 256 + ext : id;

            bool seen = false;

            for (uint8_t i = 0; i < count; i++)
            {
                if (list[i] == code)
                {
                    seenIn[i] |= source;
                    seen = true;
                }
            }

            if (!seen && count < capacity)
            {
                seenIn[count] = source;
                list[count++] = code;
            }
        }

        pos += 2 + l;
    }
}

/*
 * Basic Multi-Link element. Layout after the extension ID:
 *
 *   Multi-Link Control (2): type in bits 0-2, 0 = Basic; then
 *     which Common Info fields are present: bit 4 Link ID Info,
 *     5 BSS Parameters Change Count, 6 Medium Sync Delay, 7 EML
 *     Capabilities, 8 MLD Capabilities, 9 AP MLD ID, 10 Extended
 *     MLD Capabilities
 *   Common Info: its own length (counting itself), the MLD MAC
 *     address, then the present fields in that order
 *   Link Info: subelements, ID 0 = Per-STA Profile, whose STA
 *     Control starts with the link ID in bits 0-3
 *
 * A station's request names the other links it wants in per-STA
 * profiles, each with the full capability set of that link, so
 * the element easily outgrows the 255 octets an element can
 * hold. It then continues in Fragment elements (242) directly
 * after it, which are put back together here before the
 * profiles are read. A profile can itself be fragmented with
 * subelement 254; those are skipped like any other subelement.
 *
 * A frame may carry more than one Multi-Link element, a beacon
 * for instance a Reconfiguration one next to the Basic one while
 * a link is being removed, so the Basic one is searched for
 * rather than the first.
 */
void addSuppChannels(const uint8_t *ies, int ieLength, bool sixGhz,
                     uint64_t &plan, uint64_t &six);
uint16_t capsWidthOf(const uint8_t *ies, int ieLength, bool band24);
bool caps8080Of(const uint8_t *ies, int ieLength, bool band24);
uint8_t profileBandOf(const uint8_t *ies, int ieLength);

bool parseMultiLink(const uint8_t *ies, int ieLength,
                    struct MloInfo &m)
{
    memset(&m, 0, sizeof(m));

    m.linkId = -1;

    // Reassembled element, without element and extension ID.
    // Only called from loop(), so one static buffer will do.
    static uint8_t buffer[1024];

    int len = 0;

    const uint8_t *p = nullptr;

    for (int pos = 0; pos + 2 <= ieLength; )
    {
        uint8_t id = ies[pos];
        uint8_t l  = ies[pos + 1];

        if (pos + 2 + l > ieLength)
            break;

        // Basic type in bits 0-2 of the first control octet; the
        // probe request, reconfiguration and other variants carry
        // no MLD description worth showing
        if (id == 255 && l >= 3 && ies[pos + 2] == 107 &&
            (ies[pos + 3] & 0x07) == 0)
        {
            len = l - 1;

            memcpy(buffer, &ies[pos + 3], len);

            // A full element continues in the Fragment elements
            // right behind it, each full one in the next
            int next = pos + 2 + l;

            while (l == 255 && next + 2 <= ieLength && ies[next] == 242)
            {
                l = ies[next + 1];

                if (next + 2 + l > ieLength ||
                    len + l > (int)sizeof(buffer))
                    break;

                memcpy(&buffer[len], &ies[next + 2], l);

                len  += l;
                next += 2 + l;
            }

            p = buffer;

            break;
        }

        pos += 2 + l;
    }

    if (p == nullptr || len < 3)
        return false;

    uint16_t control = p[0] | (p[1] << 8);

    const uint8_t *info    = &p[2];
    int            infoLen = info[0];

    if (infoLen < 7 || 2 + infoLen > len)
        return false;

    memcpy(m.mldMac, &info[1], 6);

    int pos = 7;

    if (control & 0x0010)
    {
        if (pos < infoLen)
            m.linkId = info[pos] & 0x0F;

        pos += 1;
    }

    if (control & 0x0020)
        pos += 1;

    if (control & 0x0040)
        pos += 2;

    if (control & 0x0080)
    {
        if (pos + 2 <= infoLen)
        {
            m.eml     = info[pos] | (info[pos + 1] << 8);
            m.fields |= 1;
        }

        pos += 2;
    }

    if (control & 0x0100)
    {
        if (pos + 2 <= infoLen)
        {
            m.mldCaps  = info[pos] | (info[pos + 1] << 8);
            m.fields  |= 2;
        }

        pos += 2;
    }

    const uint8_t *sub    = &info[infoLen];
    int            subLen = len - 2 - infoLen;

    for (int q = 0; q + 2 <= subLen; )
    {
        uint8_t id = sub[q];
        uint8_t l  = sub[q + 1];

        if (q + 2 + l > subLen)
            break;

        if (id == 0 && l >= 2)
        {
            uint16_t staControl = sub[q + 2] | (sub[q + 3] << 8);
            uint8_t  link       = staControl & 0x0F;

            m.linkMask |= 1 << link;

            // STA Control bit 5: the STA Info, which follows the
            // control and its own length octet, starts with the
            // station address of that link
            if ((staControl & 0x0020) && l >= 2 + 1 + 6 &&
                m.linkCount < 3)
            {
                m.linkIds[m.linkCount] = link;

                memcpy(m.linkMacs[m.linkCount], &sub[q + 5], 6);

                m.linkCount++;
            }

            /*
             * The profile proper: STA Control, STA Info with its
             * own length, the Capability Information field, then
             * the elements of that link. A long profile continues
             * in subelement Fragments (254) right after it.
             */
            uint8_t profile[600];
            int     profLen = l;

            memcpy(profile, &sub[q + 2], l);

            int next = q + 2 + l;
            int last = l;

            while (last == 255 && next + 2 <= subLen && sub[next] == 254)
            {
                last = sub[next + 1];

                if (next + 2 + last > subLen ||
                    profLen + last > (int)sizeof(profile))
                    break;

                memcpy(&profile[profLen], &sub[next + 2], last);

                profLen += last;
                next    += 2 + last;
            }

            int start = 2 + (profLen > 2 ? profile[2] : 0) + 2;

            /*
             * STA Info after its length octet, each field only when
             * its STA Control bit is set: MAC address (bit 5, 6
             * octets), Beacon Interval (6, 2), TSF Offset (7, 8),
             * DTIM Info (8, 2), then the NSTR Indication Bitmap
             * (bit 9; one octet, two with bit 10).
             */
            uint16_t nstr = 0;

            if (profLen > 2 && (staControl & 0x0200))
            {
                int f   = 3;
                int end = 3 + profile[2] - 1;

                if (staControl & 0x0020) f += 6;
                if (staControl & 0x0040) f += 2;
                if (staControl & 0x0080) f += 8;
                if (staControl & 0x0100) f += 2;

                bool wide = staControl & 0x0400;

                if (f + (wide ? 2 : 1) <= end && end <= profLen)
                    nstr = profile[f] | (wide ? profile[f + 1] << 8 : 0);
            }

            uint8_t  band  = 0;
            uint16_t width = 0;

            if (start < profLen)
            {
                const uint8_t *pies = &profile[start];
                int            plen = profLen - start;

                band = profileBandOf(pies, plen);

                // The profile's own capability elements; a profile
                // that inherits them sends none
                int elen = 0;

                if (findIE(pies, plen, 45, elen) != nullptr ||
                    findExtendedIE(pies, plen, 35, elen) != nullptr)
                    width = capsWidthOf(pies, plen, band == 2);

                addSuppChannels(pies, plen, band == 6,
                                m.profChans, m.profChans6);
            }

            if (m.profCount < 3)
            {
                m.profLink[m.profCount]  = link;
                m.profBand[m.profCount]  = band;
                m.profWidth[m.profCount] = width;
                m.profNstr[m.profCount]  = nstr;

                m.profCount++;
            }
        }

        q += 2 + l;
    }

    m.present = true;

    return true;
}

/*
 * The band of a per-STA profile, in GHz: 6 when it carries HE 6 GHz
 * Band Capabilities (ext 59), else from the current class of its
 * Supported Operating Classes (59), else from the first channel of
 * its Supported Channels (36). 0 when it says nothing of the kind.
 */
uint8_t profileBandOf(const uint8_t *ies, int ieLength)
{
    int len = 0;

    if (findExtendedIE(ies, ieLength, 59, len) != nullptr)
        return 6;

    const uint8_t *oc = findIE(ies, ieLength, 59, len);

    if (oc != nullptr && len >= 1)
    {
        uint8_t c = oc[0];

        if (c >= 81 && c <= 84)
            return 2;

        if (c >= 115 && c <= 130)
            return 5;

        if (c >= 131 && c <= 137)
            return 6;
    }

    const uint8_t *sc = findIE(ies, ieLength, 36, len);

    if (sc != nullptr && len >= 2)
        return sc[0] <= 14 ? 2 : 5;

    return 0;
}

/*
 * 80+80 offered beside 160 MHz: VHT Supported Channel Width Set 2, or
 * the HE channel width set's bit for 160/80+80 in 5 GHz (bit 3). Not
 * in 2.4 GHz.
 */
bool caps8080Of(const uint8_t *ies, int ieLength, bool band24)
{
    int len = 0;

    if (band24)
        return false;

    const uint8_t *vht = findIE(ies, ieLength, 191, len);

    if (vht != nullptr && len >= 12 && ((vht[0] >> 2) & 0x03) == 2)
        return true;

    const uint8_t *he = findExtendedIE(ies, ieLength, 35, len);

    return he != nullptr && len >= 19 && (((he[6] >> 1) & 0x3F) & 0x08);
}

/*
 * The widest channel the capability elements allow: HT 40 MHz,
 * VHT 80/160 (not in 2.4 GHz, where VHT is a vendor extension),
 * HE by its channel width set, EHT 320 MHz, which exists in 6 GHz
 * only.
 */
uint16_t capsWidthOf(const uint8_t *ies, int ieLength, bool band24)
{
    int len = 0;

    uint16_t width = 20;

    const uint8_t *ht = findIE(ies, ieLength, 45, len);

    if (ht != nullptr && len >= 26)
        width = ((ht[0] | (ht[1] << 8)) & 0x0002) ? 40 : 20;

    const uint8_t *vht = findIE(ies, ieLength, 191, len);

    if (vht != nullptr && len >= 12 && !band24)
    {
        uint8_t widthSet = (vht[0] >> 2) & 0x03;

        width = (widthSet == 1 || widthSet == 2) ? 160 : 80;
    }

    const uint8_t *he = findExtendedIE(ies, ieLength, 35, len);

    if (he != nullptr && len >= 19)
    {
        uint8_t widthSet = (he[6] >> 1) & 0x3F;

        if (widthSet & 0x0C)
            width = 160;
        else if (widthSet & 0x02)
            width = 80;
        else if (widthSet & 0x01)
            width = 40;
    }

    const uint8_t *eht = findExtendedIE(ies, ieLength, 108, len);

    if (eht != nullptr && len >= 3 && (eht[2] & 0x02))
        width = 320;

    return width;
}

/*
 * Where a station can operate, from two elements of its requests:
 *
 *   Supported Operating Classes (59), in probe and association
 *   requests: the current class, then every class it supports.
 *   The global classes 81..137 cover 2.4 GHz (81-84), 5 GHz by
 *   UNII range and width (115-130) and 6 GHz (131-137); they are
 *   kept as a bitmask, bit = class - 81. The list ends at a 130
 *   or 0, which open the optional extension and duple sequences.
 *
 *   Supported Channels (36), only in association requests and
 *   only from stations that do spectrum management: pairs of
 *   first channel and count. Kept as a bitmask over the sensor's
 *   own channel plan, which the browser knows as well.
 */
uint64_t opClassesOf(const uint8_t *ies, int ieLength)
{
    int len = 0;

    const uint8_t *p = findIE(ies, ieLength, 59, len);

    uint64_t mask = 0;

    for (int i = 0; p != nullptr && i < len; i++)
    {
        uint8_t c = p[i];

        // The list proper starts after the current class
        if (i > 0 && (c == 130 || c == 0))
            break;

        if (c >= 81 && c <= 137)
            mask |= 1ULL << (c - 81);
    }

    return mask;
}

uint64_t planBitOf(uint8_t channel)
{
    for (uint16_t i = 0; i < NUM_CHANNELS_24; i++)
        if (channels24[i] == channel)
            return 1ULL << i;

    for (uint16_t i = 0; i < NUM_CHANNELS_5; i++)
        if (channels5[i] == channel)
            return 1ULL << (NUM_CHANNELS_24 + i);

    return 0;
}

/*
 * The pairs of first channel and count. The numbers alone do not
 * say the band: 6 GHz counts from 1 again, in steps of four. So
 * the caller says whether the element belongs to a 6 GHz link;
 * otherwise channels up to 14 are 2.4 GHz in steps of one and the
 * rest 5 GHz in steps of four.
 */
void addSuppChannels(const uint8_t *ies, int ieLength, bool sixGhz,
                     uint64_t &plan, uint64_t &six)
{
    int len = 0;

    const uint8_t *p = findIE(ies, ieLength, 36, len);

    for (int i = 0; p != nullptr && i + 1 < len; i += 2)
    {
        uint8_t first = p[i];
        uint8_t count = p[i + 1];

        for (uint8_t k = 0; k < count && k < 64; k++)
        {
            if (sixGhz)
            {
                int ch = first + k * 4;

                if (ch >= 1 && ch <= 233)
                    six |= 1ULL << ((ch - 1) / 4);
            }
            else
            {
                uint8_t step = (first <= 14) ? 1 : 4;

                plan |= planBitOf(first + k * step);
            }
        }
    }
}

// The request's own element; the sensor only hears 2.4 and 5 GHz
uint64_t suppChannelsOf(const uint8_t *ies, int ieLength)
{
    uint64_t plan = 0;
    uint64_t six  = 0;

    addSuppChannels(ies, ieLength, false, plan, six);

    return plan;
}

/*
 * Spatial streams a device can receive, the highest of what its
 * capability elements say. Used for stations and APs alike.
 *
 *   HT  (45):        Rx MCS bitmask from octet 3, one octet per
 *                    stream, any bit set = stream supported
 *   VHT (191):       Rx MCS map, 2 bits per stream, 3 = none;
 *                    only in 5 GHz, in 2.4 GHz it is a vendor
 *                    extension
 *   HE  (ext 35):    Rx MCS map for up to 80 MHz, same coding,
 *                    after 6 octets MAC and 11 octets PHY caps
 *   EHT (ext 108):   Supported EHT-MCS And NSS Set after 2 octets
 *                    MAC and 9 octets PHY caps; one octet per MCS
 *                    group, Rx max NSS in the low nibble
 *
 * An EHT device sends HE capabilities as well, normally with the
 * same count, but EHT allows up to 16 streams where HE stops at
 * 8, so it is read too. 0 means none of them was present.
 */
uint8_t maxStreamsOf(const uint8_t *ies, int ieLength,
                     uint8_t channel)
{
    int len = 0;

    uint8_t streams = 0;

    const uint8_t *ht = findIE(ies, ieLength, 45, len);

    if (ht != nullptr && len >= 26)
    {
        for (int i = 0; i < 4; i++)
        {
            if (ht[3 + i] != 0 && i + 1 > streams)
                streams = i + 1;
        }
    }

    const uint8_t *maps[2] = { nullptr, nullptr };

    const uint8_t *vht = findIE(ies, ieLength, 191, len);

    if (vht != nullptr && len >= 12 && channel > 14)
        maps[0] = &vht[4];

    const uint8_t *he = findExtendedIE(ies, ieLength, 35, len);

    if (he != nullptr && len >= 19)
        maps[1] = &he[17];

    for (int m = 0; m < 2; m++)
    {
        if (maps[m] == nullptr)
            continue;

        uint16_t rxMap = maps[m][0] | (maps[m][1] << 8);

        for (int i = 0; i < 8; i++)
        {
            if (((rxMap >> (i * 2)) & 0x03) != 3 && i + 1 > streams)
                streams = i + 1;
        }
    }

    /*
     * The first three octets of the set: for a 20 MHz-only
     * station MCS 0-7, 8-9 and 10-11, for everything else MCS
     * 0-9, 10-11 and 12-13 up to 80 MHz. Either way each octet
     * starts with the Rx max NSS, and the highest of them is the
     * stream count.
     */
    const uint8_t *eht = findExtendedIE(ies, ieLength, 108, len);

    if (eht != nullptr && len >= 2 + 9 + 3)
    {
        for (int i = 0; i < 3; i++)
        {
            uint8_t n = eht[11 + i] & 0x0F;

            if (n > streams)
                streams = n;
        }
    }

    return streams;
}

/*
 * VHT Capabilities (191) beyond streams and width: four octets of
 * capabilities, then the Rx and Tx VHT-MCS maps. As bits: 1 MU
 * beamformer (bit 19, Wave 2: downlink MU-MIMO), 2 MU beamformee
 * (20), 4 SU beamformer (11), 8 SU beamformee (12), 16 256-QAM (a
 * stream with MCS 0-8 or 0-9 in the Rx map), 32 the map was read, so
 * without 16 no 256-QAM. 0 for 2.4 GHz, where VHT is a vendor
 * extension (vht24).
 */
uint8_t vhtCapsOf(const uint8_t *ies, int ieLength, bool band24)
{
    int len = 0;

    const uint8_t *p = findIE(ies, ieLength, 191, len);

    if (p == nullptr || len < 12 || band24)
        return 0;

    uint32_t info = p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
    uint16_t map  = p[4] | (p[5] << 8);
    uint8_t  caps = 32;

    if (info & (1UL << 19)) caps |= 1;
    if (info & (1UL << 20)) caps |= 2;
    if (info & (1UL << 11)) caps |= 4;
    if (info & (1UL << 12)) caps |= 8;

    for (int i = 0; i < 8; i++)
    {
        uint8_t m = (map >> (i * 2)) & 0x03;

        if (m == 1 || m == 2)
            caps |= 16;
    }

    return caps;
}

/*
 * HE Capabilities (ext 35) beyond streams and width: six octets of
 * MAC, eleven of PHY capabilities, then the HE-MCS maps. As bits:
 * 1 1024-QAM (a stream with MCS 0-11 in the Rx map up to 80 MHz),
 * 2 UL MU-MIMO (PHY bit 22 full or 23 partial bandwidth), 4 MU
 * beamformer (PHY bit 33), 8 SU beamformer (31), 16 DCM (PHY bits
 * 24-25, the highest constellation for DCM, not 0).
 */
uint8_t heCapsOf(const uint8_t *ies, int ieLength)
{
    int len = 0;

    const uint8_t *p = findExtendedIE(ies, ieLength, 35, len);

    if (p == nullptr || len < 6 + 11)
        return 0;

    const uint8_t *phy = p + 6;
    uint8_t caps = 0;

    if (phy[2] & 0xC0) caps |= 2;
    if (phy[4] & 0x02) caps |= 4;
    if (phy[3] & 0x80) caps |= 8;
    if (phy[3] & 0x03) caps |= 16;

    if (len >= 6 + 11 + 2)
    {
        uint16_t map = p[17] | (p[18] << 8);

        for (int i = 0; i < 8; i++)
            if (((map >> (i * 2)) & 0x03) == 2)
                caps |= 1;
    }

    return caps;
}

/*
 * TWT support from the HE MAC Capabilities (ext 35): bit 1 TWT
 * requester, 2 TWT responder, 20 broadcast TWT. As 1 requester,
 * 2 responder, 4 broadcast.
 */
uint8_t heTwtOf(const uint8_t *ies, int ieLength)
{
    int len = 0;

    const uint8_t *p = findExtendedIE(ies, ieLength, 35, len);

    if (p == nullptr || len < 6)
        return 0;

    return ((p[0] & 0x02) ? 1 : 0) | ((p[0] & 0x04) ? 2 : 0) |
           ((p[2] & 0x10) ? 4 : 0);
}

/*
 * Broadcast TWT schedules of the TWT element (216): control (bits
 * 2-3 the negotiation type, 2 or 3 broadcast; bit 5 the wake
 * duration unit, 256 µs or 1 TU; bit 6 a link ID bitmap of two
 * octets follows), then parameter sets: request type (bit 5 last
 * set, bits 7-9 the recommendation, 4 restricted TWT, bits 10-14 the
 * wake interval exponent), target wake time (2), nominal wake
 * duration (1), wake interval mantissa (2), broadcast TWT info
 * (bits 3-7 the ID, bit 0 restricted TWT traffic info of three
 * octets follows). Up to max schedules; their count.
 */
uint8_t broadcastTwtOf(const uint8_t *ies, int ieLength, uint8_t *ids,
                       uint8_t *recs, uint32_t *intervals,
                       uint32_t *durations, uint8_t max)
{
    int len = 0;

    const uint8_t *p = findIE(ies, ieLength, 216, len);

    if (p == nullptr || len < 1)
        return 0;

    uint8_t ctrl = p[0];
    uint8_t type = (ctrl >> 2) & 0x03;

    if (type != 2 && type != 3)
        return 0;

    uint32_t unit = (ctrl & 0x20) ? 1024 : 256;
    int      at   = 1 + ((ctrl & 0x40) ? 2 : 0);
    uint8_t  n    = 0;

    while (at + 9 <= len && n < max)
    {
        uint16_t req  = p[at] | (p[at + 1] << 8);
        uint16_t mant = p[at + 5] | (p[at + 6] << 8);
        uint16_t info = p[at + 7] | (p[at + 8] << 8);

        ids[n]       = (info >> 3) & 0x1F;
        recs[n]      = (req >> 7) & 0x07;
        intervals[n] = (uint32_t)mant << ((req >> 10) & 0x1F);
        durations[n] = p[at + 4] * unit;
        n++;

        at += 9 + ((info & 0x01) ? 3 : 0);

        if (req & 0x20)
            break;
    }

    return n;
}

/*
 * The Reconfiguration variant of the Multi-Link element (type 2):
 * an AP MLD announcing that it removes affiliated APs. Control, the
 * common info (its length first), then Per-STA Profiles (subelement
 * 0): STA Control with the link ID in bits 0-3, MAC address present
 * bit 5, AP Removal Timer present bit 6, the operation type in bits
 * 7-10 (0 AP removal), then STA Info, its length first, the MAC if
 * present and the timer in TBTTs. False without such an element.
 */
bool mlRemovalOf(const uint8_t *ies, int ieLength, uint16_t &links,
                 uint16_t &tbtts)
{
    links = 0;
    tbtts = 0;

    for (int pos = 0; pos + 2 <= ieLength; )
    {
        uint8_t id = ies[pos];
        uint8_t l  = ies[pos + 1];

        if (pos + 2 + l > ieLength)
            break;

        const uint8_t *e = ies + pos + 2;

        pos += 2 + l;

        if (id != 255 || l < 4 || e[0] != 107 || (e[1] & 0x07) != 2)
            continue;

        int at = 3 + e[3];          // past control and common info

        while (at + 2 <= l)
        {
            uint8_t sid = e[at];
            uint8_t sl  = e[at + 1];

            if (at + 2 + sl > l)
                break;

            const uint8_t *s = e + at + 2;

            at += 2 + sl;

            if (sid != 0 || sl < 3)
                continue;

            uint16_t ctrl = s[0] | (s[1] << 8);
            uint8_t  type = (ctrl >> 7) & 0x0F;

            if (type != 0 || !(ctrl & 0x40))
                continue;

            int t = 3 + ((ctrl & 0x20) ? 6 : 0);    // info length, MAC

            if (t + 2 > sl)
                continue;

            uint16_t left = s[t] | (s[t + 1] << 8);

            links |= 1 << (ctrl & 0x0F);

            if (tbtts == 0 || left < tbtts)
                tbtts = left;
        }
    }

    return links != 0;
}

/*
 * EHT Capabilities (ext 108) beyond the streams: the MAC capabilities
 * (bit 0 EPCS priority access, 4 restricted TWT, 5 SCS traffic
 * description), the first PHY octet (bit 1 320 MHz in 6 GHz), and
 * whether any stream does MCS 12/13, 4096-QAM: the third octet of the
 * MCS set, or the fourth for a 20 MHz-only station, whose set starts
 * at MCS 0-7. True when the element is there.
 */
bool ehtCapsOf(const uint8_t *ies, int ieLength, bool twentyOnly,
               uint16_t &mac, uint8_t &phy, bool &qam4k)
{
    int len = 0;

    const uint8_t *p = findExtendedIE(ies, ieLength, 108, len);

    if (p == nullptr || len < 2 + 9)
        return false;

    mac = p[0] | (p[1] << 8);
    phy = p[2];

    int at = 2 + 9 + (twentyOnly ? 3 : 2);

    qam4k = len > at && (p[at] & 0x0F) != 0;

    return true;
}

/*
 * RM Enabled Capabilities (element 70): five octets saying which
 * radio measurements a device can make or answer — link
 * measurement, neighbor report, the three beacon report modes,
 * channel load, LCI and so on. Kept as the raw 40 bits and
 * decoded in the browser; 802.11-2020 table 9-191 has the list.
 */
uint64_t rrmCapsOf(const uint8_t *ies, int ieLength)
{
    int len = 0;

    const uint8_t *p = findIE(ies, ieLength, 70, len);

    uint64_t bits = 0;

    for (int i = 0; p != nullptr && i < len && i < 5; i++)
        bits |= (uint64_t)p[i] << (8 * i);

    return bits;
}

/*
 * The 802.11v (WNM) and FTM features from Extended Capabilities
 * (element 127). The element runs past 64 bits, so only the
 * ones worth showing are picked out, one bit each:
 *
 *   0 BSS Transition Mgmt   ext bit 19
 *   1 WNM Sleep Mode                17
 *   2 TIM Broadcast                 18
 *   3 Proxy ARP                     12
 *   4 FMS                           11
 *   5 TFS                           16
 *   6 DMS                           26
 *   7 WNM Notification              46
 *   8 FTM Responder                 70
 *   9 FTM Initiator                 71
 */
uint16_t wnmCapsOf(const uint8_t *ies, int ieLength)
{
    static const uint8_t extBits[] =
        { 19, 17, 18, 12, 11, 16, 26, 46, 70, 71 };

    int len = 0;

    const uint8_t *p = findIE(ies, ieLength, 127, len);

    uint16_t mask = 0;

    for (uint8_t i = 0; p != nullptr && i < sizeof(extBits); i++)
    {
        uint8_t bit = extBits[i];

        if (bit / 8 < len && (p[bit / 8] & (1 << (bit % 8))))
            mask |= (1 << i);
    }

    return mask;
}

void parseStationCapabilities(const uint8_t *ies, int ieLength,
                              struct ClientEntry &c, uint8_t source)
{
    if (ies == nullptr || ieLength <= 0)
        return;

    int len = 0;

    uint8_t  standard = PHY_UNKNOWN;
    uint16_t width    = 20;
    uint8_t  streams  = 0;

    // --------------------------------------------------------
    // HT Capabilities (IE 45)
    // --------------------------------------------------------

    const uint8_t *ht = findIE(ies, ieLength, 45, len);

    if (ht != nullptr && len >= 26)
    {
        standard = PHY_HT;
    }

    // --------------------------------------------------------
    // VHT Capabilities (IE 191)
    // --------------------------------------------------------

    const uint8_t *vht = findIE(ies, ieLength, 191, len);

    // Same caveat as for beacons: VHT in 2.4 GHz is a vendor
    // extension, not 802.11ac
    if (vht != nullptr && len >= 12 && c.channel > 14)
    {
        standard = PHY_VHT;
    }

    // --------------------------------------------------------
    // HE Capabilities (Extended IE 35)
    //
    // 6 byte MAC cap, 11 byte PHY cap, then the MCS/NSS set.
    // --------------------------------------------------------

    const uint8_t *he = findExtendedIE(ies, ieLength, 35, len);

    if (he != nullptr && len >= 19)
    {
        standard = PHY_HE;
    }

    width   = capsWidthOf(ies, ieLength, c.channel <= 14);
    streams = maxStreamsOf(ies, ieLength, c.channel);

    // --------------------------------------------------------
    // EHT Capabilities (Extended IE 108)
    //
    // 2 byte MAC cap, then PHY cap.
    // --------------------------------------------------------

    const uint8_t *eht = findExtendedIE(ies, ieLength, 108, len);

    if (eht != nullptr && len >= 3)
        standard = PHY_EHT;

    // --------------------------------------------------------
    // Without HT/VHT/HE/EHT classify via supported rates
    // --------------------------------------------------------

    if (standard == PHY_UNKNOWN)
    {
        bool ofdm = false;

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

                if (r == 12 || r == 18 || r == 24 || r == 36 ||
                    r == 48 || r == 72 || r == 96 || r == 108)
                {
                    ofdm = true;
                }
            }
        }

        if (c.channel > 14)
            standard = PHY_11A;
        else if (ofdm)
            standard = PHY_11G;
        else
            standard = PHY_11B;
    }

    // --------------------------------------------------------
    // Feature flags
    // --------------------------------------------------------

    // RM enabled capabilities -> 802.11k
    if (findIE(ies, ieLength, 70, len) != nullptr)
        c.rm11k = true;

    // Extended capabilities, bit 19 = BSS transition -> 802.11v
    const uint8_t *ext = findIE(ies, ieLength, 127, len);

    if (ext != nullptr && len >= 3 && (ext[2] & 0x08))
        c.bss11v = true;

    // Mobility domain -> 802.11r, with the domain asked for
    const uint8_t *md = findIE(ies, ieLength, 54, len);

    if (md != nullptr)
    {
        c.ft11r = true;

        if (len >= 2)
            c.mdid = md[0] | (md[1] << 8);
    }

    // Like the flags above, collected over every request seen
    c.rrmCaps |= rrmCapsOf(ies, ieLength);
    c.wnmCaps |= wnmCapsOf(ies, ieLength);

    c.opClasses |= opClassesOf(ies, ieLength);
    c.suppChans |= suppChannelsOf(ies, ieLength);

    collectUnknownElements(ies, ieLength, c.unknownIes,
                           c.unknownSeen, c.unknownCount, 6, source);

    collectVendorOuis(ies, ieLength, c.vendorOui, c.vendorOuiCount);

    // Only (re)association requests carry a Basic Multi-Link
    // element; keep the last one seen. Its per-STA profiles add the
    // channels of the other links.
    MloInfo mlo;

    if (parseMultiLink(ies, ieLength, mlo))
    {
        mlo.reqBand = c.channel <= 14 ? 2 : 5;

        c.mlo = mlo;

        c.suppChans  |= mlo.profChans;
        c.suppChans6 |= mlo.profChans6;
    }

    // HE 6 GHz band capabilities
    if (findExtendedIE(ies, ieLength, 59, len) != nullptr)
        c.band6 = true;

    // WMM information element
    if (findVendorIE(ies, ieLength,
                     0x00, 0x50, 0xF2, 0x02, len) != nullptr)
    {
        c.wmm = true;
    }

    // Wi-Fi QoS Management, see parseWfaElements()
    if (wfaCapaOf(ies, ieLength, c.wfaCapa))
        c.wfaCapaSeen = true;

    // --------------------------------------------------------
    // Station RSN: AKM, cipher, PMF
    // --------------------------------------------------------

    const uint8_t *rsnIE = findIE(ies, ieLength, 48, len);

    if (rsnIE != nullptr)
    {
        RsnInfo rsn;
        memset(&rsn, 0, sizeof(rsn));

        parseSuites(rsnIE, len, false, rsn);

        if (rsn.akm[0])
        {
            memset(c.akm, 0, sizeof(c.akm));
            strncpy(c.akm, rsn.akm, sizeof(c.akm) - 1);
        }

        if (rsn.pairwise[0])
        {
            memset(c.cipher, 0, sizeof(c.cipher));
            strncpy(c.cipher, rsn.pairwise, sizeof(c.cipher) - 1);
        }

        if (rsn.mfpCapable || rsn.mfpRequired)
            c.pmf = true;
    }

    // --------------------------------------------------------
    // Keep the highest value seen so far.
    //
    // A device reports less in a 2.4 GHz probe than in a
    // 5 GHz probe, so do not simply overwrite.
    // --------------------------------------------------------

    if (standard > c.standard)
        c.standard = standard;

    if (width > c.maxWidth)
        c.maxWidth = width;

    c.w8080 |= caps8080Of(ies, ieLength, c.channel <= 14);

    if (streams > c.streams)
        c.streams = streams;

    // A 20 MHz-only station lays out its EHT-MCS set differently
    {
        uint16_t mac = 0;
        uint8_t  phy = 0;
        bool     qam4k = false;

        if (ehtCapsOf(ies, ieLength, width == 20, mac, phy, qam4k))
        {
            c.ehtMac   |= mac;
            c.ehtPhy   |= phy;
            c.ehtQam4k |= qam4k;
        }
    }

    c.heTwt  |= heTwtOf(ies, ieLength);
    c.heCaps |= heCapsOf(ies, ieLength);
    c.vhtCaps |= vhtCapsOf(ies, ieLength, c.channel <= 14);
}
