// SPDX-License-Identifier: GPL-2.0-only WITH AdditionRef-wlanscout-linking-exception
// SPDX-FileCopyrightText: 2026 Dr.-Ing. Simon Wunderlich Systementwicklung & Beratung

// ============================================================
// Event log on flash
//
// LOG_FILE holds LogEntry records behind a 4-byte tag. Each new
// entry is appended at once, so a power cut loses nothing that was
// logged; once the file holds twice LOG_ENTRIES it is written anew
// from the ring, which holds the newest LOG_ENTRIES.
// ============================================================

#define LOG_FILE  "/eventlog.bin"
#define LOG_TMP   "/eventlog.tmp"
// "EVL1"; a new layout of LogEntry needs a new tag, an old file is
// then ignored rather than read as garbage
#define LOG_TAG   0x314C5645u

uint16_t logFileRecords = 0;

// The ring, oldest first, into a fresh file
void logRewrite()
{
    if (!catalogFsOk || eventLog == nullptr)
        return;

    FLASH_TIMED("event log rewrite");

    File f = LittleFS.open(LOG_TMP, "w");

    if (!f)
        return;

    uint32_t tag = LOG_TAG;

    f.write((const uint8_t *)&tag, sizeof(tag));

    uint8_t start = (logCount < LOG_ENTRIES) ? 0 : logWrite;

    for (uint8_t i = 0; i < logCount; i++)
        f.write((const uint8_t *)&eventLog[(start + i) % LOG_ENTRIES],
                sizeof(LogEntry));

    f.close();

    LittleFS.rename(LOG_TMP, LOG_FILE);

    logFileRecords = logCount;
}

void logPersist(const struct LogEntry &e)
{
    if (!logLoaded || !catalogFsOk)
        return;

    if (logFileRecords >= 2 * LOG_ENTRIES)
    {
        logRewrite();
        return;
    }

    FLASH_TIMED("event log entry");

    File f = LittleFS.open(LOG_FILE, "a");

    if (!f)
        return;

    f.write((const uint8_t *)&e, sizeof(LogEntry));
    f.close();

    logFileRecords++;
}

/*
 * After the file system is mounted: the entries of earlier boots
 * first, then whatever this boot logged before, and the file
 * written anew from that.
 */
void logLoad()
{
    if (!catalogFsOk || eventLog == nullptr)
        return;

    // This boot's entries so far, put aside
    uint8_t   early = logCount;
    LogEntry *mine  = (LogEntry *)ps_malloc(sizeof(LogEntry) *
                                            (early ? early : 1));

    if (mine == nullptr)
        return;

    uint8_t start = (logCount < LOG_ENTRIES) ? 0 : logWrite;

    for (uint8_t i = 0; i < early; i++)
        mine[i] = eventLog[(start + i) % LOG_ENTRIES];

    logWrite = 0;
    logCount = 0;

    File f = LittleFS.open(LOG_FILE, "r");

    uint32_t tag = 0;

    if (f && f.read((uint8_t *)&tag, sizeof(tag)) == sizeof(tag) &&
        tag == LOG_TAG)
    {
        LogEntry e;

        while (f.read((uint8_t *)&e, sizeof(e)) == sizeof(e))
        {
            e.who[sizeof(e.who) - 1]   = 0;
            e.text[sizeof(e.text) - 1] = 0;

            eventLog[logWrite] = e;
            logWrite = (logWrite + 1) % LOG_ENTRIES;

            if (logCount < LOG_ENTRIES)
                logCount++;
        }
    }

    if (f)
        f.close();

    for (uint8_t i = 0; i < early; i++)
    {
        eventLog[logWrite] = mine[i];
        logWrite = (logWrite + 1) % LOG_ENTRIES;

        if (logCount < LOG_ENTRIES)
            logCount++;
    }

    free(mine);

    logLoaded = true;

    logRewrite();
}

/*
 * Once the clock is known: this boot's entries from before get
 * their wall time, in the ring and in the file. See clockFillLoop().
 */
void logClockFill(uint32_t nowEpoch, uint32_t nowUp)
{
    if (eventLog == nullptr)
        return;

    for (uint8_t i = 0; i < logCount; i++)
    {
        LogEntry &e = eventLog[i];

        if (e.boot == bootCount && e.epoch == 0)
            e.epoch = nowEpoch - (nowUp - e.up);
    }

    if (logLoaded)
        logRewrite();
}

/*
 * Set just before a reboot asked for over /api/reboot. RTC memory
 * survives a software restart but not a power cut, so the start
 * after it can tell that reboot from a restart of the firmware's own.
 */
#define REBOOT_ASKED 0x52424F54UL

RTC_NOINIT_ATTR uint32_t rebootAsked;

// Read once at the start, the RTC word is cleared then
bool rebootOnRequest = false;

/*
 * Why the chip started: a power cut, a crash, a watchdog, a
 * restart of its own (firmware update, rollback) or a reboot asked for.
 */
const char *resetReasonText()
{
    switch (esp_reset_reason())
    {
        case ESP_RST_POWERON:   return "power on";
        case ESP_RST_EXT:       return "reset pin";
        case ESP_RST_SW:        return rebootOnRequest
                                    ? "reboot on request"
                                    : "restart by the firmware";
        case ESP_RST_PANIC:     return "crash";
        case ESP_RST_INT_WDT:   return "interrupt watchdog";
        case ESP_RST_TASK_WDT:  return "task watchdog";
        case ESP_RST_WDT:       return "watchdog";
        case ESP_RST_DEEPSLEEP: return "wake from deep sleep";
        case ESP_RST_BROWNOUT:  return "brownout, supply too weak";
        case ESP_RST_SDIO:      return "SDIO";
        default:                return "unknown reason";
    }
}

