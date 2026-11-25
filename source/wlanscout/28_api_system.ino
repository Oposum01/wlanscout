// SPDX-License-Identifier: GPL-2.0-only WITH AdditionRef-wlanscout-linking-exception
// SPDX-FileCopyrightText: 2026 Dr.-Ing. Simon Wunderlich Systementwicklung & Beratung

// A plain reboot, the same firmware again; rebootLoop() restarts
// once the answer is out. In every build, with or without OTA.
uint32_t rebootAt = 0;           // millis(), 0 = none due

void handleReboot()
{
    logRequest("reboot requested");

    reqSend(200, "application/json", "{\"ok\":1}");

    rebootAsked = REBOOT_ASKED;
    rebootAt = millis() + 1500;
}

void rebootLoop()
{
    if (rebootAt != 0 && (int32_t)(millis() - rebootAt) >= 0)
    {
        catalogSave();
        bleHitsSave();

        ESP.restart();
    }
}

void handleAssocHistoryClear()
{
    uint32_t kept = 0;

    if (assocHistory != nullptr)
    {
        for (int i = 0; i < MAX_ASSOC_HISTORY; i++)
        {
            if (assocHistory[i].valid)
                kept++;

            assocHistory[i].valid = false;
        }
    }

    assocHistoryDropped = 0;

    logRequest("association history cleared, %u stations discarded",
               (unsigned)kept);

    reqSend(200, "application/json", "{\"ok\":1}");
}

/*
 * Flush button above the BSSID and the station graph: empties the
 * live table, which then fills again from what is heard next.
 * ?what=networks takes the 6 GHz list with it, whose entries hang
 * on a reporting AP; ?what=stations the names learned from probe
 * and association requests. The association history, the capture
 * buffers and the IE catalog stay, they have their own buttons.
 */
void handleFlush()
{
    String what = reqArg("what");

    uint32_t dropped = 0;

    if (what == "networks")
    {
        for (int i = 0; i < MAX_NETWORKS; i++)
        {
            if (networks[i].valid)
                dropped++;

            networks[i].valid = false;
        }

        if (sixGhz != nullptr)
            for (int i = 0; i < MAX_SIXG_BSS; i++)
                sixGhz[i].valid = false;

        logRequest("network table flushed, %u BSSIDs discarded",
                   (unsigned)dropped);
    }
    else if (what == "stations")
    {
        for (int i = 0; i < MAX_CLIENTS; i++)
        {
            if (clients[i].valid)
                dropped++;

            clients[i].valid = false;
        }

        if (bssNames != nullptr)
            for (int i = 0; i < MAX_BSS_NAMES; i++)
                bssNames[i].valid = false;

        for (int i = 0; i < MAX_AWDL; i++)
            awdlDevices[i].valid = false;

        logRequest("station table flushed, %u stations discarded",
                   (unsigned)dropped);
    }
    else
    {
        reqSend(400, "application/json", "{\"ok\":0}");
        return;
    }

    reqSend(200, "application/json", "{\"ok\":1}");
}

void handleTime()
{
    if (reqHasArg("epoch"))
    {
        int64_t epoch = (int64_t)reqArg("epoch").toInt();

        // NTP outranks the browser clock, the browser an AP
        if (pcapClockSource != 2)
        {
            clockOffsetUs =
                epoch * 1000000LL - esp_timer_get_time();

            pcapClockSource = 1;

            Serial.println("Clock taken from the browser");
        }
    }

    reqSend(200, "application/json", "{\"ok\":1}");
}

// ============================================================
// API: system
//
// Everything the settings page shows: addressing as handed
// out by DHCP or configured statically, memory, where the
// tables ended up and what the counters are at.
// ============================================================

// ============================================================
// CPU, loop and queue statistics
//
// FreeRTOS counts the run time of every task in µs (run time
// stats on esp_timer, built into the Arduino core's libraries).
// Every 10 s the counters are compared with the last ones: the
// share of each task in that window, the load being what the idle
// task did not get. Interrupts count for the task they interrupt.
//
// Beside it the longest round of loop() and the longest request
// it answered, the highest fill of the management queue, and the
// chip's own temperature (not the air around it).
// ============================================================

