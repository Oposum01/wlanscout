// SPDX-License-Identifier: GPL-2.0-only WITH AdditionRef-wlanscout-linking-exception
// SPDX-FileCopyrightText: 2026 Dr.-Ing. Simon Wunderlich Systementwicklung & Beratung

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
 * Daylight saving on top of tzMinutes, an index into dstRules; 0
 * for none. tzMinutes stays the standard offset, the rule adds an
 * hour between its two Sundays.
 */
uint8_t tzDst = 0;

/*
 * Each switch falls on a Sunday: the nth of its month, 5 for the
 * last. The minutes count from midnight, in UTC where utc is set,
 * in local standard time otherwise. A rule that starts later in
 * the year than it ends is one of the southern hemisphere.
 */
struct DstRule
{
    uint8_t startMonth, startWeek;
    int16_t startMin;
    uint8_t endMonth, endWeek;
    int16_t endMin;
    bool utc;
};

const DstRule dstRules[] =
{
    { 0, 0, 0, 0, 0, 0, false },          // none
    { 3, 5, 60, 10, 5, 60, true },        // Europe
    { 3, 2, 120, 11, 1, 60, false },      // North America
    { 10, 1, 120, 4, 1, 120, false },     // Australia, south-east
    { 9, 5, 120, 4, 1, 120, false },      // New Zealand
};

const uint8_t DST_RULES = sizeof(dstRules) / sizeof(dstRules[0]);

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

// The UTC epoch of a switch in the given year
int64_t dstSwitch(int year, uint8_t month, uint8_t week, int16_t minutes,
                  bool utc)
{
    int32_t first = daysFromCivil(year, month, 1);

    // 1970-01-01 was a Thursday; 0 is Sunday
    int sunday = 1 + (7 - (int)((first + 4) % 7)) % 7;
    int day    = sunday + 7 * (week - 1);

    if (week == 5)
    {
        int length = daysFromCivil(year + (month == 12), month % 12 + 1, 1) -
                     first;

        day = sunday + 28 <= length ? sunday + 28 : sunday + 21;
    }

    int64_t at = ((int64_t)(first + day - 1) * 1440 + minutes) * 60;

    return utc ? at : at - tzMinutes * 60;
}

/*
 * The offset in force at an epoch: the standard one, plus an hour
 * while the rule has summer time. A clock not yet set gets the
 * standard offset.
 */
int16_t tzOffsetAt(uint32_t epoch)
{
    if (tzDst == 0 || tzDst >= DST_RULES || epoch < 1600000000UL)
        return tzMinutes;

    const DstRule &r = dstRules[tzDst];

    time_t local = (time_t)epoch + tzMinutes * 60;

    struct tm t;
    gmtime_r(&local, &t);

    int year = t.tm_year + 1900;

    int64_t start = dstSwitch(year, r.startMonth, r.startWeek, r.startMin,
                              r.utc);
    int64_t end   = dstSwitch(year, r.endMonth, r.endWeek, r.endMin, r.utc);

    bool summer = r.startMonth < r.endMonth
        ? (epoch >= start && epoch < end)
        : (epoch >= start || epoch < end);

    return tzMinutes + (summer ? 60 : 0);
}

/*
 * The timezone from the country codes the APs send, as long as
 * nobody picked one: on by default, off once tz or dst is set by
 * hand, on again with tz=auto.
 */
bool tzAuto = true;

// The country most BSSs name, and of how many with a code; known
// when it has a single timezone
char    tzAutoCc[3] = { 0, 0, 0 };
uint8_t tzAutoCount = 0;
uint8_t tzAutoOf    = 0;
bool    tzAutoKnown = false;

/*
 * Countries with a single timezone: the standard offset in minutes
 * and the daylight saving rule. Those spanning several zones (US,
 * CA, MX, BR, AU, RU, ...) or keeping a rule of their own (EG, IL,
 * MA, CL, ...) are not in it and leave the timezone alone. Where
 * only islands or an outlying region differ (Azores, Canaries,
 * Chatham, Galapagos, Crimea, Xinjiang), the mainland's.
 */