void catalogMount()
{
    prefs.begin("analyzer", false);

    bootCount = prefs.getUShort("boots", 0) + 1;

    prefs.putUShort("boots", bootCount);

    prefs.end();

    catalog = (CatalogEntry *)ps_calloc(CATALOG_MAX,
                                        sizeof(CatalogEntry));

    // Formats the partition on first use, takes a few seconds
    catalogFsOk = LittleFS.begin(true, "/littlefs", 5, "spiffs");

    if (!catalogFsOk || catalog == nullptr)
    {
        Serial.println("Catalog: no file system");
        return;
    }

    fsSmall = LittleFS.totalBytes() < FS_SMALL_BYTES;

    File f = LittleFS.open(CATALOG_FILE, "r");

    if (!f)
        return;

    CatalogHeader h;

    bool read = f.read((uint8_t *)&h, sizeof(h)) == sizeof(h);

    if (read && h.magic == CATALOG_MAGIC &&
        h.entrySize == sizeof(CatalogEntry) &&
        h.count <= CATALOG_MAX)
    {
        size_t want = (size_t)h.count * sizeof(CatalogEntry);

        if (f.read((uint8_t *)catalog, want) == want)
            catalogCount = h.count;

        f.close();
    }
    else
    {
        f.close();

        /*
         * A catalog of an older firmware, written in another
         * layout: not converted, the catalog starts empty. Its
         * pcap file goes too, frames without rows would only
         * confuse.
         */
        LittleFS.remove(CATALOG_FILE);
        LittleFS.remove(CATALOG_PCAP);

        catalogFormatReset = true;

        Serial.println("Catalog: older format, started empty");
    }

    catalogPrune();

    Serial.printf("Catalog: %u entries, %s, %u of %u kB used\r\n",
                  catalogCount, catalogEnabled ? "recording" : "off",
                  (unsigned)(LittleFS.usedBytes() / 1024),
                  (unsigned)(LittleFS.totalBytes() / 1024));
}

void catalogSave()
{
    catalogSavedAt = millis();
    catalogDirty   = false;
    catalogUrgent  = false;

    if (!catalogFsOk)
        return;

    FLASH_TIMED("catalog save");

    // Written aside and renamed, so a power cut keeps the old one
    File f = LittleFS.open(CATALOG_TMP, "w");

    if (!f)
        return;

    CatalogHeader h = { CATALOG_MAGIC, sizeof(CatalogEntry),
                        catalogCount };

    bool ok = f.write((const uint8_t *)&h, sizeof(h)) == sizeof(h);

    size_t size = (size_t)catalogCount * sizeof(CatalogEntry);

    ok = ok && f.write((const uint8_t *)catalog, size) == size;

    f.close();

    if (ok)
    {
        LittleFS.remove(CATALOG_FILE);
        LittleFS.rename(CATALOG_TMP, CATALOG_FILE);
    }
}

void catalogLoop()
{
    if (catalogUrgent && millis() - catalogSavedAt > CATALOG_URGENT_MS)
        catalogSave();
    else if (catalogDirty && millis() - catalogSavedAt > CATALOG_SAVE_MS)
        catalogSave();
}

void catalogClear()
{
    catalogCount = 0;
    catalogFull  = 0;

    if (catalogFsOk)
    {
        LittleFS.remove(CATALOG_FILE);
        LittleFS.remove(CATALOG_PCAP);
    }

    catalogDirty  = false;
    catalogUrgent = false;
}

/*
 * The frame being looked at, for the example and the pcap file.
 */
struct CatalogFrame
{
    const uint8_t       *frame;
    uint8_t              seen;       // SEEN_*
    const char          *ssid;
    uint8_t              channel;
    CatalogEntry        *takers[8];  // entries that get this frame
    uint8_t              takerSender[8];   // their sender slot
    uint8_t              takerCount;
};

/*
 * Set while a download picks the frames of one entry out of the
 * pcap file: catalogScan() then only answers whether a frame
 * carries that element, nothing is counted.
 */
bool     catalogMatching = false;
uint8_t  catalogMatchKind;
uint32_t catalogMatchKey;
bool     catalogMatched;
bool     catalogMatchAny = false;    // any entry, see catalogPrune()
bool    *catalogRowHasFrame = nullptr;

// The rows a frame carries, collected for a snapshot's frames
#define CATALOG_HITS_MAX 16

bool     catalogCollectHits = false;
uint8_t  catalogHitKind[CATALOG_HITS_MAX];
uint32_t catalogHitKey[CATALOG_HITS_MAX];
uint8_t  catalogHitCount;

