// SPDX-License-Identifier: GPL-2.0-only WITH AdditionRef-wlanscout-linking-exception
// SPDX-FileCopyrightText: 2026 Dr.-Ing. Simon Wunderlich Systementwicklung & Beratung
// SPDX-FileContributor: Jan Fuchs <jf [at] simonwunderlich . de>

#include "config.h"

#include <algorithm>
#include <LittleFS.h>
#if ENABLE_OTA
#include <Update.h>
#endif
#include <esp_ota_ops.h>
#include <Arduino.h>
#include <WiFi.h>
#if ENABLE_ETHERNET
#include <WebServer.h>
#include <SPI.h>
#include <ETH.h>
#include <lwip/sockets.h>
#include <esp_netif_net_stack.h>
#include <lwip/dhcp.h>
#include <lwip/prot/dhcp.h>
#include <lwip/tcpip.h>
#include <lwip/etharp.h>
#endif
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_core_dump.h>
#include <esp_partition.h>
#include <Preferences.h>
#include <esp_wifi.h>
#include <esp_heap_caps.h>
#include <driver/temperature_sensor.h>
#include <time.h>
#include <sys/time.h>

#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

// IEEE 802.15.4, see ENABLE_IEEE802154 in config.h
#if ENABLE_IEEE802154 && __has_include("esp_ieee802154.h")
#include "esp_ieee802154.h"
#define HAVE_IEEE802154 1
#else
#define HAVE_IEEE802154 0
#endif

/*
 * OUI vendor table.
 *
 * oui_table.h is generated from the IEEE registries (MA-L,
 * MA-M, MA-S, IAB) by tools/make_oui.py and must sit next to
 * this sketch. It needs a large app partition, which the
 * sketch's own partitions.csv provides.
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
extern "C" bool verifyRollbackLater();
void saveSettings();
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
    uint8_t  btwtCount;          // broadcast TWT schedules
    uint32_t btwtInterval;       //   the first one's interval, µs
    uint32_t btwtDuration;       //   and its wake duration
    bool     twtRequired;        // HE Operation: TWT required
    uint16_t mlRemoving;         // links announced for removal
    uint16_t ehtWidth;           // EHT Operation, see ehtOperationOf()
    uint8_t  ehtCenter;
    uint16_t punctured;          // 20 MHz subchannels left out
    bool     mde;                // Mobility Domain element sent
    uint16_t mdid;               // its ID
    bool     ftOverDs;           // FT over the DS
};

/*
 * A BSS whose course is recorded while its tooltip shows the Graphs
 * tab, see handleWatch(): one sample per WATCH_STEP_MS in a ring in
 * PSRAM, nothing for any other BSS.
 */
#define WATCH_MAX       4
#define WATCH_STEP_MS   5000
#define WATCH_SAMPLES   17280          // 24 h of 5 s
// Dropped without a renewal; long enough for a background tab, which
// browsers let poll about once a minute
#define WATCH_IDLE_MS   180000

struct WatchSample
{
    int8_t   rssiMin, rssiAvg, rssiMax;  // 0 nothing heard
    uint8_t  util;                       // BSS Load %, 255 none
    uint16_t stations;                   // BSS Load, 0xFFFF none
    uint8_t  seen;                       // stations the sensor sees
    uint8_t  heard;                      // beacons in the step
};

struct BssWatch
{
    bool         used;
    bool         keep;                   // recorded without a viewer,
                                         // kept in NVS across restarts
    uint8_t      bssid[6];
    uint32_t     renewed;                // millis()
    uint32_t     stepStart;              // millis() of the open step
    uint32_t     next;                   // samples written so far
    uint32_t     startEpoch;             // of sample 0, 0 no clock
    uint32_t     startUp;                // seconds since boot, sample 0
    WatchSample *ring;

