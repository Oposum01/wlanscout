// SPDX-License-Identifier: GPL-2.0-only WITH AdditionRef-wlanscout-linking-exception
// SPDX-FileCopyrightText: 2026 Dr.-Ing. Simon Wunderlich Systementwicklung & Beratung

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
 * not fit; the IE catalog's frames are, with the catalog, see
 * writeSnapshotFrames().
 *
 * The files are /snap/<seq>_<epoch>_<uptime>.json, so the list
 * needs no file to be opened; epoch is 0 while the clock was not
 * set. The last SNAP_KEEP stay, the oldest goes, sooner when the
 * file system runs short (SNAP_MIN_FREE).
 *
 * BOOT is GPIO 28 on the Waveshare boards and the XIAO, a strapping pin read
 * only at reset: held then it starts the download mode, pressed
 * while running it is an ordinary input with the board's pull-up.
 * The RGB LED on GPIO 27 blinks green twice when the snapshot is
 * written, red when it failed; the XIAO's orange one twice short or
 * once long.
 */
#define SNAP_DIR        "/snap"
#define SNAP_KEEP       10

// Snapshots of this start taken without a clock, see snapClockFill()
#define SNAP_UNDATED_MAX 16

uint32_t snapUndated[SNAP_UNDATED_MAX];
uint8_t  snapUndatedCount = 0;
#define SNAP_IMPORT_TMP SNAP_DIR "/import.tmp"   // see "Snapshot import"
// Free before a snapshot is written, the oldest going first; less on
// a small file system (fsSmall), where 512 kB would leave room for
// one or two
#define SNAP_MIN_FREE   (fsSmall ? 256 * 1024 : 512 * 1024)

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
    { "/api/history?range=86400000", sendHistoryJSON, SNAP_SPECTRUM },
    { "/api/ble",          sendBleJSON,          SNAP_BLE },
    { "/api/blehits",      sendBleHitsJSON,      SNAP_BLE },
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

#if STATUS_LED_TYPE == LED_RGB
    // The kit's LED takes red first; the core's default (GRB, as
    // on a WS2812B) turned green into red
    rgbLedWriteOrdered(STATUS_LED, LED_COLOR_ORDER_RGB,
                       on ? ledRed : 0, on ? ledGreen : 0, 0);
#elif STATUS_LED_TYPE == LED_PLAIN_HIGH || STATUS_LED_TYPE == LED_PLAIN_LOW
    // One colour: the length tells, long for a failure
    pinMode(STATUS_LED, OUTPUT);
    digitalWrite(STATUS_LED, on == (STATUS_LED_TYPE == LED_PLAIN_HIGH));
#endif

    ledNext = millis() + (on ? (ledRed ? 1500 : 250) : 200);
    ledPhases--;
}

/*
 * The IE catalog's frames in a snapshot, so that one viewed later or
 * on another sensor still hands out its pcap: the 24-byte pcap header
 * and each frame (record header and bytes) in base64, with the
 * catalog rows it carries as kind and key pairs. Up to
 * SNAP_FRAMES_MAX bytes of the file, oldest first; "cut" counts the
 * frames left out.
 *
 *   "catalogframes":{"hdr":"1MOyoQ…","frames":[["…",[2,1575429]],…],
 *                    "cut":0}
 */
#define SNAP_FRAMES_MAX (fsSmall ? 128 * 1024 : 256 * 1024)

void writeBase64(File &f, const uint8_t *data, size_t length)
{
    static const char digits[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    char out[64];
    size_t n = 0;

    for (size_t i = 0; i < length; i += 3)
    {
        uint32_t v = (uint32_t)data[i] << 16;

        if (i + 1 < length)
            v |= (uint32_t)data[i + 1] << 8;

        if (i + 2 < length)
            v |= data[i + 2];

        out[n++] = digits[(v >> 18) & 63];
        out[n++] = digits[(v >> 12) & 63];
        out[n++] = i + 1 < length ? digits[(v >> 6) & 63] : '=';
        out[n++] = i + 2 < length ? digits[v & 63] : '=';

        if (n == sizeof(out))
        {
            f.write((const uint8_t *)out, n);
            n = 0;
        }
    }

    f.write((const uint8_t *)out, n);
}

void writeSnapshotFrames(File &f)
{
    File in = LittleFS.open(CATALOG_PCAP, "r");

    if (!in || in.size() < 24)
        return;

    uint8_t *buf = (uint8_t *)heap_caps_malloc_prefer(
        16 + RADIOTAP_LEN + MGMT_PARSE_MAX, 2,
        MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT);

    if (buf == nullptr || in.read(buf, 24) != 24)
    {
        free(buf);
        return;
    }

    f.print(",\"catalogframes\":{\"hdr\":\"");
    writeBase64(f, buf, 24);
    f.print("\",\"frames\":[");

    catalogMatching    = true;
    catalogMatchAny    = true;
    catalogCollectHits = true;

    uint32_t written = 0;
    uint32_t cut     = 0;
    bool     first   = true;

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

        if (written + 16 + len > SNAP_FRAMES_MAX)
        {
            cut++;
            continue;
        }

        CatalogFrame cf = {};

        catalogMatched  = false;
        catalogHitCount = 0;

        catalogScan(buf + 16 + rtLen, len - rtLen, 0, cf);

        f.print(first ? "[\"" : ",[\"");
        writeBase64(f, buf, 16 + len);
        f.print("\",[");

        for (uint8_t i = 0; i < catalogHitCount; i++)
            f.printf("%s%u,%" PRIu32, i ? "," : "",
                     (unsigned)catalogHitKind[i], catalogHitKey[i]);

        f.print("]]");

        written += 16 + len;
        first    = false;
    }

    catalogMatching    = false;
    catalogMatchAny    = false;
    catalogCollectHits = false;

    f.printf("],\"cut\":%" PRIu32 "}", cut);

    free(buf);
    in.close();
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

    FLASH_TIMED("snapshot");

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

    f.print("}");

    if (snapParts & SNAP_CATALOG)
        writeSnapshotFrames(f);

    f.printf(",\"parts\":%u}", (unsigned)snapParts);

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

    // Dated once the clock is known, see clockFillLoop()
    if (ok && taken.epoch == 0 && snapUndatedCount < SNAP_UNDATED_MAX)
        snapUndated[snapUndatedCount++] = taken.seq;

    return ok;
}

