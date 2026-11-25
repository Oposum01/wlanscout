// SPDX-License-Identifier: GPL-2.0-only WITH AdditionRef-wlanscout-linking-exception
// SPDX-FileCopyrightText: 2026 Dr.-Ing. Simon Wunderlich Systementwicklung & Beratung

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


    beaconSlots =
        (BeaconSlot *)allocateTable("beacon slots",
                                    sizeof(BeaconSlot) *
                                        MAX_BEACON_SLOTS);

    // Without it there are simply no beacon versions
    beaconTracks =
        (BeaconTrack *)allocateTable("beacon versions, index",
                                     sizeof(BeaconTrack) *
                                         MAX_BEACON_SLOTS);

    if (beaconTracks != nullptr)
        memset(beaconTracks, 0, sizeof(BeaconTrack) * MAX_BEACON_SLOTS);

    // The frame logs start with one chunk each and grow from there
    bool logsOk = frameLogGrow(ringLog) && frameLogGrow(assocLog);

    Serial.printf("  %-14s %8.1f kB in PSRAM, each, growing\r\n",
                  "frame logs", FRAME_CHUNK_BYTES / 1024.0);

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
        bssNames == nullptr || !logsOk ||
        eventLog == nullptr || beaconSlots == nullptr ||
        sixGhz == nullptr)
    {
        Serial.println("FATAL: table allocation failed, restarting");

        delay(5000);

        ESP.restart();
    }

    Serial.printf("Internal heap after tables: %.1f kB\r\n",
                  ESP.getFreeHeap() / 1024.0);
}

bool allocateHistoryRing(struct HistoryRing &h, const char *name,
                         uint32_t rows, uint32_t caps)
{
    size_t bytes = (size_t)rows * sizeof(WaterfallEntry);

    h.rows = (WaterfallEntry *)heap_caps_malloc(bytes, caps);

    if (h.rows == nullptr)
        return false;

    h.capacity = rows;

    noteAllocation(name, bytes, caps == MALLOC_CAP_SPIRAM);

    for (uint32_t i = 0; i < rows; i++)
    {
        h.rows[i].timestamp = 0;

        for (uint16_t c = 0; c < TOTAL_CHANNELS; c++)
            h.rows[i].rssi[c] = -127;
    }

    Serial.printf("  %-18s %6u rows, %.1f kB\r\n", name, (unsigned)rows,
                  bytes / 1024.0);

    return true;
}

void allocateHistory()
{
    Serial.println();
    Serial.println("Allocating waterfall history...");

    Serial.printf("Entry size: %u bytes\r\n",
                  (unsigned)sizeof(WaterfallEntry));

    if (!allocateHistoryRing(historyFine, "waterfall, sweeps",
                             HISTORY_FINE_ROWS, MALLOC_CAP_SPIRAM))
    {
        Serial.println("PSRAM allocation failed, using internal heap.");

        if (!allocateHistoryRing(historyFine, "waterfall, sweeps",
                                 HISTORY_SIZE_FALLBACK, MALLOC_CAP_DEFAULT))
        {
            Serial.println("History allocation FAILED, waterfall disabled.");
        }

        return;
    }

    /*
     * The rows per step: as many hours as PSRAM can give while
     * HISTORY_LEAVE stays free for the frame logs, never more than
     * HISTORY_SPAN_MS nor, while it fits at all, less than
     * HISTORY_SPAN_MIN_MS.
     */
    uint32_t maxRows = HISTORY_SPAN_MS / HISTORY_STEP_MS;
    uint32_t minRows = HISTORY_SPAN_MIN_MS / HISTORY_STEP_MS;
    size_t   psram   = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t   spare   = psram > HISTORY_LEAVE ? psram - HISTORY_LEAVE : 0;
    uint32_t rows    = min((size_t)maxRows, spare / sizeof(WaterfallEntry));

    if (rows < minRows)
        rows = minRows;

    while (!allocateHistoryRing(historyCoarse, "waterfall, 5 s", rows,
                                MALLOC_CAP_SPIRAM))
    {
        rows /= 2;

        if (rows < 60)
        {
            Serial.println("No rows per step, long ranges come from the "
                           "rows per sweep.");
            break;
        }
    }

    historyReset();

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

#if HEAP_PROBE_RX_ALL
    filter.filter_mask |= WIFI_PROMIS_FILTER_MASK_CTRL |
                          WIFI_PROMIS_FILTER_MASK_FCSFAIL;

    wifi_promiscuous_filter_t ctrl;

    ctrl.filter_mask = WIFI_PROMIS_CTRL_FILTER_MASK_ALL;
    esp_wifi_set_promiscuous_ctrl_filter(&ctrl);
#endif

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

    /*
     * Arduino starts the driver in WiFi.mode() with 32 receive
     * buffers and no way to say otherwise. Started here first with
     * Arduino's values but the number of buffers; Arduino's own
     * esp_wifi_init() then finds it running and leaves it as it is.
     */
    Network.begin();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    cfg.static_tx_buf_num  = 0;
    cfg.dynamic_tx_buf_num = 32;
    cfg.tx_buf_type        = 1;
    cfg.cache_tx_buf_num   = 4;
    cfg.static_rx_buf_num  = 4;
    cfg.dynamic_rx_buf_num = WIFI_RX_BUFFERS;

    esp_err_t initErr = esp_wifi_init(&cfg);

    Serial.printf("WiFi receive buffers: %d (0x%X)\r\n",
                  WIFI_RX_BUFFERS, initErr);

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