void catalogNote(struct CatalogFrame &cf, uint8_t kind, uint32_t key,
                 const uint8_t *body, int len)
{
    if (catalogMatching)
    {
        if (catalogMatchAny ||
            (kind == catalogMatchKind && key == catalogMatchKey))
            catalogMatched = true;

        // catalogPrune() notes which rows still have a frame
        if (catalogMatchAny && catalogRowHasFrame != nullptr)
            for (uint16_t i = 0; i < catalogCount; i++)
                if (catalog[i].key == key && catalog[i].kind == kind)
                    catalogRowHasFrame[i] = true;

        // A snapshot notes the rows, each once
        if (catalogCollectHits)
        {
            bool known = false;

            for (uint8_t i = 0; i < catalogHitCount; i++)
                if (catalogHitKind[i] == kind && catalogHitKey[i] == key)
                    known = true;

            if (!known && catalogHitCount < CATALOG_HITS_MAX)
            {
                catalogHitKind[catalogHitCount] = kind;
                catalogHitKey[catalogHitCount]  = key;
                catalogHitCount++;
            }
        }

        return;
    }

    CatalogEntry *e = nullptr;

    for (uint16_t i = 0; i < catalogCount; i++)
    {
        if (catalog[i].key == key && catalog[i].kind == kind)
        {
            e = &catalog[i];
            break;
        }
    }

    uint32_t now = deviceEpoch();

    if (e == nullptr)
    {
        if (catalogCount >= CATALOG_MAX)
        {
            catalogFull++;
            return;
        }

        e = &catalog[catalogCount++];

        memset(e, 0, sizeof(*e));

        e->kind       = kind;
        e->key        = key;
        e->firstAt    = now;
        e->firstBoot  = bootCount;
        e->firstClock = pcapClockSource;
        e->channel    = cf.channel;

        memcpy(e->mac, &cf.frame[10], 6);
        strncpy(e->ssid, cf.ssid, sizeof(e->ssid) - 1);

        if (body != nullptr && len > 0)
        {
            e->fullLen   = (uint16_t)len;
            e->sampleLen = len > CATALOG_SAMPLE ? CATALOG_SAMPLE : len;
            memcpy(e->sample, body, e->sampleLen);
        }

        catalogUrgent = true;
    }

    if (!(e->seenIn & cf.seen))
    {
        e->seenIn |= cf.seen;
        catalogUrgent = true;
    }

    e->count++;
    e->lastAt    = now;
    e->lastBoot  = bootCount;
    e->lastClock = pcapClockSource;

    catalogDirty = true;

    // Which sender: a new one gets a slot while there are any
    const uint8_t *sa = &cf.frame[10];

    uint8_t slot = CATALOG_SENDERS;

    for (uint8_t i = 0; i < e->senderCount; i++)
        if (memcmp(e->senders[i], sa, 6) == 0)
            slot = i;

    if (slot == CATALOG_SENDERS)
    {
        if (e->senderCount < CATALOG_SENDERS)
        {
            slot = e->senderCount++;

            memcpy(e->senders[slot], sa, 6);

            e->senderFrames[slot] = 0;

            catalogUrgent = true;
        }
        else if (e->sendersMore < 0xFFFF)
        {
            // Counted every time, so only a rough figure
            e->sendersMore++;
        }
    }

    if (slot < CATALOG_SENDERS &&
        e->senderFrames[slot] < CATALOG_FRAMES && cf.takerCount < 8)
    {
        // Once per frame, an element can occur twice in it
        for (uint8_t i = 0; i < cf.takerCount; i++)
            if (cf.takers[i] == e)
                return;

        cf.takerSender[cf.takerCount] = slot;
        cf.takers[cf.takerCount++]    = e;
    }
}

/*
 * Vendor elements (OUI and type) the sensor decodes: Apple's Device
 * IE gives name, manufacturer and model, MikroTik's element the
 * radio name, 00:30:1A type 1 the firmware version; the rest of
 * their fields is not read.
 */
static const uint32_t decodedVendor[] = {
    0x0050F201,     // WPA
    0x0050F202,     // WMM
    0x0050F204,     // WPS
    0x506F9A09,     // P2P (Wi-Fi Direct)
    0x506F9A0A,     // Wi-Fi Display (Miracast)
    0x506F9A10,     // Hotspot 2.0
    0x506F9A12,     // OSEN
    0x506F9A16,     // MBO-OCE
    0x506F9A1C,     // OWE transition
    0x506F9A29,     // RSNE Override
    0x506F9A2A,     // RSNE Override 2
    0x506F9A2B,     // RSNXE Override
    0x506F9A23,     // Wi-Fi Alliance Capabilities (QoS Management)
    0x506F9A2C,     // RSN Selection
    0x506F9A13,     // NAN: master indication and cluster
    0xFA0BBC0D,     // Drone Remote ID: all but serial and operator ID
    0x08000900,     // HP printer: model (the serial is not read)
    0x00A04000,     // Apple Device IE: name, maker, model; the rest
                    // is flags, an OUI and MAC addresses
    0x000C4200,     // MikroTik: the radio name
    0x5C5B3501,     // Mist: the AP name
    0x00A05704,     // LANCOM: the device UUID
    0x00A05708,     // LANCOM: the AP name
    0x30FF8368,     // camera kit: its status text
    0x00301A01      // 00:30:1A type 1: the firmware version
};

/*
 * Vendor elements gone through by hand (October 2026, five
 * catalogs): proprietary flags, counters, hashes, MAC addresses,
 * nothing worth showing. Their OUI still names maker or chip
 * under BEST GUESS, but they are no longer catalogued. A new OUI,
 * or a new type of a known one, still is.
 */
