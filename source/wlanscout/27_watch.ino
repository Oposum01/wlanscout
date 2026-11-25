// SPDX-License-Identifier: GPL-2.0-only WITH AdditionRef-wlanscout-linking-exception
// SPDX-FileCopyrightText: 2026 Dr.-Ing. Simon Wunderlich Systementwicklung & Beratung

// ============================================================
// Graphs of one BSS, recorded only while asked for
// ============================================================

BssWatch watches[WATCH_MAX];

int watchIndex(const uint8_t *bssid)
{
    for (int i = 0; i < WATCH_MAX; i++)
        if (watches[i].used && memcmp(watches[i].bssid, bssid, 6) == 0)
            return i;

    return -1;
}

// For /api/system: every recording, kept or watched
/*
 * The wall time of a recording started before the clock was known:
 * its start in seconds since boot plus the clock's offset. From the
 * epoch now and the time since, each cut to whole seconds, it moved
 * by a second from one request to the next.
 */
uint32_t watchStartEpoch(uint32_t startUp)
{
    return startUp + (uint32_t)(clockOffsetUs / 1000000LL);
}

void watchListJson()
{
    bool firstOne = true;

    for (int i = 0; i < WATCH_MAX; i++)
    {
        const BssWatch &w = watches[i];

        if (!w.used)
            continue;

        uint32_t epoch = w.startEpoch;

        if (epoch == 0 && pcapClockSource != 0)
            epoch = watchStartEpoch(w.startUp);

        outStr(firstOne ? "[" : ",[");
        outMac(w.bssid);
        outStr(",");
        outNum(w.keep ? 1 : 0);
        outStr(",");
        outNum(epoch);
        outStr(",");
        outNum(w.startUp);
        outStr(",");
        outNum(w.next > WATCH_SAMPLES ? WATCH_SAMPLES : w.next);
        outStr("]");

        firstOne = false;
    }
}

// For the network table: recorded without a viewer
bool watchKept(const uint8_t *bssid)
{
    int i = watchIndex(bssid);

    return i >= 0 && watches[i].keep;
}

/*
 * A new recording of the BSS in a free slot: the slot, -1 when all
 * are taken, -2 without the memory for its ring.
 */
int watchStart(const uint8_t *mac)
{
    int i = -1;

    for (int k = 0; k < WATCH_MAX && i < 0; k++)
        if (!watches[k].used)
            i = k;

    if (i < 0)
        return -1;

    WatchSample *ring = (WatchSample *)ps_malloc(sizeof(WatchSample) *
                                                 WATCH_SAMPLES);

    if (ring == nullptr)
        return -2;

    BssWatch &w = watches[i];

    memset(&w, 0, sizeof(w));
    memcpy(w.bssid, mac, 6);

    w.used       = true;
    w.ring       = ring;
    w.stepStart  = millis();
    w.renewed    = millis();
    w.startUp    = millis() / 1000;
    w.startEpoch = pcapClockSource != 0 ? deviceEpoch() : 0;

    watchStepReset(i);

    return i;
}

// The kept BSSIDs in NVS, six octets each
void watchKeepSave()
{
    uint8_t list[WATCH_MAX * 6];
    size_t  n = 0;

    for (int i = 0; i < WATCH_MAX; i++)
        if (watches[i].used && watches[i].keep)
        {
            memcpy(list + n, watches[i].bssid, 6);
            n += 6;
        }

    prefs.begin("analyzer", false);

    if (n)
        prefs.putBytes("watchkeep", list, n);
    else
        prefs.remove("watchkeep");

    prefs.end();
}

// At the start: the kept ones are recorded again, from nothing
void watchKeepLoad()
{
    uint8_t list[WATCH_MAX * 6];

    prefs.begin("analyzer", true);

    size_t len = prefs.getBytesLength("watchkeep");

    if (len > sizeof(list) || len % 6)
        len = 0;

    if (len)
        prefs.getBytes("watchkeep", list, len);

    prefs.end();

    for (size_t o = 0; o < len; o += 6)
    {
        int i = watchStart(list + o);

        if (i >= 0)
            watches[i].keep = true;
    }
}

void watchStepReset(int i)
{
    BssWatch &w = watches[i];

    w.rMin     = 0;
    w.rMax     = 0;
    w.rSum     = 0;
    w.rCount   = 0;
    w.util     = -1;
    w.stations = -1;
}