    // The open step
    int8_t       rMin, rMax;
    int32_t      rSum;
    uint16_t     rCount;
    int16_t      util, stations;
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
    CFG_MESH_AUTH, CFG_MDID, CFG_FT_DS, CFG_PUNCT, CFG_ML_REMOVE,
    CFG_TWT, CFG_TWT_REQUIRED
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
 * For looking into heap dips: control frames and frames that failed
 * the FCS reach the callback as well, which counts them for the heap
 * probe and drops them at once, so a snapshot shows everything the
 * driver had to deal with. 0 for normal use.
 */
#ifndef HEAP_PROBE_RX_ALL
#define HEAP_PROBE_RX_ALL 0
#endif

/*
 * Receive buffers the WiFi driver may hold at once, each about 2.3 kB
 * of internal RAM. Arduino starts the driver with 32, and under bursts
 * of traffic the driver fills all of them before the callback sees a
 * frame, up to 74 kB, whatever the filter says. 16 caps that dip at
 * about 37 kB; frames coming faster than that are dropped by the
 * driver.
 */
#ifndef WIFI_RX_BUFFERS
#define WIFI_RX_BUFFERS 16
#endif

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
 * An NTP server handed out by DHCP (option 42) is asked first,
 * the two public servers below stay behind it as fallback.
 * Without option 42 the public servers are all there is.
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

#if ENABLE_ETHERNET && __has_include("esp_sntp.h")
#include "esp_sntp.h"
#define HAVE_ESP_SNTP 1
#else
#define HAVE_ESP_SNTP 0
#endif

#if HAVE_ESP_SNTP && defined(CONFIG_LWIP_DHCP_GET_NTP_SRV)
#define HAVE_DHCP_NTP 1
#else
#define HAVE_DHCP_NTP 0
#endif

// ============================================================
// Scanner configuration
// ============================================================

#define CHANNEL_DWELL_MS 120

/*
 * Waterfall history, in two parts.
 *
 * One row per sweep was all there was, 12000 of them, and how far
 * back that reached depended on the channel plan: 16 h with all 42
 * channels, 72 minutes with three, so the long ranges came up mostly
 * empty. Now the rows per sweep only serve the short ranges, and
 * the long ones come from one row per HISTORY_STEP_MS, the mean of
 * the sweeps in it (a sweep longer than the step makes one row per
 * sweep). That reaches back the same time whatever the plan.
 *
 * The rows per step are sized at boot from the PSRAM there is: up
 * to HISTORY_SPAN_MS, at least HISTORY_SPAN_MIN_MS, leaving
 * HISTORY_LEAVE for the frame logs to grow into.
 */
#define HISTORY_FINE_ROWS    2400
#define HISTORY_STEP_MS      5000
#define HISTORY_SPAN_MS      (24UL * 3600 * 1000)
#define HISTORY_SPAN_MIN_MS  (8UL * 3600 * 1000)
#define HISTORY_LEAVE        (4UL * 1024 * 1024)

// Rows per sweep without PSRAM, and none per step
#define HISTORY_SIZE_FALLBACK 600

// Maximum number of detected WLAN networks
#define MAX_NETWORKS 250

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
 * Capture
 *
 * Management frames are kept as they came off the air so they
 * can be downloaded and opened in Wireshark: in the frame logs
 * (see FRAME_CHUNK_BYTES) at their own length, and the newest
 * beacon of each BSSID in a slot of MGMT_FRAME_MAX octets.
 */
#define MGMT_QUEUE_SIZE 96

/*
 * 512 octets cut off the end of most Wi-Fi 7 beacons, whose Reduced
 * Neighbor Report and Multi-Link elements come last. 1536 holds
 * nearly all of them; the longest are still kept whole among the
 * beacon versions (BV_CHUNKS_MAX).
 */
#define MGMT_FRAME_MAX  1536

/*
 * The parser gets the whole frame, though, up to the largest
 * MMPDU the standard allows. 512 octets was the limit for both
 * before, and a Wi-Fi 7 association request is longer than that:
 * its own EHT Capabilities sit behind a long Multi-Link element
 * and were cut off, so such a station passed for Wi-Fi 6. The
 * frame logs keep full frames as well. Only the beacon slots stay
 * at MGMT_FRAME_MAX and record the on-air length separately.
 */
#define MGMT_PARSE_MAX  2304

/*
 * Radiotap header put in front of every exported frame, see
 * pcapBuildRadiotap(). Firmware before 2.8.27 wrote 15 octets (no
 * rate, no noise), and the catalog's pcap file on flash still holds
 * such frames: whoever reads frames back takes the length from each
 * frame's own header, radiotapLenOf(), never this.
 */
#define RADIOTAP_LEN 16

/*
 * The receive rate as the driver reports it, kept per frame for the
 * radiotap Rate field. For 802.11b frames rx_ctrl.rate is a
 * wifi_phy_rate_t (0-3 long, 5-7 short preamble), for 802.11a/g
 * frames the L-SIG rate code (8-15), which wifi_phy_rate_t mirrors.
 * Anything else, HT and later, has no single legacy rate.
 */
#define RX_RATE_UNKNOWN 0xFF

/*
 * The capture ring and the association ring are frame logs: frames
 * back to back at their own length, in chunks of PSRAM that are
 * added while the log fills, see "Frame logs". A slot of fixed size
 * held 528 octets for a probe request of 150; the same memory now
 * holds about three times as many frames, and a quiet site takes
 * only what it needs.
 *
 * A log grows only while PSRAM keeps its reserve free for the rest:
 * the graphs of watched BSSs (138 kB each), JSON answers, the
 * core's own buffers. The ring stops earlier than the association
 * ring, which is rare and more valuable; when a log cannot grow it
 * drops its oldest chunk and reuses it.
 */
#define FRAME_CHUNK_BYTES   (64 * 1024)
#define FRAME_CHUNKS_MAX    80            // 5 MB, the table is in DRAM

#define RING_CHUNKS_MAX     80            // capture ring, up to 5 MB
#define RING_RESERVE        (1024 * 1024) // PSRAM it leaves free

#define ASSOC_CHUNKS_MAX    24            // association ring, 1.5 MB
#define ASSOC_RESERVE       (512 * 1024)

/*
 * Beacon versions, see "Beacon versions": the first beacon of each
 * BSS and each one whose content changed for good, whole, in a log
 * of their own. Rare, so it may grow while the association ring's
 * reserve stays free.
 */
#define BV_CHUNKS_MAX       16            // up to 1 MB
#define BV_RESERVE          (512 * 1024)
#define BV_KEEP             8             // versions per BSS
#define BV_ELEMS            32            // elements compared per beacon
#define BV_VOLATILE         8             // elements learned as volatile
#define BV_VOLATILE_CHANGES 4             // changes within BV_VOLATILE_MS
#define BV_VOLATILE_MS      60000         //   that make an element volatile

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
#define MAX_BEACON_SLOTS MAX_NETWORKS     // one per BSSID

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
 * practice means they stay for days. A log of its own, see
 * ASSOC_CHUNKS_MAX.
 */

// BLE
#define MAX_BLE_DEVICES 80
#define BLE_TIMEOUT_MS 90000
#define BLE_QUEUE_SIZE 32

// Advertising data and scan response, 31 octets each (legacy PDUs)
#define BLE_ADV_MAX 62

/*
 * The advertisements as received, for the pcap download: a frame
 * log like the Wi-Fi rings, see "BLE capture".
 */
#define BLE_CHUNKS_MAX  16            // up to 1 MB
#define BLE_RESERVE     (512 * 1024)

/*
 * Devices with an address that stays (public or random static) and
 * how often they were around, see "BLE hits": a hit is a device
 * heard again after BLE_HIT_GAP_MS of silence. Kept on flash.
 */
#define BLE_HITS_MAX     512
#define BLE_HIT_GAP_MS   300000
#define BLE_HITS_SAVE_MS 600000

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
#define FW_VERSION "3.0.0"

/*
 * Every build carries these markers, so an image can be told apart
 * from other ESP32-C5 firmware, and its version and board are known
 * before it is written: by an update from the page (see "Firmware
 * update") and by the flasher (flash.html), which refuses an image
 * for another board unless told to ignore the checks.
 */
#define FW_MARKER_PREFIX    "\x01wlanscout-fw:"
#define BOARD_MARKER_PREFIX "\x01wlanscout-board:"

const char fwMarker[]    = FW_MARKER_PREFIX FW_VERSION "\x02";
const char boardMarker[] = BOARD_MARKER_PREFIX BOARD_ID "\x02";

// ============================================================

// ============================================================
// Entry points
//
// The rest of the sketch is in the numbered tabs next to this
// file, which Arduino appends in name order; setup and the main
// loop themselves are in 35_setup_loop.ino.
// ============================================================

void setup()
{
    sensorSetup();
}

void loop()
{
    sensorLoop();
}
