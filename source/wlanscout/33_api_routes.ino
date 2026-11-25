// SPDX-License-Identifier: GPL-2.0-only WITH AdditionRef-wlanscout-linking-exception
// SPDX-FileCopyrightText: 2026 Dr.-Ing. Simon Wunderlich Systementwicklung & Beratung

// ============================================================
// API routes, for the web server and the USB port
// ============================================================

struct ApiRoute
{
    const char *path;
    void (*handler)();
};

// Every GET endpoint; the firmware upload is a POST and stays on
// Ethernet
const ApiRoute apiRoutes[] = {
    { "/api/status",        sendStatusJSON },
    { "/api/system",        sendSystemJSON },
    { "/api/log",           sendLogJSON },
    { "/api/export",        sendExportJSON },
    { "/api/live",          sendLiveJSON },
    { "/api/history",       sendHistoryJSON },
    { "/api/networks",      sendNetworksJSON },
    { "/api/clients",       sendClientsJSON },
    { "/api/config",        handleConfig },
    { "/api/ble",           sendBleJSON },
    { "/api/zb",            sendZbJSON },
    { "/api/zbwaterfall",   sendZbWaterfallJSON },
    { "/api/pcap",          sendPcap },
    { "/api/beaconversions", sendBeaconVersions },
    { "/api/blehits",       sendBleHitsJSON },
    { "/api/blehitsclear",  handleBleHitsClear },
    { "/api/pcapclear",     handlePcapClear },
    { "/api/plan",          sendPlanJSON },
    { "/api/assochistory",  sendAssocHistoryJSON },
    { "/api/assocclear",    handleAssocHistoryClear },
    { "/api/flush",         handleFlush },
    { "/api/catalog",       sendCatalogJSON },
    { "/api/catalogclear",  handleCatalogClear },
    { "/api/catalog.pcap",  sendCatalogPcap },
    { "/api/time",          handleTime },
    { "/api/mode",          handleMode },
#if ENABLE_OTA
    { "/api/otarollback",   handleOtaRollback },
#endif
    { "/api/reboot",        handleReboot },
    { "/api/coredump",      handleCoreDump },
    { "/api/changes",       sendChangesJSON },
    { "/api/cfgmute",       handleCfgMute },
    { "/api/watch",         handleWatch },
    { "/api/snapshot",      handleSnapshot }
};

void runRoute(const struct ApiRoute &route)
{
    // Every request counts, also those answered with reqSend()
    // alone (handleMode, handleTime, ...): see lastRequestServed
    lastRequestServed = millis();

    // For the longest request in the CPU statistics: the web
    // server has forgotten the path once the answer is out
    strlcpy(statsPath, route.path, sizeof(statsPath));

#if HEAP_PROBE
    runProbed(route.path, route.handler);
#else
    route.handler();
#endif
}

#if ENABLE_ETHERNET

// The page, the API on port 80, uploads; from setup()
void webServerSetup()
{
    server.on("/", HTTP_GET, PROBED(handleRoot));
    server.on("/favicon.svg", HTTP_GET, PROBED(handleFavicon));
    for (const ApiRoute &route : apiRoutes)
        server.on(route.path, HTTP_GET, [&route]() { runRoute(route); });

#if ENABLE_OTA
    server.on("/api/ota", HTTP_POST, handleOtaDone, handleOtaUpload);
#endif
    server.on("/api/snapshotimport", HTTP_POST, handleImportDone,
              handleImportUpload);

    server.onNotFound(PROBED(handleNotFound));

    server.begin();

    Serial.println("HTTP server started");
}

#endif