struct CountryZone
{
    char    cc[3];
    int16_t minutes;
    uint8_t dst;
};

const CountryZone countryZones[] =
{
    { "GB", 0, 1 }, { "IE", 0, 1 }, { "PT", 0, 1 }, { "FO", 0, 1 },
    { "IS", 0, 0 },

    { "DE", 60, 1 }, { "AT", 60, 1 }, { "CH", 60, 1 }, { "LI", 60, 1 },
    { "FR", 60, 1 }, { "BE", 60, 1 }, { "NL", 60, 1 }, { "LU", 60, 1 },
    { "IT", 60, 1 }, { "SM", 60, 1 }, { "VA", 60, 1 }, { "MT", 60, 1 },
    { "ES", 60, 1 }, { "AD", 60, 1 }, { "MC", 60, 1 }, { "DK", 60, 1 },
    { "NO", 60, 1 }, { "SE", 60, 1 }, { "PL", 60, 1 }, { "CZ", 60, 1 },
    { "SK", 60, 1 }, { "HU", 60, 1 }, { "SI", 60, 1 }, { "HR", 60, 1 },
    { "BA", 60, 1 }, { "RS", 60, 1 }, { "ME", 60, 1 }, { "MK", 60, 1 },
    { "AL", 60, 1 }, { "GI", 60, 1 },

    { "FI", 120, 1 }, { "EE", 120, 1 }, { "LV", 120, 1 }, { "LT", 120, 1 },
    { "RO", 120, 1 }, { "BG", 120, 1 }, { "GR", 120, 1 }, { "CY", 120, 1 },
    { "UA", 120, 1 }, { "MD", 120, 1 },

    { "BY", 180, 0 }, { "TR", 180, 0 }, { "SA", 180, 0 }, { "QA", 180, 0 },
    { "BH", 180, 0 }, { "KW", 180, 0 }, { "IQ", 180, 0 }, { "JO", 180, 0 },
    { "KE", 180, 0 }, { "IR", 210, 0 }, { "AE", 240, 0 }, { "OM", 240, 0 },
    { "PK", 300, 0 }, { "IN", 330, 0 }, { "LK", 330, 0 }, { "NP", 345, 0 },
    { "BD", 360, 0 }, { "TH", 420, 0 }, { "VN", 420, 0 }, { "KH", 420, 0 },
    { "LA", 420, 0 }, { "CN", 480, 0 }, { "TW", 480, 0 }, { "HK", 480, 0 },
    { "MO", 480, 0 }, { "SG", 480, 0 }, { "MY", 480, 0 }, { "PH", 480, 0 },
    { "JP", 540, 0 }, { "KR", 540, 0 }, { "NZ", 720, 4 },

    { "ZA", 120, 0 }, { "NG", 60, 0 }, { "GH", 0, 0 },

    { "AR", -180, 0 }, { "UY", -180, 0 }, { "VE", -240, 0 },
    { "PR", -240, 0 }, { "DO", -240, 0 }, { "CO", -300, 0 },
    { "PE", -300, 0 }, { "EC", -300, 0 }, { "PA", -300, 0 },
    { "JM", -300, 0 }, { "CR", -360, 0 }, { "GT", -360, 0 },
};

// Its index in countryZones, -1 for a country not in it
int countryZone(const char *cc)
{
    for (size_t i = 0; i < sizeof(countryZones) / sizeof(countryZones[0]);
         i++)
        if (countryZones[i].cc[0] == cc[0] && countryZones[i].cc[1] == cc[1])
            return (int)i;

    return -1;
}

/*
 * Once a minute in auto mode: the country most BSSs name, by the
 * rule of countryMajority() on the page, more than half of at least
 * five with a Country element, and with a single timezone. A wrong
 * code here and there (an AP left at "US") does not outvote the
 * rest.
 */
