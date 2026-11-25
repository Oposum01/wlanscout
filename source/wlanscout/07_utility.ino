// SPDX-License-Identifier: GPL-2.0-only WITH AdditionRef-wlanscout-linking-exception
// SPDX-FileCopyrightText: 2026 Dr.-Ing. Simon Wunderlich Systementwicklung & Beratung

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

/*
 * The channel tuned last and when, for the heap probe: a frame the
 * callback gets with another channel was received before the switch.
 */
volatile uint8_t cbChannel  = 0;
volatile int64_t cbSwitchUs = 0;

void switchChannel(uint16_t index)
{
    uint8_t channel = getChannelNumber(index);

    esp_err_t err = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);

    cbSwitchUs = esp_timer_get_time();
    cbChannel  = channel;

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
