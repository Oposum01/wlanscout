// SPDX-License-Identifier: GPL-2.0-only WITH AdditionRef-wlanscout-linking-exception
// SPDX-FileCopyrightText: 2026 Dr.-Ing. Simon Wunderlich Systementwicklung & Beratung

// ============================================================
// SETUP
// ============================================================

/*
 * RESET pressed twice: a way back to Wi-Fi when "remember capture
 * mode" starts the sensor in a mode it cannot be reached in. The
 * button looks like a power cut to the chip (reason "power on",
 * RAM lost), so one press cannot be told apart; two can. Every
 * power-on start marks itself in NVS and clears the mark after
 * DOUBLE_RESET_MS; a power-on start that finds the mark still set
 * came within that time of the one before.
 *
 * The chip needs about 2 s from the button to the sketch (bootloader,
 * PSRAM test); a second press before that cuts the first start off
 * before it set the mark. So: press, wait for the LED (blue on the
 * kit), press again while it is lit.
 */
#define DOUBLE_RESET_MS 10000

void doubleResetLed(bool on)
{
#if STATUS_LED_TYPE == LED_RGB
    rgbLedWriteOrdered(STATUS_LED, LED_COLOR_ORDER_RGB, 0, 0, on ? 60 : 0);
#elif STATUS_LED_TYPE == LED_PLAIN_HIGH || STATUS_LED_TYPE == LED_PLAIN_LOW
    pinMode(STATUS_LED, OUTPUT);
    digitalWrite(STATUS_LED, on == (STATUS_LED_TYPE == LED_PLAIN_HIGH));
#endif
}

bool doubleReset     = false;
bool doubleResetMark = false;

void doubleResetCheck()
{
    Preferences p;

    if (!p.begin("wlsboot", false))
        return;

    bool early = p.getBool("early", false);

    // Any other start (a crash, an update) ends a window that is
    // open, so that its mark cannot count for a press much later
    if (esp_reset_reason() != ESP_RST_POWERON)
    {
        if (early)
            p.putBool("early", false);

        p.end();
        return;
    }

    doubleReset = early;

    // A third quick press counts as a fresh first one
    p.putBool("early", !doubleReset);

    p.end();

    doubleResetMark = !doubleReset;

    // Lit while a second press counts
    if (doubleResetMark)
        doubleResetLed(true);
}

void doubleResetClear()
{
    if (!doubleResetMark || millis() < DOUBLE_RESET_MS)
        return;

    doubleResetMark = false;

    doubleResetLed(false);

    Preferences p;

    if (p.begin("wlsboot", false))
    {
        p.putBool("early", false);
        p.end();
    }
}

void sensorSetup()
{
    doubleResetCheck();

    // Nothing reads the markers, keep the linker from dropping them
    asm volatile("" : : "r"(fwMarker), "r"(boardMarker));

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

    bleHitsLoad();

    statsSetup();

    cfgMuteLoad();

    watchKeepLoad();

#if ENABLE_OTA
    otaInit();
#endif

    loadSensorName();

#if ENABLE_ETHERNET
    ethSetup();
#endif

    setupWiFiScanner();

#if ENABLE_ETHERNET
    webServerSetup();
#endif

    setupUsbApi();

    // BOOT as a button for snapshots, see "Snapshots"
    pinMode(BOOT_BUTTON, INPUT_PULLUP);
    buttonDown = digitalRead(BOOT_BUTTON) == LOW;

    startHeapProbe();

#if ENABLE_ETHERNET
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
#endif

    Serial.printf("Sensor id %s%s%s\r\n",
                  sensorMacText().c_str(),
                  sensorName[0] ? ", name " : "",
                  sensorName);

    rebootOnRequest = esp_reset_reason() == ESP_RST_SW &&
                       rebootAsked == REBOOT_ASKED;
    rebootAsked = 0;

    logEventFrom("system", "device started, firmware v%s, %s",
                 FW_VERSION, resetReasonText());

    coreDumpSetup();

#if ENABLE_OTA
    otaLogRollback();
#endif

    catalogLogPrune();

    // Remember capture mode: back in the mode picked last, switched
    // to from the main loop like a mode a page asks for
    if (doubleReset)
    {
        logEventFrom("system", "RESET pressed twice: starting in Wi-Fi%s",
                     keepMode && savedMode != MODE_WIFI
                         ? ", the remembered mode forgotten" : "");

        if (savedMode != MODE_WIFI)
        {
            savedMode = MODE_WIFI;
            saveSettings();
        }

        ledSignal(true);
    }
    else if (keepMode && savedMode != MODE_WIFI)
    {
        pendingMode = savedMode;

        logEventFrom("system", "starting in %s mode, as picked last",
                     savedMode == MODE_BLE ? "BLE" : "802.15.4");
    }

    Serial.println();
    Serial.println("========================================");
    Serial.println(" ANALYZER RUNNING");
    Serial.println("========================================");
}

// ============================================================
// LOOP
// ============================================================

/*
 * Commands on the serial console (115200 baud), one per line:
 *
 *   ethreinit   take the Ethernet interface down and up again, for
 *               a sensor that has link but answers nothing
 *   w5500       the W5500's own registers, see w5500Dump()
 *   help        this list
 *
 * On the kit's CH343 the terminal has to keep DTR and RTS off: they
 * drive the board's reset and BOOT.
 */
#define CONSOLE_LINE_MAX 32

void consoleCommand(const char *line)
{
#if ENABLE_ETHERNET
    if (strcmp(line, "ethreinit") == 0)
    {
        ethReinitRequested = true;
        return;
    }

    if (strcmp(line, "w5500") == 0)
    {
        w5500Dump("console");
        return;
    }
#endif

    if (strcmp(line, "help") != 0)
        Serial.printf("Unknown command \"%s\"\r\n", line);

#if ENABLE_ETHERNET
    Serial.println("Commands: ethreinit, w5500, help");
#else
    Serial.println("Commands: help");
#endif
}

void consoleLoop()
{
    static char   line[CONSOLE_LINE_MAX];
    static size_t length = 0;

    while (Serial.available() > 0)
    {
        int c = Serial.read();

        if (c == '\r' || c == '\n')
        {
            line[length] = 0;

            if (length > 0)
                consoleCommand(line);

            length = 0;
        }
        else if (length < sizeof(line) - 1)
        {
            line[length++] = (char)c;
        }
    }
}

void sensorLoop()
{
    doubleResetClear();

    statsLoopRound();

    consoleLoop();

    char path[48];

    statsPath[0] = 0;

    int64_t t0 = esp_timer_get_time();

#if ENABLE_ETHERNET
    server.handleClient();
#endif

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

    bleHitsLoop();

    clockFillLoop();

    tzAutoLoop();

    watchLoop();

#if ENABLE_OTA
    otaLoop();
#endif

    rebootLoop();

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

#if ENABLE_ETHERNET
    ntpLoop();

    if (!ethLoop())
        return;
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
        heapProbeRxLine();
#endif

        heapSnapPrint();

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