#define STATS_WINDOW_MS 10000
#define STATS_TASKS     32

struct TaskShare
{
    char     name[16];
    uint16_t share;          // tenths of a percent of the window
    uint32_t stackFree;      // bytes never used since its start
};

struct StatsWindow
{
    bool     valid;
    uint16_t load;           // tenths of a percent
    uint8_t  taskCount;
    TaskShare tasks[STATS_TASKS];
    uint32_t loopUs;         // longest loop() round
    uint32_t httpUs;         // longest request answered
    char     httpUri[48];
    uint8_t  queueFill;
};

StatsWindow statsLast;       // the last complete window

uint32_t loopUsWin  = 0, loopUsBoot = 0;
char     statsPath[40] = "";     // the API route answered last
uint32_t httpUsWin  = 0, httpUsBoot = 0;
char     httpUriWin[48] = "", httpUriBoot[48] = "";

static TaskStatus_t *statsNow  = nullptr;
static uint32_t      statsPrevNum[STATS_TASKS];
static uint32_t      statsPrevRun[STATS_TASKS];
static uint8_t       statsPrevCount = 0;
static uint32_t      statsPrevTotal = 0;
static uint32_t      statsAt = 0;

static temperature_sensor_handle_t chipTemp = nullptr;
float chipTempC = NAN;

void statsSetup()
{
    statsNow = (TaskStatus_t *)heap_caps_malloc(
        sizeof(TaskStatus_t) * STATS_TASKS, MALLOC_CAP_SPIRAM);

    // 20 to 100 °C: what a chip in a closed housing reaches
    temperature_sensor_config_t cfg =
        TEMPERATURE_SENSOR_CONFIG_DEFAULT(20, 100);

    if (temperature_sensor_install(&cfg, &chipTemp) != ESP_OK)
        chipTemp = nullptr;
    else if (temperature_sensor_enable(chipTemp) != ESP_OK)
    {
        temperature_sensor_uninstall(chipTemp);
        chipTemp = nullptr;
    }
}

// The longest request of the window and since the start
void statsRequest(uint32_t us, const char *uri)
{
    if (us > httpUsWin)
    {
        httpUsWin = us;
        strlcpy(httpUriWin, uri, sizeof(httpUriWin));
    }

    if (us > httpUsBoot)
    {
        httpUsBoot = us;
        strlcpy(httpUriBoot, uri, sizeof(httpUriBoot));
    }
}

// Called at the top of every loop(): the time since the last round
void statsLoopRound()
{
    static int64_t last = 0;

    int64_t now = esp_timer_get_time();

    if (last != 0)
    {
        uint32_t us = (uint32_t)(now - last);

        if (us > loopUsWin)
            loopUsWin = us;

        if (us > loopUsBoot)
            loopUsBoot = us;
    }

    last = now;
}