static const uint32_t reviewedVendor[] = {
    0x00000FFE, 0x0000F00C, 0x0000F00F, 0x0000F022, 0x00025B03,
    0x0002BC01, 0x00037F01, 0x00037F04, 0x00037F05, 0x00037F0A,
    0x00039301, 0x00040E01, 0x00045602, 0x00090F01, 0x00090F06,
    0x00090F08, 0x00090F0B, 0x00098600, 0x00098601, 0x000B8601,
    0x000C4300, 0x000C4303, 0x000C4304, 0x000C4307, 0x000C4308,
    0x000C4309, 0x000C430B, 0x000C430F, 0x000C43A5, 0x000CE700,
    0x000CE701, 0x000CE708, 0x000CE709, 0x000CE780, 0x000CE781,
    0x000CE788, 0x000CE789, 0x000E5802, 0x000E5808, 0x00101802,
    0x0012FB01, 0x00134901, 0x00134902, 0x00139201, 0x00146C00,
    0x00146C04, 0x00156D00, 0x00163280, 0x0017F201, 0x0017F206,
    0x0017F207, 0x0017F20A, 0x00180A00, 0x00180A07, 0x00197701,
    0x001C51AA, 0x001C6200, 0x001D0F01, 0x001D0F02, 0x001D0F10,
    0x001F4101, 0x001F4104, 0x00268601, 0x00268609, 0x00268614,
    0x00268615, 0x00268616, 0x00268617, 0x00268618, 0x002D2520,
    0x00319201, 0x00319210, 0x00409603, 0x0040960B, 0x0040960C,
    0x00409614, 0x0040962C, 0x0050F205, 0x0050F208, 0x0050F211,
    0x00904C04, 0x00904C33, 0x00904C34, 0x00904C5C, 0x009D8565,
    0x00A05701, 0x00A0F805, 0x00E04C00, 0x00E04C02,
    0x00E0FC01, 0x00E0FC02, 0x00E0FC11, 0x00E0FC12, 0x00E0FC20,
    0x00E0FC40, 0x00E0FC80, 0x00E0FC81, 0x00FDB800, 0x08002801,
    0x0DD49500, 0x11223301, 0x1422DB02, 0x1422DB03, 0x1422DB06,
    0x18FE3403, 0x1C5A7C04, 0x30CDA701, 0x4045DA01,
    0x6CB22700, 0x74317018, 0x7D3F80B3, 0x8CBEBE87, 0x8CFDF000,
    0x8CFDF001, 0x8CFDF003, 0x8CFDF004, 0x94BBAEFA, 0xAC853D20,
    0xAC853D82, 0xAC853DBD, 0xC83A6B01, 0xF832E401,

    // hostapd's QCA HE Capabilities/Operation from before 802.11ax,
    // and Intel's type 8 in probe requests (October 2026)
    0x00137401, 0x00137402, 0x00173508,

    // .176's catalog (October 2026): flags and zeros, MAC and IP
    // addresses, a serial number and a network name; 00:00:00 and
    // AA:BB:CC are no OUIs at all
    0x00000000, 0x000B8209, 0x000B820A, 0x000C4306, 0x000E5800,
    0x00117400, 0x001F4102, 0x00504303, 0xAABBCC00, 0xF4BEC200,

    // .233's and a powerbank sensor's catalogs (October 2026):
    // DJI's element under 44:32:44, ASCII "D2D" and no OUI, with
    // its own BSSID and counters, Xirrus' with two BSSIDs,
    // MediaTek's type 1 with three zero bytes
    0x44324400, 0x000F7DF9, 0x000C4301,

    // The magsafe sensor's catalog (October 2026): the Wi-Fi
    // Alliance's DPP Configurator Connectivity element, empty by
    // design, from a FRITZ!Box; NETGEAR's type 8 (one byte) and
    // Quantenna's type 2 (34 bytes of small values, flags most
    // likely), neither described anywhere
    0x506F9A1E, 0x00146C08, 0x00268602,

    // .233's catalog (October 2026): the Car Connectivity
    // Consortium's type 10 in a car's Wi-Fi Direct group (MBUX),
    // ten octets that look like type-length-value, not described
    // anywhere public
    0x04DF690A,

    // A catalog of October 2026: NETGEAR's type 3, seven octets of
    // 01 and zeros, the same on both nodes of a mesh, and Belkin's
    // type 0 from a Linksys, 00 01 00 03 on all three BSSIDs;
    // neither described anywhere
    0x00146C03, 0x08863B00,

    // The magsafe sensor's catalog (November 2026): devolo's type 7,
    // three zero octets from a devolo adapter, and Realtek's type 5,
    // 06 01 40 beside its known type 2; the OUI names maker and chip,
    // the bytes are not described anywhere
    0x000B3B07, 0x00E04C05,

    // .233's catalog (November 2026): TP-Link's type 10 in a probe
    // request, 00 01 01 00 00 01 02 02 00, and Sonos' type 15 in
    // its speakers' probe requests, 02 31 02 10 01 02; always the
    // same octets, described nowhere
    0x000AEB0A, 0x000E580F,

    // The magsafe sensor's snapshot (November 2026): Cisco Meraki's
    // type 5, six zero octets and three more; Qualcomm's type 2,
    // 01 01 and zeros; Arlo's type 0, one zero octet; 02:4D:52
    // type 1, a locally administered "OUI", five octets. Meraki
    // still names the maker and Qualcomm the chip under BEST GUESS
    0x00180A05, 0x8CFDF002, 0xA4116200, 0x024D5201
};