/*
 * The time comes later, a sensor on a power bank finds out when a
 * phone is plugged into its other port: the snapshots taken since
 * the start get their epoch. It lives in the file name, which the
 * list and the download read; the file itself is left as written.
 */
void snapClockFill(uint32_t nowEpoch, uint32_t nowUp)
{
    if (snapUndatedCount == 0)
        return;

    SnapInfo list[SNAP_KEEP + 4];

    uint8_t count = listSnapshots(list, SNAP_KEEP + 4);

    for (uint8_t i = 0; i < count; i++)
        for (uint8_t k = 0; k < snapUndatedCount; k++)
        {
            if (list[i].seq != snapUndated[k] || list[i].epoch != 0 ||
                list[i].uptime > nowUp)
                continue;

            SnapInfo dated = list[i];

            dated.epoch = nowEpoch - (nowUp - list[i].uptime);

            LittleFS.rename(snapshotPath(list[i]), snapshotPath(dated));
        }

    snapUndatedCount = 0;
}

/*
 * This start's catalog sightings from before the clock: their
 * epoch was the uptime, now it is the time.
 */
void catalogClockFill(uint32_t nowEpoch, uint32_t nowUp)
{
    if (catalog == nullptr)
        return;

    for (uint16_t i = 0; i < catalogCount; i++)
    {
        CatalogEntry &e = catalog[i];

        if (e.firstBoot == bootCount && e.firstClock == 0 &&
            e.firstAt <= nowUp)
        {
            e.firstAt    = nowEpoch - (nowUp - e.firstAt);
            e.firstClock = pcapClockSource;
            catalogDirty = true;
        }

        if (e.lastBoot == bootCount && e.lastClock == 0 &&
            e.lastAt <= nowUp)
        {
            e.lastAt    = nowEpoch - (nowUp - e.lastAt);
            e.lastClock = pcapClockSource;
            catalogDirty = true;
        }
    }
}

// The association history keeps epochs, without a clock the uptime
void assocClockFill(uint32_t nowEpoch, uint32_t nowUp)
{
    if (assocHistory == nullptr)
        return;

    for (int i = 0; i < MAX_ASSOC_HISTORY; i++)
    {
        AssocHistory &h = assocHistory[i];

        if (!h.valid)
            continue;

        if (h.firstEpoch <= nowUp)
            h.firstEpoch = nowEpoch - (nowUp - h.firstEpoch);

        if (h.lastEpoch <= nowUp)
            h.lastEpoch = nowEpoch - (nowUp - h.lastEpoch);
    }
}

/*
 * The moment the clock is first known in a start, by NTP, the
 * browser or an AP: what was recorded before it with the uptime
 * only gets its time, the event log, the snapshots, the IE
 * catalog and the association history. The capture ring needs
 * nothing, its timestamps are worked out when it is downloaded.
 */
bool clockFilled = false;

void clockFillLoop()
{
    if (clockFilled || pcapClockSource == 0)
        return;

    clockFilled = true;

    uint32_t nowEpoch = deviceEpoch();
    uint32_t nowUp    = (uint32_t)(esp_timer_get_time() / 1000000LL);

    logClockFill(nowEpoch, nowUp);
    snapClockFill(nowEpoch, nowUp);
    catalogClockFill(nowEpoch, nowUp);
    assocClockFill(nowEpoch, nowUp);
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

        /*
         * The file's first bytes, with the epoch of the name for a 0
         * written before the clock was known (see snapClockFill())
         */
        char   start[80];
        size_t got = f.read((uint8_t *)start, sizeof(start) - 1);

        start[got] = 0;

        String first(start);

        if (list[found].epoch)
            first.replace("\"epoch\":0,",
                          "\"epoch\":" + String(list[found].epoch) + ",");

        if (given.length())
        {
            outBuf = "";
            outStr("{\"name\":");
            outJSON(given.c_str());
            outStr(",");

            head = outBuf + first.substring(1);
            outBuf = "";
        }
        else
            head = first;

        reqStart(200, "application/json",
                 f.size() - got + head.length(), name);

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

#if ENABLE_ETHERNET
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
#endif

void handleImportDone()
{
    lastRequestServed = millis();

    importAnswer(!importError[0] && importEnd());
}