void statsLoop()
{
    if (statsNow == nullptr || millis() - statsAt < STATS_WINDOW_MS)
        return;

    statsAt = millis();

    uint32_t total = 0;

    UBaseType_t n = uxTaskGetSystemState(statsNow, STATS_TASKS, &total);

    if (chipTemp != nullptr)
    {
        float c;

        if (temperature_sensor_get_celsius(chipTemp, &c) == ESP_OK)
            chipTempC = c;
    }

    uint32_t span = total - statsPrevTotal;

    if (statsPrevCount > 0 && span > 0 && n > 0)
    {
        StatsWindow &w = statsLast;

        w.taskCount = 0;
        uint64_t idle = 0;

        for (UBaseType_t i = 0; i < n; i++)
        {
            const TaskStatus_t &s = statsNow[i];

            uint32_t before = 0;
            bool     known  = false;

            for (uint8_t j = 0; j < statsPrevCount; j++)
                if (statsPrevNum[j] == s.xTaskNumber)
                {
                    before = statsPrevRun[j];
                    known  = true;
                    break;
                }

            // A task started in the window counts from zero
            uint32_t ran = s.ulRunTimeCounter - (known ? before : 0);

            if (strncmp(s.pcTaskName, "IDLE", 4) == 0)
                idle += ran;

            if (w.taskCount < STATS_TASKS)
            {
                TaskShare &t = w.tasks[w.taskCount++];

                strlcpy(t.name, s.pcTaskName, sizeof(t.name));
                t.share = (uint16_t)std::min<uint64_t>(
                    1000, (uint64_t)ran * 1000 / span);
                t.stackFree = s.usStackHighWaterMark;
            }
        }

        std::sort(w.tasks, w.tasks + w.taskCount,
                  [](const TaskShare &a, const TaskShare &b)
                  { return a.share > b.share; });

        w.load = idle >= span ? 0
               : (uint16_t)(1000 - idle * 1000 / span);

        w.loopUs    = loopUsWin;
        w.httpUs    = httpUsWin;
        strlcpy(w.httpUri, httpUriWin, sizeof(w.httpUri));
        w.queueFill = mgmtFillWin;
        w.valid     = true;
    }

    statsPrevCount = 0;

    for (UBaseType_t i = 0; i < n && i < STATS_TASKS; i++)
    {
        statsPrevNum[statsPrevCount] = statsNow[i].xTaskNumber;
        statsPrevRun[statsPrevCount] = statsNow[i].ulRunTimeCounter;
        statsPrevCount++;
    }

    statsPrevTotal = total;

    loopUsWin   = 0;
    httpUsWin   = 0;
    httpUriWin[0] = 0;
    mgmtFillWin = 0;
}

void outStatsJSON()
{
    const StatsWindow &w = statsLast;

    if (w.valid)
    {
        outStr(",\"cpu\":{\"win\":");
        outNum(STATS_WINDOW_MS / 1000);

        outStr(",\"load\":");
        outNum(w.load);

        outStr(",\"tasks\":[");

        for (uint8_t i = 0; i < w.taskCount; i++)
        {
            outStr(i ? ",[" : "[");
            outJSON(w.tasks[i].name);
            outStr(",");
            outNum(w.tasks[i].share);
            outStr(",");
            outNum(w.tasks[i].stackFree);
            outStr("]");
        }

        outStr("]}");
    }

    if (!isnan(chipTempC))
    {
        char buf[16];

        snprintf(buf, sizeof(buf), "%.1f", chipTempC);

        outStr(",\"temp\":");
        outStr(buf);
    }

    if (crashInfo.valid)
    {
        char hex[12];

        outStr(",\"crash\":{\"task\":");
        outJSON(crashInfo.task);
        outStr(",\"cause\":");
        outJSON(crashCauseText());
        snprintf(hex, sizeof(hex), "0x%08lx", (unsigned long)crashInfo.pc);
        outStr(",\"pc\":");
        outJSON(hex);
        snprintf(hex, sizeof(hex), "0x%08lx", (unsigned long)crashInfo.ra);
        outStr(",\"ra\":");
        outJSON(hex);
        snprintf(hex, sizeof(hex), "0x%08lx", (unsigned long)crashInfo.mtval);
        outStr(",\"mtval\":");
        outJSON(hex);
        outStr(",\"mcause\":");
        outNum(crashInfo.mcause);
        outStr(",\"size\":");
        outNum(crashInfo.size);
        outStr(",\"elf\":");
        outJSON(crashInfo.elf);
        outStr(",\"same\":");
        outNum(crashInfo.sameFirmware ? 1 : 0);
        outStr("}");
    }

    // [loop round µs, request µs, its path, highest queue fill]
    outStr(",\"loop\":{\"win\":[");
    outNum(w.valid ? w.loopUs : 0);
    outStr(",");
    outNum(w.valid ? w.httpUs : 0);
    outStr(",");
    outJSON(w.valid ? w.httpUri : "");
    outStr(",");
    outNum(w.valid ? w.queueFill : 0);

    outStr("],\"boot\":[");
    outNum(loopUsBoot);
    outStr(",");
    outNum(httpUsBoot);
    outStr(",");
    outJSON(httpUriBoot);
    outStr(",");
    outNum(mgmtFillBoot);

    outStr("],\"queue\":");
    outNum(MGMT_QUEUE_SIZE - 1);

    // [answers given up, seconds since the last, its client]
    outStr(",\"givenup\":[");
    outNum(txGivenUp);
    outStr(",");
    outNum(txGivenUp ? (millis() - txGivenUpAt) / 1000 : 0);
    outStr(",");
    outJSON(txGivenUpWho);
    outStr("]}");
}