bool vendorDecoded(uint32_t key)
{
    for (uint32_t k : decodedVendor)
        if (k == key)
            return true;

    return false;
}

bool vendorReviewed(uint32_t key)
{
    for (uint32_t k : reviewedVendor)
        if (k == key)
            return true;

    return false;
}

// Whether a vendor element goes into the catalog
bool vendorCatalogued(uint32_t key)
{
    return !vendorDecoded(key) && !vendorReviewed(key);
}

bool cipherKnown(uint32_t suite)
{
    uint32_t oui = suite >> 8;
    uint8_t  t   = suite & 0xFF;

    if (oui == 0x000FAC)
        return t <= 13 && t != 3 && t != 7;

    // 00-00-00-00, in the WPA element of some cable routers
    if (suite == 0)
        return true;

    // WPA (version 1) suites; 0 is "none", use the group cipher
    if (oui == 0x0050F2)
        return t == 0 || t == 1 || t == 2 || t == 4 || t == 5;

    return vendorSuiteName(suite, false) != nullptr;
}

bool akmKnown(uint32_t suite, bool wpa1)
{
    uint32_t oui = suite >> 8;

    if (oui != (wpa1 ? 0x0050F2u : 0x000FACu))
        return vendorSuiteName(suite, true) != nullptr;

    return strncmp(akmName(suite & 0xFF, wpa1), "AKM ", 4) != 0;
}

uint32_t suiteAt(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
            (uint32_t)p[2] << 8 | p[3]);
}

/*
 * Walks an RSN element (or the body of a WPA one after its
 * OUI and type) and notes suites without a name.
 */
void catalogSuites(struct CatalogFrame &cf, const uint8_t *p, int len,
                   bool wpa1)
{
    int pos = 2;                       // version

    auto cipher = [&](const uint8_t *q)
    {
        uint32_t s = suiteAt(q);

        if (!cipherKnown(s))
            catalogNote(cf, CAT_CIPHER, s, q, 4);
    };

    if (pos + 4 > len)
        return;

    cipher(p + pos);                   // group cipher
    pos += 4;

    for (int list = 0; list < 2; list++)
    {
        if (pos + 2 > len)
            return;

        int n = p[pos] | (p[pos + 1] << 8);

        pos += 2;

        for (int i = 0; i < n && pos + 4 <= len; i++, pos += 4)
        {
            if (list == 0)
            {
                cipher(p + pos);
            }
            else
            {
                uint32_t s = suiteAt(p + pos);

                if (!akmKnown(s, wpa1))
                    catalogNote(cf, CAT_AKM, s, p + pos, 4);
            }
        }
    }

    if (wpa1)
        return;

    pos += 2;                          // RSN capabilities

    if (pos + 2 > len)
        return;

    int pmkids = p[pos] | (p[pos + 1] << 8);

    pos += 2 + 16 * pmkids;

    if (pos + 4 <= len)
        cipher(p + pos);               // group management cipher
}

/*
 * Appends one frame to the pcap file, in the same form as the
 * export: radiotap, then the frame as captured.
 */
void catalogWriteFrame(const struct MgmtFrameCopy &entry)
{
    if (!catalogFsOk)
        return;

    FLASH_TIMED("catalog frame");

    bool fresh = !LittleFS.exists(CATALOG_PCAP);

    File f = LittleFS.open(CATALOG_PCAP, "a");

    if (!f)
        return;

    if (f.size() + 16 + RADIOTAP_LEN + entry.length > CATALOG_PCAP_MAX)
    {
        f.close();
        return;
    }

    if (fresh || f.size() == 0)
    {
        uint8_t hdr[24];

        uint32_t magic    = 0xA1B2C3D4;
        uint32_t snaplen  = MGMT_PARSE_MAX + RADIOTAP_LEN;
        uint32_t linktype = 127;

        memcpy(&hdr[0], &magic, 4);
        hdr[4] = 2;  hdr[5] = 0;
        hdr[6] = 4;  hdr[7] = 0;
        memset(&hdr[8], 0, 8);
        memcpy(&hdr[16], &snaplen, 4);
        memcpy(&hdr[20], &linktype, 4);

        f.write(hdr, sizeof(hdr));
    }

    int64_t  stamp = (int64_t)entry.us + clockOffsetUs;
    uint32_t sec   = (uint32_t)(stamp / 1000000LL);
    uint32_t usec  = (uint32_t)(stamp % 1000000LL);
    uint32_t len   = RADIOTAP_LEN + entry.length;

    uint8_t rec[16];

    memcpy(&rec[0], &sec, 4);
    memcpy(&rec[4], &usec, 4);
    memcpy(&rec[8], &len, 4);
    memcpy(&rec[12], &len, 4);

    uint8_t rt[RADIOTAP_LEN];

    pcapBuildRadiotap(rt, entry.channel, entry.rssi, entry.noise,
                      entry.rxRate);

    f.write(rec, sizeof(rec));
    f.write(rt, RADIOTAP_LEN);
    f.write(entry.data, entry.length);

    f.close();
}

/*
 * Walks the elements of a beacon, probe response, probe request
 * or (re)association request and hands everything unknown to
 * catalogNote(). Returns false for any other frame.
 */