// A beacon or probe response of the BSS, with its BSS Load (-1 none)
void watchFrame(const uint8_t *bssid, int8_t rssi, int16_t stations,
                int16_t util)
{
    int i = watchIndex(bssid);

    if (i < 0)
        return;

    BssWatch &w = watches[i];

    if (w.rCount == 0 || rssi < w.rMin)
        w.rMin = rssi;

    if (w.rCount == 0 || rssi > w.rMax)
        w.rMax = rssi;

    w.rSum += rssi;
    w.rCount++;

    if (util >= 0)
        w.util = util;

    if (stations >= 0)
        w.stations = stations;
}

void watchClose(int i)
{
    BssWatch &w = watches[i];
    WatchSample &s = w.ring[w.next % WATCH_SAMPLES];

    s.rssiMin  = w.rCount ? w.rMin : 0;
    s.rssiMax  = w.rCount ? w.rMax : 0;
    s.rssiAvg  = w.rCount ? (int8_t)(w.rSum / (int32_t)w.rCount) : 0;
    s.util     = w.util >= 0 ? (uint8_t)w.util : 255;
    s.stations = w.stations >= 0 ? (uint16_t)w.stations : 0xFFFF;

    uint16_t seen = countClients(w.bssid);

    s.seen  = seen > 255 ? 255 : seen;
    s.heard = w.rCount > 255 ? 255 : w.rCount;

    w.next++;
    w.stepStart += WATCH_STEP_MS;

    watchStepReset(i);
}

void watchStop(int i)
{
    free(watches[i].ring);
    memset(&watches[i], 0, sizeof(watches[i]));
}

// From loop(): steps closed on time, watches nobody renews dropped
void watchLoop()
{
    uint32_t now = millis();

    for (int i = 0; i < WATCH_MAX; i++)
    {
        if (!watches[i].used)
            continue;

        if (!watches[i].keep && now - watches[i].renewed > WATCH_IDLE_MS)
        {
            watchStop(i);
            continue;
        }

        // A stalled loop gives empty steps, not one long one
        while (now - watches[i].stepStart >= WATCH_STEP_MS)
            watchClose(i);
    }
}

/*
 * The samples a watch holds as CSV, one line per 5 s step: its start
 * in the display timezone (seconds since boot without a clock), the
 * RSSI, the BSS Load and the stations; empty cells where nothing was
 * heard or the AP sends no BSS Load.
 */
void watchCsv(const BssWatch &w, uint32_t first)
{
    char name[48];
    char stamp[24];
    char tz[16];

    snprintf(name, sizeof(name), "graph-%02X%02X%02X%02X%02X%02X.csv",
             w.bssid[0], w.bssid[1], w.bssid[2], w.bssid[3], w.bssid[4],
             w.bssid[5]);

    lastRequestServed = millis();
    outBuf = "";
    outBuf.reserve(1400);

    reqStart(200, "text/csv", -1, name);

    outStr(w.startEpoch ? "time (" : "seconds since boot");

    if (w.startEpoch)
    {
        uint32_t step = WATCH_STEP_MS / 1000;
        uint32_t from = w.startEpoch + first * step;
        uint32_t to   = w.startEpoch + (w.next ? w.next - 1 : 0) * step;

        formatTzLabelAt(from, tz, sizeof(tz));
        outStr(tz);

        // Daylight saving changed within the recording
        if (tzOffsetAt(from) != tzOffsetAt(to))
        {
            formatTzLabelAt(to, tz, sizeof(tz));
            outStr(" then ");
            outStr(tz);
        }

        outStr(")");
    }

    outStr(",rssi min,rssi avg,rssi max,bss load %,stations per the AP,"
           "stations seen,beacons\r\n");

    for (uint32_t k = first; k < w.next; k++)
    {
        const WatchSample &s = w.ring[k % WATCH_SAMPLES];
        uint32_t at = k * (WATCH_STEP_MS / 1000);

        if (w.startEpoch)
        {
            formatStamp(w.startEpoch + at, stamp, sizeof(stamp));
            outStr(stamp);
        }
        else
            outNum(w.startUp + at);

        if (s.heard)
        {
            outStr(",");
            outNum(s.rssiMin);
            outStr(",");
            outNum(s.rssiAvg);
            outStr(",");
            outNum(s.rssiMax);
        }
        else
            outStr(",,,");

        outStr(",");

        if (s.util != 255)
            outNum(s.util);

        outStr(",");

        if (s.stations != 0xFFFF)
            outNum(s.stations);

        outStr(",");
        outNum(s.seen);
        outStr(",");
        outNum(s.heard);
        outStr("\r\n");
    }

    outEnd();
}