/*
 * One frame log for /api/system: frames and their octets, the
 * memory it holds and the most it may take, frames stored since the
 * last clear and those dropped with a reused chunk. The ring's keys
 * have no prefix ("count", "bytes", ..), the association ring's
 * "assoc" ("assoc", "assocbytes", ..), the beacon versions' "bv".
 */
void outFrameLog(const char *prefix, const struct FrameLog &log)
{
    const char *keys[] = { "count", "bytes", "mem", "max", "total",
                           "dropped" };
    uint32_t    vals[] = { log.frameCount, log.frameBytes,
                           (uint32_t)log.chunkCount * FRAME_CHUNK_BYTES,
                           (uint32_t)log.maxChunks * FRAME_CHUNK_BYTES,
                           log.framesStored, log.framesDropped };

    for (int i = 0; i < 6; i++)
    {
        if (i)
            outStr(",");

        outStr("\"");
        outStr(prefix);

        // "assoc" alone for the count, as before
        if (!(*prefix && i == 0))
            outStr(keys[i]);

        outStr("\":");
        outNum(vals[i]);
    }
}

void sendSystemJSON()
{
    outBegin();

    outStr("{\"version\":");
    outJSON(FW_VERSION);

    outStr(",\"board\":");
    outJSON(BOARD_NAME);

    outStr(",\"boardid\":");
    outJSON(BOARD_ID);

    outStr(",\"uptime\":");
    outNum((long)(esp_timer_get_time() / 1000000ULL));

    // The device time as a number, for the page to count on from
    outStr(",\"now\":");
    outNum((long)deviceEpoch());

    outStr(",\"oui\":");
    outNum(OUI_FULL_COUNT + OUI_MAM_COUNT + OUI_MAS_COUNT);
    outStr(",\"oui28\":");
    outNum(OUI_MAM_COUNT);
    outStr(",\"oui36\":");
    outNum(OUI_MAS_COUNT);

    outStr(",\"sensor\":");
    outJSON(sensorName);

    outStr(",\"sensorid\":");
    outJSON(sensorMacText().c_str());

    // The flash found on the board
    outStr(",\"flash\":");
    outNum(ESP.getFlashChipSize());

    outStr(",\"boot\":");
    outNum(bootCount);

    outStr(",\"reset\":");
    outJSON(resetReasonText());

#if ENABLE_OTA
    outOtaJSON();
#endif

    // The IE catalog, see the IE catalog view
    outStr(",\"catalog\":{\"enabled\":");
    outNum(catalogEnabled ? 1 : 0);
    outStr(",\"fs\":");
    outNum(catalogFsOk ? 1 : 0);
    outStr(",\"rows\":");
    outNum(catalogCount);
    outStr(",\"max\":");
    outNum(CATALOG_MAX);
    outStr(",\"used\":");
    outNum(catalogFsOk ? (long)LittleFS.usedBytes() : 0);
    outStr(",\"total\":");
    outNum(catalogFsOk ? (long)LittleFS.totalBytes() : 0);
    outStr("}");

    outStr(",\"clock\":");
    outNum(pcapClockSource);

    // Which AP the clock came from, and when it was last renewed
    if (pcapClockSource == 3)
    {
        char mac[18];

        snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
                 wifiClockBssid[0], wifiClockBssid[1],
                 wifiClockBssid[2], wifiClockBssid[3],
                 wifiClockBssid[4], wifiClockBssid[5]);

        outStr(",\"clockap\":");
        outJSON(mac);

        outStr(",\"clockapn\":");
        outNum(wifiClockApCount);

        outStr(",\"clockapago\":");
        outNum((long)((millis() - wifiClockLastSet) / 1000));
    }

    outStr(",\"utc\":");
    {
        char buffer[24];

        formatStamp(deviceEpoch(), buffer, sizeof(buffer));

        outJSON(buffer);
    }

    outStr(",\"tz\":");
    outNum(tzMinutes);

    outStr(",\"dst\":");
    outNum(tzDst);

    outStr(",\"tzauto\":");
    outNum(tzAuto ? 1 : 0);

    if (tzAuto && tzAutoCc[0])
    {
        outStr(",\"tzcc\":");
        outJSON(tzAutoCc);
        outStr(",\"tzccn\":");
        outNum(tzAutoCount);
        outStr(",\"tzccof\":");
        outNum(tzAutoOf);
        outStr(",\"tzccok\":");
        outNum(tzAutoKnown ? 1 : 0);
    }

    outStr(",\"tzlabel\":");
    {
        char label[12];
        formatTzLabel(label, sizeof(label));
        outJSON(label);
    }

    outStr(",\"ntpsync\":");
    {
        if (ntpLastSync == 0)
        {
            outJSON("");
        }
        else
        {
            char buffer[24];

            formatStamp(ntpLastSync, buffer, sizeof(buffer));

            outJSON(buffer);
        }
    }

    outStr(",\"ntpago\":");
    {
        uint32_t nowSec = deviceEpoch();

        outNum(ntpLastSync ? (long)(nowSec - ntpLastSync) : -1);
    }

    outStr(",\"ntpadjust\":");
    outNum(ntpLastAdjust);

    outStr(",\"ntpcount\":");
    outNum(ntpSyncCount);

    outStr(",\"ntpinterval\":");
    outNum(NTP_SYNC_INTERVAL_MS / 1000);

    outStr(",\"ntpserver\":");
    outJSON(ntpDhcpServer[0] ? ntpDhcpServer : NTP_SERVER1);

    // 1 when the server above came with the DHCP lease
    outStr(",\"ntpdhcp\":");
    outNum(ntpDhcpServer[0] ? 1 : 0);

    // ---- network: left out without Ethernet ----