/*
 * Whether an element has a length the standard allows. Some
 * devices put content of their own under standard element
 * numbers; from the first such element on, nothing in that frame
 * can be trusted to be what its number says, so the catalog takes
 * nothing more from it. Only elements with a fixed length are
 * checked, Neighbor Report (52) for its minimum, also the FH and
 * CF Parameter Sets (2, 4) the sensor does not read otherwise.
 */
bool elementLengthOk(uint8_t id, int len)
{
    switch (id)
    {
        case 2:   return len == 5;
        case 4:   return len == 6;
        case 6:   return len == 2;
        case 56:  return len == 5;
        case 90:  return len == 3;
        case 52:  return len >= 13;

        case 3:   case 32:  case 42:  case 46:  case 47:
        case 62:  case 72:  case 199:
            return len == 1;

        case 35:  return len == 2;
        case 37:  case 54:
                  return len == 3;
        case 60:  return len == 4;
        case 11:  case 70:  case 192:
                  return len == 5;
        case 113: return len == 7;
        case 191: return len == 12;
        case 74:  return len == 14;
        case 61:  return len == 22;
        case 45:  return len == 26;
    }

    return true;
}

/*
 * Whether the elements of a frame hang together: none of a length
 * the standard does not allow, one SSID. A beacon or probe
 * response that fails is broken past the FCS check (a PAX9500
 * sent one with an "element 24" and a configuration of garbage)
 * and is not parsed. A station's frames are, some put content of
 * their own under standard numbers.
 */
bool elementsPlausible(const uint8_t *ies, int ieLength)
{
    bool ssidSeen = false;

    for (int pos = 0; pos + 2 <= ieLength; )
    {
        uint8_t id  = ies[pos];
        uint8_t len = ies[pos + 1];

        if (pos + 2 + len > ieLength)
            break;

        if (!elementLengthOk(id, len) || (id == 0 && ssidSeen))
            return false;

        if (id == 0)
            ssidSeen = true;

        pos += 2 + len;
    }

    return true;
}

bool catalogScan(const uint8_t *frame, int length, uint8_t channel,
                 struct CatalogFrame &cf)
{
    if (length < 24 || protectedNonsense(frame, length))
        return false;

    // Management frames only, version 0
    if ((frame[0] & 0x0F) != 0)
        return false;

    uint8_t subtype = (frame[0] >> 4) & 0x0F;

    int offset;
    uint8_t seen;

    switch (subtype)
    {
        case 0:  offset = 28; seen = SEEN_ASSOC_REQ;  break;
        case 2:  offset = 34; seen = SEEN_ASSOC_REQ;  break;
        case 4:  offset = 24; seen = SEEN_PROBE_REQ;  break;
        case 5:  offset = 36; seen = SEEN_PROBE_RESP; break;
        case 8:  offset = 36; seen = SEEN_BEACON;     break;
        default: return false;
    }

    if (length <= offset)
        return false;

    const uint8_t *ies = frame + offset;

    int ieLength = length - offset;

    char ssid[33] = "";

    int ssidLen = 0;

    const uint8_t *ssidIe = findIE(ies, ieLength, 0, ssidLen);

    if (ssidIe != nullptr && ssidLen > 0)
        memcpy(ssid, ssidIe, ssidLen > 32 ? 32 : ssidLen);

    int dsLen = 0;

    const uint8_t *ds = findIE(ies, ieLength, 3, dsLen);

    cf.frame   = frame;
    cf.seen    = seen;
    cf.ssid    = ssid;
    cf.channel = (ds != nullptr && dsLen >= 1) ? ds[0] : channel;

    int pos = 0;

    // Length of the element before, for Fragment elements
    int before = 0;

    bool ssidSeen = false;

    while (pos + 2 <= ieLength)
    {
        uint8_t id  = ies[pos];
        int     len = ies[pos + 1];

        // Cut off by the capture, nothing reliable from here on
        if (pos + 2 + len > ieLength)
            break;

        // Not the element its number says, nor what follows
        if (!elementLengthOk(id, len))
            break;

        /*
         * A frame carries one SSID element; a second one is a
         * frame filled up with zeros (seen from a phone: hundreds
         * of empty elements 0 with a stray FH and CF Parameter Set
         * among them), nothing in it can be trusted
         */
        if (id == 0)
        {
            if (ssidSeen)
                break;

            ssidSeen = true;
        }

        const uint8_t *body = ies + pos + 2;

        bool full = before == 255;

        before = len;

        /*
         * A Fragment (242) right after a full element is the rest
         * of that element, e.g. a Multi-Link element with its
         * per-STA profiles; the parsers put it back together and
         * the element before is catalogued if it is unknown
         */
        if (id == 242 && full)
        {
            pos += 2 + len;
            continue;
        }

        if (id == 255)
        {
            if (len >= 1 && !elementKnown(255, body[0]))
                catalogNote(cf, CAT_EXT, body[0], body + 1, len - 1);
        }
        else if (id == 221)
        {
            if (len >= 4)
            {
                uint32_t key = suiteAt(body);

                if (vendorCatalogued(key))
                    catalogNote(cf, CAT_VENDOR, key, body + 4, len - 4);
                else if (key == 0x0050F201)
                    catalogSuites(cf, body + 4, len - 4, true);
            }
        }
        /*
         * Interworking is decoded, but only the fields the tooltip
         * shows. It stays in the catalog, one row as before, so its
         * frames can still be looked through for anything else.
         */
        else if (!elementKnown(id, -1) || elementPartial(id))
        {
            catalogNote(cf, CAT_ELEMENT, id, body, len);
        }
        else if (id == 48)
        {
            catalogSuites(cf, body, len, false);
        }

        pos += 2 + len;
    }

