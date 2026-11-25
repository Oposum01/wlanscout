// SPDX-License-Identifier: GPL-2.0-only WITH AdditionRef-wlanscout-linking-exception
// SPDX-FileCopyrightText: 2026 Dr.-Ing. Simon Wunderlich Systementwicklung & Beratung

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

// The row written next, the oldest once the ring is full
struct WaterfallEntry &historyNextRow(struct HistoryRing &h)
{
    WaterfallEntry &e = h.rows[h.write];

    h.write = (h.write + 1) % h.capacity;

    if (h.count < h.capacity)
        h.count++;

    return e;
}

// Row i counted from the oldest
const struct WaterfallEntry &historyRow(const struct HistoryRing &h,
                                        uint32_t i)
{
    uint32_t start = (h.count < h.capacity) ? 0 : h.write;

    return h.rows[(start + i) % h.capacity];
}

void historyReset()
{
    historyFine.write   = 0;
    historyFine.count   = 0;
    historyCoarse.write = 0;
    historyCoarse.count = 0;

    memset(coarseSum, 0, sizeof(coarseSum));
    memset(coarseCount, 0, sizeof(coarseCount));

    coarseSince = millis();
}

void finishSweep()
{
    uint32_t now = millis();

    WaterfallEntry *entry =
        historyFine.capacity ? &historyNextRow(historyFine) : nullptr;

    if (entry != nullptr)
        entry->timestamp = now;

    for (uint16_t i = 0; i < TOTAL_CHANNELS; i++)
    {
        // Only channels actually visited in this sweep carry a
        // value, the rest stay empty instead of repeating the
        // previous reading
        int8_t v = -127;

        if (channelSelected[i] && channelVisited[i] &&
            currentMeasurement[i].rssiCount)
        {
            v = (int8_t)(currentMeasurement[i].rssiSum /
                         (int32_t)currentMeasurement[i].rssiCount);

            coarseSum[i] += v;
            coarseCount[i]++;
        }

        if (entry != nullptr)
            entry->rssi[i] = v;
    }

    // A step is over: its mean per channel goes into the long rows
    if (historyCoarse.capacity && now - coarseSince >= HISTORY_STEP_MS)
    {
        WaterfallEntry &row = historyNextRow(historyCoarse);

        row.timestamp = now;

        for (uint16_t i = 0; i < TOTAL_CHANNELS; i++)
        {
            row.rssi[i] = coarseCount[i]
                ? (int8_t)(coarseSum[i] / (int32_t)coarseCount[i])
                : -127;

            coarseSum[i]   = 0;
            coarseCount[i] = 0;
        }

        coarseSince = now;
    }

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
    bool sameChannel   = (next == (int)currentChannelIndex);

    currentChannelIndex = next;

    if (sweepFinished)
        finishSweep();

    // With one channel selected the radio stays where it is
    if (!sameChannel)
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

/*
 * The rows a range is answered from: those per sweep while they
 * reach back far enough (or hold everything since the start), else
 * those per step.
 */
const struct HistoryRing &historyFor(uint32_t startTime)
{
    const HistoryRing &f = historyFine;

    if (historyCoarse.count == 0)
        return f;

    if (f.count == 0 || f.count < f.capacity)
        return f.count ? f : historyCoarse;

    return historyRow(f, 0).timestamp <= startTime ? f : historyCoarse;
}

// First row at or after a time; rows are in time order
uint32_t historyFirstAfter(const struct HistoryRing &h, uint32_t startTime)
{
    uint32_t lo = 0;
    uint32_t hi = h.count;

    while (lo < hi)
    {
        uint32_t mid = (lo + hi) / 2;

        if (historyRow(h, mid).timestamp < startTime)
            lo = mid + 1;
        else
            hi = mid;
    }

    return lo;
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

    historyReset();

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