#if ENABLE_ETHERNET

    outStr(",\"net\":{\"ip\":");
    outJSON(ETH.localIP().toString().c_str());

    outStr(",\"gw\":");
    outJSON(ETH.gatewayIP().toString().c_str());

    outStr(",\"mask\":");
    outJSON(ETH.subnetMask().toString().c_str());

    outStr(",\"dns\":");
    outJSON(ETH.dnsIP().toString().c_str());

    outStr(",\"mac\":");
    outJSON(sensorMacText().c_str());

    // 0 on USB alone: the MAC above is then only derived
    outStr(",\"eth\":");
    outNum(ethPresent ? 1 : 0);

    outStr(",\"speed\":");
    outNum(ETH.linkSpeed());

    outStr(",\"duplex\":");
    outNum(ETH.fullDuplex() ? 1 : 0);

    outStr(",\"link\":");
    outNum(ethLinkUp ? 1 : 0);

    outStr(",\"static\":");
    outNum(ethStaticActive ? 1 : 0);

    outStr(",\"dhcptries\":");
    outNum(dhcpAttempts);

    outStr(",\"hostname\":");
    outJSON(dhcpHostname);

    outStr(",\"dhcpstate\":");
    outJSON(dhcpStatusText());

    if (dhcpLeaseStateText(dhcpLease.state) != nullptr)
    {
        outStr(",\"lease\":{\"state\":");
        outJSON(dhcpLeaseStateText(dhcpLease.state));

        if (dhcpLease.ackAt != 0)
        {
            uint32_t used = (millis() - dhcpLease.ackAt) / 1000;

            outStr(",\"time\":");
            outNum(dhcpLease.seconds);
            outStr(",\"left\":");
            outNum(used < dhcpLease.seconds ? dhcpLease.seconds - used : 0);
            outStr(",\"ack\":");
            outNum(used);
            outStr(",\"server\":");
            outJSON(IPAddress(dhcpLease.server).toString().c_str());
        }

        // lwIP's coarse DHCP timer ticks every
        // DHCP_COARSE_TIMER_SECS, one second in ESP-IDF
        if (dhcpLease.state == DHCP_STATE_BOUND && dhcpLease.renewIn != 0)
        {
            outStr(",\"renew\":");
            outNum((long)dhcpLease.renewIn * DHCP_COARSE_TIMER_SECS);
        }

        outStr(",\"renewals\":");
        outNum(dhcpLease.renewals);
        outStr("}");
    }

    outStr("}");