void tzAutoLoop()
{
    static uint32_t lastRun = 0;

    if (!tzAuto || networks == nullptr ||
        (lastRun != 0 && millis() - lastRun < 60000))
        return;

    lastRun = millis();

    char    codes[MAX_NETWORKS][2];
    uint8_t counts[MAX_NETWORKS];
    int     kinds = 0;
    int     total = 0;

    for (int i = 0; i < MAX_NETWORKS; i++)
    {
        const WiFiNetwork &n = networks[i];

        if (!n.valid || n.country[0] == 0)
            continue;

        total++;

        int k = 0;

        while (k < kinds && (codes[k][0] != n.country[0] ||
                             codes[k][1] != n.country[1]))
            k++;

        if (k == kinds)
        {
            codes[kinds][0] = n.country[0];
            codes[kinds][1] = n.country[1];
            counts[kinds++] = 0;
        }

        counts[k]++;
    }

    int best = -1;

    for (int k = 0; k < kinds; k++)
        if (best < 0 || counts[k] > counts[best])
            best = k;

    if (total < 5 || counts[best] * 2 <= total)
        return;

    tzAutoCount = counts[best];
    tzAutoOf    = (uint8_t)total;

    char cc[3] = { codes[best][0], codes[best][1], 0 };

    memcpy(tzAutoCc, cc, 3);

    int k = countryZone(cc);

    tzAutoKnown = k >= 0;

    if (k < 0 || (countryZones[k].minutes == tzMinutes &&
                  countryZones[k].dst == tzDst))
        return;

    tzMinutes = countryZones[k].minutes;
    tzDst     = countryZones[k].dst;

    saveSettings();

    char label[12];
    formatTzLabel(label, sizeof(label));

    logEventFrom("system", "timezone %s%s from the country code %s, "
                 "sent by %d of %d APs", label,
                 tzDst ? " with daylight saving" : "", cc, tzAutoCount,
                 tzAutoOf);
}

/*
 * Format an epoch for display, with the offset applied.
 */
void formatStamp(uint32_t epoch, char *buffer, size_t size)
{
    time_t shifted = (time_t)epoch + tzOffsetAt(epoch) * 60;

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
void formatTzLabelAt(uint32_t epoch, char *buffer, size_t size)
{
    int16_t offset = tzOffsetAt(epoch);

    if (offset == 0)
    {
        snprintf(buffer, size, "UTC");
        return;
    }

    int m = offset < 0 ? -offset : offset;

    snprintf(buffer, size, "UTC%c%02d:%02d",
             offset < 0 ? '-' : '+', m / 60, m % 60);
}

// The offset in force now
void formatTzLabel(char *buffer, size_t size)
{
    formatTzLabelAt(deviceEpoch(), buffer, size);
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
    tzDst       = prefs.getUChar("dst", 0);

    // Also after an update: a timezone picked before had no daylight
    // saving rule, and a country auto cannot place leaves it alone
    tzAuto      = prefs.getBool("tzauto", true);

    bleActiveScan = prefs.getBool("bleactive", false);

    catalogEnabled = prefs.getBool("catalog", false);

    snapParts = prefs.getUChar("snapparts", SNAP_DEFAULT);

    keepMode  = prefs.getBool("keepmode", false);
    savedMode = prefs.getUChar("mode", MODE_WIFI);

    if (savedMode > MODE_ZB)
        savedMode = MODE_WIFI;

    prefs.end();

    // Start with the default plan
    for (uint16_t i = 0; i < TOTAL_CHANNELS && i < 64; i++)
        channelSelected[i] = (defaultMask & (1ULL << i)) != 0;
}

void saveSettings()
{
    FLASH_TIMED("settings (NVS)");

    prefs.begin("analyzer", false);

    prefs.putBool("fast", fastSweep);
    prefs.putBool("cap", pcapEnabled);
    prefs.putUChar("capmask", pcapMask);
    prefs.putShort("tz", tzMinutes);
    prefs.putUChar("dst", tzDst);
    prefs.putBool("tzauto", tzAuto);
    prefs.putBool("bleactive", bleActiveScan);
    prefs.putBool("catalog", catalogEnabled);
    prefs.putUChar("snapparts", snapParts);
    prefs.putBool("keepmode", keepMode);
    prefs.putUChar("mode", savedMode);

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