    return true;
}

/*
 * Whether an entry still belongs in the catalog with the firmware
 * running now: its element, vendor element or suite is still not
 * decoded, or only in part.
 */
bool catalogStillWanted(const struct CatalogEntry &e)
{
    switch (e.kind)
    {
        case CAT_ELEMENT:
            return !elementKnown(e.key, -1) || elementPartial(e.key);

        case CAT_EXT:
            return !elementKnown(255, e.key);

        case CAT_VENDOR:
            return vendorCatalogued(e.key);

        case CAT_AKM:
            return !akmKnown(e.key, (e.key >> 8) == 0x0050F2);

        default:
            return !cipherKnown(e.key);
    }
}

uint16_t catalogPrunedRows;
uint32_t catalogPrunedBytes;

// Set when the pcap file could not be rewritten for lack of space
uint32_t catalogPruneNeeded;
uint32_t catalogPruneFree;

/*
 * Walks the pcap file and keeps the frames that still carry
 * something catalogued: copied to out when given, only counted
 * otherwise. Returns the size kept, header included, 0 when the
 * file cannot be read or out not written.
 */
uint32_t catalogPcapKeep(File &in, File *out)
{
    static uint8_t buf[16 + RADIOTAP_LEN + MGMT_PARSE_MAX];

    in.seek(0);

    if (in.read(buf, 24) != 24)
        return 0;

    if (out && out->write(buf, 24) != 24)
        return 0;

    uint32_t kept = 24;

    catalogMatching = true;
    catalogMatchAny = true;

    while (in.read(buf, 16) == 16)
    {
        uint32_t len;

        memcpy(&len, &buf[8], 4);

        if (len > RADIOTAP_LEN + MGMT_PARSE_MAX ||
            in.read(buf + 16, len) != len)
        {
            break;
        }

        // 15 octets from firmware before 2.8.27, then 16
        uint16_t rtLen = radiotapLenOf(buf + 16, len);

        if (rtLen == 0)
            break;

        CatalogFrame cf = {};

        catalogMatched = false;

        catalogScan(buf + 16 + rtLen, len - rtLen, 0, cf);

        if (!catalogMatched)
            continue;

        if (out && out->write(buf, 16 + len) != 16 + len)
        {
            kept = 0;
            break;
        }

        kept += 16 + len;
    }

    catalogMatching = false;
    catalogMatchAny = false;

    return kept;
}

/*
 * Once after every firmware change (the version is kept in NVS):
 *
 *   1. drops the rows of what this firmware decodes or has on its
 *      reviewed list,
 *   2. rewrites the pcap file with only the frames that still
 *      carry something catalogued, read with today's rules, the
 *      length check of elementLengthOk() among them,
 *   3. drops the rows that had frames stored but none of them is
 *      left: what they were found in no longer counts.
 *
 * Rows without any stored frame (the pcap was full) stay, there is
 * nothing to check them against. The pcap file is written aside
 * and renamed like the catalog, so the frames kept have to fit next
 * to it. They are counted first; when they do not fit (snapshots
 * take the space), the file stays as it is, and so do the rows of
 * step 3, and the next start tries again.
 */
void catalogPrune()
{
    prefs.begin("analyzer", false);

    String done = prefs.getString("catver", "");

    prefs.end();

    if (done == FW_VERSION)
        return;

    uint16_t before = catalogCount;
    uint16_t kept   = 0;

    for (uint16_t i = 0; i < catalogCount; i++)
        if (catalogStillWanted(catalog[i]))
            catalog[kept++] = catalog[i];

    catalogCount = kept;

    bool finished = true;

    File in = LittleFS.open(CATALOG_PCAP, "r");

    catalogRowHasFrame = (bool *)calloc(CATALOG_MAX, sizeof(bool));

    if (in && catalogRowHasFrame != nullptr)
    {
        uint32_t size      = in.size();
        uint32_t keep      = catalogPcapKeep(in, nullptr);
        size_t   freeBytes = LittleFS.totalBytes() - LittleFS.usedBytes();
        bool     pruned    = false;

        if (keep == size)
        {
            // Every frame still counts: nothing to copy
            pruned = true;
        }
        else if (keep > 0 && freeBytes < keep + CATALOG_PRUNE_SLACK)
        {
            catalogPruneNeeded = keep;
            catalogPruneFree   = freeBytes;

            finished = false;
        }
        else if (keep > 0)
        {
            File out = LittleFS.open(CATALOG_PCAP_TMP, "w");

            pruned = out && catalogPcapKeep(in, &out) == keep;

            out.close();
        }

        in.close();

        if (pruned && keep < size)
        {
            LittleFS.remove(CATALOG_PCAP);
            LittleFS.rename(CATALOG_PCAP_TMP, CATALOG_PCAP);
        }
        else
        {
            LittleFS.remove(CATALOG_PCAP_TMP);
        }

        if (pruned)
        {
            catalogPrunedBytes = size - keep;

            kept = 0;

            for (uint16_t i = 0; i < catalogCount; i++)
                if (catalog[i].frames == 0 || catalogRowHasFrame[i])
                    catalog[kept++] = catalog[i];

            catalogCount = kept;
        }
    }

    free(catalogRowHasFrame);

    catalogRowHasFrame = nullptr;

    catalogPrunedRows = before - catalogCount;

    if (catalogPrunedRows)
        catalogSave();

    // Not marked done without room, so the next start tries again
    if (finished)
    {
        prefs.begin("analyzer", false);
        prefs.putString("catver", FW_VERSION);
        prefs.end();
    }

    Serial.printf("Catalog: %u rows removed, %lu bytes of frames\r\n",
                  catalogPrunedRows, (unsigned long)catalogPrunedBytes);
}

