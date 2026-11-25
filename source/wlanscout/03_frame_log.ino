// SPDX-License-Identifier: GPL-2.0-only WITH AdditionRef-wlanscout-linking-exception
// SPDX-FileCopyrightText: 2026 Dr.-Ing. Simon Wunderlich Systementwicklung & Beratung

// ============================================================
// Frame logs
//
// Frames stored back to back in PSRAM chunks of FRAME_CHUNK_BYTES.
// The chunks form a ring: writeChunk holds the newest frames, the
// chunk after it the oldest. A full log adds a chunk after
// writeChunk while PSRAM allows, otherwise it empties and reuses
// the oldest one. Only loop() touches a log, so there is no lock.
// ============================================================

static_assert(RING_CHUNKS_MAX <= FRAME_CHUNKS_MAX &&
                  ASSOC_CHUNKS_MAX <= FRAME_CHUNKS_MAX &&
                  BV_CHUNKS_MAX <= FRAME_CHUNKS_MAX &&
                  BLE_CHUNKS_MAX <= FRAME_CHUNKS_MAX,
              "a log cannot hold more than FRAME_CHUNKS_MAX chunks");

// Stored in front of every frame
struct FrameHead
{
    uint64_t us;          // esp_timer, microseconds since boot
    uint16_t length;
    uint16_t origLength;
    int8_t   rssi;
    uint8_t  channel;
    int8_t   noise;
    uint8_t  rxRate;
};

struct FrameChunk
{
    uint8_t *memory;
    uint32_t usedBytes;   // heads and padding included
    uint32_t frameCount;
    uint32_t frameBytes;  // the frames alone
};

struct FrameLog
{
    const char *name;
    uint8_t     maxChunks;
    uint32_t    psramReserve;   // PSRAM it leaves free for others
    uint8_t     chunkCount;
    uint8_t     writeChunk;
    uint32_t    frameCount;
    uint32_t    frameBytes;
    uint32_t    framesStored;   // since the last clear
    uint32_t    framesDropped;  // with a reused chunk
    FrameChunk  chunks[FRAME_CHUNKS_MAX];
};

FrameLog ringLog  = { "pcap ring",  RING_CHUNKS_MAX,  RING_RESERVE };
FrameLog assocLog = { "assoc ring", ASSOC_CHUNKS_MAX, ASSOC_RESERVE };

// Starts empty, the first version takes its first chunk
FrameLog versionLog = { "beacon versions", BV_CHUNKS_MAX, BV_RESERVE };

// BLE advertisements as link layer packets, see "BLE capture"
FrameLog bleLog = { "BLE log", BLE_CHUNKS_MAX, BLE_RESERVE };

// Head, frame and padding to keep the next head 8-aligned
uint32_t frameSlotSize(uint32_t frameLength)
{
    return (sizeof(FrameHead) + frameLength + 7) & ~7u;
}

uint8_t nextChunk(const struct FrameLog &log, uint8_t chunk)
{
    return (chunk + 1) % log.chunkCount;
}

bool psramAllowsAnotherChunk(const struct FrameLog &log)
{
    size_t freePsram    = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

    return freePsram >= FRAME_CHUNK_BYTES + log.psramReserve &&
           largestBlock >= FRAME_CHUNK_BYTES;
}

void updateLogAllocation(const struct FrameLog &log)
{
    setAllocation(log.name, (size_t)log.chunkCount * FRAME_CHUNK_BYTES);
}

// The new chunk goes right after writeChunk and becomes it, so the
// ring stays in order from oldest to newest
bool frameLogGrow(struct FrameLog &log)
{
    bool firstChunk = log.chunkCount == 0;

    if (log.chunkCount >= log.maxChunks)
        return false;

    if (!firstChunk && !psramAllowsAnotherChunk(log))
        return false;

    uint8_t *memory =
        (uint8_t *)heap_caps_malloc(FRAME_CHUNK_BYTES, MALLOC_CAP_SPIRAM);

    if (memory == nullptr)
        return false;

    uint8_t newChunk      = firstChunk ? 0 : log.writeChunk + 1;
    uint8_t chunksToShift = log.chunkCount - newChunk;

    memmove(&log.chunks[newChunk + 1], &log.chunks[newChunk],
            chunksToShift * sizeof(FrameChunk));

    log.chunks[newChunk] = { memory, 0, 0, 0 };

    log.chunkCount++;
    log.writeChunk = newChunk;

    updateLogAllocation(log);

    return true;
}

