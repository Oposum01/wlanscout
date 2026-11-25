// SPDX-License-Identifier: GPL-2.0-only WITH AdditionRef-wlanscout-linking-exception
// SPDX-FileCopyrightText: 2026 Dr.-Ing. Simon Wunderlich Systementwicklung & Beratung

// ============================================================
// IE catalog
//
// For finding out what this sensor does not decode yet, out in
// the field: carried around without a network, it notes every
// element it does not know, every vendor element it does not
// read and every AKM or cipher suite it has no name for, and
// keeps them across reboots. Each entry holds a counter, when
// and in which frame types it was seen, one example with the
// element's first bytes, and the first few frames carrying it
// go into a pcap file for Wireshark.
//
// It lives in the LittleFS file system on the 3.4 MB "spiffs"
// data partition, together with the event log and the snapshots.
// Flashing the application leaves it alone; only a full image
// written at 0x0 or an erase wipes it.
// ============================================================

#define CATALOG_MAX         256
#define CATALOG_SAMPLE      48
#define CATALOG_FRAMES      3          // frames kept per sender
#define CATALOG_SENDERS     10         // senders kept per entry

bool catalogFormatReset = false;       // older layout found at start

/*
 * Set at start when the file system is below FS_SMALL_BYTES: the
 * 1.4 MB of the XIAO with two firmware slots (partitions_xiao.csv).
 * The catalog's pcap file and the snapshots then get less, see
 * CATALOG_PCAP_MAX, SNAP_MIN_FREE and SNAP_FRAMES_MAX.
 */
#define FS_SMALL_BYTES      (2 * 1024 * 1024)

bool fsSmall = false;

/*
 * The pcap file shares the partition with the snapshots, which come
 * first: a snapshot is written only with SNAP_MIN_FREE left, the
 * oldest going first, so with 1 MB of frames here fewer of them stay
 * once each carries the catalog's frames too (SNAP_FRAMES_MAX).
 * CATALOG_PRUNE_SLACK is the room catalogPrune() needs to copy them.
 */
#define CATALOG_PCAP_MAX    (fsSmall ? 327680 : 1024 * 1024)
#define CATALOG_PRUNE_SLACK (16 * 1024)    // LittleFS metadata
#define CATALOG_SAVE_MS     300000     // counters
#define CATALOG_URGENT_MS   5000       // new entries
#define CATALOG_MAGIC       0x32435357 // "WSC2", senders per entry

#define CATALOG_FILE        "/catalog.bin"
#define CATALOG_TMP         "/catalog.tmp"
#define CATALOG_PCAP        "/catalog.pcap"
#define CATALOG_PCAP_TMP    "/catalog.pcap.tmp"

enum CatalogKind : uint8_t
{
    CAT_ELEMENT = 0,     // key: element ID
    CAT_EXT     = 1,     // key: extension ID of element 255
    CAT_VENDOR  = 2,     // key: OUI << 8 | vendor type
    CAT_AKM     = 3,     // key: suite OUI << 8 | type
    CAT_CIPHER  = 4      // key: suite OUI << 8 | type
};

struct CatalogEntry
{
    uint8_t  kind;
    uint8_t  seenIn;         // SEEN_*
    uint8_t  channel;        // of the example
    uint8_t  frames;         // frames written to the pcap file
    uint32_t key;
    uint32_t count;

    /*
     * Device epoch and clock source (pcapClockSource) at first
     * and last sighting, and the boot they happened in. With no
     * clock the epoch is the uptime of that boot.
     */
    uint32_t firstAt;
    uint32_t lastAt;
    uint16_t firstBoot;
    uint16_t lastBoot;
    uint8_t  firstClock;
    uint8_t  lastClock;

    // The example: sender, its SSID, the element's first bytes
    uint8_t  mac[6];
    char     ssid[33];
    uint8_t  sampleLen;
    uint16_t fullLen;
    uint8_t  sample[CATALOG_SAMPLE];

    /*
     * The first senders seen with it and how many of their frames
     * are stored, so every device gets its own samples instead of
     * the first one taking all of them. senderCount stops at
     * CATALOG_SENDERS; sendersMore counts sightings from others
     * beyond that, which only says that there were more.
     */
    uint8_t  senderCount;
    uint8_t  senders[CATALOG_SENDERS][6];
    uint8_t  senderFrames[CATALOG_SENDERS];
    uint16_t sendersMore;
};

struct CatalogHeader
{
    uint32_t magic;
    uint16_t entrySize;
    uint16_t count;
};

CatalogEntry *catalog = nullptr;     // PSRAM

uint16_t catalogCount   = 0;
bool     catalogEnabled = false;     // persisted, "catalog"
bool     catalogFsOk    = false;
uint16_t bootCount      = 0;         // persisted, "boots"

bool     catalogDirty   = false;     // counters changed
bool     catalogUrgent  = false;     // something new
uint32_t catalogSavedAt = 0;
uint32_t catalogFull    = 0;         // sightings lost, table full