// Into the event log once it runs, see setup()
void catalogLogPrune()
{
    if (catalogFormatReset)
        logEventFrom("system", "IE catalog: written by an older "
                     "firmware in another layout, started empty");

    if (catalogPrunedRows)
        logEventFrom("system", "IE catalog: %u row%s of now decoded "
                     "or implausible elements removed, %lu kB of frames",
                     catalogPrunedRows, catalogPrunedRows == 1 ? "" : "s",
                     (unsigned long)((catalogPrunedBytes + 512) / 1024));

    if (catalogPruneNeeded)
        logEventFrom("system", "IE catalog: frames not cleaned up, the "
                     "copy needs %lu kB and %lu kB are free; tried again "
                     "at the next start",
                     (unsigned long)((catalogPruneNeeded + 1023) / 1024),
                     (unsigned long)(catalogPruneFree / 1024));
}

void catalogFrame(const struct MgmtFrameCopy &entry)
{
    if (!catalogEnabled || catalog == nullptr)
        return;

    CatalogFrame cf = {};

    if (!catalogScan(entry.data, entry.length, entry.channel, cf))
        return;

    if (cf.takerCount > 0)
    {
        catalogWriteFrame(entry);

        for (uint8_t i = 0; i < cf.takerCount; i++)
        {
            cf.takers[i]->frames++;
            cf.takers[i]->senderFrames[cf.takerSender[i]]++;
        }

        catalogUrgent = true;
    }
}

void processManagementQueue()
{
    while (mgmtRead != mgmtWrite)
    {
        MgmtFrameCopy &entry = mgmtQueue[mgmtRead];

        pcapStore(entry);

        beaconVersionFrame(entry);

        mgmtFrameUs = entry.us;

        catalogFrame(entry);

        parseManagementFrame(entry.data,
                             entry.length,
                             entry.rssi,
                             entry.channel);

        mgmtRead = (mgmtRead + 1) % MGMT_QUEUE_SIZE;
    }
}

void processDataQueue()
{
    while (dataRead != dataWrite)
    {
        DataFrameInfo &entry = dataQueue[dataRead];

        registerDataFrame(entry.sta,
                          entry.bssid,
                          entry.peer,
                          entry.rssi,
                          entry.channel,
                          entry.uplink,
                          entry.length);

        dataRead = (dataRead + 1) % DATA_QUEUE_SIZE;
    }
}

// ============================================================
// API: event log
// ============================================================

void sendLogJSON()
{
    outBegin();

    outStr("{\"total\":");
    outNum(logTotal);

    outStr(",\"entries\":[");

    if (eventLog != nullptr)
    {
        uint8_t start =
            (logCount < LOG_ENTRIES) ? 0 : logWrite;

        uint32_t nowEpoch = deviceEpoch();
        uint32_t nowUp    =
            (uint32_t)(esp_timer_get_time() / 1000000LL);

        // Newest first
        for (uint8_t i = 0; i < logCount; i++)
        {
            uint8_t idx =
                (start + logCount - 1 - i) % LOG_ENTRIES;

            if (i)
                outStr(",");

            LogEntry &e = eventLog[idx];

            /*
             * This boot: the age from the time since boot, the time
             * stored with the entry or filled in once the clock was
             * known; the wall clock and the uptime each cut to whole
             * seconds made a time worked out from both jump by one
             * from one request to the next. An earlier boot: its
             * wall time if the clock was known by then, else only
             * the time after that start.
             */
            bool    thisBoot = e.boot == bootCount;
            int32_t ago      = -1;
            uint32_t at      = 0;

            if (thisBoot)
            {
                ago = (int32_t)(nowUp - e.up);
                at  = e.epoch ? e.epoch : nowEpoch - ago;
            }
            else if (e.epoch)
            {
                at = e.epoch;

                if (pcapClockSource != 0 && nowEpoch >= e.epoch)
                    ago = (int32_t)(nowEpoch - e.epoch);
            }

            outStr("{\"at\":");
            {
                char buffer[40];

                if (thisBoot || e.epoch)
                    formatStamp(at, buffer, sizeof(buffer));
                else
                    snprintf(buffer, sizeof(buffer),
                             "%" PRIu32 " s after a start", e.up);

                outJSON(buffer);
            }

            outStr(",\"ago\":");
            outNum((long)ago);

            outStr(",\"boot\":");
            outNum(e.boot);

            outStr(",\"who\":");
            outJSON(e.who);

            outStr(",\"text\":");
            outJSON(e.text);

            outStr("}");
        }
    }

    outStr("]}");

    outEnd();
}