void frameLogDropChunk(struct FrameLog &log, struct FrameChunk &chunk)
{
    log.frameCount    -= chunk.frameCount;
    log.frameBytes    -= chunk.frameBytes;
    log.framesDropped += chunk.frameCount;

    chunk.usedBytes  = 0;
    chunk.frameCount = 0;
    chunk.frameBytes = 0;
}

// Keeps one empty chunk, frees the others
void frameLogClear(struct FrameLog &log)
{
    for (uint8_t i = 1; i < log.chunkCount; i++)
        heap_caps_free(log.chunks[i].memory);

    if (log.chunkCount > 0)
    {
        log.chunks[0]  = { log.chunks[0].memory, 0, 0, 0 };
        log.chunkCount = 1;
    }

    log.writeChunk    = 0;
    log.frameCount    = 0;
    log.frameBytes    = 0;
    log.framesStored  = 0;
    log.framesDropped = 0;

    updateLogAllocation(log);
}

// Walks a log from the oldest frame to the newest
struct FrameCursor
{
    const struct FrameLog *log;
    uint8_t                chunk;
    uint8_t                chunksLeft;
    uint32_t               offset;
};

void frameCursorStart(struct FrameCursor &cursor, const struct FrameLog &log)
{
    cursor.log        = &log;
    cursor.chunk      = log.chunkCount ? nextChunk(log, log.writeChunk) : 0;
    cursor.chunksLeft = log.chunkCount;
    cursor.offset     = 0;
}

bool frameCursorNext(struct FrameCursor &cursor, struct PcapView &view)
{
    const FrameLog &log = *cursor.log;

    while (cursor.chunksLeft > 0)
    {
        const FrameChunk &chunk = log.chunks[cursor.chunk];

        if (cursor.offset < chunk.usedBytes)
        {
            const FrameHead *head =
                (const FrameHead *)(chunk.memory + cursor.offset);

            view = { head->us, head->length, head->origLength, head->rssi,
                     head->channel, head->noise, head->rxRate,
                     (const uint8_t *)(head + 1) };

            cursor.offset += frameSlotSize(head->length);

            return true;
        }

        cursor.chunk  = nextChunk(log, cursor.chunk);
        cursor.offset = 0;
        cursor.chunksLeft--;
    }

    return false;
}

// Time of the oldest frame, 0 when empty
uint64_t frameLogOldest(const struct FrameLog &log)
{
    FrameCursor cursor;
    PcapView    view;

    frameCursorStart(cursor, log);

    return frameCursorNext(cursor, view) ? view.us : 0;
}

struct BeaconSlot
{
    bool       valid;
    uint8_t    bssid[6];
    PcapRecord rec;
};

BeaconSlot *beaconSlots = nullptr;    // PSRAM

/*
 * An element of a beacon as compared for its versions: a key for
 * its kind (the ID, 0x100 + the extension ID, or 0x8000 and the
 * vendor's OUI and type folded) and a hash of its content. The last
 * beacon's also counts how often each one changed of late.
 */
struct BvElem
{
    uint16_t key;
    uint8_t  changes;            // within the window below
    uint8_t  spare;
    uint32_t hash;
    uint32_t windowStart;        // millis(), of the first change
};

struct BvVersion
{
    uint64_t us;                 // its frame in versionLog
    uint64_t sinceUs;            // when it last became the current one
    uint32_t hash;
    uint16_t seq;                // 1 for the first beacon of the BSS
    uint16_t times;              // how often it became the current one
    uint16_t origLength;
};

struct BeaconTrack
{
    bool      used;
    uint8_t   bssid[6];
    uint8_t   prevCount;
    uint8_t   agreedCount;
    uint8_t   versionCount;
    uint8_t   current;           // index into versions
    uint8_t   volatileCount;
    uint8_t   pendingCount;
    uint16_t  nextSeq;
    uint32_t  lastSeen;          // millis()
    uint32_t  pendingHash;
    uint32_t  changes;           // switches between versions
    uint16_t  volatileKeys[BV_VOLATILE];
    BvElem    prev[BV_ELEMS];    // the last beacon
    BvElem    agreed[BV_ELEMS];  // the current version
    BvVersion versions[BV_KEEP];
};