/*
 * GET /api/watch?bssid=…&since=N: start recording this BSS or renew
 * it, and answer the samples from number N on; &stop=1 ends it. A
 * watch nobody renews for WATCH_IDLE_MS ends by itself, the page
 * renews it with each poll while the Graphs tab is open.
 * {"ok":1,"step":5,"next":…,"first":…,"epoch":…,"up":…,"s":[[min,
 * avg,max,util,stations,seen,heard],…]}: sample k covers the step
 * from epoch (or up, seconds since boot) + k * step on; min, avg,
 * max 0 when nothing was heard, util 255 and stations 65535 without
 * a BSS Load element.
 */
void handleWatch()
{
    uint8_t mac[6];

    if (!reqHasArg("bssid") || !parseMacArg(reqArg("bssid"), mac))
    {
        reqSend(400, "text/plain", "bssid missing or not a MAC address");
        return;
    }

    int i = watchIndex(mac);

    bool hasKeep = reqHasArg("keep");
    bool keep    = reqArg("keep").toInt() != 0;

    /*
     * Closing the tab stops a recording, but not a kept one: that
     * takes keep=0 as well, from Settings or the checkbox
     */
    if (reqArg("stop").toInt() != 0)
    {
        if (i >= 0 && (!watches[i].keep || (hasKeep && !keep)))
        {
            bool wasKept = watches[i].keep;

            watchStop(i);

            if (wasKept)
                watchKeepSave();
        }

        reqSend(200, "application/json", "{\"ok\":1}");
        return;
    }

    // The CSV of one that runs; it starts none
    bool csv = reqArg("csv").toInt() != 0;

    if (i < 0 && csv)
    {
        reqSend(404, "text/plain", "this BSS is not being recorded");
        return;
    }

    if (i < 0)
    {
        i = watchStart(mac);

        if (i == -1)
        {
            reqSend(200, "application/json",
                    "{\"ok\":0,\"error\":\"too many graphs open\"}");
            return;
        }

        if (i < 0)
        {
            reqSend(200, "application/json",
                    "{\"ok\":0,\"error\":\"no memory for the graphs\"}");
            return;
        }
    }

    BssWatch &w = watches[i];

    w.renewed = millis();

    if (hasKeep && keep != w.keep)
    {
        w.keep = keep;
        watchKeepSave();
    }

    // The clock may have come since the start
    if (w.startEpoch == 0 && pcapClockSource != 0)
        w.startEpoch = watchStartEpoch(w.startUp);

    uint32_t first = w.next > WATCH_SAMPLES ? w.next - WATCH_SAMPLES : 0;
    uint32_t since = reqHasArg("since") ? (uint32_t)reqArg("since").toInt()
                                        : first;

    if (since < first)
        since = first;

    if (csv)
    {
        watchCsv(w, first);
        return;
    }

    outBegin();

    outStr("{\"ok\":1,\"keep\":");
    outNum(w.keep ? 1 : 0);
    outStr(",\"step\":");
    outNum(WATCH_STEP_MS / 1000);
    outStr(",\"next\":");
    outNum(w.next);
    outStr(",\"first\":");
    outNum(first);
    outStr(",\"epoch\":");
    outNum(w.startEpoch);
    outStr(",\"up\":");
    outNum(w.startUp);
    outStr(",\"s\":[");

    for (uint32_t k = since; k < w.next; k++)
    {
        const WatchSample &s = w.ring[k % WATCH_SAMPLES];

        outStr(k > since ? ",[" : "[");
        outNum(s.rssiMin);
        outStr(",");
        outNum(s.rssiAvg);
        outStr(",");
        outNum(s.rssiMax);
        outStr(",");
        outNum(s.util);
        outStr(",");
        outNum(s.stations);
        outStr(",");
        outNum(s.seen);
        outStr(",");
        outNum(s.heard);
        outStr("]");
    }

    outStr("]}");
    outEnd();
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
