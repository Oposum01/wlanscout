// SPDX-License-Identifier: GPL-2.0-only WITH AdditionRef-wlanscout-linking-exception
// SPDX-FileCopyrightText: 2026 Dr.-Ing. Simon Wunderlich Systementwicklung & Beratung

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
// built from the IEEE registries: an address in an MA-M or
// MA-S block (28 and 36 bit) gets the block's holder, not
// "IEEE Registration Authority" who holds the OUI around it.
// ============================================================

const char *lookupVendor(const uint8_t *mac, uint8_t *bits = nullptr)
{
    if (bits != nullptr)
        *bits = 0;

    // Bit 1 of the first byte: locally administered
    if (mac[0] & 0x02)
        return "randomised (LAA)";

    const char *name = ouiLookupMac(mac, bits);

    return name != nullptr ? name : "";
}

/*
 * The owner of a vendor element's OUI: 24 bit only, the octets
 * after it are no address and must not hit an MA-M or MA-S block.
 */
const char *lookupOui(const uint8_t *oui)
{
    if (oui[0] & 0x02)
        return "";

    const char *name = ouiLookupFull(((uint32_t)oui[0] << 16) |
                                     ((uint32_t)oui[1] << 8) | oui[2]);

    return name != nullptr ? name : "";
}

/*
 * ,"vblk":28 or 36 after a vendor that came from an MA-M or MA-S
 * block (also IAB); nothing for a plain OUI.
 */
void outVendorBlock(uint8_t bits)
{
    if (bits != 28 && bits != 36)
        return;

    outStr(",\"vblk\":");
    outNum(bits);
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
                              const struct MloInfo &mlo,
                              uint8_t *bits = nullptr)
{
    if (!(bssid[0] & 0x02))
        return lookupVendor(bssid, bits);

    if (mlo.present && !(mlo.mldMac[0] & 0x02))
    {
        const char *name = lookupVendor(mlo.mldMac, bits);

        if (*name)
            return name;
    }

    uint8_t base[6];

    memcpy(base, bssid, sizeof(base));
    base[0] &= ~0x02;

    const char *name = ouiLookupMac(base, bits);

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

struct HistoryRing
{
    WaterfallEntry *rows;
    uint32_t        capacity;
    uint32_t        write;
    uint32_t        count;
};

HistoryRing historyFine   = {};    // one row per sweep
HistoryRing historyCoarse = {};    // one row per HISTORY_STEP_MS

// The sweeps of the step not written yet, per channel
int32_t  coarseSum[TOTAL_CHANNELS];
uint16_t coarseCount[TOTAL_CHANNELS];
uint32_t coarseSince = 0;

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
    uint8_t  centerChannel2;     // 80+80: the second segment's, else 0

    // EHT Operation (ext 106), see ehtOperationOf(): width and centre
    // for Wi-Fi 7 stations, 20 MHz subchannels left out, 0 none
    uint16_t ehtWidth;           // 0 without EHT Operation Information
    uint8_t  ehtCenter;
    uint16_t punctured;          // bit per 20 MHz, lowest first
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

    // EHT Capabilities detail, see ehtCapsOf(); ehtMac 0 none
    uint16_t ehtMac;
    uint8_t  ehtPhy;
    bool     ehtQam4k;

    // HE Capabilities detail (heCapsOf()) and HE Operation's flags:
    // 1 TWT required, 2 co-hosted BSS
    uint8_t  heCaps;
    uint8_t  heOpFlags;

    // VHT Capabilities detail, see vhtCapsOf(); 0 none or 2.4 GHz
    uint8_t  vhtCaps;

    // TWT: what HE Capabilities say (heTwtOf()) and the broadcast
    // schedules of the TWT element (broadcastTwtOf())
    uint8_t  heTwt;
    uint8_t  btwtCount;
    uint8_t  btwtId[2];
    uint8_t  btwtRec[2];         // Broadcast TWT Recommendation, 4 restricted
    uint32_t btwtInterval[2];    // µs
    uint32_t btwtDuration[2];    // µs

    // Links the AP announces to remove, see mlRemovalOf(): bit per
    // link ID, and the beacons left until the first goes
    uint16_t mlRemoving;
    uint16_t mlRemoveIn;

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
    uint8_t  csaMode;            // 1: stations stop sending until the switch

    // The width after it, see csaWideOf(): 0 not announced
    uint16_t csaWidth;
    uint8_t  csaCenter, csaCenter2;
    char     csaCountry[3];      // New Country, "" none

    // Max Channel Switch Time (ext 52) in TU, 0 not sent
    uint32_t csaTime;

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

    // An Xbox's link to its controllers, see xboxWirelessCost()
    bool     xbox;

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
    char     apName[33];         // Cisco 133, MikroTik, Mist or LANCOM name
    char     vendorFirmware[64]; // firmware version, 00:30:1A type 1
    uint8_t  lancomUuid[16];     // LANCOM's device UUID, zeros none
    char     camKit[41];         // camera kit status text, 30:FF:83 type 104
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
    uint16_t pdtCat;             // WSC Primary Device Type, 0 none
    uint16_t pdtSub;             //   and its subcategory (WFA OUI)
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
    bool     w8080;              // 80+80 offered, see caps8080Of()
    uint8_t  streams;

    // EHT Capabilities detail, collected over its requests
    uint16_t ehtMac;
    uint8_t  ehtPhy;
    bool     ehtQam4k;
    uint8_t  heTwt;              // TWT support, see heTwtOf()
    uint8_t  heCaps;             // HE Capabilities detail, see heCapsOf()
    uint8_t  vhtCaps;            // VHT Capabilities detail, see vhtCapsOf()

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

#define ALLOC_INFO_MAX 32

AllocInfo allocInfo[ALLOC_INFO_MAX];

uint8_t allocInfoCount = 0;

void noteAllocation(const char *name, size_t bytes, bool psram)
{
    // 16 entries once dropped the energy history and the waterfall
    // without a word
    if (allocInfoCount >= ALLOC_INFO_MAX)
    {
        Serial.printf("Memory list full, %s not listed\r\n", name);
        return;
    }

    AllocInfo &a = allocInfo[allocInfoCount++];

    memset(&a, 0, sizeof(a));

    strncpy(a.name, name, sizeof(a.name) - 1);

    a.bytes = (uint32_t)bytes;
    a.psram = psram;
}

/*
 * For memory that changes size, the frame logs: the entry of that
 * name is updated, or added (in PSRAM) on first use.
 */
void setAllocation(const char *name, size_t bytes)
{
    for (uint8_t i = 0; i < allocInfoCount; i++)
    {
        if (strcmp(allocInfo[i].name, name) == 0)
        {
            allocInfo[i].bytes = (uint32_t)bytes;
            return;
        }
    }

    noteAllocation(name, bytes, true);
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

// The newest beacon of a BSSID, see BeaconSlot
struct PcapRecord
{
    uint64_t us;          // esp_timer, microseconds since boot
    uint16_t length;      // octets kept in data
    uint16_t origLength;  // octets on the air, may be more
    int8_t   rssi;
    uint8_t  channel;
    int8_t   noise;       // dBm, 0 when the driver gave none
    uint8_t  rxRate;      // see RX_RATE_UNKNOWN
    uint8_t  data[MGMT_FRAME_MAX];
};

// One record of any of the three buffers, as the export sees it
struct PcapView
{
    uint64_t       us;
    uint16_t       length;
    uint16_t       origLength;
    int8_t         rssi;
    uint8_t        channel;
    int8_t         noise;
    uint8_t        rxRate;
    const uint8_t *data;
};