#endif

    // ---- graphs recorded: [bssid, kept, epoch, up, samples] ----

    outStr(",\"watch\":[");
    watchListJson();
    outStr("]");

    // ---- memory ----

    outStr(",\"mem\":{\"heap\":");
    outNum(ESP.getFreeHeap());

    outStr(",\"heapmin\":");
    outNum(ESP.getMinFreeHeap());

    outStr(",\"heaptotal\":");
    outNum(ESP.getHeapSize());

    outStr(",\"block\":");
    outNum(heap_caps_get_largest_free_block(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

    outStr(",\"psramtotal\":");
    outNum(ESP.getPsramSize());

    outStr(",\"psramfree\":");
    outNum(heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    outStr("}");

    outStatsJSON();

    // ---- allocations ----

    outStr(",\"alloc\":[");

    for (uint8_t i = 0; i < allocInfoCount; i++)
    {
        if (i)
            outStr(",");

        outStr("{\"name\":");
        outJSON(allocInfo[i].name);

        outStr(",\"bytes\":");
        outNum(allocInfo[i].bytes);

        outStr(",\"psram\":");
        outNum(allocInfo[i].psram ? 1 : 0);

        outStr("}");
    }

    outStr("]");

    // ---- counters ----

    outStr(",\"counters\":{\"sweeps\":");
    outNum(sweepCount);

    outStr(",\"mgmtdrops\":");
    outNum(mgmtDrops);

    outStr(",\"datadrops\":");
    outNum(dataDrops);

    outStr(",\"fcs\":");
    outNum(fcsErrors);

    outStr(",\"selected\":");
    outNum(countSelectedChannels());

    outStr(",\"plan\":");
    outNum(planCount);

    outStr(",\"fast\":");
    outNum(fastSweep ? 1 : 0);

    outStr(",\"mode\":");
    outNum(scanMode);

    outStr(",\"keepmode\":");
    outNum(keepMode ? 1 : 0);

    outStr("}");

    // ---- capture ----

    outStr(",\"pcap\":{");

    outFrameLog("", ringLog);
    outStr(",");
    outFrameLog("assoc", assocLog);
    outStr(",");
    outFrameLog("bv", versionLog);
    outStr(",");
    outFrameLog("ble", bleLog);

    outStr(",\"blehits\":");
    outNum(countBleHits());

    outStr(",\"beacons\":");
    outNum(countBeaconSlots());

    outStr(",\"beaconslots\":");
    outNum(MAX_BEACON_SLOTS);

    outStr(",\"beacontotal\":");
    outNum(beaconTotal);

    outStr(",\"chunk\":");
    outNum(FRAME_CHUNK_BYTES);

    outStr(",\"enabled\":");
    outNum(pcapEnabled ? 1 : 0);

    outStr(",\"mask\":");
    outNum(pcapMask);

    outStr("}}");

    outEnd();
}

// ============================================================