BeaconTrack *beaconTracks = nullptr;  // PSRAM, MAX_BEACON_SLOTS

uint32_t beaconTotal = 0;

bool    pcapEnabled = true;
uint8_t pcapMask    = PCAP_BEACONS | PCAP_PROBEREQ | PCAP_ASSOC;

/*
 * There is no clock on board. The browser hands over its own
 * epoch once, everything before that is stamped relative to
 * boot. Deltas between frames are exact either way.
 */
/*
 * Offset between the boot timer and wall clock, in
 * microseconds.
 *
 * Seconds were not enough: the system clock and esp_timer
 * tick with different sub-second phases, so an offset
 * computed from two truncated second values flipped by one
 * every so often. That looked like a clock correction and was
 * counted as an NTP sync, five of them within four minutes of
 * uptime. At microsecond resolution the offset is stable.
 */
int64_t clockOffsetUs = 0;

/*
 * 0 = none, 1 = set by the browser, 2 = NTP, 3 = an AP's Time
 * Advertisement. NTP outranks the browser, the browser outranks
 * an AP: the AP's clock is only as good as its own NTP, one
 * seen here ran 15 s behind.
 */
uint8_t pcapClockSource = 0;

/*
 * Taking the clock from an AP, see noteApClock(). Two frames of
 * the same BSS have to agree before the clock is set; while an
 * AP stays the source the clock is renewed from all of them,
 * first after WIFI_CLOCK_FIRST_MS (once the sweep has been
 * round the channels), then every WIFI_CLOCK_INTERVAL_MS.
 */
#define WIFI_CLOCK_FIRST_MS    120000
#define WIFI_CLOCK_INTERVAL_MS 900000

uint32_t wifiClockWaitMs  = WIFI_CLOCK_FIRST_MS;
uint8_t  wifiClockApCount = 0;    // APs in the last median, 0 = one AP

uint8_t  wifiClockBssid[6];      // the AP the clock came from
uint32_t wifiClockLastSet = 0;   // millis

// Receive time of the management frame being parsed
uint64_t mgmtFrameUs = 0;

bool     ntpStarted   = false;
uint32_t lastNtpCheck = 0;

// When the clock was last corrected, and by how much
uint32_t ntpLastSync   = 0;      // device epoch seconds
int32_t  ntpLastAdjust = 0;      // milliseconds
uint32_t ntpSyncCount  = 0;

// The NTP server from DHCP option 42, empty without one
char ntpDhcpServer[48] = "";

#if ENABLE_ETHERNET

/*
 * lwIP writes the server of every DHCP ACK into SNTP slot 0 as
 * a bare address, without a name, and empties the other slots.
 * Taken from there, then the public servers are put back behind
 * it. True while slot 0 holds a server from DHCP.
 */
bool ntpTakeDhcpServer()
{
#if HAVE_DHCP_NTP
    const ip_addr_t *addr = esp_sntp_getserver(0);

    if (esp_sntp_getservername(0) != nullptr || addr == nullptr ||
        ip_addr_isany(addr))
        return false;

    char text[sizeof(ntpDhcpServer)];

    ipaddr_ntoa_r(addr, text, sizeof(text));

    if (ntpStarted && esp_sntp_getservername(1) == nullptr)
    {
        esp_sntp_setservername(1, NTP_SERVER1);
        esp_sntp_setservername(2, NTP_SERVER2);
    }

    if (strcmp(text, ntpDhcpServer) != 0)
    {
        strcpy(ntpDhcpServer, text);

        Serial.printf("NTP: server %s from DHCP\r\n", text);

        logEventFrom("system", "NTP server %s from DHCP", text);
    }

    return true;
#else
    return false;
#endif
}

// A new network: whatever its DHCP says, not the last one's
void ntpForgetDhcpServer()
{
#if HAVE_DHCP_NTP
    if (ntpDhcpServer[0] == 0)
        return;

    ntpDhcpServer[0] = 0;

    if (ntpStarted)
    {
        esp_sntp_setservername(0, NTP_SERVER1);
        esp_sntp_setservername(1, NTP_SERVER2);
        esp_sntp_setservername(2, nullptr);
    }
    else
        esp_sntp_setserver(0, nullptr);
#endif
}

#endif
