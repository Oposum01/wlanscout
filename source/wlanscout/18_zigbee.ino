// SPDX-License-Identifier: GPL-2.0-only WITH AdditionRef-wlanscout-linking-exception
// SPDX-FileCopyrightText: 2026 Dr.-Ing. Simon Wunderlich Systementwicklung & Beratung

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

            zbFrames += 1;
        }
        else
        {
            zbDrops += 1;
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

// Gives the BLE stack's heap back (about 32 kB on the kit) when
// leaving BLE mode. deinit(false) keeps the controller's static
// memory, so BLEDevice::init() works again on the next switch
void releaseBle()
{
    if (!bleInitialised)
        return;

    stopBleScan();

    BLEDevice::deinit(false);

    bleScan        = nullptr;
    bleInitialised = false;

    Serial.printf("BLE: released, heap %.1f kB\r\n",
                  ESP.getFreeHeap() / 1024.0);
}

// ------------------------------------------------------------
// Mode switching
//
// Wi-Fi is deliberately NOT deinitialised here. Tearing the
// Wi-Fi driver down and bringing it back up while the HTTP
// server and the W5500 are running turned out to be fragile.
// BLE and 802.15.4 stop it (esp_wifi_stop), which keeps the
// driver but gives back part of its internal heap, and BLE
// gets the radio to itself; Wi-Fi mode starts it again.
// ------------------------------------------------------------

void enterBleMode()
{
    if (scanMode == MODE_BLE)
        return;

    Serial.printf("Mode: BLE, heap %.1f kB\r\n", ESP.getFreeHeap() / 1024.0);

    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(NULL);

    // From 802.15.4 Wi-Fi is stopped already
    if (scanMode == MODE_ZB)
        zbStop();
    else
        Serial.printf("BLE: esp_wifi_stop 0x%X\r\n", esp_wifi_stop());

    if (!bleInitialised)
    {
        BLEDevice::init("");

        bleScan = BLEDevice::getScan();

        if (bleScan == nullptr)
        {
            Serial.println("BLE: getScan() failed, staying on Wi-Fi");

            esp_wifi_start();

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

    releaseBle();

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

    releaseBle();

    Serial.println("Wi-Fi: BLE stopped");

    // BLE and 802.15.4 both stop Wi-Fi
    bool wasStopped = (scanMode == MODE_ZB || scanMode == MODE_BLE);

    zbStop();

    if (wasStopped)
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

// Set with pendingMode when a page asked for it, see keepMode
volatile bool    pendingModeAsked = false;

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

        if (reqArg("tz") == "auto")
        {
            if (!tzAuto)
            {
                tzAuto = true;

                saveSettings();

                logRequest("timezone from the country codes");
            }
        }
        else if (want >= -720 && want <= 840)
        {
            if (want != tzMinutes || tzAuto)
            {
                tzMinutes = (int16_t)want;
                tzAuto    = false;

                saveSettings();

                char label[12];
                formatTzLabel(label, sizeof(label));

                logRequest("display timezone set to %s", label);
            }
        }
    }

    if (reqHasArg("dst"))
    {
        long want = reqArg("dst").toInt();

        if (want >= 0 && want < DST_RULES && (want != tzDst || tzAuto))
        {
            tzDst  = (uint8_t)want;
            tzAuto = false;

            saveSettings();

            logRequest("daylight saving rule set to %ld", want);
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

        pendingModeAsked = true;

        if (pendingMode != (int8_t)scanMode)
            logRequest("mode change requested: %s", mode.c_str());
    }

    if (reqHasArg("keepmode"))
    {
        bool on = reqArg("keepmode").toInt() != 0;

        if (on != keepMode)
            logRequest("remember capture mode %s", on ? "on" : "off");

        keepMode  = on;

        // The mode now, or the one just asked for
        savedMode = pendingMode >= 0 ? (uint8_t)pendingMode : scanMode;

        saveSettings();
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
    reply += ",\"keepmode\":";
    reply += keepMode ? "1" : "0";
    reply += "}";

    reqSend(200, "application/json", reply);
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
