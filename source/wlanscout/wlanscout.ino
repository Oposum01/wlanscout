// SPDX-License-Identifier: GPL-2.0-only WITH AdditionRef-wlanscout-linking-exception
// SPDX-FileCopyrightText: 2026 Dr.-Ing. Simon Wunderlich Systementwicklung & Beratung
// SPDX-FileContributor: Jan Fuchs <jf [at] simonwunderlich . de>

#include <algorithm>
#include <LittleFS.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <SPI.h>
#include <ETH.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_core_dump.h>
#include <esp_partition.h>
#include <lwip/sockets.h>
#include <esp_netif_net_stack.h>
#include <lwip/dhcp.h>
#include <lwip/prot/dhcp.h>
#include <lwip/tcpip.h>
#include <Preferences.h>
#include <esp_wifi.h>
#include <esp_heap_caps.h>
#include <driver/temperature_sensor.h>
#include <time.h>
#include <sys/time.h>

#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

/*
 * IEEE 802.15.4
 *
 * The C5 has the radio, but whether the IEEE802154 component
 * is compiled into the Arduino core build is another matter.
 * Set this to 0 if the link step complains about undefined
 * references to esp_ieee802154_*.
 */
#define ENABLE_IEEE802154 1

#if ENABLE_IEEE802154 && __has_include("esp_ieee802154.h")
#include "esp_ieee802154.h"
#define HAVE_IEEE802154 1
#else
#define HAVE_IEEE802154 0
#endif

/*
 * OUI vendor table.
 *
 * oui_table.h is generated from the IEEE MA-L registry by
 * tools/make_oui.py and must sit next to this sketch. It
 * needs a partition scheme with a large app partition,
 * e.g. "Huge APP" in the Arduino IDE.
 */
#include "oui_table.h"

/*
 * Forward declarations.
 *
 * The web interface used to sit in this file as one large raw
 * string, and the Arduino prototype generator stumbled over
 * it: functions defined after the literal were unknown to
 * code above it, and in the end the generated prototype block
 * was inserted into the middle of the string itself. The page
 * now lives in index_html.h, but keeping these declarations
 * costs nothing and documents the call order.
 */
void logEventFrom(const char *who, const char *fmt, ...);
void logRequest(const char *fmt, ...);
extern char statsPath[40];
String reqClientText();
extern bool outToUsb;
uint32_t suiteAt(const uint8_t *p);

/*
 * Configuration changes. What a beacon says about channel, width,
 * security, PHY, rates, country and roaming is the AP's
 * configuration and stays put; a change of it while the BSS keeps
 * running is worth seeing, and so is the old value. Only beacons
 * count (probe responses carry more), and a new state counts once
 * two beacons agree, so one odd frame changes nothing.
 */
struct CfgSnap
{
    uint8_t  channel;
    uint8_t  standard;
    uint16_t bandwidth;
    uint8_t  streams;
    uint8_t  basicRate;          // 500 kbit/s units
    uint16_t beaconInterval;
    char     country[4];
    uint8_t  pmf;                // 0 off, 1 capable, 2 required
    uint8_t  dtimPeriod;
    bool     rm11k;
    bool     bss11v;
    bool     ft11r;
    bool     wps;
    int8_t   bssColor;
    char     security[16];
    char     akm[64];
    char     pairwise[36];
    char     group[18];

    /*
     * The rest is read from the beacon's elements by cfgExtrasOf(),
     * not from the network entry: there much is kept from the last
     * frame that had it, or comes from probe responses, so a value
     * switched off would never show and one only in probe responses
     * would come and go.
     */
    char     ssid[33];           // "" hidden
    bool     ovr;                // RSNE Override (2) sent
    char     ovrAkm[64];
    char     ovrPairwise[36];
    uint8_t  ovrPmf;             // 0 off, 1 capable, 2 required
    bool     ovrH2E;             // RSNXE Override: SAE hash-to-element
    uint8_t  mboDisallowed;      // MBO Association Disallowed reason, 0 none
    uint8_t  powerConstraint;    // dB, 255 none
    uint8_t  tpe[12];            // the first Transmit Power Envelope
    uint8_t  tpeLen;
    uint8_t  wpsState;           // 0 none, 1 not configured, 2 configured
    bool     wpsLocked;          // AP Setup Locked
    uint16_t wpsMethods;         // Config Methods, 0 not in the beacon
    bool     oweTrans;
    uint8_t  oweBssid[6];
    char     oweSsid[33];
    bool     hs20;
    uint8_t  iwType;             // Interworking access network type, 255 none
    uint8_t  mldMac[6];          // all 0 without a Multi-Link element
    uint16_t mloLinks;           // link IDs of this MLD, own and in the RNR
    uint16_t basicRates;         // bit per entry of cfgRates[]
    bool     preAuth;
    uint8_t  acm;                // WMM admission control per AC, 255 no WMM
    bool     intolerant;         // HT 40 MHz Intolerant
    bool     meshGate;
    bool     meshAccepting;
    uint8_t  meshAuth;           // authentication protocol, 255 no mesh
    bool     mde;                // Mobility Domain element sent
    uint16_t mdid;               // its ID
    bool     ftOverDs;           // FT over the DS
};

/*
 * Fine Timing Measurement (802.11mc): a station measures its distance
 * to a responder from the time FTM frames take. Public Action frames,
 * category 4: action 32 the FTM Request of the station, 33 the FTM
 * frames of the responder. The request may carry FTM Parameters (206)
 * and Measurement Requests (38) for the responder's position (LCI,
 * type 8) and address (Location Civic, type 11); the responder's first
 * FTM frame its FTM Parameters and the Measurement Reports (39).
 */
struct FtmInfo
{
    bool    paramSeen;
    uint8_t param[9];
    bool    askLci;
    bool    askCivic;
    bool    lciSeen;
    uint8_t lci[16];             // the LCI field, decoded in the browser
    uint8_t civicLen;
    uint8_t civic[64];           // Location Civic field, RFC 4776 style
};

#define CFG_CHANGES 8

struct CfgChange
{
    uint32_t at;                 // millis()
    uint8_t  field;              // CFG_*
    char     from[40];
    char     to[40];
};

enum
{
    CFG_RESTART, CFG_CHANNEL, CFG_WIDTH, CFG_STANDARD, CFG_STREAMS,
    CFG_BASIC_RATE, CFG_BEACON_INTERVAL, CFG_COUNTRY, CFG_PMF,
    CFG_11K, CFG_11V, CFG_11R, CFG_WPS, CFG_COLOR, CFG_SECURITY,
    CFG_AKM, CFG_PAIRWISE, CFG_GROUP, CFG_FLAP, CFG_DTIM, CFG_CLONE,
    CFG_SSID, CFG_OVR, CFG_OVR_AKM, CFG_OVR_CIPHER, CFG_OVR_PMF,
    CFG_OVR_H2E, CFG_MBO, CFG_PWR_CONSTRAINT, CFG_TPE, CFG_WPS_STATE,
    CFG_WPS_LOCK, CFG_WPS_METHODS, CFG_OWE, CFG_OWE_BSSID, CFG_OWE_SSID,
    CFG_HS20, CFG_INTERWORKING, CFG_MLD, CFG_MLO_LINKS, CFG_BASIC_RATES,
    CFG_PREAUTH, CFG_ACM, CFG_INTOLERANT, CFG_MESH_GATE, CFG_MESH_ACCEPT,
    CFG_MESH_AUTH, CFG_MDID, CFG_FT_DS
};

/*
 * A BSS that goes back to the state before its last change within
 * this time is flapping: one entry that counts the switches, rather
 * than every field twice per switch.
 */
#define CFG_FLAP_MS 600000

/*
 * The same changes for all BSSs together, for the bell in the
 * page header: numbered, so the page knows which it has shown.
 * The number starts over with every start of the sensor.
 */
#define CFG_EVENTS 64

struct CfgEvent
{
    uint32_t seq;                // 0: empty
    uint32_t at;                 // millis()
    uint8_t  bssid[6];
    char     ssid[33];
    uint8_t  field;
    char     from[40];
    char     to[40];
};

CfgEvent *cfgEvents  = nullptr;  // PSRAM, see allocateTables()
uint32_t  cfgSeq     = 0;        // the newest entry
uint32_t  cfgVersion = 0;        // any entry added or updated

/*
 * BSSs whose changes stay out of the bell, chosen in the page and
 * kept in NVS: an AP that flaps by design need not ring it. Their
 * changes are still recorded and shown in the tooltip.
 */
#define CFG_MUTE_MAX 16

uint8_t cfgMuted[CFG_MUTE_MAX][6];
uint8_t cfgMutedCount = 0;

// The network entry carrying them is further down, see WiFiNetwork
void setupEthernet();
uint8_t countBeaconSlots();
void refreshPcapCounts();
const char *dhcpStatusText();
void applyPromiscuousFilter();
void setupWiFiScanner();
void allocateHistory();
void handleFavicon();
void handleRoot();
void handleNotFound();

// ============================================================
// W5500
// ============================================================

#define W5500_SCLK 23
#define W5500_MOSI 24
#define W5500_MISO 25
#define W5500_CS   26
#define W5500_RST  4
#define W5500_INT  -1

// ============================================================
// Ethernet addressing
//
// DHCP first. If no lease arrives within DHCP_TIMEOUT_MS the
// attempt is counted and DHCP is restarted; after
// DHCP_MAX_ATTEMPTS tries the static address below is used so
// the device stays reachable on a network without a server.
// Unplugging and replugging the cable starts over with DHCP.
// ============================================================

/*
 * 8 s per attempt was too tight. A managed switch port runs
 * through spanning tree listening and learning after a link
 * change, typically 30 s, and nothing passes during that
 * time. Six attempts of 15 s cover that with room to spare.
 */
#define DHCP_TIMEOUT_MS  15000
#define DHCP_MAX_ATTEMPTS 6

/*
 * Optional: try DHCP again while sitting on the static
 * address, in case a server shows up later. Off by default,
 * because a retry drops the static address for up to a
 * minute and a half and the device would be unreachable
 * meanwhile. A link change always returns to DHCP regardless.
 */
/*
 * After a link flap the W5500 can come back with a dead
 * transmit path: the DHCP client runs, the PHY reports link,
 * and not a single discover reaches the wire. Taking the
 * interface down and up again fixes it within a second, so
 * that is tried early rather than after the full timeout.
 */
#define DHCP_REINIT_AFTER 2       // failed attempts
#define DHCP_REINIT_MAX   3       // consecutive reinits
#define DHCP_REINIT_COOLDOWN_MS 60000

/*
 * Which frame types the radio hands to the callback.
 *
 * Without a filter every acknowledgement and every RTS/CTS
 * ends up there as well. In a busy environment those
 * outnumber everything else, and the resulting interrupt load
 * competes with the SPI traffic to the W5500 - which is what
 * tens of thousands of dropped frames alongside link flaps
 * look like. Management and data frames carry everything that
 * is actually evaluated; control frames only contributed to
 * the RSSI average. Set to 0 to receive everything again.
 */
#define FILTER_CONTROL_FRAMES 1

/*
 * Export schema version.
 *
 * Bumped whenever the shape of /api/export changes, so a
 * collector merging several sensors can tell what it is
 * looking at instead of guessing from the firmware version.
 */
#define EXPORT_SCHEMA 1

// Free text name for this sensor, kept in NVS
#define SENSOR_NAME_MAX 32

#define DHCP_RETRY_AFTER_STATIC_MS 0

#define ETH_STATIC_IP   IPAddress(192, 168, 1, 1)
#define ETH_STATIC_GW   IPAddress(192, 168, 1, 1)
#define ETH_STATIC_MASK IPAddress(255, 255, 255, 0)
#define ETH_STATIC_DNS  IPAddress(192, 168, 1, 1)

/*
 * SNTP
 *
 * Only the offset between the system clock and the boot timer
 * is taken from NTP. Every frame keeps its own microsecond
 * timestamp from esp_timer, so the deltas inside a capture
 * stay exact no matter how coarse the network time is.
 *
 * The NTP server handed out by DHCP (option 42) is not used,
 * the lwIP option for that is not enabled in the stock
 * Arduino build.
 */
#define NTP_SERVER1 "pool.ntp.org"
#define NTP_SERVER2 "time.nist.gov"

#define NTP_CHECK_INTERVAL_MS 5000

/*
 * How often SNTP asks again.
 *
 * The ESP-IDF default is one hour (CONFIG_LWIP_SNTP_UPDATE_DELAY).
 * Setting it explicitly removes the guesswork; 15 minutes is
 * plenty for timestamping a capture and still polite towards
 * a public pool server.
 */
#define NTP_SYNC_INTERVAL_MS 900000

#if __has_include("esp_sntp.h")
#include "esp_sntp.h"
#define HAVE_ESP_SNTP 1
#else
#define HAVE_ESP_SNTP 0
#endif

// ============================================================
// Scanner configuration
// ============================================================

#define CHANNEL_DWELL_MS 120

// Desired number of stored sweeps (PSRAM).
#define HISTORY_SIZE 12000

// Fallback when no PSRAM is available.
#define HISTORY_SIZE_FALLBACK 600

// Maximum number of detected WLAN networks
#define MAX_NETWORKS 160

// Deliver at most this many history rows to the browser
#define MAX_HISTORY_POINTS 400

/*
 * A network is dropped from the table after this time.
 *
 * 15 s was too tight: with all channels selected one sweep
 * takes nearly 5 s, and the 120 ms dwell does not always
 * catch a beacon. Three missed sweeps in a row were enough to
 * drop an AP that is sitting right there, which made its name
 * disappear from the stations view for a few seconds.
 */
#define NETWORK_TIMEOUT_MS 60000

// Observed stations
#define MAX_CLIENTS 100
#define CLIENT_TIMEOUT_MS 120000

// Randomised MACs without an association expire faster,
// otherwise the table fills up with one-shot addresses.
#define PROBE_ONLY_TIMEOUT_MS 30000

#define MAX_PROBE_SSIDS 8

// Recent BSSID changes kept per station, with timestamps
#define MAX_ROAM_EVENTS 8

// BSSID to SSID, learned from stations rather than beacons
#define MAX_BSS_NAMES 48

/*
 * 6 GHz networks, learned from the Reduced Neighbor Report
 * element (201) in 2.4 and 5 GHz beacons.
 *
 * Passive scanning of the 6 GHz band would be far too slow,
 * so an AP announces its co-located 6 GHz BSSs out of band.
 * Since this radio cannot tune above 5 GHz at all, that
 * report is the only thing we will ever see of them.
 */
#define MAX_SIXG_BSS 40

/*
 * pcap ring
 *
 * Management frames are kept as they came off the air so they
 * can be downloaded and opened in Wireshark. Fixed size slots
 * waste a little PSRAM but keep the ring trivial and safe.
 */
#define MGMT_QUEUE_SIZE 96
#define MGMT_FRAME_MAX  512

/*
 * The parser gets the whole frame, though, up to the largest
 * MMPDU the standard allows. 512 octets was the limit for both
 * before, and a Wi-Fi 7 association request is longer than that:
 * its own EHT Capabilities sit behind a long Multi-Link element
 * and were cut off, so such a station passed for Wi-Fi 6. The
 * association ring keeps full frames as well; they are rare and
 * the most valuable ones. Ring and beacon slots stay at
 * MGMT_FRAME_MAX and record the on-air length separately.
 */
#define MGMT_PARSE_MAX  2304

// Radiotap header put in front of every exported frame
#define RADIOTAP_LEN 15

/*
 * 6000 slots is about 3.2 MB of PSRAM. With the waterfall and
 * the energy history that still leaves roughly half of the
 * 8 MB unused, and it buys a much longer window before probe
 * and association requests are pushed out by beacons.
 */
#define PCAP_SLOTS 6000

#define PCAP_BEACONS  0x01   // beacon and probe response
#define PCAP_PROBEREQ 0x02   // probe request
#define PCAP_ASSOC    0x04   // association and reassociation request

/*
 * Everything else in the management class: authentication,
 * deauthentication, disassociation, association responses and
 * action frames. Action frames carry the interesting parts of
 * 802.11k, v and r, so this is where roaming decisions become
 * visible. They are only queued when this bit is set, except
 * for the ones the parser reads anyway: association responses
 * and AWDL action frames. The bit decides only what goes into
 * the capture.
 */
#define PCAP_OTHER    0x08

/*
 * Beacons do not go into the ring.
 *
 * One AP sends ten beacons per second and they are all alike,
 * so they used to push everything interesting out within a
 * couple of minutes. Instead the newest beacon of each BSSID
 * is held in its own slot and simply overwritten. The ring is
 * then left to the frames that actually differ from each
 * other, and reaches back hours instead of minutes.
 */
#define MAX_BEACON_SLOTS 160

/*
 * Association and reassociation requests get a ring of their
 * own.
 *
 * They are the most informative frames a station ever sends,
 * the full capability set rather than the subset a probe
 * carries, and they are rare. In the shared ring they were
 * pushed out by probe requests within the hour while the
 * table still marked the station green. Here they are only
 * ever displaced by other association requests, which in
 * practice means they stay for days.
 */
#define ASSOC_SLOTS 300

// Index space for the export: ring, beacons, assoc requests
#define PCAP_IDX_BEACON_BASE (PCAP_SLOTS)
#define PCAP_IDX_ASSOC_BASE  (PCAP_SLOTS + MAX_BEACON_SLOTS)
#define PCAP_IDX_TOTAL       (PCAP_SLOTS + MAX_BEACON_SLOTS + \
                              ASSOC_SLOTS)

// BLE
#define MAX_BLE_DEVICES 80
#define BLE_TIMEOUT_MS 90000
#define BLE_QUEUE_SIZE 32

/*
 * A scan is armed for this long and re-armed from loop().
 *
 * Kept short on purpose: the BLE library keeps every device
 * it sees in its own result list, and with randomised
 * addresses that list grows without bound. clearResults() on
 * every re-arm caps it, so a shorter window means a lower
 * peak. This is what the sawtooth in the heap log was.
 */
#define BLE_SCAN_DURATION_S 10

#define MODE_WIFI 0
#define MODE_BLE  1
#define MODE_ZB   2

// IEEE 802.15.4: channels 11..26, 5 MHz apart, 2 MHz wide
#define ZB_FIRST_CHANNEL 11
#define ZB_LAST_CHANNEL  26
#define ZB_NUM_CHANNELS  (ZB_LAST_CHANNEL - ZB_FIRST_CHANNEL + 1)

#define ZB_DWELL_MS 300

// Sub modes inside 802.15.4: decode frames, or run a fast
// energy sweep that needs no decodable signal at all.
#define ZB_SCAN_FRAMES 0
#define ZB_SCAN_ENERGY 1

/*
 * Energy detect duration.
 *
 * The minimum an 802.15.4 receiver needs is 8 symbols, which
 * is 128 us. The IDF takes this parameter as microseconds;
 * should a build interpret it as symbols the sweep simply
 * gets slower, it does not break.
 */
#define ZB_ED_DURATION 200

// Energy waterfall history, one row per completed sweep
#define ED_HISTORY_SIZE 20000
#define ED_MAX_POINTS 400
#define ZB_MAX_NODES 40
#define ZB_NODE_TIMEOUT_MS 180000
#define ZB_QUEUE_SIZE 16
#define ZB_HEADER_BYTES 24

// Firmware version, shown in the header and in /api/live
#define FW_VERSION "2.6.11"

// ============================================================
// HEAP PROBE  (diagnostic, remove once the culprit is known)
// ============================================================

/*
 * The internal-heap low watermark drops in rare bursts that the
 * 15 s heap line cannot attribute to anything. A sampling task
 * polls the heap faster than any single request can run, so the
 * dip is recorded together with whatever was executing at the
 * time. Set HEAP_PROBE to 0 to compile all of this out.
 */
#define HEAP_PROBE          1
#define HEAP_PROBE_PERIOD_MS 1

#if HEAP_PROBE

static const char *volatile probeLabel  = "idle";
static volatile uint32_t    probeIdleSince = 0;

/*
 * The request answered last and who asked: a low labelled "idle"
 * often comes just after one, while lwIP still holds its buffers.
 */
static const char *volatile probeLast = "";
static char                 probeWho[16] = "";
static TaskHandle_t         probeTarget = nullptr;

struct HeapLowRecord
{
    uint32_t watermark;      /* the allocator's own low mark */
    uint32_t freeNow;        /* free when the drop was noticed */
    uint32_t freeDma;        /* separates DMA buffers from ordinary heap */
    uint32_t idleForMs;      /* how long no handler had been running */
    uint32_t stackWords;
    uint32_t atMillis;
    char     label[40];
    char     last[40];
    char     who[16];
    bool     fresh;
};

static volatile uint32_t probeMinFree = 0xFFFFFFFFu;
static HeapLowRecord     probeLow     = {};

/* Only report a low that is meaningfully deeper than the last one. */
#define HEAP_PROBE_STEP_BYTES 2048u

static inline uint32_t internalFree()
{
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

/*
 * The allocator maintains this itself on every malloc and free, so it
 * catches dips far shorter than any polling interval. Sampling it
 * instead of the instantaneous free size is the whole point: we cannot
 * see the dip, but we can see that one happened, and roughly when.
 */
static inline uint32_t internalWatermark()
{
    return heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL |
                                           MALLOC_CAP_8BIT);
}

static void heapProbeTask(void *)
{
    for (;;)
    {
        uint32_t w = internalWatermark();

        if (probeMinFree - w >= HEAP_PROBE_STEP_BYTES)
        {
            uint32_t now = millis();

            probeMinFree = w;

            probeLow.watermark = w;
            probeLow.freeNow   = internalFree();
            probeLow.freeDma   = heap_caps_get_free_size(
                MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
            probeLow.stackWords = probeTarget
                                ? uxTaskGetStackHighWaterMark(probeTarget)
                                : 0;
            probeLow.atMillis  = now;
            probeLow.idleForMs = now - probeIdleSince;

            /* Copy, do not alias: the label may change immediately. */
            strncpy(probeLow.label,
                    (const char *)probeLabel,
                    sizeof(probeLow.label) - 1);
            probeLow.label[sizeof(probeLow.label) - 1] = 0;

            strlcpy(probeLow.last, (const char *)probeLast,
                    sizeof(probeLow.last));
            strlcpy(probeLow.who, probeWho, sizeof(probeLow.who));

            probeLow.fresh = true;
        }

        vTaskDelay(pdMS_TO_TICKS(HEAP_PROBE_PERIOD_MS));
    }
}

static void startHeapProbe()
{
    /* Above the Arduino loop task, so a busy handler cannot starve it. */
    xTaskCreate(heapProbeTask, "heapprobe", 2560, nullptr, 10, nullptr);
}

/*
 * Wraps a request handler so the sampler knows what was running.
 * Also reports the handler's own net effect, which separates a
 * leak (free does not return) from a transient peak.
 */
static void runProbed(const char *name, void (*fn)())
{
    if (!probeTarget)
        probeTarget = xTaskGetCurrentTaskHandle();

    uint32_t before = internalFree();

    strlcpy(probeWho, outToUsb ? "USB" : reqClientText().c_str(),
            sizeof(probeWho));

    probeLast  = name;
    probeLabel = name;
    fn();
    probeLabel     = "idle";
    probeIdleSince = millis();

    uint32_t after = internalFree();

    /*
     * lwIP still holds the response buffers when the handler
     * returns; they are freed once the client ACKs. Anything
     * below roughly 15 kB is that, not a leak.
     */
    if (before > after && before - after > 20480)
    {
        Serial.printf("[probe] %s did not return %.1f kB\r\n",
                      name, (before - after) / 1024.0);
    }
}

#define PROBED(fn) ([]() { runProbed(#fn, fn); })
#define PROBE_MARK(name) do { probeLabel = (name); } while (0)

#else   /* HEAP_PROBE */

#define PROBED(fn)       (fn)
#define PROBE_MARK(name) do { } while (0)
static void startHeapProbe() { }

#endif  /* HEAP_PROBE */


// ============================================================
// Channels
// ============================================================

const uint8_t channels24[] = {
    1, 2, 3, 4, 5, 6, 7,
    8, 9, 10, 11, 12, 13
};

/*
 * UNII-3 (149 to 165) and UNII-4 (169 to 177) are part of the
 * plan but are not selected at startup.
 *
 * Neither belongs to the European channel plan, which is why
 * a transmitter stops at 144 — but that rule is about
 * transmitting, and this device only listens. The driver
 * tunes all of them with the country set to DE, and there is
 * real traffic to find: imported equipment on UNII-3, and in
 * UNII-4 the C-V2X band, where a vehicle hotspot can show up.
 *
 * They stay unselected because eight extra dwells lengthen
 * every sweep by about a second, which is a poor trade when
 * most sites have nothing there. Tick them when looking.
 */
const uint8_t channels5[] = {
    36, 40, 44, 48,
    52, 56, 60, 64,
    100, 104, 108, 112,
    116, 120, 124, 128,
    132, 136, 140, 144,

    // UNII-3 and UNII-4, see the note above
    149, 153, 157, 161, 165,
    169, 173, 177
};

#define NUM_CHANNELS_24 (sizeof(channels24) / sizeof(channels24[0]))
#define NUM_CHANNELS_5  (sizeof(channels5) / sizeof(channels5[0]))
#define TOTAL_CHANNELS  (NUM_CHANNELS_24 + NUM_CHANNELS_5)

// ============================================================
// Selected channels
// ============================================================

bool channelSelected[TOTAL_CHANNELS];

/*
 * Fast sweep
 *
 * The plain sweep spends the same dwell time on every
 * selected channel, including the ones that have never
 * carried anything. The fast sweep narrows that down to:
 *
 * - channels on which a BSSID is currently known
 * - a coverage set for 2.4 GHz: 1, 5, 9 and 13
 *   These four are the non-overlapping 20 MHz channels in
 *   the EU band. A receiver parked on one of them still
 *   picks up beacons from its neighbours, and the DS
 *   Parameter Set in the beacon states the real channel, so
 *   an AP on channel 3 is still discovered and its channel
 *   then joins the plan on the next round.
 *   5 GHz has no such overlap, every 20 MHz channel there
 *   has to be visited on its own.
 * - one idle channel per sweep, rotating, so that a new
 *   network on a so far empty channel is still noticed
 */
bool fastSweep = false;

bool channelInPlan[TOTAL_CHANNELS];
bool channelVisited[TOTAL_CHANNELS];

uint16_t planCount      = 0;
uint16_t idleProbeIndex = 0;

static const uint8_t coverage24[] = { 1, 5, 9, 13 };

// Frames dropped on a full queue (defined further below)
extern volatile uint32_t mgmtDrops;
extern volatile uint32_t dataDrops;
extern volatile uint32_t fcsErrors;

/*
 * What the Basic Multi-Link element (802.11be, element 255/107)
 * says about a device. APs carry it in every beacon, stations in
 * their (re)association request. Decoded in the browser; only the
 * fields are kept here.
 */
// One snapshot on flash, see "Snapshots"
struct SnapInfo
{
    uint32_t seq;
    uint32_t epoch;
    uint32_t uptime;
    uint32_t bytes;
};

struct MloInfo
{
    bool     present;
    uint8_t  mldMac[6];   // MLD address, shared by all links
    int8_t   linkId;      // this AP's link, -1 if not given
    uint8_t  fields;      // 1 = EML caps present, 2 = MLD caps present
    uint16_t eml;         // EML Capabilities (EMLSR / EMLMR)
    uint16_t mldCaps;     // MLD Capabilities and Operations
    uint16_t linkMask;    // links named in per-STA profiles

    // The station address each per-STA profile gives for its link,
    // so the sensor can tell that a "new" station on another band
    // is only the same device on a further link
    uint8_t  linkCount;
    uint8_t  linkIds[3];
    uint8_t  linkMacs[3][6];

    // Supported Channels of the other links, from the per-STA
    // profiles: 2.4/5 GHz over the sensor's plan, 6 GHz by index
    // (channel - 1) / 4
    uint64_t profChans;
    uint64_t profChans6;

    // Per profile of a station's request: link ID, band in GHz
    // (2, 5, 6, 0 = not told), widest channel its capabilities
    // allow, and the NSTR indication bitmap: bit n set = this link
    // and link n cannot send and receive at the same time. The
    // band of the link the request itself came in on is reqBand.
    uint8_t  profCount;
    uint8_t  profLink[3];
    uint8_t  profBand[3];
    uint16_t profWidth[3];
    uint16_t profNstr[3];
    uint8_t  reqBand;
};

// ============================================================
// OUI / Vendor
//
// Lookups go against the generated oui_table.h, which is
// built from the IEEE MA-L registry. MA-M and MA-S blocks
// (28 and 36 bit) are not covered by that registry, so for
// those only the block holder would be named.
// ============================================================

const char *lookupVendor(const uint8_t *mac)
{
    // Bit 1 of the first byte: locally administered
    if (mac[0] & 0x02)
        return "randomised (LAA)";

    uint32_t key =
        ((uint32_t)mac[0] << 16) |
        ((uint32_t)mac[1] << 8) |
        (uint32_t)mac[2];

    const char *name = ouiLookupFull(key);

    return name != nullptr ? name : "";
}

/*
 * Vendor for a BSSID. Unlike a phone, an AP that sets the locally
 * administered bit has usually not randomised anything: it derives
 * further BSSIDs from its base MAC by setting that bit, for extra
 * SSIDs or, with MLO, for some of its links. So for a BSSID
 *
 *   1. if the AP belongs to an MLD with a universal MLD address,
 *      that address names the vendor reliably
 *   2. otherwise the OUI with the bit cleared is looked up; a hit
 *      is marked "(local)", since a truly random BSSID, a phone
 *      hotspot for instance, matches a real OUI now and then
 *   3. only without either is it called randomised
 *
 * Returns a static buffer in case 2, valid until the next call.
 */
const char *lookupBssidVendor(const uint8_t *bssid,
                              const struct MloInfo &mlo)
{
    if (!(bssid[0] & 0x02))
        return lookupVendor(bssid);

    if (mlo.present && !(mlo.mldMac[0] & 0x02))
    {
        const char *name = lookupVendor(mlo.mldMac);

        if (*name)
            return name;
    }

    uint32_t key =
        ((uint32_t)(bssid[0] & ~0x02) << 16) |
        ((uint32_t)bssid[1] << 8) |
        (uint32_t)bssid[2];

    const char *name = ouiLookupFull(key);

    if (name != nullptr)
    {
        static char buffer[40];

        snprintf(buffer, sizeof(buffer), "%s (local)", name);

        return buffer;
    }

    return "randomised (LAA)";
}

// ============================================================
// Live measurement
// ============================================================

struct ChannelMeasurement
{
    uint32_t packets;
    int16_t  peakRSSI;
    int32_t  rssiSum;
    uint32_t rssiCount;
};

ChannelMeasurement currentMeasurement[TOTAL_CHANNELS];

volatile uint32_t rxPackets   = 0;
volatile int16_t  rxPeakRSSI  = -127;
volatile int32_t  rxRSSISum   = 0;
volatile uint32_t rxRSSICount = 0;

portMUX_TYPE measurementMux = portMUX_INITIALIZER_UNLOCKED;

// ============================================================
// Compact waterfall history
// ============================================================

struct WaterfallEntry
{
    uint32_t timestamp;
    int8_t   rssi[TOTAL_CHANNELS];
};

WaterfallEntry *history = nullptr;

uint32_t historyCapacity   = 0;
uint32_t historyWriteIndex = 0;
uint32_t historyCount      = 0;

// ============================================================
// Scanner state
// ============================================================

uint16_t currentChannelIndex = 0;
uint32_t lastChannelSwitch   = 0;
uint32_t sweepCount          = 0;

// ============================================================
// PHY-Generation
//
// Wi-Fi 1 to 3 are names the Wi-Fi Alliance assigned
// retroactively, only 4 to 7 are official.
// ============================================================

#define PHY_UNKNOWN 0
#define PHY_11B     1
#define PHY_11A     2
#define PHY_11G     3
#define PHY_HT      4
#define PHY_VHT     5
#define PHY_HE      6
#define PHY_EHT     7

/*
 * Which PHY generations a BSS announces, as a set rather than
 * as the single highest one.
 *
 * An 802.11ax AP on 2.4 GHz normally still carries the DSSS
 * rates of 802.11b and the OFDM rates of 802.11g, because a
 * legacy client has to be able to associate. "Wi-Fi 6" alone
 * hides that, and whether b rates are still enabled matters:
 * they cost airtime for everyone on the channel.
 */
#define PHY_MODE_B  0x01
#define PHY_MODE_G  0x02
#define PHY_MODE_A  0x04
#define PHY_MODE_N  0x08
#define PHY_MODE_AC 0x10
#define PHY_MODE_AX 0x20
#define PHY_MODE_BE 0x40

// ============================================================
// BSSID / Network information
// ============================================================

// Vendor elements (OUI and type) kept per BSS, see collectVendorOuis()
#define VENDOR_OUI_MAX 6

// Written out by hand, the generated prototypes miss this one
void outVendorOuis(const uint8_t (*list)[4], uint8_t count);

/*
 * Drone Remote ID (ASTM F3411 / ASD-STAN EN 4709-002), as a drone
 * sends it in its beacons: vendor element FA:0B:BC type 0x0D, a
 * message counter, then a message pack of 25-octet messages. The
 * law requires it to be broadcast for anyone to read, the pilot's
 * position included. Of the serial number only the manufacturer
 * code is kept, its first four characters (ANSI/CTA-2063-A); the
 * operator ID is not read.
 */
struct DroneInfo
{
    bool     seen;
    uint8_t  uaType;            // Basic ID: 2 multirotor, ...
    char     maker[5];          // manufacturer code of the serial
    bool     haveLocation;
    uint8_t  status;            // 1 ground, 2 airborne, 3 emergency
    bool     heightAgl;         // height above ground, else takeoff
    int32_t  lat, lon;          // 1e-7 degrees, 0 unknown
    uint16_t altBaro;           // raw: value / 2 - 1000 m, 0 unknown
    uint16_t altGeo;
    uint16_t height;
    int32_t  speed;             // cm/s, -1 unknown
    int16_t  vspeed;            // cm/s
    int16_t  track;             // degrees, 361 unknown
    bool     haveOperator;
    int32_t  opLat, opLon;      // pilot, 1e-7 degrees
    uint16_t opAlt;             // raw as above
    uint8_t  euClass;           // category << 4 | class, 0 none
    char     selfId[24];        // free text, "Drone flight" say
    uint32_t at;                // millis of the last message pack
};

struct WiFiNetwork
{
    bool     valid;
    uint8_t  bssid[6];
    char     ssid[33];
    uint8_t  channel;
    uint16_t bandwidth;
    uint8_t  centerChannel;
    int8_t   rssi;

    char     security[16];
    char     pairwise[36];
    char     groupCipher[18];
    char     mgmtCipher[18];
    char     akm[64];

    uint8_t  standard;

    // Every generation the BSS announces, see PHY_MODE_*
    uint8_t  phyModes;

    bool     mfpCapable;
    bool     mfpRequired;
    bool     wps;

    // SSID learned from a directed probe or association
    // request rather than from the beacon itself
    bool     ssidResolved;

    /*
     * Name taken from the OWE Transition element of the
     * partner BSS. That is a beacon of the same AP rather
     * than a guess from station traffic, so it deserves to be
     * told apart from the two cases above.
     */
    bool     ssidFromOwe;

    // VHT elements in a 2.4 GHz beacon: not standard, some
    // vendors use them to announce proprietary 256-QAM rates
    bool     vht24;

    /*
     * HT Greenfield, bit 4 of the HT Capability Info field.
     *
     * A Greenfield preamble carries no legacy part, so an
     * 802.11a or g station cannot even see the frame to defer
     * to it. The mode was optional in 802.11n and was struck
     * from the standard in 802.11-2016, and nothing after HT
     * has an equivalent, so it is a rarity worth pointing out
     * rather than a column of its own.
     */
    bool     htGreenfield;

    // HT Cap Info bit 14: demands 20 MHz from every neighbour
    bool     fortyIntolerant;

    // Roaming support announced by the AP, mirrors the station side
    bool     rm11k;
    bool     bss11v;
    bool     ft11r;

    // Mobility Domain (54): its ID, and FT Capability and Policy
    // (bit 0 FT over the DS, bit 1 resource request protocol)
    uint16_t mdid;
    uint8_t  ftPolicy;

    /*
     * Fine Timing Measurement with this BSSID as the responder, see
     * parseFtm(): requests to it, FTM frames from it, the stations
     * ranging, and what its FTM frames say: the parameters it
     * granted, its position (LCI) and its address (Location Civic).
     */
    uint16_t ftmRequests;
    uint16_t ftmFrames;
    uint8_t  ftmPeers[4][6];
    uint8_t  ftmPeerCount;
    bool     ftmParamSeen;
    uint8_t  ftmParam[9];
    bool     ftmLciSeen;
    uint8_t  ftmLci[16];
    uint8_t  ftmCivicLen;
    uint8_t  ftmCivic[64];

    // The detail behind 11k and 11v, see rrmCapsOf() / wnmCapsOf()
    uint64_t rrmCaps;
    uint16_t wnmCaps;

    // Spatial streams from HT/VHT/HE capabilities, 0 = unknown
    uint8_t  streams;

    // Multi-Link element (802.11be), see parseMultiLink()
    MloInfo  mlo;

    // Elements the parser does not know, see elementKnown(), and
    // the frame types they came in (SEEN_*)
    uint16_t unknownIes[6];
    uint8_t  unknownSeen[6];
    uint8_t  unknownCount;

    // Transmit Power Envelope elements (195), raw, see tpeOf()
    uint8_t  tpe[2][12];
    uint8_t  tpeLen[2];

    // TPC Report (35): the power this frame was sent with, in
    // dBm, 127 = not sent. Power Constraint (32): dB the AP keeps
    // below the Country element's limit, 255 = not sent.
    int8_t   tpcPower;
    uint8_t  powerConstraint;

    // ERP Information (42), the octet as sent, 255 = not sent
    uint8_t  erp;

    // OUI and type of the vendor elements (221) the AP sends,
    // without the standards bodies, see collectVendorOuis()
    uint8_t  vendorOui[VENDOR_OUI_MAX][4];
    uint8_t  vendorOuiCount;

    /*
     * Extended BSS Load (193), 0 = not sent: MU-MIMO capable
     * stations, then spatial stream underutilization and the
     * busy time seen on the secondary 20, 40 and 80 MHz
     * channels, each scaled to 255.
     */
    bool     xloadPresent;
    uint16_t xloadMu;
    uint8_t  xload[4];

    /*
     * Time Advertisement (69): the AP's idea of UTC, kept as an
     * offset to esp_timer like clockOffsetUs, 0 = never sent.
     * Time Zone (98): its POSIX TZ string. Both are kept when a
     * frame comes without them, some APs put the zone only into
     * probe responses.
     */
    int64_t  apClockOffsetUs;
    char     apTz[40];

    // Several BSSs share this beacon (element 71)
    bool     multiBssid;

    /*
     * Channel Switch Announcement. csaChannel is the channel
     * the BSS is moving to and csaCount the number of beacons
     * left until it does; both zero when no switch is pending.
     */
    uint8_t  csaChannel;
    uint8_t  csaCount;

    // Lowest basic rate in units of 500 kbit/s, 0 if none
    uint8_t  basicRate;

    /*
     * The full basic rate set, same units. Every station has
     * to support all of these to join, and management frames
     * go out at one of them, so the set says more about the
     * airtime an AP costs than the single lowest value does.
     */
    uint8_t  basicRates[12];
    uint8_t  basicRateCount;

    /*
     * WPS detail, read from the attributes of the WPS element
     * rather than from its mere presence.
     *
     * wpsMethods is the Config Methods bitmask, wpsLocked the
     * AP Setup Locked flag that an AP raises after too many
     * failed PIN attempts, and wpsState 1 = not configured,
     * 2 = configured.
     */
    uint16_t wpsMethods;
    bool     wpsLocked;
    uint8_t  wpsState;

    // RSN Capabilities bit 0
    bool     preAuth;

    // A suite with a misplaced OUI (noteBadSuite()): 0 none,
    // 1 in the RSN element, 2 in the WPA element
    uint8_t  badSuiteIn;
    uint32_t badSuite;

    // A WPA element that offers neither PSK nor 802.1X (AKM
    // 00-50-F2-00), as some mesh points send beside an SAE RSN one
    bool     wpaNoAkm;

    // Hotspot 2.0 Indication element, 50:6F:9A type 0x10
    bool     hs20;

    // Interworking (107) as sent, decoded in the browser
    uint8_t  iw[9];
    uint8_t  iwLen;

    /*
     * Apple Device IE, vendor element 00:A0:40 type 0: sent by
     * accessories made for Apple devices, car head units with
     * wireless CarPlay for instance. Name, manufacturer and model
     * as the accessory gives them; acc is set once one was seen.
     */
    bool     acc;
    char     accName[33];
    char     accMaker[25];
    char     accModel[25];

    /*
     * Smaller things an AP may send, see parseApExtras(). Each is
     * kept from the last frame that had it.
     */
    bool     quietSeen;          // Quiet (40)
    uint8_t  quietPeriod;        // beacon intervals between quiets
    uint16_t quietDuration;      // TU
    uint16_t quietOffset;        // TU after the beacon
    int16_t  mptInterval;        // Measurement Pilot (66), -1 none
    bool     ibss;               // capability bit 1: ad-hoc, no AP
    int32_t  atimWindow;         // IBSS Parameter Set (6), TU, -1 none
    uint8_t  aac[26];            // BSS Available Admission Cap. (67)
    uint8_t  aacLen;
    bool     beaconProt;         // MME (76) or ext. capability 84
    uint8_t  advProto[4];        // Advertisement Protocol IDs (108)
    uint8_t  advProtoCount;
    char     apName[33];         // Cisco 133, MikroTik or Mist name
    char     vendorFirmware[64]; // firmware version, 00:30:1A type 1
    int8_t   acmMask;            // WMM ACM per AC, bit = ACI, -1 none

    /*
     * Wi-Fi Alliance elements, see parseWfaElements(): the RSNE
     * and RSNXE Override for newer clients, P2P and Wi-Fi Display,
     * MBO-OCE. And an HP printer's model from HP's element.
     */
    bool     ovrSeen;
    char     ovrAkm[64];
    char     ovrPairwise[36];
    uint8_t  ovrPmf;             // 0 none, 1 capable, 2 required
    bool     ovrH2E;             // RSNXE Override: SAE hash-to-element
    bool     wfaCapaSeen;        // Wi-Fi Alliance Capabilities
    uint8_t  wfaCapa;            // its first octet, WFA_CAPA_*
    bool     p2pSeen;
    uint8_t  p2pDevCap;
    uint8_t  p2pGroupCap;
    char     p2pName[33];
    int32_t  wfdInfo;            // WFD Device Information, -1 none
    bool     mboSeen;
    uint8_t  mboCap;             // AP Capability Indication
    uint8_t  mboDisallowed;      // Association Disallowed reason, 0 none
    int16_t  oceCtrl;            // OCE Capability Indication, -1 none
    uint8_t  mboNpc[8];          // non-preferred channels
    uint8_t  mboNpcCount;
    char     hpModel[33];
    char     hpNumber[13];
    int16_t  nanPref;            // NAN Master Indication, -1 none
    uint8_t  nanRandom;          //   and its random factor
    int16_t  nanAmPref;          // NAN Cluster: anchor master's, -1 none
    uint8_t  nanAmRandom;
    uint8_t  nanHops;            // hops to the anchor master, 0 it is

    char     wpsName[33];        // WPS Device Name (0x1011)
    char     wpsMaker[33];       // WPS Manufacturer (0x1021)
    char     wpsModel[33];       // WPS Model Name (0x1023)
    char     wpsModelNo[33];     // WPS Model Number (0x1024)
    int16_t  omn;                // Operating Mode Notification, -1 none

    /*
     * OWE Transition Mode, vendor element 50:6F:9A type 0x1C.
     *
     * Enhanced Open is deployed as a pair of BSSs: an open one
     * that legacy clients can still use, and a hidden OWE one
     * that capable clients move to. Each beacon names its
     * partner, which is the only way to tell that an open
     * network and a nameless BSSID next to it belong together.
     */
    bool     oweTrans;
    uint8_t  oweBssid[6];
    char     oweSSID[33];

    // 802.11s mesh, name taken from the Mesh ID element
    bool     mesh;
    uint8_t  meshPeerings;
    bool     meshGate;
    bool     meshAccepting;
    bool     meshForwarding;
    uint8_t  meshPathProto;
    uint8_t  meshMetric;
    uint8_t  meshAuth;

    // BSS Load (IE 11), -1 when absent
    int16_t  stations;
    int16_t  channelUtil;

    uint16_t beaconInterval;
    char     country[4];

    uint32_t firstSeen;
    uint32_t lastSeen;
    uint32_t beacons;

    // How much of this AP is currently in the capture buffer
    uint16_t pcapFrames;
    bool     pcapBeacon;

    // BSS colour from HE Operation (ext 36), -1 none; changes
    // announced in BSS Color Change Announcements (ext 42), one per
    // countdown however many beacons carry it
    int8_t   bssColor;
    bool     bssColorOff;
    int8_t   colorNext;          // -1 none announced
    uint8_t  colorChanges;
    uint32_t colorChangeAt;      // millis of the last announcement

    // Beacon timestamp: the AP's microsecond timer, which normally
    // runs from when the BSS started. Going back means a restart.
    uint64_t tsf;
    uint32_t tsfAt;              // millis it was read, 0 never
    uint8_t  tsfResets;
    uint32_t tsfResetAt;
    uint8_t  probeTsfZero;       // probe responses with timestamp 0
    uint8_t  probeTsfSet;        // and with one

    // BSS Max Idle Period (90), units of 1000 TU, 0 none
    uint8_t  dtimPeriod;         // TIM (5): beacons between DTIMs, 0 none
    bool     timMcast;           //   group traffic buffered
    uint8_t  timWaiting;         //   stations with frames buffered
    uint16_t maxIdle;
    uint8_t  maxIdleOpt;         // bit 0: protected keep-alive

    DroneInfo drone;

    // Configuration changes, see CfgSnap
    CfgSnap   cfg;               // the state last agreed on
    bool      cfgKnown;
    uint32_t  cfgPending;        // hash of a new state not yet agreed
    uint8_t   cfgPendingCount;
    CfgChange cfgChanges[CFG_CHANGES];   // ring, newest at cfgHead - 1
    uint8_t   cfgHead;
    uint16_t  cfgTotal;
    CfgSnap   cfgPrev;           // the state before the last change
    bool      cfgPrevKnown;
    uint32_t  cfgChangedAt;      // millis() of the last change
    uint16_t  cfgFlaps;          // switches while flapping, 0 none
    int8_t    cfgFlapSlot;       // its entry in cfgChanges, -1 none
    uint32_t  cfgFlapEvent;      // and in cfgEvents, its number

    /*
     * Two clocks under one BSSID: every device has its own TSF, and
     * a copied MAC address does not copy it. The offset between the
     * TSF in a frame and the time it was received stays put for one
     * device; a second one, alternating with it, is a second device.
     */
    int64_t   clkOff[2];         // TSF minus capture time, µs
    uint16_t  clkHits[2];
    uint16_t  clkSwitches;       // frames from the other clock than before
    uint8_t   clkLast;           // the clock of the last frame
    bool      clkReported;       // in the changes already

    /*
     * Gaps between two beacons heard back to back on its channel.
     * A beacon can come late on a busy channel, the next one then
     * early, but hardly ever less than half an interval after the
     * last: many such gaps mean a second sender under this BSSID.
     */
    uint64_t  beaconUs;          // capture time of the last beacon
    uint16_t  gapCount;          // gaps measured
    uint16_t  gapShort;          // of them under half an interval
    uint32_t  gapMinUs;          // the shortest
};

WiFiNetwork *networks = nullptr;      // PSRAM, see allocateTables()

// ============================================================
// Observed stations
//
// Sources:
// - data frames (ToDS/FromDS) -> associated station
// - probe requests -> unassociated stations as well
//
// peer = Addr3, the address behind the AP.
// ============================================================

struct ClientEntry
{
    bool     valid;
    bool     associated;

    uint8_t  mac[6];
    uint8_t  bssid[6];
    uint8_t  prevBssid[6];
    uint8_t  peer[6];

    // A BSSID change is only accepted after several frames
    // agree on it, see noteBssidChange()
    uint8_t  bssidCandidate[6];
    uint8_t  bssidCandidateCount;

    uint32_t roams;
    uint32_t roamsConfirmed;      // stated by an assoc request

    struct
    {
        uint32_t at;
        uint8_t  bssid[6];
        bool     confirmed;
        bool     ft;              // reassociation with an FT MIC
    } roamLog[MAX_ROAM_EVENTS];

    uint8_t  roamLogWrite;
    uint8_t  roamLogCount;

    int8_t   rssi;
    uint8_t  channel;

    uint32_t toAp;
    uint32_t fromAp;
    uint32_t toApBytes;
    uint32_t fromApBytes;

    uint32_t probes;

    /*
     * The outcome of the last association the sensor saw.
     *
     * An association response carries a status code: 0 is
     * success, anything else says why the AP turned the
     * station away. Seeing a station retry forever against a
     * BSS that keeps answering 17 ("too many stations") is a
     * different problem from one that never gets a reply at
     * all, and the two look identical without this.
     *
     * assocStatusAt is 0 while no response has been seen.
     */
    uint16_t assocStatus;

    // BSS Max Idle Period (90): asked for in the association
    // request, granted in the response; units of 1000 TU, 0 none
    uint16_t maxIdleAsked;
    uint8_t  maxIdleAskedOpt;
    uint16_t maxIdleGranted;
    uint8_t  maxIdleGrantedOpt;

    uint32_t roamsFt;             // by Fast BSS Transition
    uint32_t assocStatusAt;
    uint8_t  assocStatusBssid[6];
    uint16_t assocFailures;

    // Association comeback time in TU from the Timeout Interval
    // element (56, type 3) of a response with status 30, 0 if none
    uint32_t assocComeback;
    char     probeSSID[MAX_PROBE_SSIDS][33];

    // From probe request and (re)association request
    bool     sawAssoc;
    uint32_t assocFrames;
    char     assocSSID[33];

    uint8_t  standard;
    uint16_t maxWidth;
    uint8_t  streams;

    bool     rm11k;
    bool     bss11v;
    bool     ft11r;
    uint16_t mdid;               // the Mobility Domain it asked for

    // Fine Timing Measurement as the initiator, see parseFtm()
    uint16_t ftmRequests;
    uint16_t ftmAnswers;         // FTM frames sent to it
    uint8_t  ftmTargets[3][6];   // the responders it asked
    uint8_t  ftmTargetCount;
    bool     ftmAskLci;          // asked for the responder's position
    bool     ftmAskCivic;        //   and for its address
    bool     ftmParamSeen;
    uint8_t  ftmParam[9];        // FTM Parameters of its request
    bool     band6;

    // The detail behind 11k and 11v, see rrmCapsOf() / wnmCapsOf()
    uint64_t rrmCaps;
    uint16_t wnmCaps;

    // Multi-Link element of the association request, if any
    MloInfo  mlo;

    // Where the station can operate, see bandsOf() below
    uint64_t opClasses;   // global operating classes 81..137
    uint64_t suppChans;   // channels of the plan, as channelSelected[]
    uint64_t suppChans6;  // 6 GHz channels, bit (channel - 1) / 4

    // Elements the parser does not know, see elementKnown(), and
    // the frame types they came in (SEEN_*)
    uint16_t unknownIes[6];
    uint8_t  unknownSeen[6];
    uint8_t  unknownCount;

    // Vendor elements of its requests, OUI and type, see
    // collectVendorOuis(): who made the device or its Wi-Fi chip,
    // also behind a randomised MAC address
    uint8_t  vendorOui[VENDOR_OUI_MAX][4];
    uint8_t  vendorOuiCount;
    bool     wmm;
    bool     pmf;

    // Wi-Fi Alliance Capabilities (QoS Management, WFA_CAPA_*) and
    // the RSN Selection of the last association request: which of
    // RSNE, RSNE Override and Override 2 the station took
    bool     wfaCapaSeen;
    uint8_t  wfaCapa;
    bool     rsnSelSeen;
    uint8_t  rsnSel;

    char     akm[32];
    char     cipher[20];

    uint32_t firstSeen;
    uint32_t lastSeen;

    // Frames of this station currently in the capture buffer
    uint16_t pcapFrames;
};

ClientEntry *clients = nullptr;       // PSRAM

/*
 * AWDL (Apple Wireless Direct Link): Apple devices talk to each
 * other directly for AirDrop, AirPlay, Sidecar and the like, in
 * action frames of category 127, OUI 00:17:F2, type 8, sent to the
 * AWDL BSSID 00:25:00:FF:94:73, mostly on the social channels 6,
 * 44 and 149. Only while such a service is in use.
 *
 * The sender is the awdl0 interface with an address of its own,
 * random and not the one the device uses as a Wi-Fi station. What
 * is kept, see parseAwdl(): the device class from the Version TLV,
 * and from the Data Path State TLV the country, the BSS the device
 * is associated with and, when it says so, its station address.
 * The host name in the Arpa TLV is personal and not read.
 */
#define MAX_AWDL 32

struct AwdlEntry
{
    bool     valid;
    uint8_t  mac[6];             // the awdl0 address
    int8_t   rssi;
    uint8_t  channel;
    uint32_t frames;
    int16_t  devClass;           // Version TLV, -1 none
    uint8_t  version;            // major << 4 | minor
    char     country[3];         // "DE", empty when not sent
    bool     infraSeen;
    uint8_t  infraBssid[6];
    uint8_t  infraChannel;
    bool     staSeen;
    uint8_t  sta[6];             // its address as a Wi-Fi station
    uint32_t firstSeen;
    uint32_t lastSeen;
};

AwdlEntry awdlDevices[MAX_AWDL];

/*
 * Stations that were seen associating, kept for good.
 *
 * The station table above ages entries out after
 * CLIENT_TIMEOUT_MS, which is what you want for a live view.
 * An association request is a different kind of event though:
 * it names the station, the BSS and usually the SSID in clear,
 * and it happens rarely. Losing that two minutes later throws
 * away the most informative thing the sensor ever sees, so a
 * copy is kept here and never times out.
 *
 * When the table is full the least recently active entry is
 * reused and assocHistoryDropped counts how often that
 * happened, so the UI can say that the record is no longer
 * complete.
 */
#define MAX_ASSOC_HISTORY 120

/*
 * A full copy of the station entry, not a reduced one.
 *
 * The live table and this one show the same columns and the
 * same pcap links, so keeping the same structure means one
 * renderer serves both. At roughly 400 bytes per station the
 * whole table is about 47 kB of PSRAM, which is nothing next
 * to the capture ring.
 */
struct AssocHistory
{
    bool        valid;

    ClientEntry sta;

    // Absolute, so the record still means something days later
    uint32_t    firstEpoch;
    uint32_t    lastEpoch;
};

AssocHistory *assocHistory = nullptr;   // PSRAM

uint32_t assocHistoryDropped = 0;


/*
 * Names learned from stations.
 *
 * A directed probe request and an association request both
 * name the network in clear. That works even when the AP
 * itself was never heard: out of range, on a channel that is
 * not being swept, or simply silent. Such a name would be
 * lost if it were only ever written into the beacon table,
 * so it is kept here as well.
 */
struct BssName
{
    bool     valid;
    uint8_t  bssid[6];
    char     ssid[33];
    bool     fromAssoc;
    uint32_t lastSeen;
};

BssName *bssNames = nullptr;          // PSRAM

// Every allocation is recorded so the settings page can show
// where the memory went
struct AllocInfo
{
    char     name[20];
    uint32_t bytes;
    bool     psram;
};

AllocInfo allocInfo[16];

uint8_t allocInfoCount = 0;

void noteAllocation(const char *name, size_t bytes, bool psram)
{
    if (allocInfoCount >= 16)
        return;

    AllocInfo &a = allocInfo[allocInfoCount++];

    memset(&a, 0, sizeof(a));

    strncpy(a.name, name, sizeof(a.name) - 1);

    a.bytes = (uint32_t)bytes;
    a.psram = psram;
}

/*
 * Event log
 *
 * Who changed what, and when. With several people poking at
 * the same analyzer, a changed channel list or a switched
 * mode is otherwise impossible to attribute.
 */
#define LOG_ENTRIES 80

/*
 * Entries keep the time since boot and the boot they belong to.
 * Events from before the first clock sync would otherwise be
 * stamped 1970 forever; for this boot their wall time is worked
 * out when the log is read, from whatever the clock knows by then,
 * and once the clock is known it is written into them (epoch), so
 * that it is still there after a restart.
 *
 * The log survives restarts and power cuts: every entry is
 * appended to LOG_FILE at once, see logPersist().
 */
struct LogEntry
{
    uint32_t up;          // seconds since boot
    uint32_t epoch;       // wall clock, 0 while not known
    uint16_t boot;        // bootCount then
    char     who[16];     // client address, or "system"
    char     text[88];
};

LogEntry *eventLog = nullptr;     // PSRAM

uint8_t logWrite = 0;
uint8_t logCount = 0;

uint32_t logTotal = 0;

extern bool     catalogFsOk;
extern uint16_t bootCount;

bool logLoaded      = false;   // the file is read, appends go there
bool logClockFilled = false;   // this boot's entries have their epoch

struct SixGhzBss
{
    bool     valid;
    uint8_t  bssid[6];
    uint8_t  channel;
    uint8_t  opClass;
    uint32_t shortSsid;
    bool     haveShortSsid;
    bool     sameSsid;
    bool     coLocated;
    bool     unsolicited;
    int8_t   psd;              // 127 = not reported
    uint8_t  reporter[6];
    // Name and channel of the reporting AP when it last reported:
    // it may leave the network table well before this entry does
    char     reporterSsid[33];
    uint8_t  reporterChannel;
    char     ssid[33];
    uint32_t lastSeen;
    uint32_t reports;
};

SixGhzBss *sixGhz = nullptr;          // PSRAM

struct PcapRecord
{
    uint64_t us;          // esp_timer, microseconds since boot
    uint16_t length;      // octets kept in data
    uint16_t origLength;  // octets on the air, may be more
    int8_t   rssi;
    uint8_t  channel;
    uint8_t  data[MGMT_FRAME_MAX];
};

// The same for association requests, which are kept whole
struct AssocRecord
{
    uint64_t us;
    uint16_t length;
    uint16_t origLength;
    int8_t   rssi;
    uint8_t  channel;
    uint8_t  data[MGMT_PARSE_MAX];
};

// One record of any of the three buffers, as the export sees it
struct PcapView
{
    uint64_t       us;
    uint16_t       length;
    uint16_t       origLength;
    int8_t         rssi;
    uint8_t        channel;
    const uint8_t *data;
};

PcapRecord *pcapRing = nullptr;       // PSRAM

struct BeaconSlot
{
    bool       valid;
    uint8_t    bssid[6];
    PcapRecord rec;
};

BeaconSlot *beaconSlots = nullptr;    // PSRAM

uint32_t beaconTotal = 0;

AssocRecord *assocRing = nullptr;     // PSRAM

uint32_t assocWrite = 0;
uint32_t assocCount = 0;
uint32_t assocTotal = 0;

// Export index, holds ring and beacon slots together
uint16_t *pcapIndex = nullptr;        // PSRAM

uint32_t pcapWrite = 0;
uint32_t pcapCount = 0;
uint32_t pcapTotal = 0;

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
uint8_t  wifiClockCandBssid[6];
int64_t  wifiClockCand    = 0;

// Receive time of the management frame being parsed
uint64_t mgmtFrameUs = 0;

bool     ntpStarted   = false;
uint32_t lastNtpCheck = 0;

// When the clock was last corrected, and by how much
uint32_t ntpLastSync   = 0;      // device epoch seconds
int32_t  ntpLastAdjust = 0;      // milliseconds
uint32_t ntpSyncCount  = 0;

// ============================================================
// Ethernet / HTTP
// ============================================================

WebServer server(80);

// False without a W5500: the sensor then runs on USB alone
bool ethPresent = false;

/*
 * The sensor's id and MAC address: the W5500's address. Without
 * a W5500 the same value is derived the way its driver does, the
 * chip's Ethernet MAC made locally administered, so the id stays
 * the same either way.
 */
String sensorMacText()
{
    if (ethPresent)
        return ETH.macAddress();

    uint8_t eth[6];
    uint8_t mac[6];

    esp_read_mac(eth, ESP_MAC_ETH);
    esp_derive_local_mac(mac, eth);

    char text[18];

    snprintf(text, sizeof(text), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    return String(text);
}

// ============================================================
// Requests over Ethernet or USB
// ============================================================
//
// The same API answers on the web server and on the chip's own
// USB port (USB Serial/JTAG, the connector marked USB). A phone
// with the sensor on its USB-C port opens the page from a static
// site and talks to it with WebUSB, without Ethernet. The log
// stays on the UART port.
//
// Handlers take their arguments and send their answer through
// the req* functions below, which go to whichever side asked.
//
// USB protocol. A request is one line:
//
//     GET /api/networks?range=60000
//
// The answer starts with 0x02 and a header line, status code,
// content type and an optional file name, then the body in
// chunks of a two-byte length (little endian) and the bytes; a
// chunk of length 0 ends it:
//
//     0x02 "200 application/json\n" <len><bytes> ... 00 00
//
// Binary bodies (pcap) need no escaping that way.

// The core only creates HWCDCSerial with "USB CDC On Boot",
// which would move the log there
HWCDC usbPort;

// Set while a request from USB is answered
bool outToUsb = false;

// Set while a snapshot is written: answers go to this file, the
// arguments come from the same arrays as for USB
File *outFile = nullptr;

/*
 * What a snapshot keeps, see "Snapshots"; status and channel plan
 * always. Stored in the settings.
 */
#define SNAP_NETWORKS   0x01    // networks with the 6 GHz list
#define SNAP_STATIONS   0x02
#define SNAP_ASSOC      0x04    // association history
#define SNAP_SPECTRUM   0x08    // live channel data and history
#define SNAP_CATALOG    0x10
#define SNAP_BLE        0x20
#define SNAP_ZB         0x40    // 802.15.4 nodes and energy
#define SNAP_ALL        0x7F

// Without a stored choice: everything but the spectrum history, the
// biggest part, and BLE and 802.15.4, which are empty in Wi-Fi mode
#define SNAP_DEFAULT    (SNAP_ALL & ~(SNAP_SPECTRUM | SNAP_BLE | SNAP_ZB))

uint8_t snapParts = SNAP_DEFAULT;

// Arguments from the arrays below rather than the web server
#define REQ_OWN_ARGS (outToUsb || outFile != nullptr)

#define USB_ARGS_MAX 16

String  usbArgNames[USB_ARGS_MAX];
String  usbArgValues[USB_ARGS_MAX];
uint8_t usbArgCount = 0;

// The server answers with a length or chunked; chunked needs an end
bool reqChunked = false;

uint8_t reqArgs()
{
    return REQ_OWN_ARGS ? usbArgCount : (uint8_t)server.args();
}

String reqArgName(uint8_t i)
{
    return REQ_OWN_ARGS ? usbArgNames[i] : server.argName(i);
}

String reqArgValue(uint8_t i)
{
    return REQ_OWN_ARGS ? usbArgValues[i] : server.arg(i);
}

bool reqHasArg(const char *name)
{
    if (!REQ_OWN_ARGS)
        return server.hasArg(name);

    for (uint8_t i = 0; i < usbArgCount; i++)
        if (usbArgNames[i] == name)
            return true;

    return false;
}

String reqArg(const char *name)
{
    if (!REQ_OWN_ARGS)
        return server.arg(name);

    for (uint8_t i = 0; i < usbArgCount; i++)
        if (usbArgNames[i] == name)
            return usbArgValues[i];

    return "";
}

// Who asked, for the event log
String reqClientText()
{
    return outToUsb ? String("usb")
                    : server.client().remoteIP().toString();
}

void usbWriteAll(const uint8_t *data, size_t length)
{
    while (length > 0)
    {
        size_t done = usbPort.write(data, length);

        // Nobody reading: the transmit timeout gave up
        if (done == 0)
            return;

        data   += done;
        length -= done;
    }
}

/*
 * What goes out over the network is gathered here and written in
 * pieces of one TCP segment, a chunk header included, with one
 * write. WebServer::sendContent() wrote every piece of about 1 kB
 * as three: chunk header, data, CRLF, and a pcap went out three
 * writes a frame, each a packet of its own over SPI to the W5500.
 * /api/networks (54 kB) took 160 packets and a quarter of a second
 * in which loop() read no frames.
 */
// One full segment of 1436 bytes (CONFIG_LWIP_TCP_MSS) with the
// chunk header ("595\r\n") and the CRLF after the data
#define TX_DATA (1436 - 7)

static char   txBuf[8 + TX_DATA + 2];   // room for "b40\r\n" in front
static size_t txLen = 0;

/*
 * A client that stops reading in the middle of an answer - a VPN
 * switched off, a laptop gone to sleep - sends no reset, its
 * packets just stop. NetworkClient::write() then waits up to 10 s
 * for room (ten selects of one second) and gives up, and the next
 * piece waited again: a 60 kB answer held loop() for almost five
 * minutes, no frames read, no other client let in. The pieces now
 * go out through txWrite(), which gives up after TX_STALL_MS
 * without progress, and after the first that does not go through
 * the answer is given up. Meanwhile the management queue fills up,
 * so the wait is kept short: a client on the LAN or over a VPN
 * makes room within milliseconds.
 */
#define TX_STALL_MS 2000
static bool     txAborted = false;
static uint32_t txSent    = 0;
static char     txWho[16] = "";    // the client, kept before a failed write can stop it
static bool     txClosed  = false; // the client closed, rather than stopped reading

/*
 * Answers given up since the start, for the CPU section in
 * Settings. They are not logged as events: a client going away is
 * not the sensor's problem and would push the rest out of the log.
 */
uint32_t txGivenUp     = 0;
uint32_t txGivenUpAt   = 0;        // millis() of the last one
char     txGivenUpWho[16] = "";

static size_t txWrite(const uint8_t *p, size_t n)
{
    int fd = server.client().fd();

    if (fd < 0)
        return 0;

    size_t   sent = 0;
    uint32_t progress = millis();

    while (sent < n)
    {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(fd, &set);

        struct timeval tv = { 0, 100000 };

        int ready = select(fd + 1, nullptr, &set, nullptr, &tv);

        if (ready < 0)
        {
            txClosed = true;
            break;
        }

        if (ready > 0)
        {
            int res = send(fd, p + sent, n - sent, MSG_DONTWAIT);

            if (res > 0)
            {
                sent    += res;
                progress = millis();
                continue;
            }

            if (res < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
            {
                txClosed = true;
                break;
            }
        }

        if (millis() - progress > TX_STALL_MS)
            break;
    }

    return sent;
}

static void txFlush()
{
    if (txLen == 0)
        return;

    char  *data = txBuf + 8;
    char  *from = data;
    size_t n    = txLen;

    if (reqChunked)
    {
        char head[8];

        int h = snprintf(head, sizeof(head), "%x\r\n", (unsigned)txLen);

        from = data - h;
        memcpy(from, head, h);

        data[txLen]     = '\r';
        data[txLen + 1] = '\n';

        n = h + txLen + 2;
    }

    txLen = 0;

    if (txAborted)
        return;

    size_t done = txWrite((const uint8_t *)from, n);

    txSent += done;

    if (done == n)
        return;

    txAborted = true;

    txGivenUp++;
    txGivenUpAt = millis();
    strlcpy(txGivenUpWho, txWho, sizeof(txGivenUpWho));

    Serial.printf("[tx] answer to %s given up after %lu kB, the client %s "
                  "(%s)\r\n", txWho, (unsigned long)(txSent / 1024),
                  txClosed ? "closed the connection" : "stopped reading",
                  statsPath);

    server.client().stop();
}

/*
 * Start an answer. length -1 when it is not known up front;
 * filename makes the browser save it.
 */
void reqStart(int code, const char *type, int32_t length,
              const char *filename)
{
    // A snapshot keeps the body only
    if (outFile)
        return;

    if (outToUsb)
    {
        String head = String((char)0x02) + code + " " + type;

        if (filename)
            head += String(" ") + filename;

        head += "\n";

        usbWriteAll((const uint8_t *)head.c_str(), head.length());

        return;
    }

    reqChunked = (length < 0);

    server.setContentLength(length < 0 ? CONTENT_LENGTH_UNKNOWN
                                       : (size_t)length);

    if (filename)
        server.sendHeader("Content-Disposition",
                          String("attachment; filename=") + filename);

    server.send(code, type, "");

    txLen     = 0;
    txSent    = 0;
    txAborted = false;
    txClosed  = false;

    strlcpy(txWho, reqClientText().c_str(), sizeof(txWho));
}

void reqContent(const void *data, size_t length)
{
    if (outFile)
    {
        outFile->write((const uint8_t *)data, length);
        return;
    }

    if (!outToUsb)
    {
        const char *p = (const char *)data;

        while (length > 0)
        {
            size_t part = std::min(length, (size_t)TX_DATA - txLen);

            memcpy(txBuf + 8 + txLen, p, part);

            txLen  += part;
            p      += part;
            length -= part;

            if (txLen == TX_DATA)
                txFlush();
        }

        return;
    }

    const uint8_t *p = (const uint8_t *)data;

    while (length > 0)
    {
        uint16_t part = length > 4096 ? 4096 : (uint16_t)length;
        uint8_t  head[2] = { (uint8_t)(part & 0xFF), (uint8_t)(part >> 8) };

        usbWriteAll(head, 2);
        usbWriteAll(p, part);

        p      += part;
        length -= part;
    }
}

void reqDone()
{
    if (outFile)
        return;

    if (outToUsb)
    {
        uint8_t end[2] = { 0, 0 };

        usbWriteAll(end, 2);
        usbPort.flush();

        return;
    }

    txFlush();

    if (reqChunked && !txAborted)
        server.sendContent("");

    reqChunked = false;
}

// A whole answer at once
void reqSend(int code, const char *type, const String &body)
{
    if (outFile)
    {
        outFile->write((const uint8_t *)body.c_str(), body.length());
        return;
    }

    if (!outToUsb)
    {
        server.send(code, type, body);
        return;
    }

    reqStart(code, type, body.length(), nullptr);
    reqContent(body.c_str(), body.length());
    reqDone();
}

Preferences prefs;

char sensorName[SENSOR_NAME_MAX] = "";

/*
 * Display offset from UTC in minutes.
 *
 * Everything inside the device stays on UTC: the pcap
 * timestamps, the export, the clock arithmetic. This only
 * shifts what is written out for people to read, so a capture
 * taken here still lines up with one taken anywhere else.
 */
int16_t tzMinutes = 0;

/*
 * Format an epoch for display, with the offset applied.
 */
void formatStamp(uint32_t epoch, char *buffer, size_t size)
{
    time_t shifted = (time_t)epoch + tzMinutes * 60;

    struct tm t;
    gmtime_r(&shifted, &t);

    snprintf(buffer, size,
             "%04d-%02d-%02d %02d:%02d:%02d",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
             t.tm_hour, t.tm_min, t.tm_sec);
}

/*
 * "UTC" or "UTC+02:00", for whatever shows a stamp.
 */
void formatTzLabel(char *buffer, size_t size)
{
    if (tzMinutes == 0)
    {
        snprintf(buffer, size, "UTC");
        return;
    }

    int m = tzMinutes < 0 ? -tzMinutes : tzMinutes;

    snprintf(buffer, size, "UTC%c%02d:%02d",
             tzMinutes < 0 ? '-' : '+', m / 60, m % 60);
}

void loadSensorName()
{
    prefs.begin("analyzer", true);

    String name = prefs.getString("name", "");

    prefs.end();

    memset(sensorName, 0, sizeof(sensorName));

    strncpy(sensorName, name.c_str(), SENSOR_NAME_MAX - 1);
}

void saveSensorName(const char *name)
{
    memset(sensorName, 0, sizeof(sensorName));

    strncpy(sensorName, name, SENSOR_NAME_MAX - 1);

    prefs.begin("analyzer", false);

    prefs.putString("name", sensorName);

    prefs.end();
}

/*
 * Settings that survive a reboot.
 *
 * Only what someone deliberately set: the channel selection,
 * the sweep mode, what goes into the capture, the display
 * offset and active or passive BLE scan. Everything else is measurement and has no business
 * in flash.
 *
 * Written only when one of them actually changes, never on a
 * poll, so the flash sees a handful of writes a session
 * rather than thousands.
 */
void saveSettings();

// Defined with the BLE code further down
extern bool bleActiveScan;
extern bool catalogEnabled;

uint64_t channelMask()
{
    uint64_t mask = 0;

    for (uint16_t i = 0; i < TOTAL_CHANNELS && i < 64; i++)
    {
        if (channelSelected[i])
            mask |= (1ULL << i);
    }

    return mask;
}

/*
 * The default channel plan: what the sensor starts with and what
 * the views offer to go back to after a click narrowed the
 * sweep. Saved from the scan panel; until then it is the startup
 * default set up before loadSettings() runs. The selection in
 * use is not stored, so a sweep narrowed to one channel does not
 * outlive a restart.
 */
uint64_t defaultMask = 0;

void loadSettings()
{
    defaultMask = channelMask();

    prefs.begin("analyzer", false);

    uint64_t def = prefs.getULong64("defmask", 0);

    /*
     * Up to 1.51.6 the selection in use was stored as "chmask"
     * and restored at start. Taken over once as the default
     * plan, so an update keeps what the sensor scanned.
     */
    if (prefs.isKey("chmask"))
    {
        if (def == 0)
        {
            def = prefs.getULong64("chmask", 0);

            if (def != 0)
                prefs.putULong64("defmask", def);
        }

        prefs.remove("chmask");
    }

    if (def != 0)
        defaultMask = def;

    fastSweep   = prefs.getBool("fast", false);
    pcapEnabled = prefs.getBool("cap", true);
    pcapMask    = (uint8_t)prefs.getUChar("capmask", pcapMask);
    tzMinutes   = (int16_t)prefs.getShort("tz", 0);

    bleActiveScan = prefs.getBool("bleactive", false);

    catalogEnabled = prefs.getBool("catalog", false);

    snapParts = prefs.getUChar("snapparts", SNAP_DEFAULT);

    prefs.end();

    // Start with the default plan
    for (uint16_t i = 0; i < TOTAL_CHANNELS && i < 64; i++)
        channelSelected[i] = (defaultMask & (1ULL << i)) != 0;
}

void saveSettings()
{
    prefs.begin("analyzer", false);

    prefs.putBool("fast", fastSweep);
    prefs.putBool("cap", pcapEnabled);
    prefs.putUChar("capmask", pcapMask);
    prefs.putShort("tz", tzMinutes);
    prefs.putBool("bleactive", bleActiveScan);
    prefs.putBool("catalog", catalogEnabled);
    prefs.putUChar("snapparts", snapParts);

    prefs.end();
}


/*
 * Who is looking at the analyzer.
 *
 * With several people on the same device it helps to see that
 * someone else is also connected before wondering why a
 * setting changed.
 */
#define MAX_WEB_CLIENTS 8
#define WEB_CLIENT_TIMEOUT_MS 900000

struct WebClient
{
    bool     valid;
    uint32_t ip;
    uint32_t lastSeen;
    uint32_t requests;
};

WebClient webClients[MAX_WEB_CLIENTS];

void noteWebClient(uint32_t ip)
{
    uint32_t now = millis();

    int      freeSlot = -1;
    uint32_t oldest   = UINT32_MAX;
    int      oldestIx = 0;

    for (int i = 0; i < MAX_WEB_CLIENTS; i++)
    {
        if (webClients[i].valid)
        {
            if (webClients[i].ip == ip)
            {
                webClients[i].lastSeen = now;
                webClients[i].requests++;

                return;
            }

            if (webClients[i].lastSeen < oldest)
            {
                oldest   = webClients[i].lastSeen;
                oldestIx = i;
            }
        }
        else if (freeSlot < 0)
        {
            freeSlot = i;
        }
    }

    int idx = (freeSlot >= 0) ? freeSlot : oldestIx;

    webClients[idx].valid    = true;
    webClients[idx].ip       = ip;
    webClients[idx].lastSeen = now;
    webClients[idx].requests = 1;
}

uint8_t countWebClients()
{
    uint32_t now = millis();

    uint8_t count = 0;

    for (int i = 0; i < MAX_WEB_CLIENTS; i++)
    {
        if (!webClients[i].valid)
            continue;

        if (now - webClients[i].lastSeen > WEB_CLIENT_TIMEOUT_MS)
        {
            webClients[i].valid = false;
            continue;
        }

        count++;
    }

    return count;
}

volatile bool ethGotIP = false;

volatile bool ethLinkUp = false;

uint32_t dhcpAttemptStart = 0;
uint8_t  dhcpAttempts     = 0;
bool     ethStaticActive  = false;

volatile bool ethLinkChanged = false;

uint8_t  reinitCount = 0;
uint32_t lastReinit  = 0;

uint32_t ethStaticSince = 0;

/*
 * The DHCP lease as lwIP holds it, followed in loop() by
 * dhcpLeaseWatch(). A renewal that keeps the address raises no
 * event in ESP-IDF, so the client's state is read instead.
 */
struct DhcpLease
{
    uint8_t  state;       // DHCP_STATE_*, as last seen
    uint32_t seconds;     // lease time handed out
    uint32_t ackAt;       // millis() of the last acknowledgement
    uint32_t renewals;    // renewals since boot
    uint32_t stateSince;  // millis() when state was entered
    uint32_t server;      // server address, network order
    uint32_t used;        // lwIP's ticks since the last ACK
    uint32_t renewIn;     // lwIP's ticks to the next renewal
};

DhcpLease dhcpLease = {};

// ============================================================
// Utility
// ============================================================

String macToString(const uint8_t *mac)
{
    char buffer[18];

    snprintf(buffer, sizeof(buffer),
             "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2],
             mac[3], mac[4], mac[5]);

    return String(buffer);
}

bool macIsZero(const uint8_t *mac)
{
    for (int i = 0; i < 6; i++)
    {
        if (mac[i] != 0)
            return false;
    }

    return true;
}

/*
 * Copy printable ASCII only.
 *
 * Device names coming out of the BLE stack are not always
 * cleanly terminated: the payload of an advertisement and
 * its scan response sit in one buffer, and if the length of
 * the local-name element does not match what is actually
 * there, the following AD bytes end up appended to the name.
 * Those bytes are not valid UTF-8 and the browser renders
 * them as replacement characters.
 *
 * Stopping at the first non-printable byte cuts that off.
 * Names with non-ASCII characters are truncated at the first
 * such character, which is the accepted trade-off here.
 */
void copyPrintable(char *dst, size_t dstSize, const char *src)
{
    size_t n = 0;

    for (size_t i = 0; src[i] != 0 && n < dstSize - 1; i++)
    {
        uint8_t c = (uint8_t)src[i];

        if (c < 0x20 || c > 0x7E)
            break;

        dst[n++] = (char)c;
    }

    while (n > 0 && dst[n - 1] == ' ')
        n--;

    dst[n] = 0;
}

// Centre frequency of a 2.4 or 5 GHz channel
uint16_t pcapChannelFreq(uint8_t channel)
{
    if (channel == 14)
        return 2484;

    if (channel >= 1 && channel <= 13)
        return 2407 + 5 * channel;

    return 5000 + 5 * channel;
}

uint32_t deviceEpoch()
{
    return (uint32_t)((esp_timer_get_time() + clockOffsetUs) /
                      1000000LL);
}

void logEventFrom(const char *who, const char *fmt, ...)
{
    if (eventLog == nullptr)
        return;

    LogEntry &e = eventLog[logWrite];

    memset(&e, 0, sizeof(e));

    e.up   = (uint32_t)(esp_timer_get_time() / 1000000LL);
    e.boot = bootCount;

    if (pcapClockSource != 0)
        e.epoch = deviceEpoch();

    strncpy(e.who, who, sizeof(e.who) - 1);

    va_list args;

    va_start(args, fmt);
    vsnprintf(e.text, sizeof(e.text), fmt, args);
    va_end(args);

    logWrite = (logWrite + 1) % LOG_ENTRIES;

    if (logCount < LOG_ENTRIES)
        logCount++;

    logTotal++;

    Serial.printf("[log] %s: %s\r\n", e.who, e.text);

    logPersist(e);
}

// Inside a request handler the client address is known
void logRequest(const char *fmt, ...)
{
    char who[16] = "unknown";

    snprintf(who, sizeof(who), "%s", reqClientText().c_str());

    char text[88];

    va_list args;

    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);

    logEventFrom(who, "%s", text);
}

void appendName(char *buffer, size_t size, const char *name)
{
    if (strstr(buffer, name) != nullptr)
        return;

    if (buffer[0] != 0)
        strncat(buffer, ", ", size - strlen(buffer) - 1);

    strncat(buffer, name, size - strlen(buffer) - 1);
}

/*
 * DHCP hostname: "wlanscout-" plus the sensor name, cut down to
 * what a hostname may hold (lower case letters, digits, hyphens),
 * or plus the last three bytes of the Ethernet MAC when there is
 * no usable name. With several sensors on one network each shows
 * up under its own name in the DHCP server. A changed name takes
 * effect with the next lease, that is after a restart.
 */
char dhcpHostname[48] = "";

void buildHostname()
{
    char part[33];
    int  k = 0;

    for (const char *p = sensorName; *p && k < (int)sizeof(part) - 1; p++)
    {
        char c = *p;

        if (c >= 'A' && c <= 'Z')
            c = c - 'A' + 'a';

        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');

        if (ok)
            part[k++] = c;
        else if (k > 0 && part[k - 1] != '-')
            part[k++] = '-';               // spaces and the like
    }

    while (k > 0 && part[k - 1] == '-')
        k--;

    part[k] = 0;

    if (k > 0)
    {
        snprintf(dhcpHostname, sizeof(dhcpHostname), "wlanscout-%s", part);
        return;
    }

    uint8_t mac[6];

    ETH.macAddress(mac);

    snprintf(dhcpHostname, sizeof(dhcpHostname), "wlanscout-%02x%02x%02x",
             mac[3], mac[4], mac[5]);
}

// ============================================================
// Ethernet events
// ============================================================

void onEvent(arduino_event_id_t event, arduino_event_info_t info)
{
    switch (event)
    {
        case ARDUINO_EVENT_ETH_START:
            Serial.println("ETH START");
            buildHostname();
            ETH.setHostname(dhcpHostname);
            Serial.printf("DHCP hostname: %s\r\n", dhcpHostname);
            break;

        case ARDUINO_EVENT_ETH_CONNECTED:
            /*
             * Speed and duplex say whether auto-negotiation
             * actually ran. A W5500 module pinned to a fixed
             * mode links to a switch through parallel
             * detection but often not to a PC NIC, and
             * 10 Mbps half duplex is the tell-tale.
             */
            Serial.printf("ETH CONNECTED: %d Mbps, %s\r\n",
                          (int)ETH.linkSpeed(),
                          ETH.fullDuplex() ? "full duplex"
                                           : "half duplex");

            ethLinkUp = true;

            // The work is done in loop(), calling into the
            // network stack from the event task is asking for
            // trouble
            ethLinkChanged = true;

            logEventFrom("system", "link up, %d Mbps %s",
                         (int)ETH.linkSpeed(),
                         ETH.fullDuplex() ? "full duplex"
                                          : "half duplex");

            break;

        case ARDUINO_EVENT_ETH_GOT_IP:
            ethGotIP = true;

            // A real lease means the interface is healthy
            if (!ethStaticActive)
                reinitCount = 0;

            Serial.printf("ETH GOT IP (%s)\r\n",
                          ethStaticActive ? "static" : "DHCP");

            Serial.print("IP: ");
            Serial.println(ETH.localIP());

            Serial.print("Gateway: ");
            Serial.println(ETH.gatewayIP());

            Serial.print("Subnet: ");
            Serial.println(ETH.subnetMask());

            Serial.println();
            Serial.print("Web UI: http://");
            Serial.println(ETH.localIP());
            break;

        case ARDUINO_EVENT_ETH_LOST_IP:
            /*
             * Raised two minutes after lwIP dropped the address,
             * usually because the lease ran out. With ethGotIP
             * cleared the DHCP watchdog in loop() takes over and
             * reinitialises the interface if no lease follows;
             * before, the sensor sat without an address until
             * the link went down.
             */
            ethGotIP = false;

            Serial.println("ETH LOST IP");

            logEventFrom("system", "IP address lost, asking DHCP "
                                   "for a new lease");

            dhcpAttempts     = 0;
            dhcpAttemptStart = millis();

            break;

        case ARDUINO_EVENT_ETH_DISCONNECTED:
            ethGotIP  = false;
            ethLinkUp = false;

            Serial.println("ETH DISCONNECTED");

            logEventFrom("system", "link down");

            // Start over with DHCP once the link is back
            dhcpAttempts     = 0;
            dhcpAttemptStart = 0;

            break;

        case ARDUINO_EVENT_ETH_STOP:
            ethGotIP  = false;
            ethLinkUp = false;

            Serial.println("ETH STOP");
            break;

        default:
            break;
    }
}

// ============================================================
// Channel helper
// ============================================================

uint8_t getChannelNumber(uint16_t index)
{
    if (index < NUM_CHANNELS_24)
        return channels24[index];

    return channels5[index - NUM_CHANNELS_24];
}

bool isCoverageChannel(uint8_t channel)
{
    for (size_t i = 0; i < sizeof(coverage24); i++)
    {
        if (coverage24[i] == channel)
            return true;
    }

    return false;
}

void buildScanPlan()
{
    for (uint16_t i = 0; i < TOTAL_CHANNELS; i++)
        channelInPlan[i] = channelSelected[i];

    planCount = countSelectedChannels();

    if (!fastSweep)
        return;

    bool active[TOTAL_CHANNELS];

    for (uint16_t i = 0; i < TOTAL_CHANNELS; i++)
        active[i] = false;

    // Channels that currently carry a known BSSID
    if (networks != nullptr)
    {
        for (int n = 0; n < MAX_NETWORKS; n++)
        {
            if (!networks[n].valid)
                continue;

            for (uint16_t i = 0; i < TOTAL_CHANNELS; i++)
            {
                if (getChannelNumber(i) == networks[n].channel)
                    active[i] = true;
            }
        }
    }

    planCount = 0;

    for (uint16_t i = 0; i < TOTAL_CHANNELS; i++)
    {
        uint8_t ch = getChannelNumber(i);

        channelInPlan[i] =
            channelSelected[i] &&
            (active[i] || isCoverageChannel(ch));

        if (channelInPlan[i])
            planCount++;
    }

    // One idle channel per sweep, rotating
    for (uint16_t n = 0; n < TOTAL_CHANNELS; n++)
    {
        uint16_t idx = (idleProbeIndex + n) % TOTAL_CHANNELS;

        if (channelSelected[idx] && !channelInPlan[idx])
        {
            channelInPlan[idx] = true;

            planCount++;

            idleProbeIndex = (idx + 1) % TOTAL_CHANNELS;

            break;
        }
    }

    // Never end up with nothing to scan
    if (planCount == 0)
    {
        for (uint16_t i = 0; i < TOTAL_CHANNELS; i++)
            channelInPlan[i] = channelSelected[i];

        planCount = countSelectedChannels();
    }
}

int findNextSelectedChannel(uint16_t current)
{
    for (uint16_t n = 1; n <= TOTAL_CHANNELS; n++)
    {
        uint16_t idx = (current + n) % TOTAL_CHANNELS;

        if (channelSelected[idx] && channelInPlan[idx])
            return idx;
    }

    return -1;
}

uint16_t countSelectedChannels()
{
    uint16_t count = 0;

    for (uint16_t i = 0; i < TOTAL_CHANNELS; i++)
    {
        if (channelSelected[i])
            count++;
    }

    return count;
}

void switchChannel(uint16_t index)
{
    uint8_t channel = getChannelNumber(index);

    esp_err_t err = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);

    if (err != ESP_OK)
    {
        Serial.printf("Channel %u error: 0x%X\r\n", channel, err);
    }
}

// ============================================================
// Reset live counters
// ============================================================

void resetLiveCounters()
{
    rxPackets   = 0;
    rxPeakRSSI  = -127;
    rxRSSISum   = 0;
    rxRSSICount = 0;
}

// ============================================================
// IE parser helpers
// ============================================================

const uint8_t *findIE(const uint8_t *ies, int ieLength,
                      uint8_t wantedID, int &foundLength)
{
    int pos = 0;

    while (pos + 2 <= ieLength)
    {
        uint8_t id  = ies[pos];
        uint8_t len = ies[pos + 1];

        pos += 2;

        if (pos + len > ieLength)
            break;

        if (id == wantedID)
        {
            foundLength = len;
            return &ies[pos];
        }

        pos += len;
    }

    foundLength = 0;
    return nullptr;
}

const uint8_t *findExtendedIE(const uint8_t *ies, int ieLength,
                              uint8_t extensionID, int &foundLength)
{
    int pos = 0;

    while (pos + 2 <= ieLength)
    {
        uint8_t id  = ies[pos];
        uint8_t len = ies[pos + 1];

        pos += 2;

        if (pos + len > ieLength)
            break;

        if (id == 255 && len >= 1 && ies[pos] == extensionID)
        {
            foundLength = len - 1;
            return &ies[pos + 1];
        }

        pos += len;
    }

    foundLength = 0;
    return nullptr;
}

const uint8_t *findVendorIE(const uint8_t *ies, int ieLength,
                            uint8_t o0, uint8_t o1, uint8_t o2,
                            uint8_t type, int &foundLength)
{
    int pos = 0;

    while (pos + 2 <= ieLength)
    {
        uint8_t id  = ies[pos];
        uint8_t len = ies[pos + 1];

        pos += 2;

        if (pos + len > ieLength)
            break;

        if (id == 221 && len >= 4 &&
            ies[pos + 0] == o0 &&
            ies[pos + 1] == o1 &&
            ies[pos + 2] == o2 &&
            ies[pos + 3] == type)
        {
            foundLength = len - 4;
            return &ies[pos + 4];
        }

        pos += len;
    }

    foundLength = 0;
    return nullptr;
}

// ============================================================
// Cipher and AKM names
// ============================================================

const char *cipherName(uint8_t type, bool wpa1)
{
    switch (type)
    {
        case 0:  return "use-group";
        case 1:  return "WEP-40";
        case 2:  return "TKIP";
        case 4:  return "CCMP-128";
        case 5:  return "WEP-104";
        case 6:  return "BIP-CMAC-128";
        case 8:  return "GCMP-128";
        case 9:  return "GCMP-256";
        case 10: return "CCMP-256";
        case 11: return "BIP-GMAC-128";
        case 12: return "BIP-GMAC-256";
        case 13: return "BIP-CMAC-256";
        default: break;
    }

    // A suite this table does not know yet: say which one it is,
    // so it can be looked up instead of guessed at
    static char buffer[16];

    snprintf(buffer, sizeof(buffer), "cipher %u", type);

    return buffer;
}

/*
 * AKM suites of OUI 00-0F-AC, 802.11-2024 table 9-190, checked
 * against Wireshark's ieee80211_rsn_keymgmt_vals. Names follow
 * the short forms hostapd and the WFA use.
 */
/*
 * Suites under a vendor's own OUI that have a name. Nintendo's
 * turned up in a network without an SSID, most likely a Switch's
 * local play (October 2026); what they do is not public.
 */
const char *vendorSuiteName(uint32_t suite, bool akm)
{
    if (suite == (akm ? 0xA4C0E102u : 0xA4C0E104u))
        return "Nintendo";

    return nullptr;
}

const char *akmName(uint8_t type, bool wpa1)
{
    if (wpa1)
    {
        switch (type)
        {
            case 0:  return "none";     // reserved, offers nothing
            case 1:  return "802.1X";
            case 2:  return "PSK";
            default: break;
        }
    }
    else
    {
        switch (type)
        {
            case 1:  return "802.1X";
            case 2:  return "PSK";
            case 3:  return "FT-802.1X";
            case 4:  return "FT-PSK";
            case 5:  return "802.1X-SHA256";
            case 6:  return "PSK-SHA256";
            case 7:  return "TDLS";
            case 8:  return "SAE";
            case 9:  return "FT-SAE";
            case 10: return "AP-PeerKey";
            case 11: return "802.1X-SuiteB";
            case 12: return "802.1X-SuiteB-192";
            case 13: return "FT-802.1X-SHA384";
            case 14: return "FILS-SHA256";
            case 15: return "FILS-SHA384";
            case 16: return "FT-FILS-SHA256";
            case 17: return "FT-FILS-SHA384";
            case 18: return "OWE";
            case 19: return "FT-PSK-SHA384";
            case 20: return "PSK-SHA384";
            case 21: return "PASN";
            case 24: return "SAE-EXT-KEY";
            case 25: return "FT-SAE-EXT-KEY";
            case 29: return "EPPKE";
            default: break;
        }
    }

    static char buffer[16];

    snprintf(buffer, sizeof(buffer), "AKM %u", type);

    return buffer;
}


// ============================================================
// RSN (IE 48) or WPA (vendor IE 221 / 00:50:F2 type 1)
// ============================================================

struct RsnInfo
{
    bool present;
    bool psk;
    bool sae;
    bool eap;
    bool owe;
    bool osen;
    bool mfpCapable;
    bool mfpRequired;

    /*
     * RSN Capabilities bit 0. Pre-authentication lets a
     * station authenticate with an AP it is not associated
     * with, over the wired side, to shorten a later roam. It
     * is the predecessor of 802.11r and only works with
     * WPA2-EAP, so its presence says something about the age
     * of the deployment.
     */
    bool preAuth;

    char group[18];
    char pairwise[36];
    char mgmt[18];
    char akm[64];

    // First suite under an OUI that has no place here, see badOui
    bool     badSeen;
    uint32_t badSuite;
};

/*
 * A suite whose OUI does not belong in this element: 00:00:00
 * anywhere, 00:0F:AC in the WPA element, 00:50:F2 in the RSN
 * element. Some cable routers list 00-0F-AC-02 and 00-00-00-00
 * among the WPA pairwise ciphers; the AP's firmware wrote them
 * wrong. Vendor suites such as Cisco's CCKM are fine.
 */
void noteBadSuite(const uint8_t *q, bool wpa1, struct RsnInfo &info)
{
    uint32_t oui = ((uint32_t)q[0] << 16) | (q[1] << 8) | q[2];

    bool bad = oui == 0 || oui == (wpa1 ? 0x000FACu : 0x0050F2u);

    if (bad && !info.badSeen)
    {
        info.badSeen  = true;
        info.badSuite = (oui << 8) | q[3];
    }
}

/*
 * OSEN, the OSU Server-only authenticated L2 Encryption
 * Network of Hotspot 2.0.
 *
 * Carried in a WFA vendor element (50:6F:9A type 0x12) rather
 * than in an RSN element, and laid out like RSN but without
 * the leading version field. The ciphers use the usual
 * 00:0F:AC suites, only the AKM is a WFA one, so this needs
 * its own reader instead of the RSN path.
 *
 * A BSS like this is an onboarding network: the client
 * authenticates the server by certificate, not the other way
 * round, and uses it only to fetch a subscription.
 */
void parseOsen(const uint8_t *p, int len, struct RsnInfo &info)
{
    if (p == nullptr || len < 6)
        return;

    info.present = true;
    info.osen    = true;

    int pos = 0;

    // Group cipher
    if (p[0] == 0x00 && p[1] == 0x0F && p[2] == 0xAC)
    {
        strncpy(info.group,
                cipherName(p[3], false),
                sizeof(info.group) - 1);
    }

    pos += 4;

    if (pos + 2 > len)
        return;

    uint16_t pairwiseCount = p[pos] | (p[pos + 1] << 8);
    pos += 2;

    for (uint16_t i = 0; i < pairwiseCount; i++)
    {
        if (pos + 4 > len)
            return;

        if (p[pos] == 0x00 && p[pos + 1] == 0x0F && p[pos + 2] == 0xAC)
        {
            appendName(info.pairwise,
                       sizeof(info.pairwise),
                       cipherName(p[pos + 3], false));
        }

        pos += 4;
    }

    if (pos + 2 > len)
        return;

    uint16_t akmCount = p[pos] | (p[pos + 1] << 8);
    pos += 2;

    for (uint16_t i = 0; i < akmCount; i++)
    {
        if (pos + 4 > len)
            return;

        // 50:6F:9A type 1 is the OSEN AKM
        if (p[pos] == 0x50 && p[pos + 1] == 0x6F &&
            p[pos + 2] == 0x9A && p[pos + 3] == 0x01)
        {
            appendName(info.akm, sizeof(info.akm), "OSEN");
        }

        pos += 4;
    }

    if (pos + 2 > len)
        return;

    uint16_t caps = p[pos] | (p[pos + 1] << 8);

    info.preAuth     = (caps & 0x0001) != 0;
    info.mfpRequired = (caps & 0x0040) != 0;
    info.mfpCapable  = (caps & 0x0080) != 0;
}


void parseSuites(const uint8_t *p, int len, bool wpa1,
                 struct RsnInfo &info)
{
    if (p == nullptr || len < 8)
        return;

    info.present = true;

    const uint8_t oui0 = 0x00;
    const uint8_t oui1 = wpa1 ? 0x50 : 0x0F;
    const uint8_t oui2 = wpa1 ? 0xF2 : 0xAC;

    int pos = 2;                  // Version

    if (pos + 4 <= len)
    {
        noteBadSuite(p + pos, wpa1, info);

        if (p[pos] == oui0 && p[pos + 1] == oui1 && p[pos + 2] == oui2)
        {
            strncpy(info.group,
                    cipherName(p[pos + 3], wpa1),
                    sizeof(info.group) - 1);
        }
        else if (vendorSuiteName(suiteAt(p + pos), false))
        {
            strncpy(info.group,
                    vendorSuiteName(suiteAt(p + pos), false),
                    sizeof(info.group) - 1);
        }
    }

    pos += 4;

    if (pos + 2 > len)
        return;

    uint16_t pairwiseCount = p[pos] | (p[pos + 1] << 8);
    pos += 2;

    for (uint16_t i = 0; i < pairwiseCount; i++)
    {
        if (pos + 4 > len)
            return;

        noteBadSuite(p + pos, wpa1, info);

        if (p[pos] == oui0 && p[pos + 1] == oui1 && p[pos + 2] == oui2)
        {
            appendName(info.pairwise,
                       sizeof(info.pairwise),
                       cipherName(p[pos + 3], wpa1));
        }
        else if (vendorSuiteName(suiteAt(p + pos), false))
        {
            appendName(info.pairwise,
                       sizeof(info.pairwise),
                       vendorSuiteName(suiteAt(p + pos), false));
        }

        pos += 4;
    }

    if (pos + 2 > len)
        return;

    uint16_t akmCount = p[pos] | (p[pos + 1] << 8);
    pos += 2;

    for (uint16_t i = 0; i < akmCount; i++)
    {
        if (pos + 4 > len)
            return;

        bool known =
            (p[pos] == oui0 && p[pos + 1] == oui1 && p[pos + 2] == oui2);

        uint8_t type = p[pos + 3];

        noteBadSuite(p + pos, wpa1, info);

        if (!known && vendorSuiteName(suiteAt(p + pos), true))
            appendName(info.akm, sizeof(info.akm),
                       vendorSuiteName(suiteAt(p + pos), true));

        if (known)
        {
            appendName(info.akm,
                       sizeof(info.akm),
                       akmName(type, wpa1));

            if (wpa1)
            {
                if (type == 1) info.eap = true;
                if (type == 2) info.psk = true;
            }
            else
            {
                switch (type)
                {
                    case 1:  case 3:  case 5:
                    case 11: case 12: case 13:
                        info.eap = true;
                        break;

                    // FILS authenticates through 802.1X as well
                    case 14: case 15: case 16: case 17:
                        info.eap = true;
                        break;

                    case 2: case 4: case 6: case 19: case 20:
                        info.psk = true;
                        break;

                    // 24/25: SAE with a group-dependent hash
                    // (SAE-EXT-KEY), the WPA3 variant for groups
                    // beyond 19 and the default with Wi-Fi 7
                    case 8: case 9: case 24: case 25:
                        info.sae = true;
                        break;

                    case 18:
                        info.owe = true;
                        break;

                    default:
                        break;
                }
            }
        }

        pos += 4;
    }

    if (wpa1)
        return;

    if (pos + 2 > len)
        return;

    uint16_t caps = p[pos] | (p[pos + 1] << 8);
    pos += 2;

    info.preAuth     = (caps & 0x0001) != 0;   // Pre-Auth
    info.mfpRequired = (caps & 0x0040) != 0;   // MFPR
    info.mfpCapable  = (caps & 0x0080) != 0;   // MFPC

    if (pos + 2 > len)
        return;

    uint16_t pmkidCount = p[pos] | (p[pos + 1] << 8);
    pos += 2 + 16 * pmkidCount;

    if (pos + 4 <= len)
    {
        if (p[pos] == oui0 && p[pos + 1] == oui1 && p[pos + 2] == oui2)
        {
            strncpy(info.mgmt,
                    cipherName(p[pos + 3], false),
                    sizeof(info.mgmt) - 1);
        }
    }
}

// ============================================================
// Station Capabilities
//
// Fed from the IEs of probe requests and (re)association
// requests. The association request is the complete one,
// probe requests carry only a subset depending on the
// device.
// ============================================================

/*
 * Days since 1970-01-01 for a date in the proleptic Gregorian
 * calendar (Howard Hinnant's days_from_civil).
 */
int32_t daysFromCivil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;

    const int era = (y >= 0 ? y : y - 399) / 400;

    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;

    return era * 146097 + (int32_t)doe - 719468;
}

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
 * and time value do not fit together.
 *
 * One AP is not very exact: a TP-Link EAP ran 15 s behind NTP on
 * its 2.4 GHz radio and 84 s on its 5 GHz one, apparently each
 * radio fixes the time at which its TSF was 0 once and never
 * again. So the renewals take the median over every AP heard.
 */
void noteApClock(const uint8_t *bssid, int64_t offsetUs)
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

    int64_t diff = offsetUs - wifiClockCand;

    bool agrees = memcmp(bssid, wifiClockCandBssid, 6) == 0 &&
                  diff > -2000000LL && diff < 2000000LL;

    memcpy(wifiClockCandBssid, bssid, 6);
    wifiClockCand = offsetUs;

    if (!agrees)
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
 *             36 Supported Channels, 37/60 channel switch, 42 ERP,
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
 *             42 BSS Color Change Announcement, 59 HE 6 GHz,
 *             107 Multi-Link, 108 EHT Capabilities
 *   passed:   33 Power Capability, 46 QoS Capability,
 *             47 ERP (the 802.11g draft's number, Broadcom sends
 *             it next to 42 with the same content),
 *             51 AP Channel Report, 62 Secondary Channel Offset, 72 20/40 BSS
 *             Coexistence, 74 OBSS Scan Parameters, 244 RSN
 *             Extension, 10 Request (what
 *             a station asks for), 128 Lucent/Agere proprietary,
 *             150 (Cisco's number, empty in probe requests of
 *             some phones), 200 and 231 as Tenda/Realtek fill them, 240 FILS
 *             Indication (FILS itself shows in the AKM list);
 *             ext 2 FILS Request Parameters, 37 UORA, 38 MU EDCA, 39 Spatial Reuse, 41 NDP
 *             Feedback Report, 56 Non-Inheritance,
 *             106 EHT Operation, 110 Multi-Link Traffic Indication,
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
    133, 191, 192, 193, 195, 199, 201
};

static const uint8_t decodedExt[] = { 35, 36, 42, 59, 107, 108 };

static const uint8_t passedIes[] = {
    10, 33, 46, 47, 51, 62, 72, 74, 128, 150, 200, 231, 240, 244
};

static const uint8_t passedExt[] = {
    2, 37, 38, 39, 41, 56, 106, 110, 137
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

void parseWfaElements(const uint8_t *ies, int ieLength,
                      struct WiFiNetwork &n)
{
    int len = 0;

    if (wfaCapaOf(ies, ieLength, n.wfaCapa))
        n.wfaCapaSeen = true;

    const uint8_t *p = findVendorIE(ies, ieLength,
                                    0x50, 0x6F, 0x9A, 0x29, len);

    if (p == nullptr)
        p = findVendorIE(ies, ieLength, 0x50, 0x6F, 0x9A, 0x2A, len);

    if (p != nullptr)
    {
        RsnInfo o;

        memset(&o, 0, sizeof(o));

        parseSuites(p, len, false, o);

        n.ovrSeen = true;
        n.ovrPmf  = o.mfpRequired ? 2 : (o.mfpCapable ? 1 : 0);

        strlcpy(n.ovrAkm, o.akm, sizeof(n.ovrAkm));
        strlcpy(n.ovrPairwise, o.pairwise, sizeof(n.ovrPairwise));
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

void collectUnknownElements(const uint8_t *ies, int ieLength,
                            uint16_t *list, uint8_t *seenIn,
                            uint8_t &count, uint8_t capacity,
                            uint8_t source)
{
    for (int pos = 0; pos + 2 <= ieLength; )
    {
        uint8_t id = ies[pos];
        uint8_t l  = ies[pos + 1];

        if (pos + 2 + l > ieLength)
            break;

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

    if (streams > c.streams)
        c.streams = streams;
}

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
     * Name it if we can: the same-SSID bit says it carries the
     * reporting AP's name, otherwise the short SSID may match
     * one we already know.
     */
    if (e.ssid[0] == 0)
    {
        if (e.sameSsid && reporterSsid != nullptr &&
            reporterSsid[0] != 0 &&
            strcmp(reporterSsid, "<hidden>") != 0)
        {
            strncpy(e.ssid, reporterSsid, 32);
        }
        else if (e.haveShortSsid && networks != nullptr)
        {
            for (int i = 0; i < MAX_NETWORKS; i++)
            {
                if (!networks[i].valid || networks[i].ssid[0] == 0)
                    continue;

                if (crc32ssid(networks[i].ssid,
                              strlen(networks[i].ssid)) == e.shortSsid)
                {
                    strncpy(e.ssid, networks[i].ssid, 32);
                    break;
                }
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
    "FT over the DS"
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

    // RSNE Override, or Override 2, and the RSNXE Override's H2E bit
    p = findVendorIE(ies, ieLength, 0x50, 0x6F, 0x9A, 0x29, len);

    if (p == nullptr)
        p = findVendorIE(ies, ieLength, 0x50, 0x6F, 0x9A, 0x2A, len);

    if (p != nullptr)
    {
        RsnInfo o;

        memset(&o, 0, sizeof(o));
        parseSuites(p, len, false, o);

        c.ovr    = true;
        c.ovrPmf = o.mfpRequired ? 2 : (o.mfpCapable ? 1 : 0);

        strlcpy(c.ovrAkm, o.akm, sizeof(c.ovrAkm));
        strlcpy(c.ovrPairwise, o.pairwise, sizeof(c.ovrPairwise));
    }

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
 * Back to the state before the last change: one entry for the
 * flapping, "width, standard, ..." and the switches so far,
 * updated in place while it goes on.
 */
void cfgFlap(WiFiNetwork &n, const CfgSnap &now)
{
    char names[40] = "";
    char count[24];

    cfgDiff(n, n.cfg, now, names, sizeof(names));

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
 * VHT Operation (192): channel width, then the two centre frequency
 * segments CCFS0 and CCFS1. Today's APs give 160 MHz as width 1 with
 * CCFS0 the centre of the primary 80 MHz and CCFS1 that of the whole
 * 160, eight channels apart (802.11-2020 Table 9-274): 36/160 is 42
 * and 50, centre 50. CCFS1 further off is 80+80. The deprecated
 * widths 2 (160) and 3 (80+80) have the centre in CCFS0. False for
 * width 0, 20 or 40 MHz, which the HT Operation element tells.
 */
bool vhtWidthOf(const uint8_t *vht, uint16_t &bandwidth, uint8_t &centre)
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

    uint16_t bandwidth     = 20;
    uint8_t  centerChannel = channel;

    int vhtLen = 0;

    const uint8_t *vht = findIE(ies, ieLength, 192, vhtLen);

    bool vhtHandled = false;

    if (vht != nullptr && vhtLen >= 3)
        vhtHandled = vhtWidthOf(vht, bandwidth, centerChannel);

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

    const uint8_t *csa = findIE(ies, ieLength, 37, capLen);

    if (csa != nullptr && capLen >= 3)
    {
        csaChannel = csa[1];
        csaCount   = csa[2];
    }
    else
    {
        csa = findIE(ies, ieLength, 60, capLen);

        if (csa != nullptr && capLen >= 4)
        {
            csaChannel = csa[2];
            csaCount   = csa[3];
        }
    }

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
    n.rssi           = rssi;
    n.standard       = standard;
    n.phyModes       = phyModes;
    n.htGreenfield   = htGreenfield;
    n.fortyIntolerant = fortyIntolerant;
    n.rm11k          = rm11k;
    n.bss11v         = bss11v;
    n.rrmCaps        = rrmCapsOf(ies, ieLength);
    n.streams        = maxStreamsOf(ies, ieLength, channel);

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
            n.apClockOffsetUs = offset;
            noteApClock(n.bssid, offset);
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

// ============================================================
// Management frame dispatch
// ============================================================

void parseManagementFrame(const uint8_t *frame, uint16_t length,
                          int8_t rssi, uint8_t channelHint)
{
    if (length < 24)
        return;

    uint16_t frameControl = frame[0] | (frame[1] << 8);
    uint8_t  subtype      = (frameControl >> 4) & 0x0F;

    if (subtype == 4)
    {
        parseProbeRequest(frame, length, rssi, channelHint);
        return;
    }

    // 0 = Association Request, 2 = Reassociation Request
    if (subtype == 0 || subtype == 2)
    {
        parseAssocRequest(frame, length, rssi, channelHint,
                          subtype == 2);
        return;
    }

    // 1 = Association Response, 3 = Reassociation Response
    if (subtype == 1 || subtype == 3)
    {
        parseAssocResponse(frame, length);
        return;
    }

    // 8 = Beacon, 5 = Probe Response
    if (subtype == 8 || subtype == 5)
        parseBeacon(frame, length, rssi, channelHint);

    // 13 = Action, of which AWDL and Fine Timing Measurement
    if (subtype == 13)
    {
        parseAwdl(frame, length, rssi, channelHint);
        parseFtm(frame, length, rssi, channelHint);
    }
}

// ============================================================
// Frame queues
//
// Single producer (Wi-Fi task) / single consumer (loop).
// ============================================================

struct MgmtFrameCopy
{
    uint64_t us;
    uint16_t length;
    int8_t   rssi;
    uint8_t  channel;
    uint8_t  data[MGMT_PARSE_MAX];
};

MgmtFrameCopy *mgmtQueue = nullptr;   // PSRAM

volatile uint8_t mgmtWrite = 0;
volatile uint8_t mgmtRead  = 0;

// Highest fill of the queue, in the current statistics window and
// since the start; MGMT_QUEUE_SIZE - 1 means it was full
volatile uint8_t mgmtFillWin  = 0;
volatile uint8_t mgmtFillBoot = 0;

#define DATA_QUEUE_SIZE 64

struct DataFrameInfo
{
    uint8_t  bssid[6];
    uint8_t  sta[6];
    uint8_t  peer[6];
    int8_t   rssi;
    uint8_t  channel;
    bool     uplink;
    uint16_t length;
};

DataFrameInfo *dataQueue = nullptr;   // PSRAM

volatile uint8_t dataWrite = 0;
volatile uint8_t dataRead  = 0;

// Frames dropped on a full queue
volatile uint32_t mgmtDrops = 0;
volatile uint32_t dataDrops = 0;

// Frames discarded because the FCS was wrong
volatile uint32_t fcsErrors = 0;

// ============================================================
// Promiscuous callback
// ============================================================

void IRAM_ATTR wifiPromiscuousCallback(void *buf,
                                       wifi_promiscuous_pkt_type_t type)
{
    if (buf == nullptr)
        return;

    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;

    /*
     * Drop frames that failed the FCS.
     *
     * Without this a single flipped bit in an address turns
     * into a phantom BSSID, a phantom station or a bogus
     * roaming event. The BSSIDs a dual-band AP uses on 2.4 and
     * 5 GHz differ in a single bit often enough for that to
     * look entirely plausible.
     *
     * rx_state is 0 when the frame was received cleanly.
     */
    if (pkt->rx_ctrl.rx_state != 0)
    {
        fcsErrors++;
        return;
    }

    if (type == WIFI_PKT_MGMT ||
        type == WIFI_PKT_DATA ||
        type == WIFI_PKT_CTRL)
    {
        int8_t rssi = pkt->rx_ctrl.rssi;

        portENTER_CRITICAL_ISR(&measurementMux);

        rxPackets++;
        rxRSSISum += rssi;
        rxRSSICount++;

        if (rssi > rxPeakRSSI)
            rxPeakRSSI = rssi;

        portEXIT_CRITICAL_ISR(&measurementMux);
    }

    const uint8_t *frame = pkt->payload;

    /*
     * A transmitter address with the group bit set cannot come
     * from a real sender. Such a frame is noise that got past the
     * FCS check, and would put made-up elements into the catalog
     * and a phantom station into the tables. Only for management
     * and data frames: in an RTS or CF-End the bit signals the
     * bandwidth (802.11ac). It still counted for the channel load
     * above, the air was busy all the same.
     */
    if ((type == WIFI_PKT_MGMT || type == WIFI_PKT_DATA) &&
        pkt->rx_ctrl.sig_len >= 16 && (frame[10] & 0x01))
        return;

    // --------------------------------------------------------
    // Data frames: derive BSSID, station and peer
    //
    // ToDS=1: Addr1 BSSID, Addr2 STA (SA), Addr3 DA
    // FromDS=1: Addr1 STA (DA), Addr2 BSSID, Addr3 SA
    // --------------------------------------------------------

    if (type == WIFI_PKT_DATA)
    {
        if (pkt->rx_ctrl.sig_len < 24)
            return;

        bool toDS   = (frame[1] & 0x01) != 0;
        bool fromDS = (frame[1] & 0x02) != 0;

        const uint8_t *a1 = &frame[4];
        const uint8_t *a2 = &frame[10];
        const uint8_t *a3 = &frame[16];

        const uint8_t *bssid = nullptr;
        const uint8_t *sta   = nullptr;
        const uint8_t *peer  = a3;

        bool uplink = false;

        /*
         * Only the two infrastructure cases are evaluated.
         *
         * ToDS=1:   Addr1 = RA = BSSID, Addr2 = TA = station
         * FromDS=1: Addr1 = RA = station, Addr2 = TA = BSSID
         *
         * ToDS=0 and FromDS=0 used to be treated as IBSS with
         * Addr3 as the BSSID. That was wrong far more often
         * than it was right: TDLS direct links, Wi-Fi Direct
         * and mesh traffic all carry those bits, and the
         * result was a station whose BSSID ended up being its
         * own address or that of another client. Those frames
         * are ignored now.
         */
        if (toDS && !fromDS)
        {
            bssid  = a1;
            sta    = a2;
            uplink = true;
        }
        else if (!toDS && fromDS)
        {
            bssid = a2;
            sta   = a1;
        }
        else
        {
            return;
        }

        if ((sta[0] & 0x01) || (bssid[0] & 0x01))
            return;

        // A station cannot be its own access point
        if (memcmp(sta, bssid, 6) == 0)
            return;

        uint8_t next = (dataWrite + 1) % DATA_QUEUE_SIZE;

        if (next == dataRead)
        {
            dataDrops++;
            return;
        }

        DataFrameInfo &d = dataQueue[dataWrite];

        memcpy(d.bssid, bssid, 6);
        memcpy(d.sta, sta, 6);
        memcpy(d.peer, peer, 6);

        d.rssi    = pkt->rx_ctrl.rssi;
        d.channel = pkt->rx_ctrl.channel;
        d.uplink  = uplink;
        d.length  = pkt->rx_ctrl.sig_len;

        dataWrite = next;

        return;
    }

    // --------------------------------------------------------
    // Beacon, probe response, probe request
    // --------------------------------------------------------

    if (type != WIFI_PKT_MGMT)
        return;

    uint16_t frameControl = frame[0] | (frame[1] << 8);
    uint8_t  subtype      = (frameControl >> 4) & 0x0F;

    /*
     * 8 beacon, 5 probe response, 4 probe request,
     * 0 assoc request, 2 reassoc request, 1 and 3 their
     * responses: these are parsed, whatever the capture keeps.
     *
     * Everything else is only queued when it is wanted for
     * the capture, since the parser ignores it anyway.
     */
    bool parsed =
        (subtype == 8 || subtype == 5 || subtype == 4 ||
         subtype == 0 || subtype == 2 || subtype == 1 || subtype == 3);

    /*
     * 13 action: only AWDL (category 127, Apple's OUI, type 8) and
     * Fine Timing Measurement (category 4, actions 32 and 33) are
     * parsed, the rest goes to the capture if wanted. Checked here
     * so that 802.11k/v and block ack traffic does not fill the
     * queue.
     */
    if (subtype == 13 && pkt->rx_ctrl.sig_len >= 24 + 16 + 4 &&
        frame[24] == 0x7F && frame[25] == 0x00 && frame[26] == 0x17 &&
        frame[27] == 0xF2 && frame[28] == 0x08)
        parsed = true;

    if (subtype == 13 && pkt->rx_ctrl.sig_len >= 24 + 3 + 4 &&
        frame[24] == 4 && (frame[25] == 32 || frame[25] == 33))
        parsed = true;

    bool wanted =
        parsed ||
        (pcapEnabled && (pcapMask & PCAP_OTHER));

    if (!wanted)
        return;

    uint16_t length = pkt->rx_ctrl.sig_len;

    // Strip the FCS
    if (length >= 4)
        length -= 4;

    if (length > MGMT_PARSE_MAX)
        length = MGMT_PARSE_MAX;

    uint8_t next = (mgmtWrite + 1) % MGMT_QUEUE_SIZE;

    uint8_t fill = (mgmtWrite + MGMT_QUEUE_SIZE - mgmtRead) %
                   MGMT_QUEUE_SIZE + 1;

    if (fill > mgmtFillWin)
        mgmtFillWin = fill;

    if (fill > mgmtFillBoot)
        mgmtFillBoot = fill;

    if (next == mgmtRead)
    {
        mgmtDrops++;
        return;
    }

    MgmtFrameCopy &entry = mgmtQueue[mgmtWrite];

    entry.us      = esp_timer_get_time();
    entry.length  = length;
    entry.rssi    = pkt->rx_ctrl.rssi;
    entry.channel = pkt->rx_ctrl.channel;

    memcpy(entry.data, frame, length);

    mgmtWrite = next;
}

// ============================================================
// Process queues
// ============================================================

/*
 * Keep the frame as it was received.
 *
 * Everything the parser reads is a subset of what is in
 * there, so holding on to the raw bytes costs a memcpy and
 * gains a full Wireshark dissection later.
 */
void pcapStore(const struct MgmtFrameCopy &entry)
{
    if (!pcapEnabled || pcapRing == nullptr || entry.length < 2)
        return;

    uint16_t fc      = entry.data[0] | (entry.data[1] << 8);
    uint8_t  type    = (fc >> 2) & 0x03;
    uint8_t  subtype = (fc >> 4) & 0x0F;

    if (type != 0)
        return;

    uint8_t want = 0;

    if (subtype == 8 || subtype == 5)
        want = PCAP_BEACONS;
    else if (subtype == 4)
        want = PCAP_PROBEREQ;
    else if (subtype == 0 || subtype == 2)
        want = PCAP_ASSOC;
    else
        want = PCAP_OTHER;      // auth, deauth, disassoc, action

    if (!(pcapMask & want))
        return;

    /*
     * A beacon replaces the previous one of the same BSSID
     * instead of consuming a ring slot. Probe responses stay
     * in the ring: they are rare and they are what reveals a
     * hidden SSID.
     */
    if (subtype == 8 && beaconSlots != nullptr && entry.length >= 16)
    {
        const uint8_t *ta = &entry.data[10];

        int      idx      = -1;
        int      freeSlot = -1;
        uint64_t oldest   = UINT64_MAX;
        int      oldestIx = 0;

        for (int i = 0; i < MAX_BEACON_SLOTS; i++)
        {
            if (!beaconSlots[i].valid)
            {
                if (freeSlot < 0)
                    freeSlot = i;

                continue;
            }

            if (memcmp(beaconSlots[i].bssid, ta, 6) == 0)
            {
                idx = i;
                break;
            }

            if (beaconSlots[i].rec.us < oldest)
            {
                oldest   = beaconSlots[i].rec.us;
                oldestIx = i;
            }
        }

        if (idx < 0)
            idx = (freeSlot >= 0) ? freeSlot : oldestIx;

        BeaconSlot &b = beaconSlots[idx];

        b.valid = true;

        memcpy(b.bssid, ta, 6);

        b.rec.us         = entry.us;
        b.rec.length     = min((int)entry.length, MGMT_FRAME_MAX);
        b.rec.origLength = entry.length;
        b.rec.rssi       = entry.rssi;
        b.rec.channel    = entry.channel;

        memcpy(b.rec.data, entry.data, b.rec.length);

        beaconTotal++;

        return;
    }

    // Association and reassociation requests have their own ring
    if ((subtype == 0 || subtype == 2) && assocRing != nullptr)
    {
        AssocRecord &a = assocRing[assocWrite];

        a.us         = entry.us;
        a.length     = entry.length;
        a.origLength = entry.length;
        a.rssi       = entry.rssi;
        a.channel    = entry.channel;

        memcpy(a.data, entry.data, entry.length);

        assocWrite = (assocWrite + 1) % ASSOC_SLOTS;

        if (assocCount < ASSOC_SLOTS)
            assocCount++;

        assocTotal++;

        return;
    }

    PcapRecord &r = pcapRing[pcapWrite];

    r.us         = entry.us;
    r.length     = min((int)entry.length, MGMT_FRAME_MAX);
    r.origLength = entry.length;
    r.rssi       = entry.rssi;
    r.channel    = entry.channel;

    memcpy(r.data, entry.data, r.length);

    pcapWrite = (pcapWrite + 1) % PCAP_SLOTS;

    if (pcapCount < PCAP_SLOTS)
        pcapCount++;

    pcapTotal++;
}

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
// It lives in the "spiffs" data partition (896 kB with the
// Huge APP scheme), which nothing else uses. Flashing the
// application leaves it alone; only a full image written at 0x0
// or an erase wipes it.
// ============================================================

#define CATALOG_MAX         256
#define CATALOG_SAMPLE      48
#define CATALOG_FRAMES      3          // frames kept per sender
#define CATALOG_SENDERS     10         // senders kept per entry

bool catalogFormatReset = false;       // older layout found at start
#define CATALOG_PCAP_MAX    655360     // bytes for the pcap file
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
 * their wall time, in the ring and in the file.
 */
void logClockLoop()
{
    if (logClockFilled || pcapClockSource == 0 || eventLog == nullptr)
        return;

    logClockFilled = true;

    uint32_t nowEpoch = deviceEpoch();
    uint32_t nowUp    = (uint32_t)(esp_timer_get_time() / 1000000LL);

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
 * Why the chip started: a power cut, a crash, a watchdog, or a
 * restart of its own (firmware update, rollback).
 */
const char *resetReasonText()
{
    switch (esp_reset_reason())
    {
        case ESP_RST_POWERON:   return "power on";
        case ESP_RST_EXT:       return "reset pin";
        case ESP_RST_SW:        return "restart by the firmware";
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
    0x00A05701, 0x00A05704, 0x00A0F805, 0x00E04C00, 0x00E04C02,
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
    0x04DF690A
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

    pcapBuildRadiotap(rt, entry.channel, entry.rssi);

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

bool catalogScan(const uint8_t *frame, int length, uint8_t channel,
                 struct CatalogFrame &cf)
{
    if (length < 24)
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
 * and renamed like the catalog.
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

    File in = LittleFS.open(CATALOG_PCAP, "r");
    File out;

    bool ok = false;

    if (in)
    {
        out = LittleFS.open(CATALOG_PCAP_TMP, "w");

        static uint8_t buf[16 + RADIOTAP_LEN + MGMT_PARSE_MAX];

        ok = out && in.read(buf, 24) == 24 && out.write(buf, 24) == 24;

        uint32_t size  = in.size();
        uint32_t after = 24;

        catalogRowHasFrame = (bool *)calloc(CATALOG_MAX, sizeof(bool));

        catalogMatching = true;
        catalogMatchAny = true;

        while (ok && in.read(buf, 16) == 16)
        {
            uint32_t len;

            memcpy(&len, &buf[8], 4);

            if (len < RADIOTAP_LEN || len > RADIOTAP_LEN + MGMT_PARSE_MAX ||
                in.read(buf + 16, len) != len)
            {
                break;
            }

            CatalogFrame cf = {};

            catalogMatched = false;

            catalogScan(buf + 16 + RADIOTAP_LEN, len - RADIOTAP_LEN, 0, cf);

            if (!catalogMatched)
                continue;

            ok = out.write(buf, 16 + len) == 16 + len;

            after += 16 + len;
        }

        catalogMatching = false;
        catalogMatchAny = false;

        in.close();
        out.close();

        if (ok && catalogRowHasFrame != nullptr)
        {
            LittleFS.remove(CATALOG_PCAP);
            LittleFS.rename(CATALOG_PCAP_TMP, CATALOG_PCAP);

            catalogPrunedBytes = size - after;

            kept = 0;

            for (uint16_t i = 0; i < catalogCount; i++)
                if (catalog[i].frames == 0 || catalogRowHasFrame[i])
                    catalog[kept++] = catalog[i];

            catalogCount = kept;
        }
        else
        {
            LittleFS.remove(CATALOG_PCAP_TMP);
        }

        free(catalogRowHasFrame);

        catalogRowHasFrame = nullptr;
    }

    catalogPrunedRows = before - catalogCount;

    if (catalogPrunedRows)
        catalogSave();

    prefs.begin("analyzer", false);
    prefs.putString("catver", FW_VERSION);
    prefs.end();

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
// Store current measurement
// ============================================================

void finishChannelMeasurement()
{
    ChannelMeasurement m;

    portENTER_CRITICAL(&measurementMux);

    m.packets   = rxPackets;
    m.peakRSSI  = rxPeakRSSI;
    m.rssiSum   = rxRSSISum;
    m.rssiCount = rxRSSICount;

    resetLiveCounters();

    portEXIT_CRITICAL(&measurementMux);

    currentMeasurement[currentChannelIndex] = m;
}

// ============================================================
// Store sweep
// ============================================================

void finishSweep()
{
    if (history == nullptr || historyCapacity == 0)
        return;

    WaterfallEntry &entry = history[historyWriteIndex];

    entry.timestamp = millis();

    for (uint16_t i = 0; i < TOTAL_CHANNELS; i++)
    {
        // Only channels actually visited in this sweep carry a
        // value, the rest stay empty instead of repeating the
        // previous reading
        if (channelSelected[i] && channelVisited[i] &&
            currentMeasurement[i].rssiCount)
        {
            entry.rssi[i] = (int8_t)(currentMeasurement[i].rssiSum /
                                     (int32_t)currentMeasurement[i].rssiCount);
        }
        else
        {
            entry.rssi[i] = -127;
        }
    }

    historyWriteIndex++;

    if (historyWriteIndex >= historyCapacity)
        historyWriteIndex = 0;

    if (historyCount < historyCapacity)
        historyCount++;

    sweepCount++;

    for (uint16_t i = 0; i < TOTAL_CHANNELS; i++)
        channelVisited[i] = false;

    // Plan the next round with what is known right now
    buildScanPlan();
}

// ============================================================
// Scanner
// ============================================================

void scannerLoop()
{
    if (countSelectedChannels() == 0)
        return;

    uint32_t now = millis();

    if (now - lastChannelSwitch < CHANNEL_DWELL_MS)
        return;

    finishChannelMeasurement();

    int next = findNextSelectedChannel(currentChannelIndex);

    if (next < 0)
        return;

    int firstSelected = -1;

    for (uint16_t i = 0; i < TOTAL_CHANNELS; i++)
    {
        if (channelSelected[i] && channelInPlan[i])
        {
            firstSelected = i;
            break;
        }
    }

    bool sweepFinished = (next == firstSelected);

    currentChannelIndex = next;

    if (sweepFinished)
        finishSweep();

    switchChannel(currentChannelIndex);

    channelVisited[currentChannelIndex] = true;

    lastChannelSwitch = now;
}

// ============================================================
// Remove stale networks
// ============================================================

void cleanupNetworks()
{
    uint32_t now = millis();

    for (int i = 0; i < MAX_NETWORKS; i++)
    {
        if (!networks[i].valid)
            continue;

        if (now - networks[i].lastSeen > NETWORK_TIMEOUT_MS)
            networks[i].valid = false;
    }
}

// ============================================================
// History helper
// ============================================================

uint32_t historyStartIndex()
{
    if (historyCount < historyCapacity)
        return 0;

    return historyWriteIndex;
}

// ============================================================
// API: scanner configuration
// ============================================================

/*
 * ?defchannels=1,6,11,36 stores the default channel plan. It
 * does not change what is being scanned.
 */
void saveDefaultChannels(const String &list)
{
    uint64_t mask = 0;

    int start = 0;

    while (start < (int)list.length())
    {
        int comma = list.indexOf(',', start);

        if (comma < 0)
            comma = list.length();

        int ch = list.substring(start, comma).toInt();

        start = comma + 1;

        for (uint16_t i = 0; i < TOTAL_CHANNELS && i < 64; i++)
            if (getChannelNumber(i) == ch)
                mask |= (1ULL << i);
    }

    if (mask == 0)
    {
        reqSend(400, "text/plain", "no valid channel");
        return;
    }

    defaultMask = mask;

    prefs.begin("analyzer", false);
    prefs.putULong64("defmask", defaultMask);
    prefs.end();

    logRequest("default channel plan saved, %d channels",
               __builtin_popcountll(defaultMask));

    reqSend(200, "application/json", "{\"ok\":1}");
}

void handleConfig()
{
    if (reqHasArg("defchannels"))
    {
        saveDefaultChannels(reqArg("defchannels"));
        return;
    }

    if (!reqHasArg("channels"))
    {
        reqSend(400, "text/plain", "missing channels");
        return;
    }

    String channels = reqArg("channels");

    bool newSelection[TOTAL_CHANNELS];

    for (uint16_t i = 0; i < TOTAL_CHANNELS; i++)
        newSelection[i] = false;

    int start = 0;

    while (start < (int)channels.length())
    {
        int comma = channels.indexOf(',', start);

        String token;

        if (comma < 0)
        {
            token = channels.substring(start);
            start = channels.length();
        }
        else
        {
            token = channels.substring(start, comma);
            start = comma + 1;
        }

        int ch = token.toInt();

        for (uint16_t i = 0; i < TOTAL_CHANNELS; i++)
        {
            if (getChannelNumber(i) == ch)
                newSelection[i] = true;
        }
    }

    bool any = false;

    for (uint16_t i = 0; i < TOTAL_CHANNELS; i++)
    {
        channelSelected[i] = newSelection[i];

        if (channelSelected[i])
            any = true;
    }

    if (!any)
        channelSelected[0] = true;

    historyWriteIndex = 0;
    historyCount      = 0;

    currentChannelIndex = 0;

    while (currentChannelIndex < TOTAL_CHANNELS &&
           !channelSelected[currentChannelIndex])
    {
        currentChannelIndex++;
    }

    if (currentChannelIndex >= TOTAL_CHANNELS)
        currentChannelIndex = 0;

    portENTER_CRITICAL(&measurementMux);
    resetLiveCounters();
    portEXIT_CRITICAL(&measurementMux);

    for (uint16_t i = 0; i < TOTAL_CHANNELS; i++)
        memset(&currentMeasurement[i], 0, sizeof(ChannelMeasurement));

    buildScanPlan();

    for (uint16_t i = 0; i < TOTAL_CHANNELS; i++)
        channelVisited[i] = false;

    switchChannel(currentChannelIndex);

    // Summarise the selection instead of listing 33 numbers
    uint8_t first = 0;
    uint8_t last  = 0;

    for (uint16_t i = 0; i < TOTAL_CHANNELS; i++)
    {
        if (!channelSelected[i])
            continue;

        if (first == 0)
            first = getChannelNumber(i);

        last = getChannelNumber(i);
    }

    if (fastSweep)
    {
        logRequest("channels set: %u selected (%u..%u), "
                   "fast sweep scans %u of them",
                   (unsigned)countSelectedChannels(),
                   (unsigned)first, (unsigned)last,
                   (unsigned)planCount);
    }
    else
    {
        logRequest("channels set: %u selected (%u..%u), "
                   "all of them scanned",
                   (unsigned)countSelectedChannels(),
                   (unsigned)first, (unsigned)last);
    }

    reqSend(200, "text/plain", "OK");
    saveSettings();
}

// ============================================================
// BLE
//
// The scanner is exclusive: 2.4 GHz Wi-Fi promiscuous mode
// and BLE share the same radio. Running both at once would
// slow each other down and skew the RSSI readings.
//
// Only the advertising channels 37/38/39 are captured. Data
// channels of established connections cannot be sniffed
// with this controller.
//
// CORE VERSION NOTE
// Written for arduino-esp32 3.x (required for the C5
// anyway). On 2.x two spots need adjusting, both marked
// "CORE 2.x" below.
// ============================================================

struct BleDevice
{
    bool     valid;
    uint8_t  addr[6];
    char     name[25];
    char     svc[20];
    int8_t   rssi;
    int8_t   rssiPeak;
    int8_t   txPower;       // 127 = not advertised
    uint16_t company;       // 0xFFFF = no manufacturer data
    uint32_t adverts;
    uint32_t firstSeen;
    uint32_t lastSeen;
};

BleDevice *bleDevices = nullptr;      // PSRAM

struct BleAdvInfo
{
    uint8_t  addr[6];
    int8_t   rssi;
    int8_t   txPower;
    uint16_t company;
    char     name[25];
    char     svc[20];
};

BleAdvInfo *bleQueue = nullptr;       // PSRAM

volatile uint8_t bleWrite = 0;
volatile uint8_t bleRead  = 0;

volatile uint32_t bleDrops  = 0;
volatile uint32_t bleFrames = 0;

uint8_t scanMode = MODE_WIFI;

bool bleInitialised = false;
bool bleActiveScan  = false;

uint32_t bleLastAdvert  = 0;
uint32_t bleScanStarted = 0;

BLEScan *bleScan = nullptr;

// ------------------------------------------------------------
// Company Identifier
//
// Small deliberate subset. The full list is maintained by
// the Bluetooth SIG under "Assigned Numbers".
// ------------------------------------------------------------

struct CompanyEntry
{
    uint16_t    id;
    const char *name;
};

static const CompanyEntry companyTable[] = {
    { 0x0001, "Nokia" },
    { 0x0002, "Intel" },
    { 0x0006, "Microsoft" },
    { 0x000D, "Texas Instruments" },
    { 0x000F, "Broadcom" },
    { 0x004C, "Apple" },
    { 0x0059, "Nordic Semiconductor" },
    { 0x0075, "Samsung" },
    { 0x0087, "Garmin" },
    { 0x009E, "Bose" },
    { 0x00E0, "Google" },
    { 0x0157, "Huami / Amazfit" },
    { 0x02E5, "Espressif" }
};

#define COMPANY_TABLE_SIZE \
    (sizeof(companyTable) / sizeof(companyTable[0]))

const char *lookupCompany(uint16_t id)
{
    for (size_t i = 0; i < COMPANY_TABLE_SIZE; i++)
    {
        if (companyTable[i].id == id)
            return companyTable[i].name;
    }

    return "";
}

// ------------------------------------------------------------
// Address type from the top two bits of the most
// significant byte
//
// 0b11 = random static
// 0b01 = resolvable private
// 0b00 = non-resolvable private
// otherwise: public address (IEEE OUI)
// ------------------------------------------------------------

const char *bleAddrType(const uint8_t *addr)
{
    uint8_t top = addr[0] >> 6;

    if (top == 0x03)
        return "random static";

    if (top == 0x01)
        return "resolvable private";

    if (top == 0x00)
        return "non-resolvable";

    return "public";
}

// ------------------------------------------------------------
// Advertising-Callback
//
// Runs in the BLE host task, so only queue here and do the
// evaluation in loop().
// ------------------------------------------------------------

void queueBleAdvert(BLEAdvertisedDevice &dev)
{
    uint8_t next = (bleWrite + 1) % BLE_QUEUE_SIZE;

    if (next == bleRead)
    {
        bleDrops++;
        return;
    }

    BleAdvInfo &e = bleQueue[bleWrite];

    memset(&e, 0, sizeof(e));

    // Parse the address from its text form, that is stable
    // across core versions.
    unsigned int b[6] = { 0, 0, 0, 0, 0, 0 };

    if (sscanf(dev.getAddress().toString().c_str(),
               "%x:%x:%x:%x:%x:%x",
               &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6)
    {
        return;
    }

    for (int i = 0; i < 6; i++)
        e.addr[i] = (uint8_t)b[i];

    e.rssi    = dev.getRSSI();
    e.txPower = dev.haveTXPower() ? dev.getTXPower() : 127;
    e.company = 0xFFFF;

    if (dev.haveName())
        copyPrintable(e.name, sizeof(e.name), dev.getName().c_str());

    if (dev.haveServiceUUID())
    {
        copyPrintable(e.svc,
                      sizeof(e.svc),
                      dev.getServiceUUID().toString().c_str());
    }

    if (dev.haveManufacturerData())
    {
        // Core 3.3.x returns an Arduino String here, older
        // cores returned std::string. Both expose length()
        // and operator[], so this works either way.
        String md = dev.getManufacturerData();

        if (md.length() >= 2)
        {
            e.company =
                (uint16_t)(uint8_t)md[0] |
                ((uint16_t)(uint8_t)md[1] << 8);
        }
    }

    bleWrite = next;

    bleFrames++;
}

// The Arduino BLE library declares
//     virtual void onResult(BLEAdvertisedDevice device) = 0;
// i.e. the device is passed by value. Some NimBLE forks use
// a pointer instead; in that case change the signature to
//     void onResult(BLEAdvertisedDevice *dev)
// and pass *dev below.
class BleScanCallbacks : public BLEAdvertisedDeviceCallbacks
{
    void onResult(BLEAdvertisedDevice dev) override
    {
        queueBleAdvert(dev);
    }
};

BleScanCallbacks bleCallbacks;

// ------------------------------------------------------------
// Drain the queue
// ------------------------------------------------------------

void processBleQueue()
{
    while (bleRead != bleWrite)
    {
        BleAdvInfo &e = bleQueue[bleRead];

        int      idx      = -1;
        int      freeSlot = -1;
        uint32_t oldest   = UINT32_MAX;
        int      oldestIx = 0;

        for (int i = 0; i < MAX_BLE_DEVICES; i++)
        {
            if (bleDevices[i].valid)
            {
                if (memcmp(bleDevices[i].addr, e.addr, 6) == 0)
                {
                    idx = i;
                    break;
                }

                if (bleDevices[i].lastSeen < oldest)
                {
                    oldest   = bleDevices[i].lastSeen;
                    oldestIx = i;
                }
            }
            else if (freeSlot < 0)
            {
                freeSlot = i;
            }
        }

        if (idx < 0)
        {
            idx = (freeSlot >= 0) ? freeSlot : oldestIx;

            memset(&bleDevices[idx], 0, sizeof(BleDevice));

            bleDevices[idx].valid     = true;
            bleDevices[idx].firstSeen = millis();
            bleDevices[idx].rssiPeak  = -127;

            memcpy(bleDevices[idx].addr, e.addr, 6);
        }

        BleDevice &d = bleDevices[idx];

        d.rssi = e.rssi;

        if (e.rssi > d.rssiPeak)
            d.rssiPeak = e.rssi;

        if (e.txPower != 127)
            d.txPower = e.txPower;

        if (e.company != 0xFFFF)
            d.company = e.company;

        if (e.name[0])
            strncpy(d.name, e.name, sizeof(d.name) - 1);

        if (e.svc[0])
            strncpy(d.svc, e.svc, sizeof(d.svc) - 1);

        d.adverts++;
        d.lastSeen = millis();

        bleLastAdvert = millis();

        bleRead = (bleRead + 1) % BLE_QUEUE_SIZE;
    }
}

void cleanupBleDevices()
{
    uint32_t now = millis();

    for (int i = 0; i < MAX_BLE_DEVICES; i++)
    {
        if (!bleDevices[i].valid)
            continue;

        if (now - bleDevices[i].lastSeen > BLE_TIMEOUT_MS)
            bleDevices[i].valid = false;
    }
}

// ============================================================
// IEEE 802.15.4
//
// Zigbee, Thread and plain 802.15.4 sit in the same 2.4 GHz
// band as Wi-Fi and BLE, so this is a third exclusive mode.
// Two things are collected per channel:
//
// - an energy detect reading, which gives a noise floor even
//   when nobody is transmitting
// - promiscuous frames, from which PAN ID, addresses and the
//   frame type are taken
//
// Channel 11 sits at 2405 MHz, channel 26 at 2480 MHz, each
// 2 MHz wide with 5 MHz spacing. Wi-Fi channel 1 therefore
// overlaps 802.15.4 channels 11 to 14, Wi-Fi 6 overlaps 15 to
// 19 and Wi-Fi 11 overlaps 20 to 24.
// ============================================================

struct ZbChannelStats
{
    uint32_t frames;
    uint32_t bytes;
    int8_t   maxRssi;
    int8_t   edLevel;      // 127 = no reading yet
    uint8_t  lqi;
};

ZbChannelStats zbChannels[ZB_NUM_CHANNELS];

struct ZbNode
{
    bool     valid;
    uint16_t panId;
    uint16_t shortAddr;
    uint8_t  extAddr[8];
    bool     hasExt;
    uint8_t  channel;
    int8_t   rssi;
    uint8_t  lqi;
    uint32_t frames;
    uint8_t  lastType;
    uint32_t firstSeen;
    uint32_t lastSeen;
};

ZbNode *zbNodes = nullptr;            // PSRAM

struct ZbFrameInfo
{
    uint8_t length;
    int8_t  rssi;
    uint8_t lqi;
    uint8_t channel;
    uint8_t data[ZB_HEADER_BYTES];
};

/*
 * This one stays in internal RAM on purpose: the 802.15.4
 * receive callback runs in interrupt context, and PSRAM must
 * not be touched from there.
 */
ZbFrameInfo zbQueue[ZB_QUEUE_SIZE];

volatile uint8_t zbWrite = 0;
volatile uint8_t zbRead  = 0;

volatile uint32_t zbDrops  = 0;
volatile uint32_t zbFrames = 0;

uint8_t  zbChannelIndex = 0;
uint32_t zbLastSwitch   = 0;
bool     zbEnabled      = false;

volatile bool zbNeedRx = false;

volatile int8_t zbEdResult  = 127;
volatile bool   zbEdPending = false;

// -1 = sweep across all channels, otherwise stay on this one
int8_t zbLockedChannel = -1;

uint32_t zbRxErrors = 0;

uint8_t zbScanMode = ZB_SCAN_FRAMES;

volatile bool zbEdReady = false;

uint32_t zbEdStart = 0;

uint32_t zbEdFailures = 0;
uint32_t zbEdLast     = 0;
uint32_t zbEdDone     = 0;

// Short note shown in the web UI when something was decided
// automatically, e.g. a fallback out of the energy sweep.
char zbNote[80] = "";

bool     zbRxArmed      = false;
uint32_t zbRxArmedCount = 0;

/*
 * Energy waterfall
 *
 * One row per completed sweep across all 16 channels. This is
 * pure power measurement, so it also catches everything that
 * is not decodable: microwave ovens, video senders, hopping
 * Bluetooth links, wireless mice, broken transmitters.
 */
struct EdEntry
{
    uint32_t timestamp;
    int8_t   level[ZB_NUM_CHANNELS];
};

EdEntry *edHistory = nullptr;      // PSRAM

uint32_t edCapacity = 0;
uint32_t edWrite    = 0;
uint32_t edCount    = 0;

int8_t edCurrent[ZB_NUM_CHANNELS];

uint32_t edSweeps    = 0;
uint32_t edRateStart = 0;
uint32_t edRateCount = 0;
float    edRate      = 0.0f;

/*
 * Reference point for the stuck-sweep check in loop().
 *
 * It has to be re-armed whenever the energy sweep (re)starts.
 * Kept as function statics it was only updated while the
 * sweep ran, so on entry it was minutes old: the check fired
 * on the very first pass, found no new sweep and fell back to
 * frame capture before a single sweep could complete.
 */
uint32_t edWatchSweeps = 0;
uint32_t edWatchSince  = 0;

void armEdWatch()
{
    edWatchSweeps = edSweeps;
    edWatchSince  = millis();
}

uint8_t zbChannelNumber(uint8_t index)
{
    return ZB_FIRST_CHANNEL + index;
}

uint16_t zbChannelFreq(uint8_t channel)
{
    return 2405 + 5 * (channel - ZB_FIRST_CHANNEL);
}

#if HAVE_IEEE802154

// ------------------------------------------------------------
// Radio callbacks
//
// Both are weak symbols inside the IDF component and carry C
// linkage, so they have to be declared extern "C" here.
// ------------------------------------------------------------

extern "C" void esp_ieee802154_receive_done(
    uint8_t *frame, esp_ieee802154_frame_info_t *frame_info)
{
    if (frame != nullptr)
    {
        uint8_t next = (zbWrite + 1) % ZB_QUEUE_SIZE;

        if (next != zbRead)
        {
            ZbFrameInfo &e = zbQueue[zbWrite];

            // frame[0] holds the PSDU length
            uint8_t len = frame[0];

            e.length  = len;
            e.rssi    = frame_info ? frame_info->rssi : -127;
            e.lqi     = frame_info ? frame_info->lqi : 0;
            e.channel = zbChannelNumber(zbChannelIndex);

            uint8_t copy = len;

            if (copy > ZB_HEADER_BYTES)
                copy = ZB_HEADER_BYTES;

            memcpy(e.data, &frame[1], copy);

            zbWrite = next;

            zbFrames++;
        }
        else
        {
            zbDrops++;
        }
    }

    if (frame != nullptr)
        esp_ieee802154_receive_handle_done(frame);
}

extern "C" void esp_ieee802154_energy_detect_done(int8_t power)
{
    zbEdResult  = power;
    zbEdPending = false;
    zbEdReady   = true;

    // The frame path needs receive re-armed, the energy path
    // decides for itself in zbLoopEnergy().
    zbNeedRx = true;
}

#endif

// ------------------------------------------------------------
// Frame parsing
//
// FCF bits: 0..2 frame type, 6 PAN ID compression,
// 10..11 destination addressing mode,
// 14..15 source addressing mode.
// Addressing mode 2 = short (16 bit), 3 = extended (64 bit).
// ------------------------------------------------------------

// The elaborated "struct" is required: the Arduino
// preprocessor puts the generated prototype above the struct
// definition, where the plain type name is not known yet.
void parseZbFrame(const struct ZbFrameInfo &e)
{
    uint8_t index = e.channel - ZB_FIRST_CHANNEL;

    if (index < ZB_NUM_CHANNELS)
    {
        ZbChannelStats &st = zbChannels[index];

        st.frames++;
        st.bytes += e.length;

        if (e.rssi > st.maxRssi)
            st.maxRssi = e.rssi;

        st.lqi = e.lqi;
    }

    if (e.length < 3)
        return;

    uint16_t fcf = e.data[0] | (e.data[1] << 8);

    uint8_t type     = fcf & 0x07;
    bool    panComp  = (fcf & 0x0040) != 0;
    uint8_t destMode = (fcf >> 10) & 0x03;
    uint8_t srcMode  = (fcf >> 14) & 0x03;

    // Acknowledgements carry no addresses
    if (type == 0x02)
        return;

    int pos = 3;                      // FCF + sequence number

    uint16_t panId = 0xFFFF;

    if (destMode != 0)
    {
        if (pos + 2 > ZB_HEADER_BYTES)
            return;

        panId = e.data[pos] | (e.data[pos + 1] << 8);
        pos += 2;

        pos += (destMode == 3) ? 8 : 2;
    }

    if (srcMode == 0)
        return;

    if (!panComp)
    {
        if (pos + 2 > ZB_HEADER_BYTES)
            return;

        panId = e.data[pos] | (e.data[pos + 1] << 8);
        pos += 2;
    }

    uint16_t shortAddr = 0xFFFF;
    uint8_t  extAddr[8];
    bool     hasExt = false;

    memset(extAddr, 0, sizeof(extAddr));

    if (srcMode == 2)
    {
        if (pos + 2 > ZB_HEADER_BYTES)
            return;

        shortAddr = e.data[pos] | (e.data[pos + 1] << 8);
    }
    else if (srcMode == 3)
    {
        if (pos + 8 > ZB_HEADER_BYTES)
            return;

        // Transmitted least significant byte first
        for (int i = 0; i < 8; i++)
            extAddr[i] = e.data[pos + 7 - i];

        hasExt = true;
    }
    else
    {
        return;
    }

    int      idx      = -1;
    int      freeSlot = -1;
    uint32_t oldest   = UINT32_MAX;
    int      oldestIx = 0;

    for (int i = 0; i < ZB_MAX_NODES; i++)
    {
        if (zbNodes[i].valid)
        {
            bool match =
                hasExt
                    ? (zbNodes[i].hasExt &&
                       memcmp(zbNodes[i].extAddr, extAddr, 8) == 0)
                    : (!zbNodes[i].hasExt &&
                       zbNodes[i].shortAddr == shortAddr &&
                       zbNodes[i].panId == panId);

            if (match)
            {
                idx = i;
                break;
            }

            if (zbNodes[i].lastSeen < oldest)
            {
                oldest   = zbNodes[i].lastSeen;
                oldestIx = i;
            }
        }
        else if (freeSlot < 0)
        {
            freeSlot = i;
        }
    }

    if (idx < 0)
    {
        idx = (freeSlot >= 0) ? freeSlot : oldestIx;

        memset(&zbNodes[idx], 0, sizeof(ZbNode));

        zbNodes[idx].valid     = true;
        zbNodes[idx].firstSeen = millis();
    }

    ZbNode &n = zbNodes[idx];

    n.panId     = panId;
    n.shortAddr = shortAddr;
    n.hasExt    = hasExt;

    if (hasExt)
        memcpy(n.extAddr, extAddr, 8);

    n.channel  = e.channel;
    n.rssi     = e.rssi;
    n.lqi      = e.lqi;
    n.lastType = type;
    n.frames++;
    n.lastSeen = millis();
}

void processZbQueue()
{
    while (zbRead != zbWrite)
    {
        parseZbFrame(zbQueue[zbRead]);

        zbRead = (zbRead + 1) % ZB_QUEUE_SIZE;
    }
}

void cleanupZbNodes()
{
    uint32_t now = millis();

    for (int i = 0; i < ZB_MAX_NODES; i++)
    {
        if (!zbNodes[i].valid)
            continue;

        if (now - zbNodes[i].lastSeen > ZB_NODE_TIMEOUT_MS)
            zbNodes[i].valid = false;
    }
}

// ------------------------------------------------------------
// Channel sweep
// ------------------------------------------------------------

void zbSelectChannel(uint8_t index)
{
#if HAVE_IEEE802154
    // Trace the first sweeps so a stalled driver is visible
    static uint8_t traceCount = 0;

    if (traceCount < 20)
    {
        traceCount++;

        Serial.printf("802.15.4: channel %u at %lu ms\r\n",
                      zbChannelNumber(index),
                      (unsigned long)millis());

        if (traceCount == 20)
        {
            Serial.println(
                "802.15.4: further channel switches not logged, "
                "the sweep keeps running");
        }
    }

    zbChannelIndex = index;

    // Defined state before touching the channel
    esp_ieee802154_sleep();

    esp_ieee802154_set_channel(zbChannelNumber(index));

    /*
     * Kick off one energy detect for the level display, but do
     * NOT make the receiver depend on its completion callback.
     * That was the bug: if the callback never arrived, receive
     * was never armed and not a single frame could ever be
     * decoded, while the sweep happily kept running.
     */
    zbEdPending = true;

    esp_err_t edErr = esp_ieee802154_energy_detect(ZB_ED_DURATION);

    if (edErr != ESP_OK)
    {
        zbEdPending = false;

        zbEdFailures++;

        if (zbEdFailures == 1)
        {
            Serial.printf(
                "802.15.4: energy_detect failed 0x%X, "
                "levels will stay empty\r\n", edErr);
        }
    }

    zbRxArmed    = false;
    zbLastSwitch = millis();
#else
    (void)index;
#endif
}

// ------------------------------------------------------------
// Energy sweep
// ------------------------------------------------------------

void zbStartEd()
{
#if HAVE_IEEE802154
    zbEdPending = true;
    zbEdStart   = millis();

    esp_err_t err = esp_ieee802154_energy_detect(ZB_ED_DURATION);

    if (err != ESP_OK)
    {
        zbEdPending = false;
        zbEdReady   = false;
        zbEdResult  = 127;

        zbEdFailures++;

        if (zbEdFailures == 1 || zbEdFailures % 100 == 0)
        {
            Serial.printf(
                "802.15.4: energy_detect failed 0x%X (%u times)\r\n",
                err, (unsigned)zbEdFailures);
        }

        /*
         * Hammering a driver that refuses the request is how
         * the device wedged before. Give up on the energy
         * sweep and go back to decoding frames.
         */
        if (zbEdFailures > 200)
        {
            Serial.println(
                "802.15.4: energy sweep unusable, back to frames");

            strncpy(zbNote,
                    "energy detect refused by the driver, "
                    "fell back to frame capture",
                    sizeof(zbNote) - 1);

            zbScanMode = ZB_SCAN_FRAMES;

            zbEdFailures = 0;

            zbSelectChannel(0);
        }
    }
    else
    {
        zbEdFailures = 0;
    }
#endif
}

void commitEdRow()
{
    edSweeps++;
    edRateCount++;

    uint32_t now = millis();

    if (now - edRateStart >= 1000)
    {
        edRate = (float)edRateCount * 1000.0f / (now - edRateStart);

        edRateCount = 0;
        edRateStart = now;
    }

    if (edHistory == nullptr || edCapacity == 0)
        return;

    EdEntry &e = edHistory[edWrite];

    e.timestamp = now;

    for (int i = 0; i < ZB_NUM_CHANNELS; i++)
        e.level[i] = edCurrent[i];

    edWrite++;

    if (edWrite >= edCapacity)
        edWrite = 0;

    if (edCount < edCapacity)
        edCount++;
}

uint32_t edStartIndex()
{
    if (edCount < edCapacity)
        return 0;

    return edWrite;
}

void zbLoopEnergy()
{
#if HAVE_IEEE802154
    if (zbEdReady)
    {
        zbEdReady = false;

        int8_t level = zbEdResult;

        zbEdResult = 127;

        edCurrent[zbChannelIndex] = level;

        if (level != 127)
            zbChannels[zbChannelIndex].edLevel = level;

        uint8_t next = zbChannelIndex + 1;

        if (next >= ZB_NUM_CHANNELS)
        {
            commitEdRow();
            next = 0;
        }

        zbChannelIndex = next;

        /*
         * Put the radio into a defined state before touching
         * the channel. Calling energy_detect() straight out of
         * the previous measurement is what the driver does not
         * expect.
         */
        esp_ieee802154_sleep();

        esp_ieee802154_set_channel(zbChannelNumber(zbChannelIndex));

        zbEdLast = millis();

        return;
    }

    // Guard against a lost completion callback
    if (zbEdPending && millis() - zbEdStart > 200)
    {
        Serial.println("802.15.4: energy detect timed out");

        zbEdPending = false;
        zbEdFailures++;

        esp_ieee802154_sleep();

        zbEdLast = millis();

        return;
    }

    // Minimum gap between two measurements
    if (!zbEdPending && millis() - zbEdLast >= 2)
        zbStartEd();
#endif
}

void zbArmReceive()
{
#if HAVE_IEEE802154
    esp_err_t err = esp_ieee802154_receive();

    if (err != ESP_OK)
        zbRxErrors++;
#endif
}

void zbLoop()
{
#if HAVE_IEEE802154
    if (!zbEnabled)
        return;

    if (zbScanMode == ZB_SCAN_ENERGY)
    {
        zbLoopEnergy();
        return;
    }

    bool hadFrames = (zbRead != zbWrite);

    processZbQueue();

    // The radio can drop out of receive after a frame, so
    // re-arm whenever something came in.
    if (hadFrames)
        zbArmReceive();

    // Pick up a finished energy detect if there was one
    if (zbNeedRx)
    {
        zbNeedRx = false;

        if (zbEdResult != 127)
        {
            zbChannels[zbChannelIndex].edLevel = zbEdResult;

            zbEdDone++;

            if (zbEdDone == 1)
            {
                Serial.printf(
                    "802.15.4: first energy detect on channel %u: "
                    "%d dBm\r\n",
                    zbChannelNumber(zbChannelIndex),
                    (int)zbEdResult);
            }

            zbEdResult = 127;
        }
    }

    /*
     * Arm the receiver a few ms into the dwell, no matter what
     * the energy detect did. Everything after that belongs to
     * listening for frames.
     */
    if (!zbRxArmed && millis() - zbLastSwitch >= 10)
    {
        zbEdPending = false;

        zbArmReceive();

        zbRxArmed = true;

        if (zbRxArmedCount == 0)
            Serial.println("802.15.4: receiver armed");

        zbRxArmedCount++;
    }

    if (zbLockedChannel >= 0)
        return;

    if (millis() - zbLastSwitch >= ZB_DWELL_MS && !zbEdPending)
    {
        zbSelectChannel((zbChannelIndex + 1) % ZB_NUM_CHANNELS);
    }
#endif
}

bool zbStart()
{
#if HAVE_IEEE802154
    for (int i = 0; i < ZB_NUM_CHANNELS; i++)
    {
        zbChannels[i].maxRssi = -127;
        zbChannels[i].edLevel = 127;
    }

    Serial.println("802.15.4: enabling radio");

    esp_err_t err = esp_ieee802154_enable();

    if (err != ESP_OK)
    {
        Serial.printf("802.15.4: enable failed 0x%X\r\n", err);
        return false;
    }

    Serial.println("802.15.4: radio enabled");

    esp_ieee802154_set_promiscuous(true);

    /*
     * Receive is armed explicitly per dwell instead of leaving
     * the receiver permanently on. A 15.4 receiver sitting in
     * a band full of Wi-Fi triggers constantly, and the
     * resulting interrupt load starved the SPI Ethernet.
     */
    esp_ieee802154_set_rx_when_idle(false);
    esp_ieee802154_set_panid(0xFFFF);

    zbEnabled = true;

    for (int i = 0; i < ZB_NUM_CHANNELS; i++)
        edCurrent[i] = 127;

    zbChannelIndex = 0;
    zbEdReady      = false;
    zbEdPending    = false;
    edRateStart    = millis();
    edRateCount    = 0;

    if (zbScanMode == ZB_SCAN_ENERGY)
    {
        esp_ieee802154_set_channel(zbChannelNumber(0));

        zbStartEd();

        Serial.println("802.15.4: energy sweep running");
    }
    else
    {
        zbSelectChannel(0);

        Serial.println("802.15.4: decoding frames, channels 11-26");
    }

    return true;
#else
    Serial.println("802.15.4: not built into this core");
    return false;
#endif
}

void zbStop()
{
#if HAVE_IEEE802154
    if (!zbEnabled)
        return;

    zbEnabled = false;

    Serial.println("802.15.4: stopping");

    zbEdPending = false;
    zbEdReady   = false;

    esp_ieee802154_sleep();

    Serial.println("802.15.4: radio asleep");

    esp_ieee802154_set_promiscuous(false);

    esp_ieee802154_disable();

    Serial.println("802.15.4: disabled");
#endif
}

// ------------------------------------------------------------
// Mode switching
// ------------------------------------------------------------

void startBleScan()
{
    if (bleScan == nullptr)
        return;

    bleScan->setActiveScan(bleActiveScan);
    bleScan->setInterval(100);
    bleScan->setWindow(99);

    /*
     * IMPORTANT
     *
     * The two argument form start(duration, is_continue)
     * BLOCKS: it waits on the scan-end semaphore. With a
     * duration of 0 that means forever, loop() never runs
     * again and the whole device appears dead.
     *
     * The three argument form with a completion callback
     * returns immediately. nullptr is fine as the callback
     * because the scan is re-armed from loop() anyway, which
     * also covers a scan that ended on its own.
     */
    bleScan->start(BLE_SCAN_DURATION_S, nullptr, false);

    bleScanStarted = millis();
    bleLastAdvert  = millis();
}

void stopBleScan()
{
    if (bleScan == nullptr)
        return;

    bleScan->stop();
    bleScan->clearResults();
}

// ------------------------------------------------------------
// Mode switching
//
// Wi-Fi is deliberately NOT deinitialised here. Tearing the
// Wi-Fi driver down and bringing it back up while the HTTP
// server and the W5500 are running turned out to be fragile.
// Switching promiscuous mode off and stopping the sweep is
// enough: an idle STA neither transmits nor hops channels,
// so BLE gets the radio.
// ------------------------------------------------------------

void enterBleMode()
{
    if (scanMode == MODE_BLE)
        return;

    Serial.printf("Mode: BLE, heap %.1f kB\r\n", ESP.getFreeHeap() / 1024.0);

    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(NULL);

    if (scanMode == MODE_ZB)
    {
        zbStop();

        esp_wifi_start();

        delay(50);
    }

    if (!bleInitialised)
    {
        BLEDevice::init("");

        bleScan = BLEDevice::getScan();

        if (bleScan == nullptr)
        {
            Serial.println("BLE: getScan() failed, staying on Wi-Fi");

            esp_wifi_set_promiscuous_rx_cb(wifiPromiscuousCallback);
            applyPromiscuousFilter();
            esp_wifi_set_promiscuous(true);

            return;
        }

        bleScan->setAdvertisedDeviceCallbacks(&bleCallbacks);

        bleInitialised = true;

        Serial.printf("BLE: initialised, heap %.1f kB\r\n",
                      ESP.getFreeHeap() / 1024.0);
    }

    startBleScan();

    scanMode = MODE_BLE;

    Serial.println("BLE: scanning");
}

void enterZbMode()
{
    if (scanMode == MODE_ZB)
        return;

    Serial.printf("Mode: 802.15.4, heap %.1f kB\r\n",
                  ESP.getFreeHeap() / 1024.0);

    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(NULL);

    stopBleScan();

    /*
     * Unlike BLE, the 802.15.4 radio does not get along with a
     * Wi-Fi driver that is merely idle: leaving it running
     * made the device stop serving HTTP entirely. Stop the
     * driver, but do not deinitialise it - Ethernet runs over
     * SPI and is not affected either way.
     */
    esp_err_t stopErr = esp_wifi_stop();

    Serial.printf("802.15.4: esp_wifi_stop 0x%X\r\n", stopErr);

    if (!zbStart())
    {
        // Fall back to Wi-Fi so the device stays usable
        esp_wifi_start();

        esp_wifi_set_promiscuous_rx_cb(wifiPromiscuousCallback);
        applyPromiscuousFilter();
        esp_wifi_set_promiscuous(true);

        return;
    }

    armEdWatch();

    scanMode = MODE_ZB;
}

void enterWifiMode()
{
    if (scanMode == MODE_WIFI)
        return;

    Serial.printf("Mode: Wi-Fi, heap %.1f kB\r\n",
                  ESP.getFreeHeap() / 1024.0);

    stopBleScan();

    Serial.println("Wi-Fi: BLE stopped");

    bool wasZb = (scanMode == MODE_ZB);

    zbStop();

    if (wasZb)
    {
        esp_err_t startErr = esp_wifi_start();

        Serial.printf("Wi-Fi: esp_wifi_start 0x%X\r\n", startErr);

        delay(50);
    }

    Serial.println("Wi-Fi: re-arming promiscuous");

    esp_wifi_set_promiscuous_rx_cb(wifiPromiscuousCallback);

    applyPromiscuousFilter();

    esp_wifi_set_promiscuous(true);

    portENTER_CRITICAL(&measurementMux);
    resetLiveCounters();
    portEXIT_CRITICAL(&measurementMux);

    switchChannel(currentChannelIndex);

    lastChannelSwitch = millis();

    scanMode = MODE_WIFI;

    Serial.printf("Wi-Fi: sweeping, heap %.1f kB\r\n",
                  ESP.getFreeHeap() / 1024.0);
}

// ------------------------------------------------------------
// The request handler only records what was asked for.
//
// BLEDevice::init() takes a while and must not run inside the
// HTTP handler, otherwise the response is never flushed and
// the browser sits there waiting.
// ------------------------------------------------------------

volatile int8_t  pendingMode   = -1;
volatile bool    pendingActive = false;

// -2 = nothing requested, -1 = resume sweep, else lock channel
volatile int8_t  pendingZbChannel = -2;

// -1 = nothing requested
volatile int8_t  pendingZbScan = -1;

// Set by every request handler. If nothing is served for a
// while in an exclusive mode, the device has made itself
// unreachable and returns to Wi-Fi on its own.
uint32_t lastRequestServed = 0;

void handleMode()
{
    // Log the request verbatim: if a setting does not stick,
    // this shows whether it arrived at all and with what
    {
        String q;

        for (uint8_t i = 0; i < reqArgs(); i++)
        {
            if (i)
                q += "&";

            q += reqArgName(i) + "=" + reqArgValue(i);
        }

        Serial.printf("[mode] %s -> %s\r\n",
                      reqClientText().c_str(),
                      q.length() ? q.c_str() : "(no arguments)");
    }

    if (reqHasArg("active"))
    {
        bool on = (reqArg("active").toInt() != 0);

        bool changed = (on != bleActiveScan);

        if (changed)
            logRequest("BLE %s scan selected", on ? "active" : "passive");

        bleActiveScan = on;
        pendingActive = true;

        if (changed)
            saveSettings();
    }

    if (reqHasArg("name"))
    {
        String name = reqArg("name");

        name.trim();

        saveSensorName(name.c_str());

        logRequest("sensor name set to \"%s\"", sensorName);
    }

    if (reqHasArg("capture"))
    {
        bool on = (reqArg("capture").toInt() != 0);

        if (on != pcapEnabled)
            logRequest("pcap capture %s", on ? "enabled" : "disabled");

        pcapEnabled = on;

        saveSettings();
    }

    if (reqHasArg("capmask"))
    {
        uint8_t mask = (uint8_t)reqArg("capmask").toInt();

        if (mask != pcapMask)
            logRequest("pcap frame types set to 0x%02X", mask);

        pcapMask = mask;

        saveSettings();
    }

    if (reqHasArg("tz"))
    {
        /*
         * Minutes east of UTC. Fourteen hours ahead and twelve
         * behind cover every zone in use, half-hour and
         * quarter-hour offsets included.
         */
        long want = reqArg("tz").toInt();

        if (want >= -720 && want <= 840)
        {
            if (want != tzMinutes)
            {
                tzMinutes = (int16_t)want;

                saveSettings();

                char label[12];
                formatTzLabel(label, sizeof(label));

                logRequest("display timezone set to %s", label);
            }
        }
    }

    if (reqHasArg("fast"))
    {
        bool on = (reqArg("fast").toInt() != 0);

        if (on != fastSweep)
        {
            fastSweep = on;

            saveSettings();

            buildScanPlan();

            logRequest("%s sweep selected, scanning %u of %u "
                       "selected channels",
                       fastSweep ? "fast" : "standard",
                       (unsigned)planCount,
                       (unsigned)countSelectedChannels());
        }
    }

    if (reqHasArg("zbscan"))
    {
        uint8_t wanted =
            (reqArg("zbscan").toInt() != 0) ? ZB_SCAN_ENERGY
                                                : ZB_SCAN_FRAMES;

        if (wanted != zbScanMode)
        {
            pendingZbScan = (int8_t)wanted;

            logRequest("802.15.4 %s selected",
                       wanted == ZB_SCAN_ENERGY ? "energy sweep"
                                                : "frame capture");
        }
    }

    if (reqHasArg("zbch"))
    {
        int ch = reqArg("zbch").toInt();

        if (ch >= ZB_FIRST_CHANNEL && ch <= ZB_LAST_CHANNEL)
        {
            pendingZbChannel = (int8_t)ch;

            logRequest("802.15.4 locked to channel %d", ch);
        }
        else
        {
            pendingZbChannel = -1;

            logRequest("802.15.4 sweep resumed");
        }
    }

    if (reqHasArg("mode"))
    {
        String mode = reqArg("mode");

        if (mode == "ble")
            pendingMode = MODE_BLE;
        else if (mode == "zb")
            pendingMode = MODE_ZB;
        else
            pendingMode = MODE_WIFI;

        if (pendingMode != (int8_t)scanMode)
            logRequest("mode change requested: %s", mode.c_str());
    }

    // Answer with the resulting state so the caller can verify
    String reply = "{\"ok\":1,\"capture\":";

    reply += pcapEnabled ? "1" : "0";
    reply += ",\"capmask\":";
    reply += pcapMask;
    reply += ",\"fast\":";
    reply += fastSweep ? "1" : "0";
    reply += ",\"mode\":";
    reply += scanMode;
    reply += "}";

    reqSend(200, "application/json", reply);
}

/*
 * Bring the W5500 up.
 *
 * Separate from setup() so the interface can be taken down
 * and started again when it stops answering, without
 * touching the SPI bus or the event handler, which only need
 * to be set up once.
 */
void setupEthernet()
{
    pinMode(W5500_RST, OUTPUT);

    digitalWrite(W5500_RST, LOW);
    delay(10);

    digitalWrite(W5500_RST, HIGH);
    delay(100);

    Serial.println();
    Serial.println("Starting W5500 Ethernet...");

    ethPresent = ETH.begin(ETH_PHY_W5500,
                           1,
                           W5500_CS,
                           W5500_INT,
                           W5500_RST,
                           SPI);

    Serial.println(ethPresent ? "ETH.begin(): OK"
                              : "ETH.begin(): FAILED, no W5500? "
                                "Running on USB only");
}

/*
 * Restart the DHCP client properly.
 *
 * ETH.config() with empty addresses was supposed to do this,
 * but its result was never checked, and after a static
 * address had been set the client stayed down: no discover
 * ever went out on the wire. Going through esp_netif directly
 * makes each step visible - stop the client, drop the address
 * that is still configured, start the client.
 */
bool restartDhcpClient(const char *why)
{
    esp_netif_t *netif = ETH.netif();

    if (netif == nullptr)
    {
        bool ok = ETH.config(IPAddress((uint32_t)0),
                             IPAddress((uint32_t)0),
                             IPAddress((uint32_t)0));

        Serial.printf("ETH: no netif handle, ETH.config(dhcp) %s\r\n",
                      ok ? "OK" : "FAILED");

        return ok;
    }

    esp_err_t stopErr = esp_netif_dhcpc_stop(netif);

    esp_netif_ip_info_t zero;

    memset(&zero, 0, sizeof(zero));

    esp_err_t clearErr = esp_netif_set_ip_info(netif, &zero);
    esp_err_t startErr = esp_netif_dhcpc_start(netif);

    Serial.printf("ETH: DHCP restart (%s): stop 0x%X, clear 0x%X, "
                  "start 0x%X\r\n",
                  why, stopErr, clearErr, startErr);

    bool ok = (startErr == ESP_OK ||
               startErr == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED);

    if (!ok)
    {
        logEventFrom("system",
                     "DHCP client would not start, error 0x%X",
                     startErr);
    }

    return ok;
}

/*
 * One word about the Ethernet state for the heap line.
 *
 * The settings page is no help while the device has no
 * address, which is exactly when this matters, so the state
 * goes where it can always be read.
 */
const char *ethStatusText()
{
    static char buffer[40];

    if (ethGotIP)
    {
        snprintf(buffer, sizeof(buffer), "up/%s",
                 ethStaticActive ? "static" : "dhcp");

        return buffer;
    }

    snprintf(buffer, sizeof(buffer), "down/link %s/dhcp %s/try %u",
             ethLinkUp ? "up" : "down",
             dhcpStatusText(),
             (unsigned)dhcpAttempts);

    return buffer;
}

// What the DHCP client is doing, for the settings page
const char *dhcpStatusText()
{
    esp_netif_t *netif = ETH.netif();

    if (netif == nullptr)
        return "no interface";

    esp_netif_dhcp_status_t status;

    if (esp_netif_dhcpc_get_status(netif, &status) != ESP_OK)
        return "unknown";

    switch (status)
    {
        case ESP_NETIF_DHCP_INIT:    return "idle";
        case ESP_NETIF_DHCP_STARTED: return "running";
        case ESP_NETIF_DHCP_STOPPED: return "stopped";
        default:                     break;
    }

    return "unknown";
}

// lwIP's DHCP client state, nullptr without a client
const char *dhcpLeaseStateText(uint8_t state)
{
    switch (state)
    {
        case DHCP_STATE_REQUESTING:  return "requesting";
        case DHCP_STATE_INIT:        return "init";
        case DHCP_STATE_REBOOTING:   return "rebooting";
        case DHCP_STATE_REBINDING:   return "rebinding";
        case DHCP_STATE_RENEWING:    return "renewing";
        case DHCP_STATE_SELECTING:   return "selecting";
        case DHCP_STATE_INFORMING:   return "informing";
        case DHCP_STATE_CHECKING:    return "checking";
        case DHCP_STATE_BOUND:       return "bound";
        case DHCP_STATE_BACKING_OFF: return "backing off";
    }

    return nullptr;
}

/*
 * Follow the lease once a second and log what deserves it:
 * the first lease, a renewal the server did not answer at once,
 * rebinding and the end of a lease. A renewal that goes through
 * on the first try is only counted, every 45 minutes in the
 * event log would push everything else out.
 */
void dhcpLeaseWatch()
{
    static uint32_t lastCheck = 0;

    if (!ethPresent || millis() - lastCheck < 1000)
        return;

    lastCheck = millis();

    esp_netif_t *esp = ETH.netif();
    struct netif *lw = esp ? (struct netif *)esp_netif_get_netif_impl(esp)
                           : nullptr;

    uint8_t  state   = DHCP_STATE_OFF;
    uint32_t seconds = 0;
    uint32_t server  = 0;
    uint32_t addr    = 0;
    uint32_t used    = 0;
    uint32_t renewIn = 0;

    if (lw != nullptr)
    {
        LOCK_TCPIP_CORE();

        struct dhcp *d = netif_dhcp_data(lw);

        if (d != nullptr)
        {
            state   = d->state;
            seconds = d->offered_t0_lease;
            server  = ip4_addr_get_u32(ip_2_ip4(&d->server_ip_addr));
            addr    = ip4_addr_get_u32(ip_2_ip4(&lw->ip_addr));
            used    = d->lease_used;
            renewIn = d->t1_renew_time;
        }

        UNLOCK_TCPIP_CORE();
    }

    uint8_t  prev = dhcpLease.state;

    /*
     * A renewal the server answers at once passes through
     * RENEWING within milliseconds, between two looks. lwIP
     * restarts its count of the lease on every ACK, so a count
     * that went back is a renewal.
     */
    bool quietRenewal = state == DHCP_STATE_BOUND &&
                        prev == DHCP_STATE_BOUND &&
                        used < dhcpLease.used;

    dhcpLease.used    = used;
    dhcpLease.renewIn = renewIn;

    if (quietRenewal)
    {
        dhcpLease.renewals++;
        dhcpLease.ackAt   = millis();
        dhcpLease.seconds = seconds;
        dhcpLease.server  = server;

        Serial.printf("DHCP: lease renewed (%lu since start)\r\n",
                      (unsigned long)dhcpLease.renewals);
    }

    if (state == prev)
        return;

    uint32_t inPrev = (millis() - dhcpLease.stateSince) / 1000;

    dhcpLease.state      = state;
    dhcpLease.stateSince = millis();

    Serial.printf("DHCP: %s -> %s\r\n",
                  dhcpLeaseStateText(prev) ? dhcpLeaseStateText(prev) : "off",
                  dhcpLeaseStateText(state) ? dhcpLeaseStateText(state) : "off");

    char ip[16];
    char srv[16];
    IPAddress(addr).toString().toCharArray(ip, sizeof(ip));
    IPAddress(server).toString().toCharArray(srv, sizeof(srv));

    bool held = prev == DHCP_STATE_BOUND ||
                prev == DHCP_STATE_RENEWING ||
                prev == DHCP_STATE_REBINDING;

    if (state == DHCP_STATE_BOUND)
    {
        uint32_t left = 0;

        if (dhcpLease.ackAt != 0 && dhcpLease.seconds != 0)
        {
            uint32_t used = (millis() - dhcpLease.ackAt) / 1000;
            left = used < dhcpLease.seconds ? dhcpLease.seconds - used : 0;
        }

        dhcpLease.seconds = seconds;
        dhcpLease.server  = server;
        dhcpLease.ackAt   = millis();

        if (prev == DHCP_STATE_RENEWING)
        {
            dhcpLease.renewals++;

            // lwIP sends the first request at once and repeats
            // it after a while, so a slow answer means a lost one
            if (inPrev > 20)
                logEventFrom("system", "lease renewed after %lu s, "
                             "%lu min left before",
                             (unsigned long)inPrev,
                             (unsigned long)(left / 60));
        }
        else if (prev == DHCP_STATE_REBINDING)
        {
            dhcpLease.renewals++;

            logEventFrom("system", "lease kept by rebinding, %s "
                         "answered with %lu min left",
                         srv, (unsigned long)(left / 60));
        }
        else
        {
            logEventFrom("system", "lease %s from %s for %lu min",
                         ip, srv, (unsigned long)(seconds / 60));
        }
    }
    else if (state == DHCP_STATE_REBINDING)
    {
        logEventFrom("system", "%s did not answer the renewal, "
                     "asking any DHCP server (rebinding)",
                     IPAddress(dhcpLease.server).toString().c_str());
    }
    else if (held && state != DHCP_STATE_RENEWING && ethLinkUp)
    {
        logEventFrom("system", "lease ended (%s), DHCP client %s",
                     prev == DHCP_STATE_REBINDING ? "not renewed in time"
                                                  : "refused by the server",
                     dhcpLeaseStateText(state) ? dhcpLeaseStateText(state)
                                               : "off");
    }
}

void applyPendingMode()
{
    if (pendingMode >= 0)
    {
        PROBE_MARK("mode switch");

        int8_t wanted = pendingMode;

        pendingMode = -1;

        if (wanted == MODE_BLE)
            enterBleMode();
        else if (wanted == MODE_ZB)
            enterZbMode();
        else
            enterWifiMode();
    }

    if (pendingZbScan >= 0)
    {
        uint8_t wanted = (uint8_t)pendingZbScan;

        pendingZbScan = -1;

        zbScanMode = wanted;

        /*
         * Switch the sweep, do NOT cycle the radio. Disabling
         * and re-enabling threw away every reading and looked
         * like the mode had not changed at all.
         */
        if (scanMode == MODE_ZB && zbEnabled)
        {
            zbNote[0] = 0;

            zbEdPending = false;
            zbEdReady   = false;
            zbEdResult  = 127;

            zbChannelIndex = 0;

            for (int i = 0; i < ZB_NUM_CHANNELS; i++)
                edCurrent[i] = 127;

            edRateStart = millis();
            edRateCount = 0;

            armEdWatch();

#if HAVE_IEEE802154
            if (zbScanMode == ZB_SCAN_ENERGY)
            {
                esp_ieee802154_sleep();
                esp_ieee802154_set_channel(zbChannelNumber(0));

                zbEdLast = millis();

                Serial.println("802.15.4: switched to energy sweep");
            }
            else
            {
                zbSelectChannel(0);

                Serial.println("802.15.4: switched to frame capture");
            }
#endif
        }
    }

    if (pendingZbChannel != -2)
    {
        int8_t wanted = pendingZbChannel;

        pendingZbChannel = -2;

        zbLockedChannel = wanted;

        if (scanMode == MODE_ZB && zbEnabled)
        {
            if (wanted >= 0)
                zbSelectChannel(wanted - ZB_FIRST_CHANNEL);
        }
    }

    if (pendingActive)
    {
        pendingActive = false;

        if (scanMode == MODE_BLE && bleScan != nullptr)
        {
            stopBleScan();
            startBleScan();
        }
    }
}

// ============================================================
// Chunked JSON output
//
// Responses used to be built as one String on the heap.
// With 33 channels and a full history that was more than
// 100 kB in one block, which fragments the heap until
// allocation fails and the W5500 driver runs out of
// buffers: link flapping and a stalled device.
//
// Everything now goes out in chunks of about 1 kB.
// ============================================================

String outBuf;

void outBegin()
{
    lastRequestServed = millis();

    if (!REQ_OWN_ARGS)
        noteWebClient((uint32_t)server.client().remoteIP());

    outBuf = "";
    outBuf.reserve(1400);

    reqStart(200, "application/json", -1, nullptr);
}

void outFlush()
{
    if (outBuf.length() == 0)
        return;

    reqContent(outBuf.c_str(), outBuf.length());

    outBuf = "";
}

void outStr(const char *text)
{
    outBuf += text;

    if (outBuf.length() >= 1024)
        outFlush();
}

void outNum(long value)
{
    outBuf += value;

    if (outBuf.length() >= 1024)
        outFlush();
}

/*
 * Operating classes as a JSON list of class numbers. As a single
 * number the mask would need 57 bits, and a browser holds only 53
 * exactly: class 134 alone lifts the value past that and the low
 * bits, 2.4 GHz among them, were rounded away.
 */
void outOpClasses(uint64_t mask)
{
    outStr("[");

    bool first = true;

    for (int i = 0; i < 57; i++)
    {
        if (!((mask >> i) & 1))
            continue;

        if (!first)
            outStr(",");

        outNum(81 + i);

        first = false;
    }

    outStr("]");
}

// "unk": the element codes, "unkin": the SEEN_* bits of each
void outUnknownElements(const uint16_t *list, const uint8_t *seenIn,
                        uint8_t count)
{
    outStr(",\"unk\":[");

    for (uint8_t i = 0; i < count; i++)
    {
        if (i)
            outStr(",");

        outNum(list[i]);
    }

    outStr("],\"unkin\":[");

    for (uint8_t i = 0; i < count; i++)
    {
        if (i)
            outStr(",");

        outNum(seenIn[i]);
    }

    outStr("]");
}

// 6 GHz channels as a list, for the same reason as the classes
void outSixGhzChannels(uint64_t mask)
{
    outStr("[");

    bool first = true;

    for (int i = 0; i < 59; i++)
    {
        if (!((mask >> i) & 1))
            continue;

        if (!first)
            outStr(",");

        outNum(1 + i * 4);

        first = false;
    }

    outStr("]");
}

// For bit fields wider than 32 bits; JSON numbers hold 53 exactly
void outNum64(uint64_t value)
{
    char buffer[24];

    snprintf(buffer, sizeof(buffer), "%llu",
             (unsigned long long)value);

    outBuf += buffer;

    if (outBuf.length() >= 1024)
        outFlush();
}

void outFixed(float value, int digits)
{
    outBuf += String(value, digits);

    if (outBuf.length() >= 1024)
        outFlush();
}

// JSON string including the quotes
/*
 * Length of the UTF-8 sequence at p, 0 when it is none: a lead byte
 * with as many continuation bytes as it announces, not overlong, no
 * surrogate, nothing above U+10FFFF.
 */
int utf8Length(const uint8_t *p)
{
    uint8_t c = p[0];

    if (c < 0x80)
        return 1;

    int      n;
    uint32_t cp;

    if (c >= 0xC2 && c <= 0xDF)
    {
        n  = 2;
        cp = c & 0x1F;
    }
    else if (c >= 0xE0 && c <= 0xEF)
    {
        n  = 3;
        cp = c & 0x0F;
    }
    else if (c >= 0xF0 && c <= 0xF4)
    {
        n  = 4;
        cp = c & 0x07;
    }
    else
        return 0;

    // The terminating 0 is no continuation byte, so this stops there
    for (int i = 1; i < n; i++)
    {
        if ((p[i] & 0xC0) != 0x80)
            return 0;

        cp = (cp << 6) | (p[i] & 0x3F);
    }

    if ((n == 3 && cp < 0x800) || (n == 4 && cp < 0x10000) ||
        (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF)
        return 0;

    return n;
}

/*
 * A JSON string. SSIDs and names come off the air as any bytes; one
 * that is not part of valid UTF-8 goes out as \u00XX, the character
 * of that value, so the answer stays valid UTF-8 for strict readers
 * and the byte still shows ("o\xa9\xf7" as "o\u00a9\u00f7").
 * Control characters are left out.
 */
void outJSON(const char *text)
{
    const uint8_t *p = (const uint8_t *)text;

    outBuf += '"';

    while (*p)
    {
        uint8_t c = *p;

        if (c == '"' || c == '\\')
        {
            outBuf += '\\';
            outBuf += (char)c;
            p++;
            continue;
        }

        if (c < 32)
        {
            p++;
            continue;
        }

        int n = utf8Length(p);

        if (n == 0)
        {
            char esc[7];

            snprintf(esc, sizeof(esc), "\\u%04x", c);
            outBuf += esc;
            p++;
            continue;
        }

        for (int i = 0; i < n; i++)
            outBuf += (char)p[i];

        p += n;
    }

    outBuf += '"';

    if (outBuf.length() >= 1024)
        outFlush();
}

// ,"name":"0A1B2C": bytes in hex, for the browser to decode
void outHexField(const char *name, const uint8_t *data, uint8_t length)
{
    char hex[3];

    outStr(",\"");
    outStr(name);
    outStr("\":\"");

    for (uint8_t i = 0; i < length; i++)
    {
        snprintf(hex, sizeof(hex), "%02X", data[i]);
        outStr(hex);
    }

    outStr("\"");
}

void outMac(const uint8_t *mac)
{
    char buffer[18];

    snprintf(buffer, sizeof(buffer),
             "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2],
             mac[3], mac[4], mac[5]);

    outStr("\"");
    outStr(buffer);
    outStr("\"");
}

// The Multi-Link fields, only for devices that sent the element
void outMlo(const struct MloInfo &m)
{
    if (!m.present)
        return;

    outStr(",\"mld\":");
    outMac(m.mldMac);

    outStr(",\"mlolink\":");
    outNum(m.linkId);

    outStr(",\"mlof\":");
    outNum(m.fields);

    outStr(",\"eml\":");
    outNum(m.eml);

    outStr(",\"mldcap\":");
    outNum(m.mldCaps);

    outStr(",\"mlolinks\":");
    outNum(m.linkMask);

    outStr(",\"mlomacs\":[");

    for (uint8_t i = 0; i < m.linkCount; i++)
    {
        if (i)
            outStr(",");

        outStr("{\"link\":");
        outNum(m.linkIds[i]);

        outStr(",\"mac\":");
        outMac(m.linkMacs[i]);

        outStr("}");
    }

    outStr("]");

    // Stations only (an AP's entry has no reqBand): the links of
    // the request, see MloInfo
    if (!m.reqBand)
        return;

    outStr(",\"mloband\":");
    outNum(m.reqBand);

    outStr(",\"mloprof\":[");

    for (uint8_t i = 0; i < m.profCount; i++)
    {
        outStr(i ? ",{\"link\":" : "{\"link\":");
        outNum(m.profLink[i]);

        outStr(",\"band\":");
        outNum(m.profBand[i]);

        outStr(",\"width\":");
        outNum(m.profWidth[i]);

        outStr(",\"nstr\":");
        outNum(m.profNstr[i]);

        outStr("}");
    }

    outStr("]");
}

void outEnd()
{
    outFlush();

    reqDone();
}

// ============================================================
// API: live data
// ============================================================

/*
 * The channel plan, so the browser does not have to keep a
 * copy of channels24[] and channels5[] in step with this
 * file. Fetched once when the page loads.
 */
void sendPlanJSON()
{
    outBegin();

    outStr("{\"b24\":[");

    for (uint16_t i = 0; i < NUM_CHANNELS_24; i++)
    {
        if (i)
            outStr(",");

        outNum(channels24[i]);
    }

    outStr("],\"b5\":[");

    for (uint16_t i = 0; i < NUM_CHANNELS_5; i++)
    {
        if (i)
            outStr(",");

        outNum(channels5[i]);
    }

    // The default channel plan, see saveDefaultChannels()
    outStr("],\"def\":[");

    bool first = true;

    for (uint16_t i = 0; i < TOTAL_CHANNELS && i < 64; i++)
    {
        if (!(defaultMask & (1ULL << i)))
            continue;

        if (!first)
            outStr(",");

        outNum(getChannelNumber(i));

        first = false;
    }

    outStr("]}");

    outEnd();
}


void sendLiveJSON()
{
    outBegin();

    outStr("{\"version\":");
    outJSON(FW_VERSION);

    outStr(",\"oui\":");
    outNum(OUI_FULL_COUNT);

    outStr(",\"currentChannel\":");
    outNum(getChannelNumber(currentChannelIndex));

    outStr(",\"netcap\":");
    outNum(MAX_NETWORKS);

    // What the device actually scans, so the checkboxes can
    // show the truth after a reload instead of all-selected
    outStr(",\"chans\":[");

    {
        bool first = true;

        for (uint16_t i = 0; i < TOTAL_CHANNELS; i++)
        {
            if (!channelSelected[i])
                continue;

            if (!first)
                outStr(",");

            first = false;

            outNum(getChannelNumber(i));
        }
    }

    outStr("]");

    outStr(",\"pcapcount\":");
    outNum(pcapCount);

    outStr(",\"pcapcap\":");
    outNum(PCAP_SLOTS);

    outStr(",\"pcaptotal\":");
    outNum(pcapTotal);

    outStr(",\"beacons\":");
    outNum(countBeaconSlots());

    outStr(",\"beacontotal\":");
    outNum(beaconTotal);

    outStr(",\"assoc\":");
    outNum(assocCount);

    outStr(",\"assoccap\":");
    outNum(ASSOC_SLOTS);

    outStr(",\"capture\":");
    outNum(pcapEnabled ? 1 : 0);

    outStr(",\"capmask\":");
    outNum(pcapMask);

    outStr(",\"fast\":");
    outNum(fastSweep ? 1 : 0);

    outStr(",\"plan\":");
    outNum(planCount);

    outStr(",\"heap\":");
    outNum(ESP.getFreeHeap());

    outStr(",\"psram\":");
    outNum(heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    outStr(",\"drops\":");
    outNum(mgmtDrops + dataDrops);

    outStr(",\"fcs\":");
    outNum(fcsErrors);

    outStr(",\"channels\":[");

    bool first = true;

    for (uint16_t i = 0; i < TOTAL_CHANNELS; i++)
    {
        if (!channelSelected[i])
            continue;

        if (!first)
            outStr(",");

        first = false;

        ChannelMeasurement &m = currentMeasurement[i];

        outStr("{\"ch\":");
        outNum(getChannelNumber(i));

        outStr(",\"band\":\"");
        outStr((i < NUM_CHANNELS_24) ? "2.4" : "5");
        outStr("\"");

        outStr(",\"packets\":");
        outNum(m.packets);

        outStr(",\"peak\":");
        outNum(m.peakRSSI);

        outStr(",\"avg\":");

        if (m.rssiCount > 0)
            outFixed((float)m.rssiSum / m.rssiCount, 1);
        else
            outStr("-127");

        outStr("}");
    }

    outStr("]}");

    outEnd();
}

// ============================================================
// API: history
// ============================================================

void sendHistoryJSON()
{
    uint32_t requestedMs = 120000;

    if (reqHasArg("range"))
        requestedMs = reqArg("range").toInt();

    if (requestedMs < 10000)
        requestedMs = 10000;

    uint32_t now = millis();

    uint32_t startTime = (now > requestedMs) ? (now - requestedMs) : 0;

    uint32_t start = historyStartIndex();

    uint32_t first = historyCount;

    for (uint32_t i = 0; i < historyCount; i++)
    {
        uint32_t idx = (start + i) % historyCapacity;

        if (history[idx].timestamp >= startTime)
        {
            first = i;
            break;
        }
    }

    uint32_t available = (first < historyCount) ? (historyCount - first) : 0;

    uint32_t step = 1;

    if (available > MAX_HISTORY_POINTS)
    {
        step = (available + MAX_HISTORY_POINTS - 1) / MAX_HISTORY_POINTS;
    }

    outBegin();

    outStr("{\"points\":[");

    bool firstOutput = true;

    for (uint32_t i = first; i < historyCount; i += step)
    {
        uint32_t idx = (start + i) % historyCapacity;

        if (!firstOutput)
            outStr(",");

        firstOutput = false;

        outStr("{\"t\":");
        outNum(history[idx].timestamp);

        outStr(",\"v\":[");

        bool firstValue = true;

        for (uint16_t c = 0; c < TOTAL_CHANNELS; c++)
        {
            if (!channelSelected[c])
                continue;

            if (!firstValue)
                outStr(",");

            firstValue = false;

            outNum(history[idx].rssi[c]);
        }

        outStr("]}");
    }

    outStr("]}");

    outEnd();
}

// ============================================================
// API: networks
// ============================================================

void sendNetworksJSON()
{
    cleanupNetworks();
    cleanupClients();

    refreshPcapCounts();

    uint32_t now = millis();

    outBegin();

    outStr("{\"networks\":[");

    bool first = true;

    for (int i = 0; i < MAX_NETWORKS; i++)
    {
        if (!networks[i].valid)
            continue;

        if (!first)
            outStr(",");

        first = false;

        WiFiNetwork &n = networks[i];

        outStr("{\"ssid\":");
        outJSON(n.ssid);

        outStr(",\"bssid\":");
        outMac(n.bssid);

        outStr(",\"vendor\":");
        outJSON(lookupBssidVendor(n.bssid, n.mlo));

        outStr(",\"resolved\":");
        outNum(n.ssidResolved ? 1 : 0);

        outStr(",\"pcapf\":");
        outNum(n.pcapFrames);

        outStr(",\"pcapb\":");
        outNum(n.pcapBeacon ? 1 : 0);

        outStr(",\"vht24\":");
        outNum(n.vht24 ? 1 : 0);

        outStr(",\"mesh\":");
        outNum(n.mesh ? 1 : 0);

        outStr(",\"meshpeers\":");
        outNum(n.meshPeerings);

        outStr(",\"meshgate\":");
        outNum(n.meshGate ? 1 : 0);

        outStr(",\"meshaccept\":");
        outNum(n.meshAccepting ? 1 : 0);

        outStr(",\"meshfwd\":");
        outNum(n.meshForwarding ? 1 : 0);

        outStr(",\"meshpath\":");
        outNum(n.meshPathProto);

        outStr(",\"meshmetric\":");
        outNum(n.meshMetric);

        outStr(",\"meshauth\":");
        outNum(n.meshAuth);

        outStr(",\"channel\":");
        outNum(n.channel);

        outStr(",\"bandwidth\":");
        outNum(n.bandwidth);

        outStr(",\"center\":");
        outNum(n.centerChannel);

        outStr(",\"rssi\":");
        outNum(n.rssi);

        outStr(",\"sec\":");
        outJSON(n.security);

        outStr(",\"pair\":");
        outJSON(n.pairwise);

        outStr(",\"group\":");
        outJSON(n.groupCipher);

        outStr(",\"mgmt\":");
        outJSON(n.mgmtCipher);

        outStr(",\"akm\":");
        outJSON(n.akm);

        outStr(",\"std\":");
        outNum(n.standard);

        outStr(",\"modes\":");
        outNum(n.phyModes);

        outStr(",\"gf\":");
        outNum(n.htGreenfield ? 1 : 0);

        outStr(",\"intol\":");
        outNum(n.fortyIntolerant ? 1 : 0);

        outStr(",\"k\":");
        outNum(n.rm11k ? 1 : 0);

        outStr(",\"v\":");
        outNum(n.bss11v ? 1 : 0);

        outStr(",\"rrm\":");
        outNum64(n.rrmCaps);

        outStr(",\"nss\":");
        outNum(n.streams);

        outMlo(n.mlo);

        outUnknownElements(n.unknownIes, n.unknownSeen, n.unknownCount);

        // "iw": "1A0A01": the Interworking element's body in hex
        if (n.iwLen)
        {
            char hex[sizeof(n.iw) * 2 + 1];

            for (uint8_t i = 0; i < n.iwLen; i++)
                snprintf(hex + i * 2, 3, "%02X", n.iw[i]);

            outStr(",\"iw\":");
            outJSON(hex);
        }

        // The smaller elements, only when seen, see parseApExtras()
        if (n.quietSeen)
        {
            outStr(",\"quiet\":[");
            outNum(n.quietPeriod);
            outStr(",");
            outNum(n.quietDuration);
            outStr(",");
            outNum(n.quietOffset);
            outStr("]");
        }

        if (n.mptInterval >= 0)
        {
            outStr(",\"mpt\":");
            outNum(n.mptInterval);
        }

        if (n.ibss)
            outStr(",\"ibss\":1");

        if (n.atimWindow >= 0)
        {
            outStr(",\"atim\":");
            outNum(n.atimWindow);
        }

        if (n.aacLen)
        {
            char hex[sizeof(n.aac) * 2 + 1];

            for (uint8_t i = 0; i < n.aacLen; i++)
                snprintf(hex + i * 2, 3, "%02X", n.aac[i]);

            outStr(",\"aac\":");
            outJSON(hex);
        }

        if (n.beaconProt)
            outStr(",\"bprot\":1");

        if (n.advProtoCount)
        {
            outStr(",\"adv\":[");

            for (uint8_t i = 0; i < n.advProtoCount; i++)
            {
                if (i)
                    outStr(",");

                outNum(n.advProto[i]);
            }

            outStr("]");
        }

        if (n.apName[0])
        {
            outStr(",\"apname\":");
            outJSON(n.apName);
        }

        if (n.vendorFirmware[0])
        {
            outStr(",\"vfw\":");
            outJSON(n.vendorFirmware);
        }

        if (n.acmMask >= 0)
        {
            outStr(",\"acm\":");
            outNum(n.acmMask);
        }

        // Wi-Fi Alliance elements and HP's, see parseWfaElements()
        if (n.ovrSeen)
        {
            outStr(",\"ovr\":{\"akm\":");
            outJSON(n.ovrAkm);
            outStr(",\"pair\":");
            outJSON(n.ovrPairwise);
            outStr(",\"pmf\":");
            outNum(n.ovrPmf);
            outStr(",\"h2e\":");
            outNum(n.ovrH2E ? 1 : 0);
            outStr("}");
        }

        if (n.wfaCapaSeen)
        {
            outStr(",\"wfacapa\":");
            outNum(n.wfaCapa);
        }

        // [colour, disabled]
        if (n.bssColor >= 0)
        {
            outStr(",\"color\":[");
            outNum(n.bssColor);
            outStr(",");
            outNum(n.bssColorOff ? 1 : 0);
            outStr("]");
        }

        // [new colour, seconds since announced, changes seen]
        if (n.colorNext >= 0)
        {
            outStr(",\"bcc\":[");
            outNum(n.colorNext);
            outStr(",");
            outNum((now - n.colorChangeAt) / 1000);
            outStr(",");
            outNum(n.colorChanges);
            outStr("]");
        }

        // Seconds the TSF has run by now; resets, seconds since one
        if (n.tsfAt != 0)
        {
            outStr(",\"tsfup\":");
            outNum((long)(n.tsf / 1000000ULL + (now - n.tsfAt) / 1000));

            if (n.tsfResets)
            {
                outStr(",\"tsfreset\":[");
                outNum(n.tsfResets);
                outStr(",");
                outNum((now - n.tsfResetAt) / 1000);
                outStr("]");
            }
        }

        // Its configuration changes kept out of the bell
        if (cfgIsMuted(n.bssid))
            outStr(",\"cfgmute\":1");

        // [frames of clock 1, of clock 2, switches, µs apart]
        if (n.clkHits[1])
        {
            outStr(",\"clocks\":[");
            outNum(n.clkHits[0]);
            outStr(",");
            outNum(n.clkHits[1]);
            outStr(",");
            outNum(n.clkSwitches);
            outStr(",");
            outNum64((uint64_t)llabs(n.clkOff[0] - n.clkOff[1]));
            outStr("]");
        }

        // [DTIM period, group traffic buffered, stations waiting]
        if (n.dtimPeriod)
        {
            outStr(",\"dtim\":[");
            outNum(n.dtimPeriod);
            outStr(",");
            outNum(n.timMcast ? 1 : 0);
            outStr(",");
            outNum(n.timWaiting);
            outStr("]");
        }

        // Probe responses: [with timestamp 0, with one]
        if (n.probeTsfZero)
        {
            outStr(",\"ptsf\":[");
            outNum(n.probeTsfZero);
            outStr(",");
            outNum(n.probeTsfSet);
            outStr("]");
        }

        // [gaps measured, of them under half an interval, shortest µs]
        if (n.gapCount)
        {
            outStr(",\"bgap\":[");
            outNum(n.gapCount);
            outStr(",");
            outNum(n.gapShort);
            outStr(",");
            outNum(n.gapMinUs);
            outStr("]");
        }

        // Configuration changes, newest first:
        // [[seconds ago, what, from, to], ...] and how many in all
        if (n.cfgTotal)
        {
            outStr(",\"cfgn\":");
            outNum(n.cfgTotal);
            outStr(",\"cfgch\":[");

            int kept = n.cfgTotal < CFG_CHANGES ? n.cfgTotal : CFG_CHANGES;

            for (int i = 0; i < kept; i++)
            {
                const CfgChange &c = n.cfgChanges[
                    (n.cfgHead + CFG_CHANGES - 1 - i) % CFG_CHANGES];

                outStr(i ? ",[" : "[");
                outNum((now - c.at) / 1000);
                outStr(",");
                outJSON(cfgFieldNames[c.field]);
                outStr(",");
                outJSON(c.from);
                outStr(",");
                outJSON(c.to);
                outStr("]");
            }

            outStr("]");
        }

        if (n.maxIdle)
        {
            outStr(",\"maxidle\":[");
            outNum(n.maxIdle);
            outStr(",");
            outNum(n.maxIdleOpt);
            outStr("]");
        }

        /*
         * Drone Remote ID, raw where the page converts: altitudes
         * and height as sent (value / 2 - 1000 m, 0 unknown),
         * positions in 1e-7 degrees, speeds in cm/s
         */
        if (n.drone.seen)
        {
            const DroneInfo &d = n.drone;

            outStr(",\"rid\":{\"ua\":");
            outNum(d.uaType);
            outStr(",\"mk\":");
            outJSON(d.maker);
            outStr(",\"ago\":");
            outNum((now - d.at) / 1000);

            if (d.haveLocation)
            {
                outStr(",\"st\":");
                outNum(d.status);
                outStr(",\"lat\":");
                outNum(d.lat);
                outStr(",\"lon\":");
                outNum(d.lon);
                outStr(",\"ab\":");
                outNum(d.altBaro);
                outStr(",\"ag\":");
                outNum(d.altGeo);
                outStr(",\"h\":");
                outNum(d.height);
                outStr(",\"hagl\":");
                outNum(d.heightAgl ? 1 : 0);
                outStr(",\"spd\":");
                outNum(d.speed);
                outStr(",\"vs\":");
                outNum(d.vspeed);
                outStr(",\"trk\":");
                outNum(d.track);
            }

            if (d.haveOperator)
            {
                outStr(",\"olat\":");
                outNum(d.opLat);
                outStr(",\"olon\":");
                outNum(d.opLon);
                outStr(",\"oalt\":");
                outNum(d.opAlt);
                outStr(",\"eu\":");
                outNum(d.euClass);
            }

            outStr(",\"sid\":");
            outJSON(d.selfId);
            outStr("}");
        }

        if (n.p2pSeen)
        {
            outStr(",\"p2p\":[");
            outNum(n.p2pDevCap);
            outStr(",");
            outNum(n.p2pGroupCap);
            outStr(",");
            outJSON(n.p2pName);
            outStr("]");
        }

        if (n.nanPref >= 0 || n.nanAmPref >= 0)
        {
            outStr(",\"nan\":[");
            outNum(n.nanPref);
            outStr(",");
            outNum(n.nanRandom);
            outStr(",");
            outNum(n.nanAmPref);
            outStr(",");
            outNum(n.nanAmRandom);
            outStr(",");
            outNum(n.nanHops);
            outStr("]");
        }

        if (n.wfdInfo >= 0)
        {
            outStr(",\"wfd\":");
            outNum(n.wfdInfo);
        }

        if (n.mboSeen)
        {
            outStr(",\"mbo\":{\"cap\":");
            outNum(n.mboCap);
            outStr(",\"dis\":");
            outNum(n.mboDisallowed);
            outStr(",\"oce\":");
            outNum(n.oceCtrl);
            outStr(",\"npc\":[");

            for (uint8_t i = 0; i < n.mboNpcCount; i++)
            {
                if (i)
                    outStr(",");

                outNum(n.mboNpc[i]);
            }

            outStr("]}");
        }

        if (n.omn >= 0)
        {
            outStr(",\"omn\":");
            outNum(n.omn);
        }

        if (n.wpsName[0])
        {
            outStr(",\"wpsname\":");
            outJSON(n.wpsName);
        }

        // Manufacturer, model name, model number
        if (n.wpsMaker[0] || n.wpsModel[0] || n.wpsModelNo[0])
        {
            outStr(",\"wpsdev\":[");
            outJSON(n.wpsMaker);
            outStr(",");
            outJSON(n.wpsModel);
            outStr(",");
            outJSON(n.wpsModelNo);
            outStr("]");
        }

        if (n.hpModel[0])
        {
            outStr(",\"hp\":[");
            outJSON(n.hpModel);
            outStr(",");
            outJSON(n.hpNumber);
            outStr("]");
        }

        // "acc": ["My SEAT 4624","SEAT","SE3821"], Apple Device IE
        if (n.acc)
        {
            outStr(",\"acc\":[");
            outJSON(n.accName);
            outStr(",");
            outJSON(n.accMaker);
            outStr(",");
            outJSON(n.accModel);
            outStr("]");
        }

        outVendorOuis(n.vendorOui, n.vendorOuiCount);

        if (n.tpcPower != 127)
        {
            outStr(",\"tpc\":");
            outNum(n.tpcPower);
        }

        if (n.powerConstraint != 255)
        {
            outStr(",\"pwrc\":");
            outNum(n.powerConstraint);
        }

        if (n.erp != 255)
        {
            outStr(",\"erp\":");
            outNum(n.erp);
        }

        if (n.xloadPresent)
        {
            outStr(",\"xload\":[");
            outNum(n.xloadMu);

            for (uint8_t k = 0; k < 4; k++)
            {
                outStr(",");
                outNum(n.xload[k]);
            }

            outStr("]");
        }

        /*
         * The AP's clock as it would read now, and how far it is
         * from the sensor's in seconds once the sensor has one.
         */
        if (n.apClockOffsetUs != 0)
        {
            int64_t apNow = (esp_timer_get_time() +
                             n.apClockOffsetUs) / 1000000LL;

            char stamp[24];

            formatStamp((uint32_t)apNow, stamp, sizeof(stamp));

            outStr(",\"apclk\":");
            outJSON(stamp);

            if (pcapClockSource != 0)
            {
                outStr(",\"apdelta\":");
                outNum((long)((n.apClockOffsetUs - clockOffsetUs) /
                              1000000LL));
            }
        }

        if (n.apTz[0])
        {
            outStr(",\"aptz\":");
            outJSON(n.apTz);
        }

        // Transmit Power Envelope, raw octets per element
        outStr(",\"tpe\":[");

        for (uint8_t k = 0; k < 2 && n.tpeLen[k]; k++)
        {
            if (k)
                outStr(",");

            outStr("[");

            for (uint8_t b = 0; b < n.tpeLen[k]; b++)
            {
                if (b)
                    outStr(",");

                outNum(n.tpe[k][b]);
            }

            outStr("]");
        }

        outStr("]");

        outStr(",\"wnm\":");
        outNum(n.wnmCaps);

        outStr(",\"r\":");
        outNum(n.ft11r ? 1 : 0);

        // Fine Timing Measurement: [requests to it, FTM frames from
        // it, [stations ranging]], what it granted, position, address
        if (n.ftmRequests || n.ftmFrames)
        {
            outStr(",\"ftm\":[");
            outNum(n.ftmRequests);
            outStr(",");
            outNum(n.ftmFrames);
            outStr(",[");

            for (uint8_t i = 0; i < n.ftmPeerCount; i++)
            {
                if (i)
                    outStr(",");

                outMac(n.ftmPeers[i]);
            }

            outStr("]]");

            if (n.ftmParamSeen)
                outHexField("ftmpar", n.ftmParam, 9);

            if (n.ftmLciSeen)
                outHexField("ftmlci", n.ftmLci, 16);

            if (n.ftmCivicLen)
                outHexField("ftmciv", n.ftmCivic, n.ftmCivicLen);
        }

        // [Mobility Domain ID, FT Capability and Policy]
        if (n.ft11r)
        {
            outStr(",\"mdid\":[");
            outNum(n.mdid);
            outStr(",");
            outNum(n.ftPolicy);
            outStr("]");
        }

        outStr(",\"mbssid\":");
        outNum(n.multiBssid ? 1 : 0);

        outStr(",\"csach\":");
        outNum(n.csaChannel);

        outStr(",\"csacnt\":");
        outNum(n.csaCount);

        outStr(",\"basic\":");
        outNum(n.basicRate);

        outStr(",\"basicrates\":[");

        for (int r = 0; r < n.basicRateCount; r++)
        {
            if (r)
                outStr(",");

            outNum(n.basicRates[r]);
        }

        outStr("]");

        outStr(",\"wpsm\":");
        outNum(n.wpsMethods);

        outStr(",\"wpslock\":");
        outNum(n.wpsLocked ? 1 : 0);

        outStr(",\"wpsstate\":");
        outNum(n.wpsState);

        outStr(",\"preauth\":");
        outNum(n.preAuth ? 1 : 0);

        if (n.badSuiteIn)
        {
            outStr(",\"suitebad\":[");
            outNum(n.badSuiteIn);
            outStr(",");
            outNum64(n.badSuite);
            outStr("]");
        }

        if (n.wpaNoAkm)
            outStr(",\"wpanoakm\":1");

        outStr(",\"hs20\":");
        outNum(n.hs20 ? 1 : 0);

        outStr(",\"owename\":");
        outNum(n.ssidFromOwe ? 1 : 0);

        outStr(",\"owetrans\":");
        outNum(n.oweTrans ? 1 : 0);

        outStr(",\"owebssid\":");
        outMac(n.oweBssid);

        outStr(",\"owessid\":");
        outJSON(n.oweSSID);

        outStr(",\"mfpc\":");
        outNum(n.mfpCapable ? 1 : 0);

        outStr(",\"mfpr\":");
        outNum(n.mfpRequired ? 1 : 0);

        outStr(",\"wps\":");
        outNum(n.wps ? 1 : 0);

        outStr(",\"sta\":");
        outNum(n.stations);

        outStr(",\"util\":");
        outNum(n.channelUtil);

        outStr(",\"clients\":");
        outNum(countClients(n.bssid));

        outStr(",\"bi\":");
        outNum(n.beaconInterval);

        outStr(",\"cc\":");
        outJSON(n.country);

        outStr(",\"beacons\":");
        outNum(n.beacons);

        outStr(",\"age\":");
        outNum((now - n.lastSeen) / 1000);

        outStr(",\"seen\":");
        outNum((now - n.firstSeen) / 1000);

        outStr("}");
    }

    outStr("]");

    // ---- 6 GHz networks heard about, never heard directly ----

    outStr(",\"sixghz\":[");

    if (sixGhz != nullptr)
    {
        bool firstSix = true;

        for (int i = 0; i < MAX_SIXG_BSS; i++)
        {
            if (!sixGhz[i].valid)
                continue;

            // Drop entries whose reporting AP has gone quiet, after
            // the same minute the network table gives an AP
            if (now - sixGhz[i].lastSeen > NETWORK_TIMEOUT_MS)
            {
                sixGhz[i].valid = false;
                continue;
            }

            if (!firstSix)
                outStr(",");

            firstSix = false;

            SixGhzBss &e = sixGhz[i];

            outStr("{\"bssid\":");
            outMac(e.bssid);

            outStr(",\"ssid\":");
            outJSON(e.ssid);

            outStr(",\"ch\":");
            outNum(e.channel);

            outStr(",\"freq\":");
            outNum(sixGhzFreq(e.channel));

            outStr(",\"width\":");
            outNum(sixGhzWidth(e.opClass));

            outStr(",\"opclass\":");
            outNum(e.opClass);

            outStr(",\"same\":");
            outNum(e.sameSsid ? 1 : 0);

            outStr(",\"colocated\":");
            outNum(e.coLocated ? 1 : 0);

            outStr(",\"unsolicited\":");
            outNum(e.unsolicited ? 1 : 0);

            outStr(",\"psd\":");
            outNum(e.psd);

            outStr(",\"shortssid\":");
            outNum((long)e.shortSsid);

            outStr(",\"reporter\":");
            outMac(e.reporter);

            outStr(",\"repssid\":");
            outJSON(e.reporterSsid);

            outStr(",\"repch\":");
            outNum(e.reporterChannel);

            outStr(",\"reports\":");
            outNum(e.reports);

            outStr(",\"age\":");
            outNum((now - e.lastSeen) / 1000);

            outStr("}");
        }
    }

    outStr("]}");

    outEnd();
}

// ============================================================
// API: stations
// ============================================================

/*
 * One station as JSON.
 *
 * Shared by /api/clients and /api/assochistory so the two
 * tables in the browser are rendered from the same fields and
 * cannot drift apart.
 *
 * The object is left open so a caller can append fields of
 * its own; closing it is the caller's job.
 */
/*
 * "voui": [["8CFDF0",1,"Qualcomm"], ...]: OUI, type and owner of
 * the vendor elements of an AP or a station, see
 * collectVendorOuis(). The page guesses maker and chip from them.
 */
void outVendorOuis(const uint8_t (*list)[4], uint8_t count)
{
    outStr(",\"voui\":[");

    for (uint8_t i = 0; i < count; i++)
    {
        char hex[8];

        snprintf(hex, sizeof(hex), "%02X%02X%02X", list[i][0],
                 list[i][1], list[i][2]);

        outStr(i ? ",[" : "[");
        outJSON(hex);
        outStr(",");
        outNum(list[i][3]);
        outStr(",");
        outJSON((list[i][0] & 0x02) ? "" : lookupVendor(list[i]));
        outStr("]");
    }

    outStr("]");
}

void outClientObject(ClientEntry &c, uint32_t now)
{
        outStr("{\"mac\":");
        outMac(c.mac);

        outStr(",\"vendor\":");
        outJSON(lookupVendor(c.mac));

        outStr(",\"bssid\":");
        outMac(c.bssid);

        outStr(",\"assoc\":");
        outNum(c.associated ? 1 : 0);

        // SSID of the BSS, even when no beacon was ever heard
        outStr(",\"apssid\":");
        outJSON(lookupBssName(c.bssid));

        outStr(",\"pcapf\":");
        outNum(c.pcapFrames);

        outStr(",\"prev\":");
        outMac(c.prevBssid);

        outStr(",\"roams\":");
        outNum(c.roams);

        outStr(",\"roamsok\":");
        outNum(c.roamsConfirmed);

        outStr(",\"roamlog\":[");

        for (int k = 0; k < c.roamLogCount; k++)
        {
            // Oldest first
            int idx =
                (c.roamLogWrite + MAX_ROAM_EVENTS - c.roamLogCount + k) %
                MAX_ROAM_EVENTS;

            if (k)
                outStr(",");

            outStr("{\"ago\":");
            outNum((now - c.roamLog[idx].at) / 1000);

            outStr(",\"bssid\":");
            outMac(c.roamLog[idx].bssid);

            outStr(",\"ok\":");
            outNum(c.roamLog[idx].confirmed ? 1 : 0);

            if (c.roamLog[idx].ft)
                outStr(",\"ft\":1");

            outStr("}");
        }

        outStr("]");

        outStr(",\"peer\":");
        outMac(c.peer);

        outStr(",\"rssi\":");
        outNum(c.rssi);

        outStr(",\"ch\":");
        outNum(c.channel);

        outStr(",\"up\":");
        outNum(c.toAp);

        outStr(",\"down\":");
        outNum(c.fromAp);

        outStr(",\"upb\":");
        outNum(c.toApBytes);

        outStr(",\"downb\":");
        outNum(c.fromApBytes);

        outStr(",\"status\":");
        outNum(c.assocStatusAt ? (int)c.assocStatus : -1);

        outStr(",\"statusago\":");
        outNum(c.assocStatusAt ? (now - c.assocStatusAt) / 1000 : 0);

        outStr(",\"statusbss\":");
        outMac(c.assocStatusBssid);

        outStr(",\"assocfail\":");
        outNum(c.assocFailures);

        if (c.assocComeback)
        {
            outStr(",\"comeback\":");
            outNum(c.assocComeback);
        }

        outStr(",\"probes\":");
        outNum(c.probes);

        outStr(",\"assocs\":");
        outNum(c.assocFrames);

        outStr(",\"sawassoc\":");
        outNum(c.sawAssoc ? 1 : 0);

        outStr(",\"joined\":");
        outJSON(c.assocSSID);

        outStr(",\"std\":");
        outNum(c.standard);

        outStr(",\"width\":");
        outNum(c.maxWidth);

        outStr(",\"nss\":");
        outNum(c.streams);

        outStr(",\"k\":");
        outNum(c.rm11k ? 1 : 0);

        outStr(",\"v\":");
        outNum(c.bss11v ? 1 : 0);

        outStr(",\"rrm\":");
        outNum64(c.rrmCaps);

        outStr(",\"wnm\":");
        outNum(c.wnmCaps);

        outStr(",\"opcl\":");
        outOpClasses(c.opClasses);

        outStr(",\"chans\":");
        outNum64(c.suppChans);

        outStr(",\"chans6\":");
        outSixGhzChannels(c.suppChans6);

        outUnknownElements(c.unknownIes, c.unknownSeen, c.unknownCount);

        outVendorOuis(c.vendorOui, c.vendorOuiCount);

        outMlo(c.mlo);

        outStr(",\"r\":");
        outNum(c.ft11r ? 1 : 0);

        if (c.ft11r)
        {
            outStr(",\"mdid\":");
            outNum(c.mdid);
        }

        // Fine Timing Measurement: [requests, FTM frames to it,
        // [responders]], asked for 1 position, 2 address, parameters
        if (c.ftmRequests)
        {
            outStr(",\"ftm\":[");
            outNum(c.ftmRequests);
            outStr(",");
            outNum(c.ftmAnswers);
            outStr(",[");

            for (uint8_t i = 0; i < c.ftmTargetCount; i++)
            {
                if (i)
                    outStr(",");

                outMac(c.ftmTargets[i]);
            }

            outStr("]],\"ftmask\":");
            outNum((c.ftmAskLci ? 1 : 0) | (c.ftmAskCivic ? 2 : 0));

            if (c.ftmParamSeen)
                outHexField("ftmpar", c.ftmParam, 9);
        }

        outStr(",\"band6\":");
        outNum(c.band6 ? 1 : 0);

        outStr(",\"wmm\":");
        outNum(c.wmm ? 1 : 0);

        outStr(",\"pmf\":");
        outNum(c.pmf ? 1 : 0);

        outStr(",\"akm\":");
        outJSON(c.akm);

        outStr(",\"cipher\":");
        outJSON(c.cipher);

        if (c.wfaCapaSeen)
        {
            outStr(",\"wfacapa\":");
            outNum(c.wfaCapa);
        }

        if (c.rsnSelSeen)
        {
            outStr(",\"rsnsel\":");
            outNum(c.rsnSel);
        }

        // BSS Max Idle Period: [asked, options, granted, options]
        if (c.maxIdleAsked || c.maxIdleGranted)
        {
            outStr(",\"maxidle\":[");
            outNum(c.maxIdleAsked);
            outStr(",");
            outNum(c.maxIdleAskedOpt);
            outStr(",");
            outNum(c.maxIdleGranted);
            outStr(",");
            outNum(c.maxIdleGrantedOpt);
            outStr("]");
        }

        outStr(",\"roamsft\":");
        outNum(c.roamsFt);

        outStr(",\"ssids\":[");

        bool firstSSID = true;

        for (int k = 0; k < MAX_PROBE_SSIDS; k++)
        {
            if (c.probeSSID[k][0] == 0)
                continue;

            if (!firstSSID)
                outStr(",");

            firstSSID = false;

            outJSON(c.probeSSID[k]);
        }

        outStr("]");

        outStr(",\"age\":");
        outNum((now - c.lastSeen) / 1000);

        outStr(",\"seen\":");
        outNum((now - c.firstSeen) / 1000);
}


void sendClientsJSON()
{
    cleanupClients();

    refreshPcapCounts();

    uint32_t now = millis();

    outBegin();

    outStr("{\"clients\":[");

    bool first = true;

    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        if (!clients[i].valid)
            continue;

        if (!first)
            outStr(",");

        first = false;

        outClientObject(clients[i], now);

        outStr("}");
    }

    // AWDL senders, see parseAwdl()
    cleanupAwdl();

    outStr("],\"awdl\":[");

    first = true;

    for (int i = 0; i < MAX_AWDL; i++)
    {
        const AwdlEntry &d = awdlDevices[i];

        if (!d.valid)
            continue;

        if (!first)
            outStr(",");

        first = false;

        outStr("{\"mac\":");
        outMac(d.mac);
        outStr(",\"rssi\":");
        outNum(d.rssi);
        outStr(",\"ch\":");
        outNum(d.channel);
        outStr(",\"frames\":");
        outNum(d.frames);
        outStr(",\"cls\":");
        outNum(d.devClass);
        outStr(",\"ver\":");
        outNum(d.version);
        outStr(",\"cc\":");
        outJSON(d.country);

        if (d.infraSeen)
        {
            outStr(",\"bssid\":");
            outMac(d.infraBssid);
            outStr(",\"bch\":");
            outNum(d.infraChannel);
        }

        if (d.staSeen)
        {
            outStr(",\"sta\":");
            outMac(d.sta);
        }

        outStr(",\"age\":");
        outNum((now - d.lastSeen) / 1000);
        outStr(",\"seen\":");
        outNum((now - d.firstSeen) / 1000);
        outStr("}");
    }

    outStr("]}");

    outEnd();
}

// ============================================================
// API: association history
//
// Separate from /api/clients on purpose: this table does not
// age out, so it is polled rarely and must not slow the live
// view down.
// ============================================================

void sendAssocHistoryJSON()
{
    refreshPcapCounts();

    uint32_t now = millis();

    outBegin();

    outStr("{\"dropped\":");
    outNum(assocHistoryDropped);

    outStr(",\"slots\":");
    outNum(MAX_ASSOC_HISTORY);

    outStr(",\"clients\":[");

    bool first = true;

    for (int i = 0; assocHistory != nullptr &&
                    i < MAX_ASSOC_HISTORY; i++)
    {
        if (!assocHistory[i].valid)
            continue;

        if (!first)
            outStr(",");

        first = false;

        /*
         * The snapshot is frozen, so its ages would count from
         * whenever it was taken. Only the two absolute stamps
         * below are meaningful here; the browser prefers them
         * and falls back to the ages for the live table.
         */
        outClientObject(assocHistory[i].sta, now);

        outStr(",\"firstepoch\":");
        outNum(assocHistory[i].firstEpoch);

        outStr(",\"lastepoch\":");
        outNum(assocHistory[i].lastEpoch);

        outStr("}");
    }

    outStr("]}");

    outEnd();
}

// ============================================================
// API: BLE
// ============================================================

void sendBleJSON()
{
    cleanupBleDevices();

    uint32_t now = millis();

    outBegin();

    outStr("{\"mode\":");
    outNum(scanMode);

    outStr(",\"active\":");
    outNum(bleActiveScan ? 1 : 0);

    outStr(",\"frames\":");
    outNum(bleFrames);

    outStr(",\"drops\":");
    outNum(bleDrops);

    outStr(",\"devices\":[");

    bool first = true;

    for (int i = 0; i < MAX_BLE_DEVICES; i++)
    {
        if (!bleDevices[i].valid)
            continue;

        if (!first)
            outStr(",");

        first = false;

        BleDevice &d = bleDevices[i];

        outStr("{\"addr\":");
        outMac(d.addr);

        outStr(",\"type\":");
        outJSON(bleAddrType(d.addr));

        outStr(",\"name\":");
        outJSON(d.name);

        outStr(",\"svc\":");
        outJSON(d.svc);

        outStr(",\"vendor\":");
        outJSON(lookupVendor(d.addr));

        outStr(",\"company\":");
        outNum(d.company);

        outStr(",\"companyName\":");
        outJSON(d.company == 0xFFFF ? "" : lookupCompany(d.company));

        outStr(",\"rssi\":");
        outNum(d.rssi);

        outStr(",\"peak\":");
        outNum(d.rssiPeak);

        outStr(",\"tx\":");
        outNum(d.txPower);

        outStr(",\"adv\":");
        outNum(d.adverts);

        uint32_t span = (now - d.firstSeen) / 1000;

        outStr(",\"rate\":");
        outFixed(span > 0 ? (float)d.adverts / span : 0.0f, 2);

        outStr(",\"age\":");
        outNum((now - d.lastSeen) / 1000);

        outStr(",\"seen\":");
        outNum(span);

        outStr("}");
    }

    outStr("]}");

    outEnd();
}

// ============================================================
// API: IEEE 802.15.4
// ============================================================

void sendZbJSON()
{
    cleanupZbNodes();

    uint32_t now = millis();

    outBegin();

    outStr("{\"available\":");
    outNum(HAVE_IEEE802154 ? 1 : 0);

    outStr(",\"mode\":");
    outNum(scanMode);

    outStr(",\"frames\":");
    outNum(zbFrames);

    outStr(",\"drops\":");
    outNum(zbDrops);

    outStr(",\"current\":");
    outNum(zbChannelNumber(zbChannelIndex));

    outStr(",\"locked\":");
    outNum(zbLockedChannel);

    outStr(",\"rxerrors\":");
    outNum(zbRxErrors);

    outStr(",\"edok\":");
    outNum(zbEdDone);

    outStr(",\"edfail\":");
    outNum(zbEdFailures);

    outStr(",\"rxarmed\":");
    outNum(zbRxArmedCount);

    outStr(",\"note\":");
    outJSON(zbNote);

    outStr(",\"scan\":");
    outNum(zbScanMode);

    outStr(",\"sweeps\":");
    outNum(edSweeps);

    outStr(",\"rate\":");
    outFixed(edRate, 1);

    outStr(",\"channels\":[");

    for (int i = 0; i < ZB_NUM_CHANNELS; i++)
    {
        if (i)
            outStr(",");

        ZbChannelStats &st = zbChannels[i];

        outStr("{\"ch\":");
        outNum(zbChannelNumber(i));

        outStr(",\"freq\":");
        outNum(zbChannelFreq(zbChannelNumber(i)));

        outStr(",\"frames\":");
        outNum(st.frames);

        outStr(",\"bytes\":");
        outNum(st.bytes);

        outStr(",\"max\":");
        outNum(st.maxRssi);

        outStr(",\"ed\":");
        outNum(st.edLevel);

        outStr(",\"lqi\":");
        outNum(st.lqi);

        outStr("}");
    }

    outStr("],\"nodes\":[");

    bool first = true;

    for (int i = 0; i < ZB_MAX_NODES; i++)
    {
        if (!zbNodes[i].valid)
            continue;

        if (!first)
            outStr(",");

        first = false;

        ZbNode &n = zbNodes[i];

        char buffer[24];

        snprintf(buffer, sizeof(buffer), "0x%04X", n.panId);

        outStr("{\"pan\":");
        outJSON(buffer);

        if (n.hasExt)
        {
            snprintf(buffer, sizeof(buffer),
                     "%02X%02X%02X%02X%02X%02X%02X%02X",
                     n.extAddr[0], n.extAddr[1],
                     n.extAddr[2], n.extAddr[3],
                     n.extAddr[4], n.extAddr[5],
                     n.extAddr[6], n.extAddr[7]);
        }
        else
        {
            snprintf(buffer, sizeof(buffer), "0x%04X", n.shortAddr);
        }

        outStr(",\"addr\":");
        outJSON(buffer);

        outStr(",\"ext\":");
        outNum(n.hasExt ? 1 : 0);

        outStr(",\"ch\":");
        outNum(n.channel);

        outStr(",\"rssi\":");
        outNum(n.rssi);

        outStr(",\"lqi\":");
        outNum(n.lqi);

        outStr(",\"type\":");
        outNum(n.lastType);

        outStr(",\"frames\":");
        outNum(n.frames);

        outStr(",\"age\":");
        outNum((now - n.lastSeen) / 1000);

        outStr(",\"seen\":");
        outNum((now - n.firstSeen) / 1000);

        outStr("}");
    }

    outStr("]}");

    outEnd();
}

// ============================================================
// API: energy waterfall
// ============================================================

void sendZbWaterfallJSON()
{
    uint32_t requestedMs = 60000;

    if (reqHasArg("range"))
        requestedMs = reqArg("range").toInt();

    if (requestedMs < 5000)
        requestedMs = 5000;

    uint32_t now = millis();

    uint32_t startTime = (now > requestedMs) ? (now - requestedMs) : 0;

    outBegin();

    outStr("{\"rate\":");
    outFixed(edRate, 1);

    outStr(",\"rows\":[");

    if (edHistory != nullptr && edCount > 0)
    {
        uint32_t start = edStartIndex();

        uint32_t first = edCount;

        for (uint32_t i = 0; i < edCount; i++)
        {
            uint32_t idx = (start + i) % edCapacity;

            if (edHistory[idx].timestamp >= startTime)
            {
                first = i;
                break;
            }
        }

        uint32_t available = (first < edCount) ? (edCount - first) : 0;

        uint32_t step = 1;

        if (available > ED_MAX_POINTS)
            step = (available + ED_MAX_POINTS - 1) / ED_MAX_POINTS;

        bool firstRow = true;

        for (uint32_t i = first; i < edCount; i += step)
        {
            uint32_t idx = (start + i) % edCapacity;

            if (!firstRow)
                outStr(",");

            firstRow = false;

            outStr("{\"t\":");
            outNum(edHistory[idx].timestamp);

            outStr(",\"v\":[");

            for (int c = 0; c < ZB_NUM_CHANNELS; c++)
            {
                if (c)
                    outStr(",");

                outNum(edHistory[idx].level[c]);
            }

            outStr("]}");
        }
    }

    outStr("]}");

    outEnd();
}

// ============================================================
// API: status
//
// Deliberately tiny and answered in every mode. The browser
// polls this one to learn which mode the device is in, so a
// heavy endpoint stalling can never leave the page guessing.
// ============================================================

void sendStatusJSON()
{
    outBegin();

    outStr("{\"version\":");
    outJSON(FW_VERSION);

    outStr(",\"mode\":");
    outNum(scanMode);

    // The newest configuration change, see /api/changes
    outStr(",\"cfgseq\":");
    outNum(cfgSeq);
    outStr(",\"cfgver\":");
    outNum(cfgVersion);
    outStr(",\"boot\":");
    outNum(bootCount);

    outStr(",\"zbscan\":");
    outNum(zbScanMode);

    outStr(",\"heap\":");
    outNum(ESP.getFreeHeap());

    outStr(",\"oui\":");
    outNum(OUI_FULL_COUNT);

    outStr(",\"clients\":");
    outNum(countWebClients());

    outStr(",\"sensor\":");
    outJSON(sensorName);

    // What the scanner is doing right now, ready to display
    outStr(",\"scan\":");
    {
        char buffer[32];

        // Fixed width so the badge does not change length
        if (scanMode == MODE_BLE)
        {
            snprintf(buffer, sizeof(buffer), "BLE %-7s",
                     bleActiveScan ? "active" : "passive");
        }
        else if (scanMode == MODE_ZB)
        {
            snprintf(buffer, sizeof(buffer), "802.15.4 ch %3u",
                     (unsigned)zbChannelNumber(zbChannelIndex));
        }
        else
        {
            snprintf(buffer, sizeof(buffer), "ch %3u",
                     (unsigned)getChannelNumber(currentChannelIndex));
        }

        outJSON(buffer);
    }

    outStr(",\"ip\":");
    outJSON(ETH.localIP().toString().c_str());

    outStr(",\"clock\":");
    outNum(pcapClockSource);

    outStr(",\"utc\":");
    {
        char buffer[24];

        formatStamp(deviceEpoch(), buffer, sizeof(buffer));

        outJSON(buffer);
    }

    outStr(",\"tz\":");
    outNum(tzMinutes);

    outStr(",\"tzlabel\":");
    {
        char label[12];
        formatTzLabel(label, sizeof(label));
        outJSON(label);
    }

    outStr(",\"static\":");
    outNum(ethStaticActive ? 1 : 0);

    outStr(",\"link\":");
    outNum(ethLinkUp ? 1 : 0);

    outStr(",\"speed\":");
    outNum(ETH.linkSpeed());

    outStr(",\"duplex\":");
    outNum(ETH.fullDuplex() ? 1 : 0);

    outStr("}");

    outEnd();
}

// ============================================================
// API: pcap download
//
// Classic libpcap format with a radiotap header per frame, so
// Wireshark shows channel and signal strength alongside the
// dissected management frame. Link type 127 is
// LINKTYPE_IEEE802_11_RADIOTAP.
//
// The FCS was stripped when the frame was queued, so the
// radiotap flags must not claim one is present.
// ============================================================

// RADIOTAP_LEN is defined next to MGMT_PARSE_MAX

void pcapBuildRadiotap(uint8_t *rt, uint8_t channel, int8_t rssi)
{
    memset(rt, 0, RADIOTAP_LEN);

    rt[0] = 0;                       // version
    rt[1] = 0;                       // pad
    rt[2] = RADIOTAP_LEN;            // length, little endian
    rt[3] = 0;

    // Flags (bit 1), Channel (bit 3), dBm antenna signal (bit 5)
    uint32_t present = (1u << 1) | (1u << 3) | (1u << 5);

    rt[4] = present & 0xFF;
    rt[5] = (present >> 8) & 0xFF;
    rt[6] = (present >> 16) & 0xFF;
    rt[7] = (present >> 24) & 0xFF;

    rt[8] = 0x00;                    // flags: no FCS at end

    // rt[9] is padding for the 2 byte alignment of Channel

    uint16_t freq = pcapChannelFreq(channel);

    // OFDM plus the band the channel sits in
    uint16_t chFlags = (channel > 14) ? 0x0140 : 0x00C0;

    rt[10] = freq & 0xFF;
    rt[11] = (freq >> 8) & 0xFF;
    rt[12] = chFlags & 0xFF;
    rt[13] = (chFlags >> 8) & 0xFF;

    rt[14] = (uint8_t)rssi;          // signed dBm
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
    if (pcapRing == nullptr || networks == nullptr ||
        clients == nullptr || assocRing == nullptr)
    {
        return;
    }

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

    uint32_t start =
        (pcapCount < PCAP_SLOTS) ? 0 : pcapWrite;

    uint32_t assocStart =
        (assocCount < ASSOC_SLOTS) ? 0 : assocWrite;

    // Both rings in one pass
    for (uint32_t k = 0; k < pcapCount + assocCount; k++)
    {
        const uint8_t *data;
        uint16_t       length;

        if (k < pcapCount)
        {
            PcapRecord &r = pcapRing[(start + k) % PCAP_SLOTS];

            data   = r.data;
            length = r.length;
        }
        else
        {
            AssocRecord &r =
                assocRing[(assocStart + k - pcapCount) % ASSOC_SLOTS];

            data   = r.data;
            length = r.length;
        }

        if (length < 16)
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

// Index values address the ring, then the beacon slots, then the
// association ring, see PCAP_IDX_*. The association range was
// missing here, so those indices ran past the end of the beacon
// slots and exported whatever memory followed as frames.
// The elaborated "struct" is needed again: the generated
// prototype sits above the struct definition
template <typename R>
struct PcapView pcapViewOf(const R &r)
{
    return { r.us, r.length, r.origLength, r.rssi, r.channel, r.data };
}

struct PcapView pcapRecordAt(uint16_t idx)
{
    if (idx < PCAP_IDX_BEACON_BASE)
        return pcapViewOf(pcapRing[idx]);

    if (idx < PCAP_IDX_ASSOC_BASE)
        return pcapViewOf(beaconSlots[idx - PCAP_IDX_BEACON_BASE].rec);

    return pcapViewOf(assocRing[idx - PCAP_IDX_ASSOC_BASE]);
}

void sendPcap()
{
    if (pcapRing == nullptr || beaconSlots == nullptr ||
        pcapIndex == nullptr || assocRing == nullptr ||
        sixGhz == nullptr)
    {
        reqSend(503, "text/plain", "capture buffers unavailable");
        return;
    }

    uint8_t beaconCount = countBeaconSlots();

    uint8_t ta[6];

    bool haveTa =
        reqHasArg("ta") && parseMacArg(reqArg("ta"), ta);

    /*
     * Which of the three buffers to export. The settings page
     * offers one download per buffer; without the argument
     * everything is merged, as before.
     */
    String src = reqHasArg("src") ? reqArg("src")
                                      : String("all");

    bool wantRing    = (src == "all" || src == "ring");
    bool wantAssoc   = (src == "all" || src == "assoc");
    bool wantBeacons = (src == "all" || src == "beacons");

    uint32_t selected =
        (wantRing ? pcapCount : 0) +
        (wantAssoc ? assocCount : 0) +
        (wantBeacons ? beaconCount : 0);

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

    uint32_t start =
        (pcapCount < PCAP_SLOTS) ? 0 : pcapWrite;

    // ---- collect matches ----

    uint32_t n = 0;

    for (uint32_t i = 0; wantRing && i < pcapCount; i++)
    {
        uint32_t idx = (start + i) % PCAP_SLOTS;

        if (haveTa)
        {
            PcapRecord &r = pcapRing[idx];

            // Addr2 starts at offset 10 of the MAC header
            if (r.length < 16 || memcmp(&r.data[10], ta, 6) != 0)
                continue;
        }

        pcapIndex[n++] = (uint16_t)idx;
    }

    uint32_t ringMatches = n;

    uint32_t assocStart =
        (assocCount < ASSOC_SLOTS) ? 0 : assocWrite;

    for (uint32_t i = 0; wantAssoc && i < assocCount; i++)
    {
        uint32_t idx = (assocStart + i) % ASSOC_SLOTS;

        if (haveTa)
        {
            AssocRecord &r = assocRing[idx];

            if (r.length < 16 || memcmp(&r.data[10], ta, 6) != 0)
                continue;
        }

        pcapIndex[n++] = (uint16_t)(PCAP_IDX_ASSOC_BASE + idx);
    }

    for (int i = 0; wantBeacons && i < MAX_BEACON_SLOTS; i++)
    {
        if (!beaconSlots[i].valid)
            continue;

        if (haveTa && memcmp(beaconSlots[i].bssid, ta, 6) != 0)
            continue;

        pcapIndex[n++] = (uint16_t)(PCAP_IDX_BEACON_BASE + i);
    }

    if (n == 0)
    {
        /*
         * The buffer holds frames, just none from this address
         * any more. Say how far back it reaches so it is clear
         * whether they aged out or were never captured.
         */
        String msg =
            "No frames from this address are in the capture.\n\n";

        uint32_t uptime =
            (uint32_t)(esp_timer_get_time() / 1000000ULL);

        uint32_t span = 0;

        if (pcapCount > 0)
        {
            span = (uint32_t)((esp_timer_get_time() -
                               pcapRing[start % PCAP_SLOTS].us) /
                              1000000ULL);
        }

        if (pcapCount < PCAP_SLOTS)
        {
            /*
             * Nothing was evicted, so the frames were never
             * recorded rather than aged out. Say why that can
             * happen, it is not obvious.
             */
            msg += "The ring is not full, it holds ";
            msg += pcapCount;
            msg += " of ";
            msg += PCAP_SLOTS;
            msg += " frames and reaches back ";
            msg += span;
            msg += " of the ";
            msg += uptime;
            msg += " seconds since start. Nothing has been "
                   "overwritten, so these frames were never "
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
            msg += "The ring is full, it holds ";
            msg += PCAP_SLOTS;
            msg += " frames reaching back ";
            msg += span;
            msg += " seconds, so older frames are being "
                   "overwritten and these may have aged out.\n";
        }

        msg += "\n";
        msg += beaconCount;
        msg += " beacons are held separately, one per AP, and ";
        msg += assocCount;
        msg += " of ";
        msg += ASSOC_SLOTS;
        msg += " association request slots are in use.\n";

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

    /*
     * The ring part is already in chronological order and the
     * beacon slots are few, so an insertion sort only has to
     * move those few into place.
     */
    if (ringMatches < n)
    {
        for (uint32_t i = 1; i < n; i++)
        {
            uint16_t  key = pcapIndex[i];
            uint64_t  us  = pcapRecordAt(key).us;

            int32_t j = (int32_t)i - 1;

            while (j >= 0 && pcapRecordAt(pcapIndex[j]).us > us)
            {
                pcapIndex[j + 1] = pcapIndex[j];
                j--;
            }

            pcapIndex[j + 1] = key;
        }
    }

    // Keep the newest entries when a limit was given
    uint32_t first = 0;

    if (limit > 0 && n > limit)
        first = n - limit;

    // ---- exact length first, that avoids chunked binary ----

    uint32_t total = 24;             // global header

    for (uint32_t i = first; i < n; i++)
        total += 16 + RADIOTAP_LEN + pcapRecordAt(pcapIndex[i]).length;

    String filename = "capture";

    if (haveTa)
    {
        char suffix[16];

        snprintf(suffix, sizeof(suffix), "-%02X%02X%02X%02X%02X%02X",
                 ta[0], ta[1], ta[2], ta[3], ta[4], ta[5]);

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

    for (uint32_t i = first; i < n; i++)
    {
        PcapView r = pcapRecordAt(pcapIndex[i]);

        int64_t  stamp = (int64_t)r.us + clockOffsetUs;

        uint32_t sec  = (uint32_t)(stamp / 1000000LL);
        uint32_t usec = (uint32_t)(stamp % 1000000LL);

        // Captured and on-air length differ for a cut-off frame,
        // which Wireshark then marks as truncated, not malformed
        uint32_t len  = RADIOTAP_LEN + r.length;
        uint32_t orig = RADIOTAP_LEN + max(r.length, r.origLength);

        memcpy(&rec[0], &sec, 4);
        memcpy(&rec[4], &usec, 4);
        memcpy(&rec[8], &len, 4);
        memcpy(&rec[12], &orig, 4);

        pcapBuildRadiotap(rt, r.channel, r.rssi);

        reqContent(rec, sizeof(rec));
        reqContent(rt, RADIOTAP_LEN);
        reqContent(r.data, r.length);
    }

    reqDone();

    lastRequestServed = millis();
}

void clearPcapRing()
{
    pcapWrite = 0;
    pcapCount = 0;
    pcapTotal = 0;
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
    assocWrite = 0;
    assocCount = 0;
    assocTotal = 0;
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
                   (unsigned)pcapCount);

        clearPcapRing();
    }

    if (all || what == "beacons")
    {
        logRequest("beacon slots cleared, %u beacons discarded",
                   (unsigned)countBeaconSlots());

        clearBeaconSlots();
    }

    if (all || what == "assoc")
    {
        logRequest("assoc ring cleared, %u requests discarded",
                   (unsigned)assocCount);

        clearAssocRing();
    }

    reqSend(200, "application/json", "{\"ok\":1}");
}

// ============================================================
// API: IE catalog
//
// /api/catalog lists the entries; ?enable=0|1 switches the
// recording, which is kept across reboots.
// ============================================================

void outByteList(const char *name, const uint8_t *list, size_t n)
{
    outStr(",\"");
    outStr(name);
    outStr("\":[");

    for (size_t i = 0; i < n; i++)
    {
        if (i)
            outStr(",");

        outNum(list[i]);
    }

    outStr("]");
}

void outKeyList(const char *name, const uint32_t *list, size_t n)
{
    outStr(",\"");
    outStr(name);
    outStr("\":[");

    for (size_t i = 0; i < n; i++)
    {
        if (i)
            outStr(",");

        outNum64(list[i]);
    }

    outStr("]");
}

/*
 * The AKM or cipher suites with a name, [key, name] each, under
 * the IEEE OUI and the WPA (version 1) one.
 */
void outSuiteList(const char *name, bool akm)
{
    outStr(",\"");
    outStr(name);
    outStr("\":[");

    bool first = true;

    for (uint32_t oui : { 0x000FACu, 0x0050F2u })
    {
        bool wpa1 = oui == 0x0050F2u;

        for (uint32_t t = 0; t < 256; t++)
        {
            uint32_t key = (oui << 8) | t;

            if (akm ? !akmKnown(key, wpa1) : !cipherKnown(key))
                continue;

            outStr(first ? "[" : ",[");
            outNum64(key);
            outStr(",");
            outJSON(akm ? akmName(t, wpa1) : cipherName(t, wpa1));
            outStr("]");

            first = false;
        }
    }

    uint32_t vendor = akm ? 0xA4C0E102u : 0xA4C0E104u;

    outStr(",[");
    outNum64(vendor);
    outStr(",");
    outJSON(vendorSuiteName(vendor, akm));
    outStr("]");

    if (!akm)
        outStr(",[0,\"zero suite (broken WPA element)\"]");

    outStr("]");
}

/*
 * "reads": what the sensor decodes, for the list below the
 * catalog. Element numbers only, the page has the names.
 */
void sendCatalogReads()
{
    outStr(",\"reads\":{\"v\":1");

    outByteList("dec", decodedIes, sizeof(decodedIes));
    outByteList("decx", decodedExt, sizeof(decodedExt));
    outByteList("part", partialIes, sizeof(partialIes));
    outByteList("pass", passedIes, sizeof(passedIes));
    outByteList("passx", passedExt, sizeof(passedExt));
    outKeyList("vend", decodedVendor,
               sizeof(decodedVendor) / sizeof(decodedVendor[0]));
    // None decoded in part at the moment; the page still reads it
    outKeyList("vpart", nullptr, 0);
    outKeyList("vrev", reviewedVendor,
               sizeof(reviewedVendor) / sizeof(reviewedVendor[0]));
    outSuiteList("akm", true);
    outSuiteList("cipher", false);

    outStr("}");
}

void sendCatalogJSON()
{
    if (reqHasArg("enable"))
    {
        bool on = reqArg("enable").toInt() != 0;

        if (on != catalogEnabled)
        {
            catalogEnabled = on;

            logRequest("IE catalog %s", on ? "on" : "off");

            saveSettings();

            // So nothing counted so far is lost by switching off
            if (!on && catalogDirty)
                catalogSave();
        }
    }

    outBegin();

    outStr("{\"enabled\":");
    outNum(catalogEnabled ? 1 : 0);

    outStr(",\"fs\":");
    outNum(catalogFsOk ? 1 : 0);

    outStr(",\"boot\":");
    outNum(bootCount);

    outStr(",\"max\":");
    outNum(CATALOG_MAX);

    outStr(",\"lost\":");
    outNum(catalogFull);

    outStr(",\"used\":");
    outNum(catalogFsOk ? (long)LittleFS.usedBytes() : 0);

    outStr(",\"total\":");
    outNum(catalogFsOk ? (long)LittleFS.totalBytes() : 0);

    long pcapSize = 0;

    if (catalogFsOk && LittleFS.exists(CATALOG_PCAP))
    {
        File f = LittleFS.open(CATALOG_PCAP, "r");

        if (f)
        {
            pcapSize = (long)f.size();
            f.close();
        }
    }

    outStr(",\"pcap\":");
    outNum(pcapSize);

    outStr(",\"pcapmax\":");
    outNum(CATALOG_PCAP_MAX);

    sendCatalogReads();

    outStr(",\"entries\":[");

    for (uint16_t i = 0; i < catalogCount; i++)
    {
        const CatalogEntry &e = catalog[i];

        if (i)
            outStr(",");

        outStr("{\"k\":");
        outNum(e.kind);

        outStr(",\"id\":");
        outNum64(e.key);

        outStr(",\"n\":");
        outNum64(e.count);

        outStr(",\"in\":");
        outNum(e.seenIn);

        outStr(",\"ch\":");
        outNum(e.channel);

        // Distinct senders; beyond CATALOG_SENDERS only "more"
        outStr(",\"snd\":");
        outNum(e.senderCount);

        if (e.sendersMore)
            outStr(",\"sndmore\":1");

        outStr(",\"fr\":");
        outNum(e.frames);

        outStr(",\"f\":[");
        outNum64(e.firstAt);
        outStr(",");
        outNum(e.firstClock);
        outStr(",");
        outNum(e.firstBoot);

        outStr("],\"l\":[");
        outNum64(e.lastAt);
        outStr(",");
        outNum(e.lastClock);
        outStr(",");
        outNum(e.lastBoot);

        outStr("],\"mac\":");
        outMac(e.mac);

        outStr(",\"ssid\":");
        outJSON(e.ssid);

        // Who owns the OUI of a vendor element or suite
        if (e.kind >= CAT_VENDOR)
        {
            uint8_t oui[6] = { (uint8_t)(e.key >> 24),
                               (uint8_t)(e.key >> 16),
                               (uint8_t)(e.key >> 8), 0, 0, 0 };

            outStr(",\"vn\":");
            outJSON((oui[0] & 0x02) ? "" : lookupVendor(oui));
        }

        outStr(",\"len\":");
        outNum(e.fullLen);

        outStr(",\"raw\":\"");

        for (uint8_t b = 0; b < e.sampleLen; b++)
        {
            char hex[3];

            snprintf(hex, sizeof(hex), "%02x", e.sample[b]);

            outStr(hex);
        }

        outStr("\"}");
    }

    outStr("]}");

    outEnd();
}

void handleCatalogClear()
{
    logRequest("IE catalog cleared, %u entries discarded",
               (unsigned)catalogCount);

    catalogClear();

    reqSend(200, "application/json", "{\"ok\":1}");
}

// ============================================================
// Firmware update over the network (OTA)
//
// partitions.csv provides two firmware slots, app0 and app1. An
// upload to /api/ota goes into the slot not running, is checked
// and then booted. The previous firmware stays in its slot:
//
//   - the bootloader starts a new firmware once, "pending". It
//     confirms itself as soon as either Ethernet has held an IP
//     address for OTA_STABLE_MS (DHCP or the static fallback),
//     or it has answered an HTTP request, whichever comes first.
//     The second covers a sensor reached some other way than
//     over the W5500. If it resets before that, the bootloader
//     goes back to the previous one; if neither happens within
//     OTA_CONFIRM_MS, it rolls back on its own
//   - /api/otarollback boots the previous firmware by hand
//
// Without a second slot (the old "Huge APP" table) an upload is
// refused; the new table has to be flashed over USB once.
// ============================================================

#define OTA_CONFIRM_MS 300000
#define OTA_STABLE_MS  30000

/*
 * Every build carries this marker, so an upload can be told
 * apart from other ESP32-C5 firmware and its version is known
 * before it is booted.
 */
#define FW_MARKER_PREFIX "\x01wlanscout-fw:"

const char fwMarker[] = FW_MARKER_PREFIX FW_VERSION "\x02";

// Finds the marker in data that arrives in pieces
struct MarkerScanner
{
    uint8_t matched;
    uint8_t vlen;
    bool    found;
    char    version[16];

    void reset()
    {
        matched = vlen = 0;
        found = false;
        version[0] = 0;
    }

    void feed(const uint8_t *data, size_t length)
    {
        static const char prefix[] = FW_MARKER_PREFIX;
        const uint8_t plen = sizeof(prefix) - 1;

        for (size_t i = 0; i < length && !found; i++)
        {
            char c = (char)data[i];

            if (matched < plen)
            {
                matched = (c == prefix[matched]) ? matched + 1
                        : (c == prefix[0]) ? 1 : 0;
                vlen = 0;
                continue;
            }

            // The prefix alone, as the search above keeps it,
            // is followed by a 0 and not by a version
            if (c == '\x02' && vlen > 0)
            {
                version[vlen] = 0;
                found = true;
            }
            else if (((c >= '0' && c <= '9') || c == '.') &&
                     vlen < sizeof(version) - 1)
                version[vlen++] = c;
            else
                matched = (c == prefix[0]) ? 1 : 0;
        }
    }
};

// Keep a new firmware pending until otaLoop() decides
extern "C" bool verifyRollbackLater()
{
    return true;
}

bool          otaRunning;
size_t        otaWritten;
char          otaError[96];
MarkerScanner otaScan;
uint32_t      otaRestartAt;      // millis(), 0 = no restart due
bool          otaPending;        // running firmware not confirmed
bool          otaIpSeen;         // ethGotIP, since otaIpSince
uint32_t      otaIpSince;        // millis()
char          otaPrevVersion[16];
bool          otaPrevChecked;

void otaInit()
{
    // Nothing reads fwMarker, keep the linker from dropping it
    asm volatile("" : : "r"(fwMarker));

    esp_ota_img_states_t state;

    otaPending =
        esp_ota_get_state_partition(esp_ota_get_running_partition(),
                                    &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY;

    if (otaPending)
        Serial.println("OTA: new firmware, waiting for confirmation");
}

/*
 * The other slot holds a firmware that did not confirm itself and
 * was rolled back from (or never finished writing): not one to go
 * back to.
 */
bool otaOtherFailed()
{
    const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
    esp_ota_img_states_t state;

    return other != NULL &&
           esp_ota_get_state_partition(other, &state) == ESP_OK &&
           (state == ESP_OTA_IMG_INVALID || state == ESP_OTA_IMG_ABORTED);
}

// Called once the log exists: say so after a rollback
void otaLogRollback()
{
    if (otaOtherFailed())
        logEventFrom("system", "firmware v%s did not confirm itself, "
                     "back on v%s", otaPreviousVersion(), FW_VERSION);
}

void otaFail(const char *text)
{
    if (otaError[0])
        return;

    snprintf(otaError, sizeof(otaError), "%s", text);

    if (otaRunning)
        Update.abort();

    otaRunning = false;
}

/*
 * The three steps of an update, the same over HTTP (a multipart
 * upload to /api/ota) and over USB ("PUT /api/ota?len=N" and the
 * bytes, see usbApiRequest()). The first piece has to hold the
 * image header.
 */
void otaStart()
{
    otaError[0] = 0;
    otaWritten  = 0;
    otaRunning  = false;
    otaScan.reset();

    if (esp_ota_get_next_update_partition(NULL) == NULL)
    {
        otaFail("no second firmware slot: flash once over USB "
                "with the partition table of 1.53.0");
        return;
    }

    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH))
    {
        otaFail(Update.errorString());
        return;
    }

    otaRunning = true;

    logRequest("firmware upload started");
}

void otaWrite(const uint8_t *data, size_t length)
{
    if (!otaRunning)
        return;

    // Image header: magic 0xE9, chip ID at offset 12
    if (otaWritten == 0 &&
        (length < 16 || data[0] != 0xE9 ||
         (data[12] | (data[13] << 8)) != CONFIG_IDF_FIRMWARE_CHIP_ID))
    {
        otaFail("not an ESP32-C5 firmware image");
        return;
    }

    otaScan.feed(data, length);

    if (Update.write((uint8_t *)data, length) != length)
    {
        otaFail(Update.errorString());
        return;
    }

    otaWritten += length;
}

void otaFinish()
{
    if (!otaRunning)
        return;

    if (!otaScan.found)
    {
        otaFail("not a wlanscout+ firmware");
        return;
    }

    // Checks length and SHA-256 and makes the slot the next one to
    // boot
    if (!Update.end(true))
    {
        otaFail(Update.errorString());
        return;
    }

    otaRunning = false;
}

void handleOtaUpload()
{
    HTTPUpload &up = server.upload();

    if (up.status == UPLOAD_FILE_START)
        otaStart();
    else if (up.status == UPLOAD_FILE_WRITE)
        otaWrite(up.buf, up.currentSize);
    else if (up.status == UPLOAD_FILE_END)
        otaFinish();
    else if (up.status == UPLOAD_FILE_ABORTED)
        otaFail("upload aborted");
}

void handleOtaDone()
{
    if (otaError[0] || otaWritten == 0)
    {
        if (!otaError[0])
            snprintf(otaError, sizeof(otaError), "no file received");

        logRequest("firmware upload failed: %s", otaError);

        outBegin();
        outStr("{\"ok\":0,\"error\":");
        outJSON(otaError);
        outStr("}");
        outEnd();

        return;
    }

    logRequest("firmware %s uploaded (%u kB), restarting",
               otaScan.version, (unsigned)(otaWritten / 1024));

    outBegin();
    outStr("{\"ok\":1,\"version\":");
    outJSON(otaScan.version);
    outStr(",\"bytes\":");
    outNum((long)otaWritten);
    outStr("}");
    outEnd();

    otaRestartAt = millis() + 1500;
    otaPrevChecked = false;
}

// Boots the other slot, the firmware that ran before
// ============================================================
// Core dump
// ============================================================

/*
 * When the firmware crashes, ESP-IDF writes a core dump into the
 * coredump partition (see partitions.csv) before the chip
 * restarts: the registers and the stacks of all tasks, as ELF.
 * It stays there through any number of restarts until it is
 * deleted or a later crash replaces it. The start after a crash
 * reads its summary for the event log and the settings page;
 * /api/coredump hands out the whole image, to be decoded with
 * esp-coredump against the .elf of the firmware that crashed.
 */
struct CrashInfo
{
    bool     valid;
    bool     sameFirmware;    // the running firmware is the one that crashed
    char     task[16];
    uint32_t pc;
    uint32_t mcause;
    uint32_t mtval;
    uint32_t ra;
    uint32_t addr;            // of the image in flash, and its size
    uint32_t size;
    char     reason[96];      // ESP-IDF's panic reason, if it kept one
    char     elf[9];          // start of the crashed firmware's ELF SHA-256
};

CrashInfo crashInfo = {};

// What a RISC-V trap cause means; the top bit marks an interrupt
const char *mcauseText(uint32_t cause)
{
    if (cause & 0x80000000u)
        return "interrupt (watchdog)";

    switch (cause)
    {
        case 0:  return "instruction address misaligned";
        case 1:  return "instruction access fault";
        case 2:  return "illegal instruction";
        case 3:  return "breakpoint";
        case 4:  return "load address misaligned";
        case 5:  return "load access fault";
        case 6:  return "store address misaligned";
        case 7:  return "store access fault";
        case 8:
        case 9:
        case 11: return "environment call";
    }

    return "exception";
}

// The short cause for the event log and the page
const char *crashCauseText()
{
    return crashInfo.reason[0] ? crashInfo.reason
                               : mcauseText(crashInfo.mcause);
}

void coreDumpSetup()
{
    crashInfo = {};

    size_t addr = 0;
    size_t size = 0;

    if (esp_core_dump_image_get(&addr, &size) != ESP_OK || size == 0)
        return;

    if (esp_core_dump_image_check() != ESP_OK)
    {
        Serial.println("Core dump: present but damaged, ignored");
        return;
    }

    // About 1.1 kB with the stack excerpt, too much for setup()'s stack
    esp_core_dump_summary_t *sum =
        (esp_core_dump_summary_t *)malloc(sizeof(esp_core_dump_summary_t));

    if (sum == nullptr)
        return;

    if (esp_core_dump_get_summary(sum) == ESP_OK)
    {
        crashInfo.valid = true;
        crashInfo.addr  = addr;
        crashInfo.size  = size;
        crashInfo.pc    = sum->exc_pc;
        crashInfo.mcause = sum->ex_info.mcause;
        crashInfo.mtval = sum->ex_info.mtval;
        crashInfo.ra    = sum->ex_info.ra;

        snprintf(crashInfo.task, sizeof(crashInfo.task), "%.*s",
                 (int)sizeof(sum->exc_task) - 1, sum->exc_task);

        memcpy(crashInfo.elf, sum->app_elf_sha256, 8);
        crashInfo.elf[8] = 0;

        char mine[17] = "";
        esp_app_get_elf_sha256(mine, sizeof(mine));
        crashInfo.sameFirmware = strncmp(mine, crashInfo.elf, 8) == 0;
    }

    free(sum);

    if (!crashInfo.valid)
        return;

    char reason[200] = "";

    if (esp_core_dump_get_panic_reason(reason, sizeof(reason)) == ESP_OK)
    {
        // One line is enough, the dump has the rest
        reason[strcspn(reason, "\r\n")] = 0;
        strlcpy(crashInfo.reason, reason, sizeof(crashInfo.reason));
    }

    Serial.printf("Core dump: %s in %s at 0x%08lx (ra 0x%08lx, mtval "
                  "0x%08lx), %lu bytes, firmware %s%s\r\n",
                  crashCauseText(), crashInfo.task,
                  (unsigned long)crashInfo.pc, (unsigned long)crashInfo.ra,
                  (unsigned long)crashInfo.mtval,
                  (unsigned long)crashInfo.size, crashInfo.elf,
                  crashInfo.sameFirmware ? " (this one)" : "");

    /*
     * Logged once per dump, not on every start until it is
     * deleted: the NVS keeps a fingerprint of the last one logged.
     */
    uint32_t mark = crashInfo.pc ^ (crashInfo.mtval << 1) ^
                    (crashInfo.size << 7) ^
                    strtoul(crashInfo.elf, nullptr, 16);

    prefs.begin("analyzer", false);

    bool fresh = prefs.getUInt("cdlog", 0) != mark;

    if (fresh)
        prefs.putUInt("cdlog", mark);

    prefs.end();

    if (fresh)
        logEventFrom("system", "crash in %s at 0x%08lx: %.40s; core dump "
                     "in Settings", crashInfo.task,
                     (unsigned long)crashInfo.pc, crashCauseText());
}

/*
 * GET /api/coredump: the image as it is in flash, for
 *   esp-coredump info_corefile -t raw -c <file> wlanscout.ino.elf
 * ?erase=1 deletes it.
 */
void handleCoreDump()
{
    if (reqHasArg("erase"))
    {
        bool had = crashInfo.valid;

        esp_err_t err = esp_core_dump_image_erase();

        crashInfo = {};

        if (had)
            logRequest("core dump deleted");

        reqSend(err == ESP_OK ? 200 : 500, "text/plain",
                err == ESP_OK ? "Core dump deleted." : "Could not delete it.");
        return;
    }

    const esp_partition_t *part =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                 ESP_PARTITION_SUBTYPE_DATA_COREDUMP,
                                 nullptr);

    if (!crashInfo.valid || part == nullptr ||
        crashInfo.addr < part->address ||
        crashInfo.addr + crashInfo.size > part->address + part->size)
    {
        reqSend(404, "text/plain", "No core dump stored.");
        return;
    }

    char name[48];

    snprintf(name, sizeof(name), "wlanscout-coredump-%s.bin", crashInfo.elf);

    reqStart(200, "application/octet-stream", crashInfo.size, name);

    static uint8_t part_buf[1024];

    uint32_t offset = crashInfo.addr - part->address;
    uint32_t left   = crashInfo.size;

    while (left > 0)
    {
        uint32_t n = min(left, (uint32_t)sizeof(part_buf));

        if (esp_partition_read(part, offset, part_buf, n) != ESP_OK)
            break;

        reqContent(part_buf, n);

        offset += n;
        left   -= n;
    }

    reqDone();

    lastRequestServed = millis();
}

/*
 * GET /api/cfgmute?bssid=…&on=1: keep this BSS's configuration
 * changes out of the bell; on=0 lets them in again.
 */
void handleCfgMute()
{
    uint8_t mac[6];

    if (!reqHasArg("bssid") || !parseMacArg(reqArg("bssid"), mac))
    {
        reqSend(400, "text/plain", "bssid missing or not a MAC address");
        return;
    }

    bool on = reqArg("on").toInt() != 0;
    bool was = cfgIsMuted(mac);

    if (on && !was)
    {
        if (cfgMutedCount >= CFG_MUTE_MAX)
        {
            reqSend(409, "text/plain", "at most 16 BSSs can be muted");
            return;
        }

        memcpy(cfgMuted[cfgMutedCount++], mac, 6);
    }
    else if (!on && was)
    {
        for (uint8_t i = 0; i < cfgMutedCount; i++)
        {
            if (memcmp(cfgMuted[i], mac, 6) == 0)
            {
                memmove(cfgMuted[i], cfgMuted[i + 1],
                        (cfgMutedCount - i - 1) * 6);
                cfgMutedCount--;
                break;
            }
        }
    }

    if (on != was)
    {
        cfgMuteSave();
        cfgVersion++;

        logRequest("configuration changes of %s %s",
                   reqArg("bssid").c_str(),
                   on ? "muted" : "notified again");
    }

    reqSend(200, "application/json",
            String("{\"ok\":1,\"muted\":") + cfgMutedCount + "}");
}

void handleOtaRollback()
{
    if (otaOtherFailed() || !Update.canRollBack() || !Update.rollBack())
    {
        reqSend(200, "application/json",
                    "{\"ok\":0,\"error\":\"no previous firmware\"}");
        return;
    }

    logRequest("rollback to firmware %s, restarting",
               otaPreviousVersion());

    reqSend(200, "application/json", "{\"ok\":1}");

    otaRestartAt = millis() + 1500;
}

// The version in the other slot, read once from its marker
const char *otaPreviousVersion()
{
    if (otaPrevChecked)
        return otaPrevVersion;

    otaPrevChecked = true;
    otaPrevVersion[0] = 0;

    const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
    esp_app_desc_t desc;

    if (other == NULL ||
        esp_ota_get_partition_description(other, &desc) != ESP_OK)
        return otaPrevVersion;

    MarkerScanner scan;
    static uint8_t chunk[2048];

    scan.reset();

    for (size_t at = 0; at < other->size && !scan.found;
         at += sizeof(chunk))
    {
        if (esp_partition_read(other, at, chunk, sizeof(chunk)) != ESP_OK)
            break;

        scan.feed(chunk, sizeof(chunk));
    }

    snprintf(otaPrevVersion, sizeof(otaPrevVersion), "%s",
             scan.found ? scan.version : "?");

    return otaPrevVersion;
}

void otaLoop()
{
    if (otaPending)
    {
        if (!ethGotIP)
            otaIpSeen = false;
        else if (!otaIpSeen)
        {
            otaIpSeen  = true;
            otaIpSince = millis();
        }

        bool ipStable = otaIpSeen &&
                        millis() - otaIpSince >= OTA_STABLE_MS;

        if (ipStable || lastRequestServed != 0)
        {
            esp_ota_mark_app_valid_cancel_rollback();

            otaPending = false;

            if (ipStable)
                logEventFrom("system", "firmware v%s confirmed, "
                             "Ethernet up for %d s", FW_VERSION,
                             OTA_STABLE_MS / 1000);
            else
                logEventFrom("system", "firmware v%s confirmed, "
                             "first request answered", FW_VERSION);
        }
        else if (millis() > OTA_CONFIRM_MS)
        {
            Serial.println("OTA: neither an IP address nor a request, "
                           "rolling back");

            esp_ota_mark_app_invalid_rollback_and_reboot();
        }
    }

    if (otaRestartAt != 0 && (int32_t)(millis() - otaRestartAt) >= 0)
    {
        catalogSave();

        ESP.restart();
    }
}

// "ota": the slots, for Settings -> System
void outOtaJSON()
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *other   = esp_ota_get_next_update_partition(NULL);

    outStr(",\"ota\":{\"slot\":");
    outJSON(running ? running->label : "");
    outStr(",\"size\":");
    outNum(running ? (long)running->size : 0);
    outStr(",\"other\":");
    outJSON(other ? other->label : "");
    outStr(",\"prev\":");
    outJSON(other ? otaPreviousVersion() : "");
    outStr(",\"rollback\":");
    outNum(other && !otaOtherFailed() && Update.canRollBack() ? 1 : 0);
    outStr(",\"failed\":");
    outNum(otaOtherFailed() ? 1 : 0);
    outStr(",\"pending\":");
    outNum(otaPending ? 1 : 0);
    outStr("}");
}

/*
 * "element-ie73", "element-ext107", "element-vendor-506F9A-41",
 * "element-akm-000FAC-30": what a download of one entry is called.
 * The page adds the element's name when it offers the link.
 */
void catalogFileName(uint8_t kind, uint32_t key, char *out, size_t size)
{
    if (kind == CAT_ELEMENT || kind == CAT_EXT)
    {
        snprintf(out, size, "element-%s%lu",
                 kind == CAT_ELEMENT ? "ie" : "ext",
                 (unsigned long)key);
        return;
    }

    snprintf(out, size, "element-%s-%06lX-%lu",
             kind == CAT_VENDOR ? "vendor"
                 : (kind == CAT_AKM ? "akm" : "cipher"),
             (unsigned long)(key >> 8), (unsigned long)(key & 0xFF));
}

/*
 * The frames of one catalog entry (?k=kind&id=key), picked out
 * of the pcap file by looking at each record again. Two passes:
 * the first only adds up the length, so the reply needs no
 * chunked encoding.
 */
void sendCatalogPcapFor(uint8_t kind, uint32_t key)
{
    File f = LittleFS.open(CATALOG_PCAP, "r");

    if (!f || f.size() < 24)
    {
        reqSend(404, "text/plain", "No frames for this entry.");
        return;
    }

    static uint8_t buf[16 + RADIOTAP_LEN + MGMT_PARSE_MAX];

    uint8_t hdr[24];

    f.read(hdr, sizeof(hdr));

    catalogMatching  = true;
    catalogMatchKind = kind;
    catalogMatchKey  = key;

    uint32_t total = 24;
    uint32_t count = 0;

    for (int pass = 0; pass < 2; pass++)
    {
        f.seek(24);

        while (true)
        {
            if (f.read(buf, 16) != 16)
                break;

            uint32_t len;

            memcpy(&len, &buf[8], 4);

            if (len < RADIOTAP_LEN || len > RADIOTAP_LEN + MGMT_PARSE_MAX ||
                f.read(buf + 16, len) != len)
            {
                break;
            }

            CatalogFrame cf = {};

            catalogMatched = false;

            catalogScan(buf + 16 + RADIOTAP_LEN, len - RADIOTAP_LEN, 0, cf);

            if (!catalogMatched)
                continue;

            if (pass == 0)
            {
                total += 16 + len;
                count++;
            }
            else
            {
                reqContent(buf, 16 + len);
            }
        }

        if (pass == 0)
        {
            if (count == 0)
                break;

            char base[32];

            catalogFileName(kind, key, base, sizeof(base));

            char name[48];

            snprintf(name, sizeof(name), "%s.pcap", base);

            reqStart(200, "application/vnd.tcpdump.pcap", total, name);
            reqContent(hdr, sizeof(hdr));
        }
    }

    catalogMatching = false;

    f.close();

    if (count == 0)
        reqSend(404, "text/plain", "No frames for this entry.");
    else
        reqDone();

    lastRequestServed = millis();
}

void sendCatalogPcap()
{
    if (!catalogFsOk || !LittleFS.exists(CATALOG_PCAP))
    {
        reqSend(404, "text/plain",
                    "No frames in the IE catalog yet.");
        return;
    }

    if (reqHasArg("k") && reqHasArg("id"))
    {
        sendCatalogPcapFor((uint8_t)reqArg("k").toInt(),
                           (uint32_t)strtoul(reqArg("id").c_str(),
                                             nullptr, 10));
        return;
    }

    File f = LittleFS.open(CATALOG_PCAP, "r");

    if (!f)
    {
        reqSend(500, "text/plain", "cannot open the catalog pcap");
        return;
    }

    reqStart(200, "application/vnd.tcpdump.pcap", f.size(),
             "element-catalog.pcap");

    static uint8_t part[1024];

    while (true)
    {
        size_t got = f.read(part, sizeof(part));

        if (got == 0)
            break;

        reqContent(part, got);
    }

    reqDone();

    f.close();

    lastRequestServed = millis();
}

void handleAssocHistoryClear()
{
    uint32_t kept = 0;

    if (assocHistory != nullptr)
    {
        for (int i = 0; i < MAX_ASSOC_HISTORY; i++)
        {
            if (assocHistory[i].valid)
                kept++;

            assocHistory[i].valid = false;
        }
    }

    assocHistoryDropped = 0;

    logRequest("association history cleared, %u stations discarded",
               (unsigned)kept);

    reqSend(200, "application/json", "{\"ok\":1}");
}

/*
 * Flush button above the BSSID and the station graph: empties the
 * live table, which then fills again from what is heard next.
 * ?what=networks takes the 6 GHz list with it, whose entries hang
 * on a reporting AP; ?what=stations the names learned from probe
 * and association requests. The association history, the capture
 * buffers and the IE catalog stay, they have their own buttons.
 */
void handleFlush()
{
    String what = reqArg("what");

    uint32_t dropped = 0;

    if (what == "networks")
    {
        for (int i = 0; i < MAX_NETWORKS; i++)
        {
            if (networks[i].valid)
                dropped++;

            networks[i].valid = false;
        }

        if (sixGhz != nullptr)
            for (int i = 0; i < MAX_SIXG_BSS; i++)
                sixGhz[i].valid = false;

        logRequest("network table flushed, %u BSSIDs discarded",
                   (unsigned)dropped);
    }
    else if (what == "stations")
    {
        for (int i = 0; i < MAX_CLIENTS; i++)
        {
            if (clients[i].valid)
                dropped++;

            clients[i].valid = false;
        }

        if (bssNames != nullptr)
            for (int i = 0; i < MAX_BSS_NAMES; i++)
                bssNames[i].valid = false;

        for (int i = 0; i < MAX_AWDL; i++)
            awdlDevices[i].valid = false;

        logRequest("station table flushed, %u stations discarded",
                   (unsigned)dropped);
    }
    else
    {
        reqSend(400, "application/json", "{\"ok\":0}");
        return;
    }

    reqSend(200, "application/json", "{\"ok\":1}");
}

void handleTime()
{
    if (reqHasArg("epoch"))
    {
        int64_t epoch = (int64_t)reqArg("epoch").toInt();

        // NTP outranks the browser clock, the browser an AP
        if (pcapClockSource != 2)
        {
            clockOffsetUs =
                epoch * 1000000LL - esp_timer_get_time();

            pcapClockSource = 1;

            Serial.println("Clock taken from the browser");
        }
    }

    reqSend(200, "application/json", "{\"ok\":1}");
}

// ============================================================
// API: system
//
// Everything the settings page shows: addressing as handed
// out by DHCP or configured statically, memory, where the
// tables ended up and what the counters are at.
// ============================================================

// ============================================================
// CPU, loop and queue statistics
//
// FreeRTOS counts the run time of every task in µs (run time
// stats on esp_timer, built into the Arduino core's libraries).
// Every 10 s the counters are compared with the last ones: the
// share of each task in that window, the load being what the idle
// task did not get. Interrupts count for the task they interrupt.
//
// Beside it the longest round of loop() and the longest request
// it answered, the highest fill of the management queue, and the
// chip's own temperature (not the air around it).
// ============================================================

#define STATS_WINDOW_MS 10000
#define STATS_TASKS     32

struct TaskShare
{
    char     name[16];
    uint16_t share;          // tenths of a percent of the window
    uint32_t stackFree;      // bytes never used since its start
};

struct StatsWindow
{
    bool     valid;
    uint16_t load;           // tenths of a percent
    uint8_t  taskCount;
    TaskShare tasks[STATS_TASKS];
    uint32_t loopUs;         // longest loop() round
    uint32_t httpUs;         // longest request answered
    char     httpUri[48];
    uint8_t  queueFill;
};

StatsWindow statsLast;       // the last complete window

uint32_t loopUsWin  = 0, loopUsBoot = 0;
char     statsPath[40] = "";     // the API route answered last
uint32_t httpUsWin  = 0, httpUsBoot = 0;
char     httpUriWin[48] = "", httpUriBoot[48] = "";

static TaskStatus_t *statsNow  = nullptr;
static uint32_t      statsPrevNum[STATS_TASKS];
static uint32_t      statsPrevRun[STATS_TASKS];
static uint8_t       statsPrevCount = 0;
static uint32_t      statsPrevTotal = 0;
static uint32_t      statsAt = 0;

static temperature_sensor_handle_t chipTemp = nullptr;
float chipTempC = NAN;

void statsSetup()
{
    statsNow = (TaskStatus_t *)heap_caps_malloc(
        sizeof(TaskStatus_t) * STATS_TASKS, MALLOC_CAP_SPIRAM);

    // 20 to 100 °C: what a chip in a closed housing reaches
    temperature_sensor_config_t cfg =
        TEMPERATURE_SENSOR_CONFIG_DEFAULT(20, 100);

    if (temperature_sensor_install(&cfg, &chipTemp) != ESP_OK)
        chipTemp = nullptr;
    else if (temperature_sensor_enable(chipTemp) != ESP_OK)
    {
        temperature_sensor_uninstall(chipTemp);
        chipTemp = nullptr;
    }
}

// The longest request of the window and since the start
void statsRequest(uint32_t us, const char *uri)
{
    if (us > httpUsWin)
    {
        httpUsWin = us;
        strlcpy(httpUriWin, uri, sizeof(httpUriWin));
    }

    if (us > httpUsBoot)
    {
        httpUsBoot = us;
        strlcpy(httpUriBoot, uri, sizeof(httpUriBoot));
    }
}

// Called at the top of every loop(): the time since the last round
void statsLoopRound()
{
    static int64_t last = 0;

    int64_t now = esp_timer_get_time();

    if (last != 0)
    {
        uint32_t us = (uint32_t)(now - last);

        if (us > loopUsWin)
            loopUsWin = us;

        if (us > loopUsBoot)
            loopUsBoot = us;
    }

    last = now;
}

void statsLoop()
{
    if (statsNow == nullptr || millis() - statsAt < STATS_WINDOW_MS)
        return;

    statsAt = millis();

    uint32_t total = 0;

    UBaseType_t n = uxTaskGetSystemState(statsNow, STATS_TASKS, &total);

    if (chipTemp != nullptr)
    {
        float c;

        if (temperature_sensor_get_celsius(chipTemp, &c) == ESP_OK)
            chipTempC = c;
    }

    uint32_t span = total - statsPrevTotal;

    if (statsPrevCount > 0 && span > 0 && n > 0)
    {
        StatsWindow &w = statsLast;

        w.taskCount = 0;
        uint64_t idle = 0;

        for (UBaseType_t i = 0; i < n; i++)
        {
            const TaskStatus_t &s = statsNow[i];

            uint32_t before = 0;
            bool     known  = false;

            for (uint8_t j = 0; j < statsPrevCount; j++)
                if (statsPrevNum[j] == s.xTaskNumber)
                {
                    before = statsPrevRun[j];
                    known  = true;
                    break;
                }

            // A task started in the window counts from zero
            uint32_t ran = s.ulRunTimeCounter - (known ? before : 0);

            if (strncmp(s.pcTaskName, "IDLE", 4) == 0)
                idle += ran;

            if (w.taskCount < STATS_TASKS)
            {
                TaskShare &t = w.tasks[w.taskCount++];

                strlcpy(t.name, s.pcTaskName, sizeof(t.name));
                t.share = (uint16_t)std::min<uint64_t>(
                    1000, (uint64_t)ran * 1000 / span);
                t.stackFree = s.usStackHighWaterMark;
            }
        }

        std::sort(w.tasks, w.tasks + w.taskCount,
                  [](const TaskShare &a, const TaskShare &b)
                  { return a.share > b.share; });

        w.load = idle >= span ? 0
               : (uint16_t)(1000 - idle * 1000 / span);

        w.loopUs    = loopUsWin;
        w.httpUs    = httpUsWin;
        strlcpy(w.httpUri, httpUriWin, sizeof(w.httpUri));
        w.queueFill = mgmtFillWin;
        w.valid     = true;
    }

    statsPrevCount = 0;

    for (UBaseType_t i = 0; i < n && i < STATS_TASKS; i++)
    {
        statsPrevNum[statsPrevCount] = statsNow[i].xTaskNumber;
        statsPrevRun[statsPrevCount] = statsNow[i].ulRunTimeCounter;
        statsPrevCount++;
    }

    statsPrevTotal = total;

    loopUsWin   = 0;
    httpUsWin   = 0;
    httpUriWin[0] = 0;
    mgmtFillWin = 0;
}

void outStatsJSON()
{
    const StatsWindow &w = statsLast;

    if (w.valid)
    {
        outStr(",\"cpu\":{\"win\":");
        outNum(STATS_WINDOW_MS / 1000);

        outStr(",\"load\":");
        outNum(w.load);

        outStr(",\"tasks\":[");

        for (uint8_t i = 0; i < w.taskCount; i++)
        {
            outStr(i ? ",[" : "[");
            outJSON(w.tasks[i].name);
            outStr(",");
            outNum(w.tasks[i].share);
            outStr(",");
            outNum(w.tasks[i].stackFree);
            outStr("]");
        }

        outStr("]}");
    }

    if (!isnan(chipTempC))
    {
        char buf[16];

        snprintf(buf, sizeof(buf), "%.1f", chipTempC);

        outStr(",\"temp\":");
        outStr(buf);
    }

    if (crashInfo.valid)
    {
        char hex[12];

        outStr(",\"crash\":{\"task\":");
        outJSON(crashInfo.task);
        outStr(",\"cause\":");
        outJSON(crashCauseText());
        snprintf(hex, sizeof(hex), "0x%08lx", (unsigned long)crashInfo.pc);
        outStr(",\"pc\":");
        outJSON(hex);
        snprintf(hex, sizeof(hex), "0x%08lx", (unsigned long)crashInfo.ra);
        outStr(",\"ra\":");
        outJSON(hex);
        snprintf(hex, sizeof(hex), "0x%08lx", (unsigned long)crashInfo.mtval);
        outStr(",\"mtval\":");
        outJSON(hex);
        outStr(",\"mcause\":");
        outNum(crashInfo.mcause);
        outStr(",\"size\":");
        outNum(crashInfo.size);
        outStr(",\"elf\":");
        outJSON(crashInfo.elf);
        outStr(",\"same\":");
        outNum(crashInfo.sameFirmware ? 1 : 0);
        outStr("}");
    }

    // [loop round µs, request µs, its path, highest queue fill]
    outStr(",\"loop\":{\"win\":[");
    outNum(w.valid ? w.loopUs : 0);
    outStr(",");
    outNum(w.valid ? w.httpUs : 0);
    outStr(",");
    outJSON(w.valid ? w.httpUri : "");
    outStr(",");
    outNum(w.valid ? w.queueFill : 0);

    outStr("],\"boot\":[");
    outNum(loopUsBoot);
    outStr(",");
    outNum(httpUsBoot);
    outStr(",");
    outJSON(httpUriBoot);
    outStr(",");
    outNum(mgmtFillBoot);

    outStr("],\"queue\":");
    outNum(MGMT_QUEUE_SIZE - 1);

    // [answers given up, seconds since the last, its client]
    outStr(",\"givenup\":[");
    outNum(txGivenUp);
    outStr(",");
    outNum(txGivenUp ? (millis() - txGivenUpAt) / 1000 : 0);
    outStr(",");
    outJSON(txGivenUpWho);
    outStr("]}");
}

void sendSystemJSON()
{
    outBegin();

    outStr("{\"version\":");
    outJSON(FW_VERSION);

    outStr(",\"uptime\":");
    outNum((long)(esp_timer_get_time() / 1000000ULL));

    outStr(",\"oui\":");
    outNum(OUI_FULL_COUNT);

    outStr(",\"sensor\":");
    outJSON(sensorName);

    outStr(",\"sensorid\":");
    outJSON(sensorMacText().c_str());

    outStr(",\"boot\":");
    outNum(bootCount);

    outStr(",\"reset\":");
    outJSON(resetReasonText());

    outOtaJSON();

    // The IE catalog, see the IE catalog view
    outStr(",\"catalog\":{\"enabled\":");
    outNum(catalogEnabled ? 1 : 0);
    outStr(",\"fs\":");
    outNum(catalogFsOk ? 1 : 0);
    outStr(",\"rows\":");
    outNum(catalogCount);
    outStr(",\"max\":");
    outNum(CATALOG_MAX);
    outStr(",\"used\":");
    outNum(catalogFsOk ? (long)LittleFS.usedBytes() : 0);
    outStr(",\"total\":");
    outNum(catalogFsOk ? (long)LittleFS.totalBytes() : 0);
    outStr("}");

    outStr(",\"clock\":");
    outNum(pcapClockSource);

    // Which AP the clock came from, and when it was last renewed
    if (pcapClockSource == 3)
    {
        char mac[18];

        snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
                 wifiClockBssid[0], wifiClockBssid[1],
                 wifiClockBssid[2], wifiClockBssid[3],
                 wifiClockBssid[4], wifiClockBssid[5]);

        outStr(",\"clockap\":");
        outJSON(mac);

        outStr(",\"clockapn\":");
        outNum(wifiClockApCount);

        outStr(",\"clockapago\":");
        outNum((long)((millis() - wifiClockLastSet) / 1000));
    }

    outStr(",\"utc\":");
    {
        char buffer[24];

        formatStamp(deviceEpoch(), buffer, sizeof(buffer));

        outJSON(buffer);
    }

    outStr(",\"tz\":");
    outNum(tzMinutes);

    outStr(",\"tzlabel\":");
    {
        char label[12];
        formatTzLabel(label, sizeof(label));
        outJSON(label);
    }

    outStr(",\"ntpsync\":");
    {
        if (ntpLastSync == 0)
        {
            outJSON("");
        }
        else
        {
            char buffer[24];

            formatStamp(ntpLastSync, buffer, sizeof(buffer));

            outJSON(buffer);
        }
    }

    outStr(",\"ntpago\":");
    {
        uint32_t nowSec = deviceEpoch();

        outNum(ntpLastSync ? (long)(nowSec - ntpLastSync) : -1);
    }

    outStr(",\"ntpadjust\":");
    outNum(ntpLastAdjust);

    outStr(",\"ntpcount\":");
    outNum(ntpSyncCount);

    outStr(",\"ntpinterval\":");
    outNum(NTP_SYNC_INTERVAL_MS / 1000);

    outStr(",\"ntpserver\":");
    outJSON(NTP_SERVER1);

    // ---- network ----

    outStr(",\"net\":{\"ip\":");
    outJSON(ETH.localIP().toString().c_str());

    outStr(",\"gw\":");
    outJSON(ETH.gatewayIP().toString().c_str());

    outStr(",\"mask\":");
    outJSON(ETH.subnetMask().toString().c_str());

    outStr(",\"dns\":");
    outJSON(ETH.dnsIP().toString().c_str());

    outStr(",\"mac\":");
    outJSON(sensorMacText().c_str());

    outStr(",\"speed\":");
    outNum(ETH.linkSpeed());

    outStr(",\"duplex\":");
    outNum(ETH.fullDuplex() ? 1 : 0);

    outStr(",\"link\":");
    outNum(ethLinkUp ? 1 : 0);

    outStr(",\"static\":");
    outNum(ethStaticActive ? 1 : 0);

    outStr(",\"dhcptries\":");
    outNum(dhcpAttempts);

    outStr(",\"hostname\":");
    outJSON(dhcpHostname);

    outStr(",\"dhcpstate\":");
    outJSON(dhcpStatusText());

    if (dhcpLeaseStateText(dhcpLease.state) != nullptr)
    {
        outStr(",\"lease\":{\"state\":");
        outJSON(dhcpLeaseStateText(dhcpLease.state));

        if (dhcpLease.ackAt != 0)
        {
            uint32_t used = (millis() - dhcpLease.ackAt) / 1000;

            outStr(",\"time\":");
            outNum(dhcpLease.seconds);
            outStr(",\"left\":");
            outNum(used < dhcpLease.seconds ? dhcpLease.seconds - used : 0);
            outStr(",\"ack\":");
            outNum(used);
            outStr(",\"server\":");
            outJSON(IPAddress(dhcpLease.server).toString().c_str());
        }

        // lwIP's coarse DHCP timer ticks every
        // DHCP_COARSE_TIMER_SECS, one second in ESP-IDF
        if (dhcpLease.state == DHCP_STATE_BOUND && dhcpLease.renewIn != 0)
        {
            outStr(",\"renew\":");
            outNum((long)dhcpLease.renewIn * DHCP_COARSE_TIMER_SECS);
        }

        outStr(",\"renewals\":");
        outNum(dhcpLease.renewals);
        outStr("}");
    }

    outStr("}");

    // ---- memory ----

    outStr(",\"mem\":{\"heap\":");
    outNum(ESP.getFreeHeap());

    outStr(",\"heapmin\":");
    outNum(ESP.getMinFreeHeap());

    outStr(",\"heaptotal\":");
    outNum(ESP.getHeapSize());

    outStr(",\"block\":");
    outNum(heap_caps_get_largest_free_block(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

    outStr(",\"psramtotal\":");
    outNum(ESP.getPsramSize());

    outStr(",\"psramfree\":");
    outNum(heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    outStr("}");

    outStatsJSON();

    // ---- allocations ----

    outStr(",\"alloc\":[");

    for (uint8_t i = 0; i < allocInfoCount; i++)
    {
        if (i)
            outStr(",");

        outStr("{\"name\":");
        outJSON(allocInfo[i].name);

        outStr(",\"bytes\":");
        outNum(allocInfo[i].bytes);

        outStr(",\"psram\":");
        outNum(allocInfo[i].psram ? 1 : 0);

        outStr("}");
    }

    outStr("]");

    // ---- counters ----

    outStr(",\"counters\":{\"sweeps\":");
    outNum(sweepCount);

    outStr(",\"mgmtdrops\":");
    outNum(mgmtDrops);

    outStr(",\"datadrops\":");
    outNum(dataDrops);

    outStr(",\"fcs\":");
    outNum(fcsErrors);

    outStr(",\"selected\":");
    outNum(countSelectedChannels());

    outStr(",\"plan\":");
    outNum(planCount);

    outStr(",\"fast\":");
    outNum(fastSweep ? 1 : 0);

    outStr(",\"mode\":");
    outNum(scanMode);

    outStr("}");

    // ---- capture ----

    uint32_t pcapBytes = 0;

    if (pcapRing != nullptr)
    {
        uint32_t start =
            (pcapCount < PCAP_SLOTS) ? 0 : pcapWrite;

        for (uint32_t i = 0; i < pcapCount; i++)
            pcapBytes += pcapRing[(start + i) % PCAP_SLOTS].length;
    }

    outStr(",\"pcap\":{\"count\":");
    outNum(pcapCount);

    outStr(",\"cap\":");
    outNum(PCAP_SLOTS);

    outStr(",\"total\":");
    outNum(pcapTotal);

    outStr(",\"beacons\":");
    outNum(countBeaconSlots());

    outStr(",\"beaconslots\":");
    outNum(MAX_BEACON_SLOTS);

    outStr(",\"beacontotal\":");
    outNum(beaconTotal);

    outStr(",\"assoc\":");
    outNum(assocCount);

    outStr(",\"assoccap\":");
    outNum(ASSOC_SLOTS);

    outStr(",\"assoctotal\":");
    outNum(assocTotal);

    outStr(",\"bytes\":");
    outNum(pcapBytes);

    outStr(",\"slot\":");
    outNum(sizeof(PcapRecord));

    outStr(",\"enabled\":");
    outNum(pcapEnabled ? 1 : 0);

    outStr(",\"mask\":");
    outNum(pcapMask);

    outStr("}}");

    outEnd();
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
             * This boot: from the time since boot. An earlier one:
             * its wall time if the clock was known by then, else
             * only the time after that start.
             */
            bool    thisBoot = e.boot == bootCount;
            int32_t ago      = -1;
            uint32_t at      = 0;

            if (thisBoot)
            {
                ago = (int32_t)(nowUp - e.up);
                at  = nowEpoch - ago;
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

// ============================================================
// API: export
//
// Everything this sensor knows, in one document, meant to be
// merged with the output of other sensors covering the same
// area. Three things matter for that and are different from
// the endpoints the web interface uses:
//
//   - every timestamp is absolute UTC epoch seconds, not an
//     age, because ages from different sensors cannot be
//     lined up
//   - the sensor identifies itself by its Ethernet MAC, which
//     is stable across reboots, plus an optional name
//   - sections and a "since" cut-off keep the document small
//     enough to be polled often
//
// Captured frames are not part of this, they stay behind
// /api/pcap.
// ============================================================

bool wantSection(const String &sections, const char *name)
{
    if (sections.length() == 0)
        return true;

    return (sections.indexOf(name) >= 0);
}

void sendExportJSON()
{
    String sections = reqHasArg("sections")
                          ? reqArg("sections")
                          : String("");

    uint32_t since = reqHasArg("since")
                         ? (uint32_t)reqArg("since").toInt()
                         : 0;

    uint32_t now      = millis();
    uint32_t nowEpoch = deviceEpoch();

    // millis to epoch for a stored timestamp
    #define EPOCH_OF(ms) ((long)(nowEpoch - (now - (ms)) / 1000))

    refreshPcapCounts();

    outBegin();

    outStr("{\"schema\":");
    outNum(EXPORT_SCHEMA);

    // ---- sensor ----

    outStr(",\"sensor\":{\"id\":");
    outJSON(sensorMacText().c_str());

    outStr(",\"name\":");
    outJSON(sensorName);

    outStr(",\"ip\":");
    outJSON(ETH.localIP().toString().c_str());

    outStr(",\"fw\":");
    outJSON(FW_VERSION);

    outStr(",\"uptime\":");
    outNum((long)(esp_timer_get_time() / 1000000LL));

    outStr(",\"time\":");
    outNum((long)nowEpoch);

    outStr(",\"clock\":");
    outNum(pcapClockSource);

    outStr(",\"mode\":");
    outNum(scanMode);

    outStr("}");

    // ---- scan configuration ----

    outStr(",\"scan\":{\"selected\":");
    outNum(countSelectedChannels());

    outStr(",\"plan\":");
    outNum(planCount);

    outStr(",\"fast\":");
    outNum(fastSweep ? 1 : 0);

    outStr(",\"dwell\":");
    outNum(CHANNEL_DWELL_MS);

    outStr(",\"sweeps\":");
    outNum(sweepCount);

    outStr(",\"channels\":[");

    {
        bool first = true;

        for (uint16_t i = 0; i < TOTAL_CHANNELS; i++)
        {
            if (!channelSelected[i])
                continue;

            if (!first)
                outStr(",");

            first = false;

            outNum(getChannelNumber(i));
        }
    }

    outStr("]}");

    // ---- networks ----

    if (wantSection(sections, "networks"))
    {
        outStr(",\"networks\":[");

        bool first = true;

        for (int i = 0; i < MAX_NETWORKS; i++)
        {
            WiFiNetwork &n = networks[i];

            if (!n.valid)
                continue;

            if (since && (uint32_t)EPOCH_OF(n.lastSeen) < since)
                continue;

            if (!first)
                outStr(",");

            first = false;

            outStr("{\"bssid\":");
            outMac(n.bssid);

            outStr(",\"ssid\":");
            outJSON(n.ssid);

            outStr(",\"vendor\":");
            outJSON(lookupBssidVendor(n.bssid, n.mlo));

            outStr(",\"ch\":");
            outNum(n.channel);

            outStr(",\"center\":");
            outNum(n.centerChannel);

            outStr(",\"bw\":");
            outNum(n.bandwidth);

            outStr(",\"freq\":");
            outNum(pcapChannelFreq(n.centerChannel));

            outStr(",\"rssi\":");
            outNum(n.rssi);

            outStr(",\"std\":");
            outNum(n.standard);

            outStr(",\"modes\":");
            outNum(n.phyModes);

            outStr(",\"sec\":");
            outJSON(n.security);

            outStr(",\"akm\":");
            outJSON(n.akm);

            outStr(",\"pair\":");
            outJSON(n.pairwise);

            outStr(",\"group\":");
            outJSON(n.groupCipher);

            outStr(",\"mfpc\":");
            outNum(n.mfpCapable ? 1 : 0);

            outStr(",\"mfpr\":");
            outNum(n.mfpRequired ? 1 : 0);

            outStr(",\"wps\":");
            outNum(n.wps ? 1 : 0);

            outStr(",\"mesh\":");
            outNum(n.mesh ? 1 : 0);

            outStr(",\"vht24\":");
            outNum(n.vht24 ? 1 : 0);

            outStr(",\"resolved\":");
            outNum(n.ssidResolved ? 1 : 0);

            outStr(",\"stations\":");
            outNum(n.stations);

            outStr(",\"util\":");
            outNum(n.channelUtil);

            outStr(",\"beaconint\":");
            outNum(n.beaconInterval);

            outStr(",\"country\":");
            outJSON(n.country);

            outStr(",\"beacons\":");
            outNum(n.beacons);

            outStr(",\"first\":");
            outNum(EPOCH_OF(n.firstSeen));

            outStr(",\"seen\":");
            outNum(EPOCH_OF(n.lastSeen));

            outStr("}");
        }

        outStr("]");
    }

    // ---- stations ----

    if (wantSection(sections, "clients"))
    {
        outStr(",\"clients\":[");

        bool first = true;

        for (int i = 0; i < MAX_CLIENTS; i++)
        {
            ClientEntry &c = clients[i];

            if (!c.valid)
                continue;

            if (since && (uint32_t)EPOCH_OF(c.lastSeen) < since)
                continue;

            if (!first)
                outStr(",");

            first = false;

            outStr("{\"mac\":");
            outMac(c.mac);

            outStr(",\"vendor\":");
            outJSON(lookupVendor(c.mac));

            outStr(",\"bssid\":");
            outMac(c.bssid);

            outStr(",\"apssid\":");
            outJSON(lookupBssName(c.bssid));

            outStr(",\"assoc\":");
            outNum(c.associated ? 1 : 0);

            outStr(",\"ch\":");
            outNum(c.channel);

            outStr(",\"rssi\":");
            outNum(c.rssi);

            outStr(",\"up\":");
            outNum(c.toAp);

            outStr(",\"down\":");
            outNum(c.fromAp);

            outStr(",\"upbytes\":");
            outNum(c.toApBytes);

            outStr(",\"downbytes\":");
            outNum(c.fromApBytes);

            outStr(",\"probes\":");
            outNum(c.probes);

            outStr(",\"assocs\":");
            outNum(c.assocFrames);

            outStr(",\"std\":");
            outNum(c.standard);

            outStr(",\"nss\":");
            outNum(c.streams);

            outStr(",\"width\":");
            outNum(c.maxWidth);

            outStr(",\"roams\":");
            outNum(c.roams);

            outStr(",\"joined\":");
            outJSON(c.assocSSID);

            outStr(",\"akm\":");
            outJSON(c.akm);

            outStr(",\"cipher\":");
            outJSON(c.cipher);

            outStr(",\"rm11k\":");
            outNum(c.rm11k ? 1 : 0);

            outStr(",\"bss11v\":");
            outNum(c.bss11v ? 1 : 0);

            outStr(",\"rrm\":");
            outNum64(c.rrmCaps);

            outStr(",\"wnm\":");
            outNum(c.wnmCaps);

            outStr(",\"opcl\":");
            outOpClasses(c.opClasses);

            outStr(",\"chans\":");
            outNum64(c.suppChans);

            outStr(",\"chans6\":");
            outSixGhzChannels(c.suppChans6);

            outUnknownElements(c.unknownIes, c.unknownSeen, c.unknownCount);

            outMlo(c.mlo);

            outStr(",\"ft11r\":");
            outNum(c.ft11r ? 1 : 0);

            outStr(",\"pmf\":");
            outNum(c.pmf ? 1 : 0);

            outStr(",\"band6\":");
            outNum(c.band6 ? 1 : 0);

            outStr(",\"ssids\":[");

            {
                bool firstSsid = true;

                for (int k = 0; k < MAX_PROBE_SSIDS; k++)
                {
                    if (c.probeSSID[k][0] == 0)
                        continue;

                    if (!firstSsid)
                        outStr(",");

                    firstSsid = false;

                    outJSON(c.probeSSID[k]);
                }
            }

            outStr("]");

            outStr(",\"first\":");
            outNum(EPOCH_OF(c.firstSeen));

            outStr(",\"seen\":");
            outNum(EPOCH_OF(c.lastSeen));

            outStr("}");
        }

        outStr("]");
    }

    // ---- 6 GHz, heard about rather than heard ----

    if (wantSection(sections, "sixghz") && sixGhz != nullptr)
    {
        outStr(",\"sixghz\":[");

        bool first = true;

        for (int i = 0; i < MAX_SIXG_BSS; i++)
        {
            if (!sixGhz[i].valid)
                continue;

            if (since && (uint32_t)EPOCH_OF(sixGhz[i].lastSeen) < since)
                continue;

            if (!first)
                outStr(",");

            first = false;

            SixGhzBss &e = sixGhz[i];

            outStr("{\"bssid\":");
            outMac(e.bssid);

            outStr(",\"ssid\":");
            outJSON(e.ssid);

            outStr(",\"ch\":");
            outNum(e.channel);

            outStr(",\"freq\":");
            outNum(sixGhzFreq(e.channel));

            outStr(",\"bw\":");
            outNum(sixGhzWidth(e.opClass));

            outStr(",\"psd\":");
            outNum(e.psd);

            outStr(",\"reporter\":");
            outMac(e.reporter);

            outStr(",\"seen\":");
            outNum(EPOCH_OF(e.lastSeen));

            outStr("}");
        }

        outStr("]");
    }

    // ---- BLE ----

    if (wantSection(sections, "ble") && bleDevices != nullptr)
    {
        outStr(",\"ble\":[");

        bool first = true;

        for (int i = 0; i < MAX_BLE_DEVICES; i++)
        {
            if (!bleDevices[i].valid)
                continue;

            if (since && (uint32_t)EPOCH_OF(bleDevices[i].lastSeen) < since)
                continue;

            if (!first)
                outStr(",");

            first = false;

            BleDevice &d = bleDevices[i];

            outStr("{\"addr\":");
            outMac(d.addr);

            outStr(",\"name\":");
            outJSON(d.name);

            outStr(",\"rssi\":");
            outNum(d.rssi);

            outStr(",\"company\":");
            outNum(d.company);

            outStr(",\"adverts\":");
            outNum(d.adverts);

            outStr(",\"seen\":");
            outNum(EPOCH_OF(d.lastSeen));

            outStr("}");
        }

        outStr("]");
    }

    // ---- 802.15.4 ----

    if (wantSection(sections, "zb") && zbNodes != nullptr)
    {
        outStr(",\"zb\":{\"channels\":[");

        for (int i = 0; i < ZB_NUM_CHANNELS; i++)
        {
            if (i)
                outStr(",");

            outStr("{\"ch\":");
            outNum(zbChannelNumber(i));

            outStr(",\"ed\":");
            outNum(zbChannels[i].edLevel);

            outStr(",\"frames\":");
            outNum(zbChannels[i].frames);

            outStr("}");
        }

        outStr("],\"nodes\":[");

        bool first = true;

        for (int i = 0; i < ZB_MAX_NODES; i++)
        {
            if (!zbNodes[i].valid)
                continue;

            if (since && (uint32_t)EPOCH_OF(zbNodes[i].lastSeen) < since)
                continue;

            if (!first)
                outStr(",");

            first = false;

            ZbNode &z = zbNodes[i];

            outStr("{\"addr\":");

            {
                char buffer[20];

                if (z.hasExt)
                {
                    snprintf(buffer, sizeof(buffer),
                             "%02X%02X%02X%02X%02X%02X%02X%02X",
                             z.extAddr[0], z.extAddr[1],
                             z.extAddr[2], z.extAddr[3],
                             z.extAddr[4], z.extAddr[5],
                             z.extAddr[6], z.extAddr[7]);
                }
                else
                {
                    snprintf(buffer, sizeof(buffer), "0x%04X",
                             z.shortAddr);
                }

                outJSON(buffer);
            }

            outStr(",\"ext\":");
            outNum(z.hasExt ? 1 : 0);

            outStr(",\"pan\":");
            outNum(z.panId);

            outStr(",\"ch\":");
            outNum(z.channel);

            outStr(",\"lqi\":");
            outNum(z.lqi);

            outStr(",\"frames\":");
            outNum(z.frames);

            outStr(",\"seen\":");
            outNum(EPOCH_OF(z.lastSeen));

            outStr("}");
        }

        outStr("]}");
    }

    // ---- counters ----

    if (wantSection(sections, "counters"))
    {
        outStr(",\"counters\":{\"mgmtdrops\":");
        outNum(mgmtDrops);

        outStr(",\"datadrops\":");
        outNum(dataDrops);

        outStr(",\"fcs\":");
        outNum(fcsErrors);

        outStr(",\"heap\":");
        outNum(ESP.getFreeHeap());

        outStr(",\"heapmin\":");
        outNum(ESP.getMinFreeHeap());

        outStr(",\"psram\":");
        outNum(heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

        outStr(",\"pcap\":");
        outNum(pcapCount);

        outStr(",\"assoc\":");
        outNum(assocCount);

        outStr(",\"beacons\":");
        outNum(countBeaconSlots());

        outStr("}");
    }

    outStr("}");

    outEnd();

    #undef EPOCH_OF
}

// ============================================================
// HTML
// ============================================================

// ============================================================
// HTML
//
// The page lives in index_html.h. Keeping a raw string this
// large in the .ino made the Arduino prototype generator cut
// it in half, so it was moved out of the way.
// ============================================================

#include "index_html.h"
#include "index_html_gz.h"

// The page goes out packed (index_html_gz.h); the unpacked one is the
// source, and the copy has to be of this very page
static_assert(sizeof(INDEX_HTML) == INDEX_HTML_SOURCE_SIZE,
              "index_html.h changed: run tools/make_page_gz.py");


// ============================================================
// HTTP handlers
// ============================================================

void handleFavicon()
{
    lastRequestServed = millis();

    server.send_P(200, "image/svg+xml", FAVICON_SVG);
}

void handleRoot()
{
    lastRequestServed = millis();

    noteWebClient((uint32_t)server.client().remoteIP());

    // Packed, a quarter of the size: every browser unpacks gzip
    server.sendHeader("Content-Encoding", "gzip");
    server.send_P(200, "text/html", (const char *)INDEX_HTML_GZ,
                  sizeof(INDEX_HTML_GZ));
}

void handleNotFound()
{
    reqSend(404, "text/plain", "Not found");
}

// ============================================================
// History allocation
// ============================================================

/*
 * The big tables live in PSRAM.
 *
 * Together they are around 60 kB, which the C5 simply does
 * not have to spare in internal RAM once Wi-Fi, BLE and the
 * Ethernet driver have taken their share. None of them is
 * touched from a real interrupt handler: the Wi-Fi
 * promiscuous callback runs in the Wi-Fi task and the BLE
 * advertising callback in the BLE host task, so PSRAM is
 * safe for both.
 */
void *allocateTable(const char *name, size_t bytes)
{
    void *p = heap_caps_calloc(1, bytes, MALLOC_CAP_SPIRAM);

    const char *where = "PSRAM";

    if (p == nullptr)
    {
        p = heap_caps_calloc(1, bytes, MALLOC_CAP_DEFAULT);
        where = "internal";
    }

    if (p == nullptr)
    {
        Serial.printf("FATAL: %s (%.1f kB) could not be allocated\r\n",
                      name, bytes / 1024.0);
    }
    else
    {
        Serial.printf("  %-14s %8.1f kB in %s\r\n",
                      name, bytes / 1024.0, where);

        noteAllocation(name, bytes, strcmp(where, "PSRAM") == 0);
    }

    return p;
}

void allocateTables()
{
    Serial.println();
    Serial.println("Allocating tables...");

    networks =
        (WiFiNetwork *)allocateTable("networks",
                                     sizeof(WiFiNetwork) * MAX_NETWORKS);

    clients =
        (ClientEntry *)allocateTable("stations",
                                     sizeof(ClientEntry) * MAX_CLIENTS);

    assocHistory =
        (AssocHistory *)allocateTable("assoc history",
                                      sizeof(AssocHistory) *
                                          MAX_ASSOC_HISTORY);

    mgmtQueue =
        (MgmtFrameCopy *)allocateTable("mgmt queue",
                                       sizeof(MgmtFrameCopy) *
                                           MGMT_QUEUE_SIZE);

    dataQueue =
        (DataFrameInfo *)allocateTable("data queue",
                                       sizeof(DataFrameInfo) *
                                           DATA_QUEUE_SIZE);

    bleDevices =
        (BleDevice *)allocateTable("ble devices",
                                   sizeof(BleDevice) * MAX_BLE_DEVICES);

    bleQueue =
        (BleAdvInfo *)allocateTable("ble queue",
                                    sizeof(BleAdvInfo) * BLE_QUEUE_SIZE);

    eventLog =
        (LogEntry *)allocateTable("event log",
                                  sizeof(LogEntry) * LOG_ENTRIES);

    cfgEvents =
        (CfgEvent *)allocateTable("config changes",
                                  sizeof(CfgEvent) * CFG_EVENTS);

    pcapRing =
        (PcapRecord *)allocateTable("pcap ring",
                                    sizeof(PcapRecord) * PCAP_SLOTS);

    beaconSlots =
        (BeaconSlot *)allocateTable("beacon slots",
                                    sizeof(BeaconSlot) *
                                        MAX_BEACON_SLOTS);

    assocRing =
        (AssocRecord *)allocateTable("assoc ring",
                                     sizeof(AssocRecord) * ASSOC_SLOTS);

    pcapIndex =
        (uint16_t *)allocateTable("pcap index",
                                  sizeof(uint16_t) * PCAP_IDX_TOTAL);

    sixGhz =
        (SixGhzBss *)allocateTable("6 GHz BSS",
                                   sizeof(SixGhzBss) * MAX_SIXG_BSS);

    bssNames =
        (BssName *)allocateTable("bss names",
                                 sizeof(BssName) * MAX_BSS_NAMES);

    zbNodes =
        (ZbNode *)allocateTable("802.15.4 nodes",
                                sizeof(ZbNode) * ZB_MAX_NODES);

    edHistory =
        (EdEntry *)allocateTable("energy history",
                                 sizeof(EdEntry) * ED_HISTORY_SIZE);

    if (edHistory != nullptr)
        edCapacity = ED_HISTORY_SIZE;

    if (networks == nullptr || clients == nullptr ||
        mgmtQueue == nullptr || dataQueue == nullptr ||
        bleDevices == nullptr || bleQueue == nullptr ||
        zbNodes == nullptr || edHistory == nullptr ||
        bssNames == nullptr || pcapRing == nullptr ||
        eventLog == nullptr || beaconSlots == nullptr ||
        pcapIndex == nullptr || assocRing == nullptr ||
        sixGhz == nullptr)
    {
        Serial.println("FATAL: table allocation failed, restarting");

        delay(5000);

        ESP.restart();
    }

    Serial.printf("Internal heap after tables: %.1f kB\r\n",
                  ESP.getFreeHeap() / 1024.0);
}

void allocateHistoryBuffer(uint32_t entries, uint32_t caps)
{
    size_t bytes = (size_t)entries * sizeof(WaterfallEntry);

    history = (WaterfallEntry *)heap_caps_malloc(bytes, caps);

    if (history != nullptr)
    {
        historyCapacity = entries;

        noteAllocation("waterfall", bytes, caps == MALLOC_CAP_SPIRAM);
    }
}

void allocateHistory()
{
    Serial.println();
    Serial.println("Allocating waterfall history...");

    Serial.printf("Entry size: %u bytes\r\n",
                  (unsigned)sizeof(WaterfallEntry));


    allocateHistoryBuffer(HISTORY_SIZE, MALLOC_CAP_SPIRAM);

    if (history == nullptr)
    {
        Serial.println("PSRAM allocation failed, using internal heap.");

        allocateHistoryBuffer(HISTORY_SIZE_FALLBACK, MALLOC_CAP_DEFAULT);
    }

    if (history == nullptr)
    {
        Serial.println("History allocation FAILED, waterfall disabled.");
        historyCapacity = 0;
        return;
    }

    for (uint32_t i = 0; i < historyCapacity; i++)
    {
        history[i].timestamp = 0;

        for (uint16_t c = 0; c < TOTAL_CHANNELS; c++)
            history[i].rssi[c] = -127;
    }

    Serial.printf("History: %u entries (%.1f kB)\r\n",
                  (unsigned)historyCapacity,
                  (historyCapacity * (double)sizeof(WaterfallEntry)) /
                      1024.0);

    Serial.printf("Free PSRAM: %.1f kB\r\n", ESP.getFreePsram() / 1024.0);
}

// ============================================================
// WiFi setup
// ============================================================

// Keep acknowledgements and RTS/CTS out of the callback
void applyPromiscuousFilter()
{
#if FILTER_CONTROL_FRAMES
    wifi_promiscuous_filter_t filter;

    filter.filter_mask =
        WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA;

    esp_err_t err = esp_wifi_set_promiscuous_filter(&filter);

    if (err != ESP_OK)
        Serial.printf("Promiscuous filter failed 0x%X\r\n", err);
#endif
}

void setupWiFiScanner()
{
    Serial.println();
    Serial.println("========================================");
    Serial.println(" WLAN SCANNER");
    Serial.println("========================================");

    WiFi.mode(WIFI_STA);

    delay(100);

    wifi_country_t country;

    memset(&country, 0, sizeof(country));

    memcpy(country.cc, "DE", 2);

    country.schan  = 1;
    country.nchan  = 13;
    country.policy = WIFI_COUNTRY_POLICY_AUTO;

    esp_err_t err = esp_wifi_set_country(&country);

    Serial.print("Country DE: ");

    if (err == ESP_OK)
        Serial.println("OK");
    else
        Serial.printf("ERROR 0x%X\r\n", err);

    esp_wifi_set_promiscuous_rx_cb(wifiPromiscuousCallback);

    applyPromiscuousFilter();

    esp_wifi_set_promiscuous(true);

    Serial.printf("Promiscuous Mode: ON (%s)\r\n",
                  FILTER_CONTROL_FRAMES ? "management and data"
                                        : "all frame types");

    /*
     * Everything up to channel 144 is on by default. UNII-3
     * and UNII-4 are in the plan but off, see the note at the
     * channel tables.
     */
    for (uint16_t i = 0; i < TOTAL_CHANNELS; i++)
        channelSelected[i] = (getChannelNumber(i) < 149);

    // Overrides the defaults above if anything was ever saved
    loadSettings();

    currentChannelIndex = 0;

    buildScanPlan();

    switchChannel(currentChannelIndex);

    lastChannelSwitch = millis();
}


// ============================================================
// Snapshots: the state of the moment, kept in flash
// ============================================================

/*
 * A press on the BOOT button, or /api/snapshot?take=1, writes the
 * answers the page reads into one file, so the state of that
 * moment can be looked at later with the same tables and
 * tooltips, from the sensor or from a downloaded copy:
 *
 *   {"wlanscout_snapshot":1,"seq":7,"epoch":..,"uptime":..,
 *    "fw":"..","sensor":"..","sensorid":"..",
 *    "answers":{"/api/status":{..},"/api/networks":{..},
 *               "/api/history?range=120000":{..}, ..}}
 *
 * Each answer is the endpoint's own JSON, keyed by the URL the
 * page asks for. The capture buffers are not included, 3 MB do
 * not fit; the IE catalog's pcap stays where it is.
 *
 * The files are /snap/<seq>_<epoch>_<uptime>.json, so the list
 * needs no file to be opened; epoch is 0 while the clock was not
 * set. The last SNAP_KEEP stay, the oldest goes, sooner when the
 * file system runs short (SNAP_MIN_FREE).
 *
 * BOOT is GPIO 28 on the Waveshare boards, a strapping pin read
 * only at reset: held then it starts the download mode, pressed
 * while running it is an ordinary input with the board's pull-up.
 * The RGB LED on GPIO 27 blinks green twice when the snapshot is
 * written, red when it failed.
 */
#define SNAP_DIR        "/snap"
#define SNAP_KEEP       10
#define SNAP_IMPORT_TMP SNAP_DIR "/import.tmp"   // see "Snapshot import"
#define SNAP_MIN_FREE   (512 * 1024)
#define BOOT_BUTTON     28
#define STATUS_LED      27

void usbParseQuery(const char *query);

struct SnapAnswer
{
    const char *url;
    void (*handler)();
    uint8_t part;       // SNAP_*, 0 = always
};

// What the page reads, with the query arguments it sends
const SnapAnswer snapAnswers[] = {
    { "/api/status",       sendStatusJSON,       0 },
    { "/api/plan",         sendPlanJSON,         0 },
    { "/api/live",         sendLiveJSON,         SNAP_SPECTRUM },
    { "/api/networks",     sendNetworksJSON,     SNAP_NETWORKS },
    { "/api/changes",      sendChangesJSON,      SNAP_NETWORKS },
    { "/api/clients",      sendClientsJSON,      SNAP_STATIONS },
    { "/api/assochistory", sendAssocHistoryJSON, SNAP_ASSOC },
    { "/api/catalog",      sendCatalogJSON,      SNAP_CATALOG },
    { "/api/history?range=120000",   sendHistoryJSON, SNAP_SPECTRUM },
    { "/api/history?range=1800000",  sendHistoryJSON, SNAP_SPECTRUM },
    { "/api/history?range=3600000",  sendHistoryJSON, SNAP_SPECTRUM },
    { "/api/history?range=7200000",  sendHistoryJSON, SNAP_SPECTRUM },
    { "/api/history?range=14400000", sendHistoryJSON, SNAP_SPECTRUM },
    { "/api/history?range=28800000", sendHistoryJSON, SNAP_SPECTRUM },
    { "/api/ble",          sendBleJSON,          SNAP_BLE },
    { "/api/zb",           sendZbJSON,           SNAP_ZB },
    { "/api/zbwaterfall?range=30000",   sendZbWaterfallJSON, SNAP_ZB },
    { "/api/zbwaterfall?range=120000",  sendZbWaterfallJSON, SNAP_ZB },
    { "/api/zbwaterfall?range=600000",  sendZbWaterfallJSON, SNAP_ZB },
    { "/api/zbwaterfall?range=1800000", sendZbWaterfallJSON, SNAP_ZB }
};

// The snapshots on flash, oldest first; returns how many
uint8_t listSnapshots(struct SnapInfo *list, uint8_t capacity)
{
    uint8_t count = 0;

    if (!catalogFsOk)
        return 0;

    File dir = LittleFS.open(SNAP_DIR);

    if (!dir || !dir.isDirectory())
        return 0;

    for (File f = dir.openNextFile(); f; f = dir.openNextFile())
    {
        SnapInfo info;

        const char *name = strrchr(f.name(), '/');

        name = name ? name + 1 : f.name();

        if (sscanf(name, "%" SCNu32 "_%" SCNu32 "_%" SCNu32 ".json",
                   &info.seq, &info.epoch, &info.uptime) != 3)
            continue;

        info.bytes = f.size();

        // Insertion by sequence; past the capacity the newest stay
        if (count < capacity)
            count++;
        else if (info.seq < list[0].seq)
            continue;
        else
        {
            memmove(&list[0], &list[1], (count - 1) * sizeof(SnapInfo));
            list[count - 1].seq = 0;
        }

        int i = count - 1;

        while (i > 0 && list[i - 1].seq > info.seq)
        {
            list[i] = list[i - 1];
            i--;
        }

        list[i] = info;
    }

    return count;
}

String snapshotPath(const struct SnapInfo &info)
{
    return String(SNAP_DIR "/") + info.seq + "_" + info.epoch + "_" +
           info.uptime + ".json";
}

/*
 * A snapshot's name, given later with rename: a file of its own
 * next to it, /snap/<seq>.name, so the snapshot stays as written.
 * The download carries it as "name" at the front.
 */
#define SNAP_NAME_MAX   40

String snapshotNamePath(uint32_t seq)
{
    return String(SNAP_DIR "/") + seq + ".name";
}

String snapshotName(uint32_t seq)
{
    File f = LittleFS.open(snapshotNamePath(seq), "r");

    if (!f)
        return "";

    String name = f.readString();

    f.close();

    return name;
}

/*
 * Name a snapshot: printable text only, cut to SNAP_NAME_MAX bytes
 * on a whole UTF-8 character; an empty name removes it.
 */
void setSnapshotName(uint32_t seq, const String &given)
{
    String name;

    for (size_t i = 0; i < given.length(); i++)
        if ((uint8_t)given[i] >= 0x20 && given[i] != 0x7f)
            name += given[i];

    name.trim();

    while (name.length() > SNAP_NAME_MAX)
    {
        name.remove(name.length() - 1);

        // Continuation bytes of a cut character, then its lead byte
        while (name.length() &&
               ((uint8_t)name[name.length() - 1] & 0xC0) == 0x80)
            name.remove(name.length() - 1);

        if (name.length() && ((uint8_t)name[name.length() - 1] & 0x80))
            name.remove(name.length() - 1);
    }

    String namePath = snapshotNamePath(seq);

    if (name.length() == 0)
    {
        if (LittleFS.exists(namePath))
            LittleFS.remove(namePath);

        return;
    }

    File f = LittleFS.open(namePath, "w");

    if (f)
    {
        f.print(name);
        f.close();
    }
}

void removeSnapshot(const struct SnapInfo &info)
{
    LittleFS.remove(snapshotPath(info));

    String name = snapshotNamePath(info.seq);

    if (LittleFS.exists(name))
        LittleFS.remove(name);
}

// Blink pattern: phases of on/off, driven from loop()
uint8_t  ledRed, ledGreen;
uint8_t  ledPhases = 0;
uint32_t ledNext   = 0;

void ledSignal(bool ok)
{
    ledRed    = ok ? 0 : 60;
    ledGreen  = ok ? 60 : 0;
    ledPhases = ok ? 4 : 2;     // green on-off-on-off, red on-off
    ledNext   = 0;
}

void ledLoop()
{
    if (ledPhases == 0 || (int32_t)(millis() - ledNext) < 0)
        return;

    bool on = ledPhases % 2 == 0;

    // The kit's LED takes red first; the core's default (GRB, as
    // on a WS2812B) turned green into red
    rgbLedWriteOrdered(STATUS_LED, LED_COLOR_ORDER_RGB,
                       on ? ledRed : 0, on ? ledGreen : 0, 0);

    ledNext = millis() + (on ? (ledRed ? 1500 : 250) : 200);
    ledPhases--;
}

/*
 * Write a snapshot. Called from loop() or from a request handler
 * before it answers; the answers go to the file instead of the
 * client, the request's own state is put back afterwards.
 */
/*
 * Room for one more snapshot of about `need` bytes, the oldest going
 * first: fewer than SNAP_KEEP stay, and `need` bytes are free. The
 * list, oldest first, is shortened to match. False when even an empty
 * list leaves too little room.
 */
bool makeSnapshotRoom(struct SnapInfo *list, uint8_t &count, uint32_t need)
{
    if (!LittleFS.exists(SNAP_DIR))
        LittleFS.mkdir(SNAP_DIR);

    while (count > 0 &&
           (count >= SNAP_KEEP ||
            LittleFS.totalBytes() - LittleFS.usedBytes() < need))
    {
        removeSnapshot(list[0]);

        memmove(&list[0], &list[1], (count - 1) * sizeof(SnapInfo));
        count--;
    }

    return LittleFS.totalBytes() - LittleFS.usedBytes() >= need;
}

bool takeSnapshot(struct SnapInfo &taken)
{
    if (!catalogFsOk)
        return false;

    SnapInfo list[SNAP_KEEP + 4];

    uint8_t count = listSnapshots(list, SNAP_KEEP + 4);

    // Room for the new one: the oldest go first
    if (!makeSnapshotRoom(list, count, SNAP_MIN_FREE))
        return false;

    // The device clock, whichever source set it (NTP, the browser,
    // an AP's Time Advertisement); the system time knows only NTP
    uint32_t now = pcapClockSource ? deviceEpoch() : 0;

    taken.seq    = count ? list[count - 1].seq + 1 : 1;
    taken.epoch  = now > 1600000000 ? now : 0;
    taken.uptime = millis() / 1000;
    taken.bytes  = 0;

    String path = snapshotPath(taken);

    File f = LittleFS.open(path, "w");

    if (!f)
        return false;

    // The request being answered, if any, is put back afterwards
    bool    wasUsb     = outToUsb;
    bool    wasChunked = reqChunked;
    uint8_t argCount   = usbArgCount;
    String  argNames[USB_ARGS_MAX];
    String  argValues[USB_ARGS_MAX];

    for (uint8_t i = 0; i < argCount; i++)
    {
        argNames[i]  = usbArgNames[i];
        argValues[i] = usbArgValues[i];
    }

    outToUsb = false;
    outFile  = &f;

    f.printf("{\"wlanscout_snapshot\":1,\"seq\":%" PRIu32
             ",\"epoch\":%" PRIu32 ",\"uptime\":%" PRIu32 ",\"fw\":",
             taken.seq, taken.epoch, taken.uptime);

    outBuf = "";
    outJSON(FW_VERSION);
    outStr(",\"sensor\":");
    outJSON(sensorName);
    outStr(",\"sensorid\":");
    outJSON(sensorMacText().c_str());
    outStr(",\"answers\":{");
    outFlush();

    bool first = true;

    for (const SnapAnswer &a : snapAnswers)
    {
        // Left out by the settings: not in the file at all
        if (a.part && !(snapParts & a.part))
            continue;

        const char *query = strchr(a.url, '?');

        usbParseQuery(query ? query + 1 : "");

        f.print(first ? "\"" : ",\"");
        f.print(a.url);
        f.print("\":");

        size_t before = f.position();

        a.handler();
        outFlush();

        // An endpoint that said nothing still leaves valid JSON
        if (f.position() == before)
            f.print("null");

        first = false;
    }

    f.printf("},\"parts\":%u}", (unsigned)snapParts);

    bool ok = !f.getWriteError();

    taken.bytes = f.position();

    f.close();

    outFile    = nullptr;
    outToUsb   = wasUsb;
    reqChunked = wasChunked;
    usbArgCount = argCount;

    for (uint8_t i = 0; i < argCount; i++)
    {
        usbArgNames[i]  = argNames[i];
        usbArgValues[i] = argValues[i];
    }

    if (!ok)
        LittleFS.remove(path);

    Serial.printf("Snapshot %" PRIu32 ": %s, %" PRIu32 " bytes\n",
                  taken.seq, ok ? "written" : "FAILED", taken.bytes);

    return ok;
}

// The BOOT button, debounced; one snapshot per press
bool     buttonDown    = false;
uint32_t buttonChanged = 0;

void buttonLoop()
{
    bool down = digitalRead(BOOT_BUTTON) == LOW;

    if (down == buttonDown)
    {
        buttonChanged = millis();
        return;
    }

    // The new level has to hold for 40 ms
    if (millis() - buttonChanged < 40)
        return;

    buttonDown = down;

    if (!down)
        return;

    SnapInfo taken;

    bool ok = takeSnapshot(taken);

    ledSignal(ok);

    if (ok)
        logEventFrom("button", "snapshot %" PRIu32 " taken (%" PRIu32 " kB)",
                     taken.seq, taken.bytes / 1024);
    else
        logEventFrom("button", "snapshot failed");
}

void outSnapInfo(const struct SnapInfo &info)
{
    outStr("{\"seq\":");
    outNum(info.seq);
    outStr(",\"epoch\":");
    outNum(info.epoch);
    outStr(",\"uptime\":");
    outNum(info.uptime);
    outStr(",\"bytes\":");
    outNum(info.bytes);
    outStr(",\"name\":");
    outJSON(snapshotName(info.seq).c_str());
    outStr("}");
}

/*
 * /api/snapshot: the list, with
 *   parts=<bits> what a snapshot keeps (SNAP_*), stored
 *   take=1    write one now (as the BOOT button does)
 *   get=<seq> that snapshot as a file
 *   del=<seq> delete it
 *   rename=<seq>&name=<text>  name it, an empty name drops it
 *   clear=1   delete them all, names included
 */
void handleSnapshot()
{
    SnapInfo list[SNAP_KEEP + 4];

    uint8_t count = listSnapshots(list, SNAP_KEEP + 4);

    if (reqHasArg("get") || reqHasArg("del") || reqHasArg("rename"))
    {
        uint32_t seq = (reqHasArg("get") ? reqArg("get")
                        : reqHasArg("del") ? reqArg("del")
                                           : reqArg("rename")).toInt();

        int found = -1;

        for (uint8_t i = 0; i < count; i++)
            if (list[i].seq == seq)
                found = i;

        if (found < 0)
        {
            reqSend(404, "text/plain", "No such snapshot");
            return;
        }

        String path = snapshotPath(list[found]);

        if (reqHasArg("del"))
        {
            removeSnapshot(list[found]);

            logRequest("snapshot %" PRIu32 " deleted", seq);
            reqSend(200, "application/json", "{\"ok\":1}");
            return;
        }

        if (reqHasArg("rename"))
        {
            setSnapshotName(seq, reqArg("name"));

            String now = snapshotName(seq);

            if (now.length())
                logRequest("snapshot %" PRIu32 " named %s", seq, now.c_str());
            else
                logRequest("snapshot %" PRIu32 " name removed", seq);

            reqSend(200, "application/json", "{\"ok\":1}");
            return;
        }

        File f = LittleFS.open(path, "r");

        if (!f)
        {
            reqSend(500, "text/plain", "Cannot read the snapshot");
            return;
        }

        char name[48];

        snprintf(name, sizeof(name), "wlanscout-snapshot-%" PRIu32 ".json",
                 seq);

        lastRequestServed = millis();

        // With a name: {"name":"..", then the file without its "{"
        String head;

        String given = snapshotName(seq);

        if (given.length())
        {
            outBuf = "";
            outStr("{\"name\":");
            outJSON(given.c_str());
            outStr(",");

            head = outBuf;
            outBuf = "";

            f.read();
        }

        reqStart(200, "application/json",
                 f.size() - (head.length() ? 1 : 0) + head.length(), name);

        if (head.length())
            reqContent(head.c_str(), head.length());

        uint8_t buffer[1024];

        for (size_t n; (n = f.read(buffer, sizeof(buffer))) > 0; )
            reqContent(buffer, n);

        reqDone();

        f.close();

        return;
    }

    if (reqHasArg("clear"))
    {
        for (uint8_t i = 0; i < count; i++)
            removeSnapshot(list[i]);

        if (LittleFS.exists(SNAP_IMPORT_TMP))
            LittleFS.remove(SNAP_IMPORT_TMP);

        logRequest("%u snapshots deleted", (unsigned)count);

        count = 0;
    }

    if (reqHasArg("parts"))
    {
        uint8_t parts = (uint8_t)(reqArg("parts").toInt() & SNAP_ALL);

        if (parts != snapParts)
        {
            snapParts = parts;
            saveSettings();
        }
    }

    bool tookOk = false;

    SnapInfo taken;

    if (reqHasArg("take"))
    {
        tookOk = takeSnapshot(taken);

        ledSignal(tookOk);

        if (tookOk)
            logRequest("snapshot %" PRIu32 " taken (%" PRIu32 " kB)",
                       taken.seq, taken.bytes / 1024);
        else
            logRequest("snapshot failed");

        count = listSnapshots(list, SNAP_KEEP + 4);
    }

    outBegin();

    outStr("{\"keep\":");
    outNum(SNAP_KEEP);
    outStr(",\"fs\":");
    outNum(catalogFsOk ? 1 : 0);
    outStr(",\"parts\":");
    outNum(snapParts);

    if (reqHasArg("take"))
    {
        outStr(",\"taken\":");
        outNum(tookOk ? (long)taken.seq : 0);
    }

    outStr(",\"snapshots\":[");

    for (uint8_t i = 0; i < count; i++)
    {
        if (i)
            outStr(",");

        outSnapInfo(list[i]);
    }

    outStr("]}");

    outEnd();
}


// ============================================================
// Snapshot import: a file from the page back onto the flash
// ============================================================

/*
 * A snapshot the page sends back, one at a time, over Ethernet as an
 * upload to /api/snapshotimport, over USB as a PUT of that URL with
 * the bytes following the request line. Arguments: len (bytes),
 * epoch and uptime of the snapshot, name. It is written to a
 * temporary file and only kept when it arrived whole and starts as
 * a snapshot does; it then takes the next number, so an import is
 * the newest in the list, and the oldest make room as for a new one.
 */
#define SNAP_IMPORT_MAX     (2 * 1024 * 1024)
#define SNAP_IMPORT_HEAD    "{\"wlanscout_snapshot\":1"

File     importFile;
SnapInfo importInfo;
uint32_t importLen = 0;
uint32_t importGot = 0;
String   importName;
char     importError[64] = "";

void importFail(const char *text)
{
    if (!importError[0])
        snprintf(importError, sizeof(importError), "%s", text);

    if (importFile)
        importFile.close();

    LittleFS.remove(SNAP_IMPORT_TMP);
}

// From the request's arguments; importError says why not
bool importBegin()
{
    importError[0] = 0;
    importGot      = 0;
    importLen      = reqArg("len").toInt();
    importName     = reqArg("name");

    importInfo.epoch  = reqArg("epoch").toInt();
    importInfo.uptime = reqArg("uptime").toInt();
    importInfo.bytes  = importLen;

    if (!catalogFsOk)
    {
        importFail("no flash file system");
        return false;
    }

    if (importLen < sizeof(SNAP_IMPORT_HEAD) || importLen > SNAP_IMPORT_MAX)
    {
        importFail("not a snapshot (size)");
        return false;
    }

    SnapInfo list[SNAP_KEEP + 4];

    uint8_t count = listSnapshots(list, SNAP_KEEP + 4);

    if (!makeSnapshotRoom(list, count, importLen + SNAP_MIN_FREE / 2))
    {
        importFail("no room on the flash");
        return false;
    }

    importInfo.seq = count ? list[count - 1].seq + 1 : 1;

    importFile = LittleFS.open(SNAP_IMPORT_TMP, "w");

    if (!importFile)
    {
        importFail("cannot write the flash");
        return false;
    }

    return true;
}

void importWrite(const uint8_t *data, size_t length)
{
    if (!importFile)
        return;

    // The start has to be a snapshot's
    for (size_t i = 0; i < length && importGot + i < sizeof(SNAP_IMPORT_HEAD) - 1; i++)
        if (data[i] != (uint8_t)SNAP_IMPORT_HEAD[importGot + i])
        {
            importFail("not a wlanscout+ snapshot");
            return;
        }

    if (importGot + length > importLen ||
        importFile.write(data, length) != length)
    {
        importFail("write failed");
        return;
    }

    importGot += length;
}

// True and the snapshot listed when it came whole
bool importEnd()
{
    if (!importFile)
    {
        importFail("nothing received");
        return false;
    }

    importFile.close();

    if (importGot != importLen)
    {
        importFail("incomplete");
        return false;
    }

    if (!LittleFS.rename(SNAP_IMPORT_TMP, snapshotPath(importInfo)))
    {
        importFail("cannot write the flash");
        return false;
    }

    if (importName.length())
        setSnapshotName(importInfo.seq, importName);

    Serial.printf("Snapshot %" PRIu32 " imported, %" PRIu32 " bytes\n",
                  importInfo.seq, importLen);

    return true;
}

// {"ok":1,"seq":12} or {"ok":0,"error":"..."}
void importAnswer(bool ok)
{
    if (ok)
        logRequest("snapshot %" PRIu32 " imported%s%s (%" PRIu32 " kB)",
                   importInfo.seq, importName.length() ? ": " : "",
                   importName.c_str(), importLen / 1024);
    else
        logRequest("snapshot import refused: %s", importError);

    outBegin();

    if (ok)
    {
        outStr("{\"ok\":1,\"seq\":");
        outNum(importInfo.seq);
        outStr("}");
    }
    else
    {
        outStr("{\"ok\":0,\"error\":");
        outJSON(importError);
        outStr("}");
    }

    outEnd();
}

// Ethernet: a multipart upload, as the firmware update
void handleImportUpload()
{
    HTTPUpload &up = server.upload();

    if (up.status == UPLOAD_FILE_START)
        importBegin();
    else if (up.status == UPLOAD_FILE_WRITE)
        importWrite(up.buf, up.currentSize);
    else if (up.status == UPLOAD_FILE_ABORTED)
        importFail("upload aborted");
}

void handleImportDone()
{
    lastRequestServed = millis();

    importAnswer(!importError[0] && importEnd());
}

// ============================================================
// API routes, for the web server and the USB port
// ============================================================

struct ApiRoute
{
    const char *path;
    void (*handler)();
};

// Every GET endpoint; the firmware upload is a POST and stays on
// Ethernet
const ApiRoute apiRoutes[] = {
    { "/api/status",        sendStatusJSON },
    { "/api/system",        sendSystemJSON },
    { "/api/log",           sendLogJSON },
    { "/api/export",        sendExportJSON },
    { "/api/live",          sendLiveJSON },
    { "/api/history",       sendHistoryJSON },
    { "/api/networks",      sendNetworksJSON },
    { "/api/clients",       sendClientsJSON },
    { "/api/config",        handleConfig },
    { "/api/ble",           sendBleJSON },
    { "/api/zb",            sendZbJSON },
    { "/api/zbwaterfall",   sendZbWaterfallJSON },
    { "/api/pcap",          sendPcap },
    { "/api/pcapclear",     handlePcapClear },
    { "/api/plan",          sendPlanJSON },
    { "/api/assochistory",  sendAssocHistoryJSON },
    { "/api/assocclear",    handleAssocHistoryClear },
    { "/api/flush",         handleFlush },
    { "/api/catalog",       sendCatalogJSON },
    { "/api/catalogclear",  handleCatalogClear },
    { "/api/catalog.pcap",  sendCatalogPcap },
    { "/api/time",          handleTime },
    { "/api/mode",          handleMode },
    { "/api/otarollback",   handleOtaRollback },
    { "/api/coredump",      handleCoreDump },
    { "/api/changes",       sendChangesJSON },
    { "/api/cfgmute",       handleCfgMute },
    { "/api/snapshot",      handleSnapshot }
};

void runRoute(const struct ApiRoute &route)
{
    // For the longest request in the CPU statistics: the web
    // server has forgotten the path once the answer is out
    strlcpy(statsPath, route.path, sizeof(statsPath));

#if HEAP_PROBE
    runProbed(route.path, route.handler);
#else
    route.handler();
#endif
}

// ============================================================
// The USB port: requests in, answers out (the protocol is
// described at "Requests over Ethernet or USB")
// ============================================================

char   usbLine[512];
size_t usbLineLen = 0;
bool   usbLineTooLong = false;

void setupUsbApi()
{
    // Room for one PUT piece and then some, see USB_PUT_PIECE
    usbPort.setRxBufferSize(4096);
    usbPort.setTxBufferSize(4096);
    usbPort.begin();

    // Without a host nothing is read; do not stall on a full buffer
    usbPort.setTxTimeoutMs(200);
}

int hexValue(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;

    return -1;
}

// Query strings come URL encoded, as from a browser
String urlDecode(const char *text, size_t length)
{
    String out;

    for (size_t i = 0; i < length; i++)
    {
        char c = text[i];

        if (c == '+')
        {
            out += ' ';
        }
        else if (c == '%' && i + 2 < length &&
                 hexValue(text[i + 1]) >= 0 && hexValue(text[i + 2]) >= 0)
        {
            out += (char)(hexValue(text[i + 1]) * 16 + hexValue(text[i + 2]));
            i += 2;
        }
        else
        {
            out += c;
        }
    }

    return out;
}

void usbParseQuery(const char *query)
{
    usbArgCount = 0;

    while (*query && usbArgCount < USB_ARGS_MAX)
    {
        const char *end = strchr(query, '&');
        size_t      len = end ? (size_t)(end - query) : strlen(query);
        const char *eq  = (const char *)memchr(query, '=', len);

        if (len > 0)
        {
            size_t nameLen = eq ? (size_t)(eq - query) : len;

            usbArgNames[usbArgCount]  = urlDecode(query, nameLen);
            usbArgValues[usbArgCount] =
                eq ? urlDecode(eq + 1, len - nameLen - 1) : String("");

            usbArgCount++;
        }

        if (!end)
            break;

        query = end + 1;
    }
}

/*
 * A file coming in over USB: "PUT /api/snapshotimport?len=N&..." for
 * a snapshot, "PUT /api/ota?len=N" for a firmware, then N bytes,
 * read by usbApiLoop() before any further request line. A stall of
 * USB_PUT_TIMEOUT_MS gives up.
 */
#define USB_PUT_TIMEOUT_MS  10000

/*
 * USB has no flow control here: once the receive queue is full, the
 * core drops what comes on (HWCDC.cpp), and writing the flash holds
 * up the loop for milliseconds while the host sends at 1 MB/s. So
 * the page sends the bytes in pieces of USB_PUT_PIECE and waits for
 * USB_PUT_ACK after each but the last, which the sensor sends once
 * it has written the piece away. The receive queue holds two.
 */
#define USB_PUT_PIECE       2048
#define USB_PUT_ACK         0x06

enum UsbPutKind { USB_PUT_IMPORT, USB_PUT_OTA };

uint32_t   usbPutLeft = 0;
uint32_t   usbPutLast = 0;
uint32_t   usbPutPiece = 0;          // bytes of the piece read so far
UsbPutKind usbPutKind = USB_PUT_IMPORT;

void usbPutFinish()
{
    outToUsb = true;

    if (usbPutKind == USB_PUT_OTA)
    {
        otaFinish();
        handleOtaDone();
    }
    else
    {
        importAnswer(!importError[0] && importEnd());
    }

    outToUsb    = false;
    usbArgCount = 0;
}

void usbPutWrite(const uint8_t *data, size_t length)
{
    if (usbPutKind == USB_PUT_OTA)
        otaWrite(data, length);
    else
        importWrite(data, length);
}

void usbPutStalled()
{
    if (usbPutKind == USB_PUT_OTA)
        otaFail("USB stalled");
    else
        importFail("USB stalled");
}

void usbApiRequest(char *line)
{
    outToUsb = true;

    if (strncmp(line, "PUT /api/ota", 12) == 0 &&
        (line[12] == '?' || line[12] == 0))
    {
        usbParseQuery(line[12] == '?' ? line + 13 : "");

        lastRequestServed = millis();

        const esp_partition_t *slot = esp_ota_get_next_update_partition(NULL);
        uint32_t length = reqArg("len").toInt();

        usbPutKind  = USB_PUT_OTA;
        usbPutPiece = 0;

        // As for an import, the bytes are read in any case and an
        // update that could not start answers with its error
        otaStart();

        if (slot != NULL && length > slot->size)
            otaFail("larger than a firmware slot");

        usbPutLeft = length;
        usbPutLast = millis();

        if (usbPutLeft == 0)
        {
            otaFail("no file received");
            usbPutFinish();
        }

        outToUsb = false;

        return;
    }

    if (strncmp(line, "PUT /api/snapshotimport", 23) == 0 &&
        (line[23] == '?' || line[23] == 0))
    {
        usbParseQuery(line[23] == '?' ? line + 24 : "");

        lastRequestServed = millis();

        usbPutKind  = USB_PUT_IMPORT;
        usbPutPiece = 0;

        // The bytes follow in any case; when the start failed they
        // are read and dropped, then the error is the answer
        importBegin();

        usbPutLeft = importLen;
        usbPutLast = millis();

        if (usbPutLeft == 0 || usbPutLeft > SNAP_IMPORT_MAX)
        {
            usbPutLeft = 0;
            usbPutFinish();
        }

        outToUsb = false;

        return;
    }

    if (strncmp(line, "GET ", 4) != 0)
    {
        reqSend(405, "text/plain", "GET only");
        outToUsb = false;

        return;
    }

    char *path  = line + 4;
    char *query = strchr(path, '?');

    if (query)
        *query++ = 0;

    usbParseQuery(query ? query : "");

    const ApiRoute *route = nullptr;

    for (const ApiRoute &r : apiRoutes)
        if (strcmp(path, r.path) == 0)
            route = &r;

    if (route)
    {
        lastRequestServed = millis();
        runRoute(*route);
    }
    else
    {
        reqSend(404, "text/plain", "Not found");
    }

    outToUsb    = false;
    usbArgCount = 0;
}

void usbApiLoop()
{
    // The body of a PUT comes first
    while (usbPutLeft > 0)
    {
        int avail = usbPort.available();

        if (avail <= 0)
        {
            if (millis() - usbPutLast > USB_PUT_TIMEOUT_MS)
            {
                usbPutLeft = 0;
                usbPutStalled();
                usbPutFinish();
            }

            return;
        }

        // A firmware's first piece has to hold its 16-byte header
        if (usbPutKind == USB_PUT_OTA && otaRunning && otaWritten == 0 &&
            (uint32_t)avail < min((uint32_t)16, usbPutLeft) &&
            millis() - usbPutLast <= USB_PUT_TIMEOUT_MS)
            return;

        uint8_t buffer[512];

        size_t n = usbPort.read(buffer,
            min((size_t)avail, min(sizeof(buffer), (size_t)usbPutLeft)));

        usbPutWrite(buffer, n);

        usbPutLeft -= n;
        usbPutLast  = millis();

        // A whole piece written away: the page may send the next one
        usbPutPiece += n;

        if (usbPutPiece >= USB_PUT_PIECE && usbPutLeft > 0)
        {
            usbPutPiece -= USB_PUT_PIECE;
            usbPort.write((uint8_t)USB_PUT_ACK);
            usbPort.flush();
        }

        if (usbPutLeft == 0)
            usbPutFinish();
    }

    while (usbPort.available())
    {
        int c = usbPort.read();

        if (c == '\r')
            continue;

        if (c == '\n')
        {
            usbLine[usbLineLen] = 0;

            if (usbLineLen && !usbLineTooLong)
                usbApiRequest(usbLine);

            usbLineLen     = 0;
            usbLineTooLong = false;

            /*
             * A PUT: the bytes right behind its line are the file,
             * usually in the same transfer. Read on here and they
             * were taken for request lines, the file arriving
             * without its start ("not a wlanscout+ snapshot")
             */
            if (usbPutLeft > 0)
                return;
        }
        else if (usbLineLen < sizeof(usbLine) - 1)
        {
            usbLine[usbLineLen++] = (char)c;
        }
        else
        {
            usbLineTooLong = true;
        }
    }
}

// ============================================================
// SETUP
// ============================================================

void setup()
{
    Serial.begin(115200);

    delay(1000);

    Serial.println();
    Serial.println("========================================");
    Serial.print(" ESP32-C5 wlanscout+  v");
    Serial.println(FW_VERSION);
    Serial.println("========================================");

    Serial.printf("Heap:       %.1f kB\r\n", ESP.getFreeHeap() / 1024.0);
    Serial.printf("PSRAM:      %.1f kB\r\n", ESP.getPsramSize() / 1024.0);
    Serial.printf("Free PSRAM: %.1f kB\r\n", ESP.getFreePsram() / 1024.0);

    allocateTables();

    allocateHistory();

    catalogMount();

    logLoad();

    statsSetup();

    cfgMuteLoad();

    otaInit();

    // --------------------------------------------------------
    // W5500 reset
    // --------------------------------------------------------

    Serial.println();
    Serial.println("W5500 reset...");

    pinMode(W5500_RST, OUTPUT);

    digitalWrite(W5500_RST, LOW);
    delay(10);

    digitalWrite(W5500_RST, HIGH);
    delay(100);

    // --------------------------------------------------------
    // SPI
    // --------------------------------------------------------

    Serial.println("Initialising SPI...");

    SPI.begin(W5500_SCLK, W5500_MISO, W5500_MOSI, W5500_CS);

    // --------------------------------------------------------
    // Ethernet
    // --------------------------------------------------------

    loadSensorName();

    Network.onEvent(onEvent);

    setupEthernet();

    setupWiFiScanner();

    // --------------------------------------------------------
    // Webserver
    // --------------------------------------------------------

    server.on("/", HTTP_GET, PROBED(handleRoot));
    server.on("/favicon.svg", HTTP_GET, PROBED(handleFavicon));
    for (const ApiRoute &route : apiRoutes)
        server.on(route.path, HTTP_GET, [&route]() { runRoute(route); });

    server.on("/api/ota", HTTP_POST, handleOtaDone, handleOtaUpload);
    server.on("/api/snapshotimport", HTTP_POST, handleImportDone,
              handleImportUpload);

    server.onNotFound(PROBED(handleNotFound));

    server.begin();

    Serial.println("HTTP server started");

    setupUsbApi();

    // BOOT as a button for snapshots, see "Snapshots"
    pinMode(BOOT_BUTTON, INPUT_PULLUP);
    buttonDown = digitalRead(BOOT_BUTTON) == LOW;

    startHeapProbe();

    // The id without a W5500 has to match the one with it
    {
        uint8_t eth[6];
        uint8_t mac[6];

        esp_read_mac(eth, ESP_MAC_ETH);
        esp_derive_local_mac(mac, eth);

        char derived[18];

        snprintf(derived, sizeof(derived),
                 "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

        if (ethPresent && ETH.macAddress() != derived)
            Serial.printf("Sensor id: derived %s, W5500 has %s\r\n",
                          derived, ETH.macAddress().c_str());
    }

    Serial.printf("Sensor id %s%s%s\r\n",
                  sensorMacText().c_str(),
                  sensorName[0] ? ", name " : "",
                  sensorName);

    logEventFrom("system", "device started, firmware v%s, %s",
                 FW_VERSION, resetReasonText());

    coreDumpSetup();

    otaLogRollback();

    catalogLogPrune();

    Serial.println();
    Serial.println("========================================");
    Serial.println(" ANALYZER RUNNING");
    Serial.println("========================================");
}

// ============================================================
// LOOP
// ============================================================

void loop()
{
    statsLoopRound();

    char path[48];

    statsPath[0] = 0;

    int64_t t0 = esp_timer_get_time();

    server.handleClient();

    uint32_t httpUs = (uint32_t)(esp_timer_get_time() - t0);

    // A round without a request takes a few µs; the page itself,
    // the favicon and uploads are no API route
    if (httpUs > 1000)
        statsRequest(httpUs, statsPath[0] ? statsPath : "page or upload");

    statsPath[0] = 0;

    t0 = esp_timer_get_time();

    usbApiLoop();

    uint32_t usbUs = (uint32_t)(esp_timer_get_time() - t0);

    if (usbUs > 1000)
    {
        snprintf(path, sizeof(path), "USB %s",
                 statsPath[0] ? statsPath : "page or upload");
        statsRequest(usbUs, path);
    }

    statsLoop();

    buttonLoop();
    ledLoop();

    catalogLoop();

    logClockLoop();

    otaLoop();

    applyPendingMode();

    if (scanMode == MODE_WIFI)
    {
        processManagementQueue();
        processDataQueue();

        scannerLoop();
    }
    else if (scanMode == MODE_ZB)
    {
        zbLoop();

        // Safety net: if the device stops answering in an
        // exclusive mode it is useless, so go back to Wi-Fi.
        if (lastRequestServed != 0 &&
            millis() - lastRequestServed > 10000)
        {
            Serial.println(
                "No HTTP request served for 10 s in 802.15.4 "
                "mode, returning to Wi-Fi");

            pendingMode = MODE_WIFI;

            lastRequestServed = millis();
        }

        // If no sweep completes for a while the energy path is
        // stuck somewhere in the driver. Say so and fall back
        // instead of silently showing an empty waterfall.
        if (zbScanMode == ZB_SCAN_ENERGY &&
            millis() - edWatchSince > 10000)
        {
            if (edSweeps == edWatchSweeps)
            {
                Serial.println(
                    "802.15.4: no energy sweep completed in 10 s, "
                    "falling back to frame capture");

                strncpy(zbNote,
                        "energy sweep produced nothing, "
                        "fell back to frame capture",
                        sizeof(zbNote) - 1);

                zbScanMode = ZB_SCAN_FRAMES;

                zbSelectChannel(0);
            }

            armEdWatch();
        }
    }
    else
    {
        processBleQueue();

        // Re-arm before the scan window expires, and also if
        // nothing has been heard for a while.
        if (bleScan != nullptr &&
            (millis() - bleScanStarted >
                 (BLE_SCAN_DURATION_S - 2) * 1000UL ||
             millis() - bleLastAdvert > 20000))
        {
            stopBleScan();
            startBleScan();
        }
    }

    static uint32_t lastCleanup = 0;

    if (millis() - lastCleanup > 5000)
    {
        lastCleanup = millis();

        cleanupNetworks();
        cleanupClients();
        cleanupBleDevices();
        cleanupZbNodes();
    }

    /*
     * Network time.
     *
     * Started once an address is up, then checked regularly:
     * a later step of the system clock shifts every timestamp
     * in the capture by the same amount, which is exactly
     * what should happen.
     */
    if (ethGotIP && !ntpStarted)
    {
        PROBE_MARK("ntp sync");

        configTime(0, 0, NTP_SERVER1, NTP_SERVER2);

#if HAVE_ESP_SNTP
        esp_sntp_set_sync_interval(NTP_SYNC_INTERVAL_MS);
#endif

        ntpStarted = true;

        Serial.printf("NTP: asking %s, re-sync every %lu min\r\n",
                      NTP_SERVER1,
                      (unsigned long)(NTP_SYNC_INTERVAL_MS / 60000));
    }

    if (ntpStarted && millis() - lastNtpCheck > NTP_CHECK_INTERVAL_MS)
    {
        lastNtpCheck = millis();

        struct timeval tv;

        gettimeofday(&tv, nullptr);

        // Anything before 2023 means the clock is still unset
        if (tv.tv_sec > 1700000000)
        {
            int64_t nowUs =
                (int64_t)tv.tv_sec * 1000000LL + tv.tv_usec;

            int64_t offset = nowUs - esp_timer_get_time();

            int64_t delta = offset - clockOffsetUs;

            if (delta < 0)
                delta = -delta;

            /*
             * Only a real correction counts. Below this the
             * difference is measurement noise between the two
             * clocks, not something NTP did.
             */
            if (pcapClockSource != 2 || delta > 50000)
            {
                // Against NTP, or against the AP the clock came from
                ntpLastAdjust =
                    (pcapClockSource == 2 || pcapClockSource == 3)
                        ? (int32_t)((offset - clockOffsetUs) / 1000LL)
                        : 0;

                clockOffsetUs = offset;

                ntpLastSync = (uint32_t)tv.tv_sec;
                ntpSyncCount++;

                char stamp[24];
                char tzName[12];

                formatStamp((uint32_t)tv.tv_sec, stamp, sizeof(stamp));
                formatTzLabel(tzName, sizeof(tzName));

                Serial.printf(
                    "NTP: clock set to %s %s"
                    "%s\r\n",
                    stamp, tzName,
                    (pcapClockSource == 2) ? " (adjusted)" : "");

                if (ntpSyncCount == 1)
                    logEventFrom("system", "clock synced via NTP");

                pcapClockSource = 2;
            }
        }
    }

    /*
     * Link diagnostics.
     *
     * The event handler only reports changes, so a link that
     * never comes up produces no output at all. Ask the PHY
     * directly instead: if it reports no link while a cable is
     * plugged in, the problem is below IP, and the other end
     * is the place to look.
     */
    static uint32_t lastLinkLog = 0;

    if (ethPresent && !ethLinkUp && millis() - lastLinkLog > 5000)
    {
        lastLinkLog = millis();

        Serial.printf("ETH: no link (phy reports %s, %d Mbps, %s)\r\n",
                      ETH.linkUp() ? "up" : "down",
                      (int)ETH.linkSpeed(),
                      ETH.fullDuplex() ? "full duplex" : "half duplex");
    }

    /*
     * A link change always returns to DHCP.
     *
     * Without this the device stayed on the static address
     * forever once it had fallen back, so unplugging and
     * replugging the cable never brought a lease back.
     */
    if (ethLinkChanged)
    {
        ethLinkChanged = false;

        if (ethLinkUp)
        {
            if (ethStaticActive)
            {
                Serial.println("ETH: link change, back to DHCP");

                logEventFrom("system",
                             "link change, leaving the static "
                             "address and trying DHCP again");

                ethStaticActive = false;
                ethGotIP        = false;
            }

            // A new lease, not the end of the old one
            dhcpLease.state = DHCP_STATE_OFF;

            restartDhcpClient("link up");

            Serial.printf("ETH: waiting for a lease, dhcp client %s\r\n",
                          dhcpStatusText());

            dhcpAttempts     = 0;
            dhcpAttemptStart = millis();
        }
    }

    /*
     * DHCP watchdog.
     *
     * While the link is up but no lease has arrived, count the
     * attempts and fall back to a static address once the
     * configured number is reached. The DHCP client is only
     * re-armed on every other attempt: restarting it every
     * time can cut off a negotiation that is still running.
     */
    if (ethLinkUp && !ethGotIP && !ethStaticActive &&
        dhcpAttemptStart != 0 &&
        millis() - dhcpAttemptStart > DHCP_TIMEOUT_MS)
    {
        dhcpAttempts++;

        Serial.printf("ETH: no DHCP lease, attempt %u of %u "
                      "(link %s, dhcp client %s, address %s)\r\n",
                      (unsigned)dhcpAttempts,
                      (unsigned)DHCP_MAX_ATTEMPTS,
                      ethLinkUp ? "up" : "down",
                      dhcpStatusText(),
                      ETH.localIP().toString().c_str());

        /*
         * Reinitialise early. A wedged transmit path cannot be
         * waited out, and a reinit recovers it in about a
         * second. The cooldown and the counter keep it from
         * turning into a loop when the cable really is out.
         */
        if (dhcpAttempts >= DHCP_REINIT_AFTER &&
            reinitCount < DHCP_REINIT_MAX &&
            (lastReinit == 0 ||
             millis() - lastReinit > DHCP_REINIT_COOLDOWN_MS))
        {
            reinitCount++;
            lastReinit = millis();

        PROBE_MARK("eth reinit");
            Serial.printf("ETH: reinitialising the interface "
                          "(attempt %u)\r\n",
                          (unsigned)reinitCount);

            logEventFrom("system",
                         "no lease after %u attempts, "
                         "reinitialising the Ethernet interface",
                         (unsigned)dhcpAttempts);

            ETH.end();

            delay(200);

            setupEthernet();

            dhcpLease.state = DHCP_STATE_OFF;

            dhcpAttempts     = 0;
            dhcpAttemptStart = millis();

            return;
        }

        if (dhcpAttempts >= DHCP_MAX_ATTEMPTS)
        {
            ethStaticActive = true;
            ethStaticSince  = millis();

            bool ok = ETH.config(ETH_STATIC_IP,
                                 ETH_STATIC_GW,
                                 ETH_STATIC_MASK,
                                 ETH_STATIC_DNS);

            logEventFrom("system",
                         "no DHCP lease after %u attempts, "
                         "falling back to a static address",
                         (unsigned)DHCP_MAX_ATTEMPTS);

            Serial.print("ETH: falling back to static ");
            Serial.print(ETH_STATIC_IP);
            Serial.println(ok ? " OK" : " FAILED");

            Serial.print("Web UI: http://");
            Serial.println(ETH_STATIC_IP);
        }
        else if (dhcpAttempts % 2 == 0)
        {
            restartDhcpClient("retry");
        }

        dhcpAttemptStart = millis();
    }

    dhcpLeaseWatch();

#if DHCP_RETRY_AFTER_STATIC_MS > 0
    if (ethStaticActive && ethLinkUp &&
        millis() - ethStaticSince > DHCP_RETRY_AFTER_STATIC_MS)
    {
        Serial.println("ETH: trying DHCP again");

        logEventFrom("system", "retrying DHCP from the static address");

        ethStaticActive = false;
        ethGotIP        = false;
        ethStaticSince  = millis();

        restartDhcpClient("periodic retry");

        dhcpAttempts     = 0;
        dhcpAttemptStart = millis();
    }
#endif

    // Heap diagnostics: if the largest free block collapses
    // the Ethernet driver runs out of buffers.
    static uint32_t lastHeapLog = 0;

    if (millis() - lastHeapLog > 15000)
    {
        lastHeapLog = millis();

        /*
         * The largest free block has to be asked for with
         * MALLOC_CAP_INTERNAL. With MALLOC_CAP_DEFAULT the
         * answer came from PSRAM and was always around 7 MB,
         * which says nothing about the memory that actually
         * runs out.
         */
        Serial.printf(
            "[heap] free %.1f kB  min %.1f kB  maxblock %.1f kB  "
            "psram %.1f kB  "
            "drops mgmt %u data %u fcs %u  eth %s  mode %s\r\n",
            ESP.getFreeHeap() / 1024.0,
            ESP.getMinFreeHeap() / 1024.0,
            heap_caps_get_largest_free_block(
                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024.0,
            heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024.0,
            (unsigned)mgmtDrops,
            (unsigned)dataDrops,
            (unsigned)fcsErrors,
            ethStatusText(),
            scanMode == MODE_BLE ? "ble"
                                 : (scanMode == MODE_ZB ? "802.15.4"
                                                        : "wifi"));

#if HEAP_PROBE
        if (probeLow.fresh)
        {
            probeLow.fresh = false;

            uint32_t sec = probeLow.atMillis / 1000;

            Serial.printf(
                "[probe] NEW LOW %.1f kB  (free now %.1f, dma %.1f)  "
                "stack %u words  label '%s'  idle %lu ms  last '%s' from %s  "
                "at %02u:%02u:%02u\r\n",
                probeLow.watermark / 1024.0,
                probeLow.freeNow / 1024.0,
                probeLow.freeDma / 1024.0,
                (unsigned)probeLow.stackWords,
                probeLow.label,
                (unsigned long)probeLow.idleForMs,
                probeLow.last,
                probeLow.who[0] ? probeLow.who : "-",
                (unsigned)(sec / 3600),
                (unsigned)((sec / 60) % 60),
                (unsigned)(sec % 60));
        }
#endif
    }

    delay(1);
}
