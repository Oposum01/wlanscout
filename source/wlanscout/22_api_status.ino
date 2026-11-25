// SPDX-License-Identifier: GPL-2.0-only WITH AdditionRef-wlanscout-linking-exception
// SPDX-FileCopyrightText: 2026 Dr.-Ing. Simon Wunderlich Systementwicklung & Beratung

// ============================================================
// API: status
//
// Deliberately tiny and answered in every mode. The browser
// polls this one to learn which mode the device is in, so a
// heavy endpoint stalling can never leave the page guessing.
// ============================================================

void sendStatusJSON()
{
    outBegin();

    outStr("{\"version\":");
    outJSON(FW_VERSION);

    outStr(",\"mode\":");
    outNum(scanMode);

    // The newest configuration change, see /api/changes
    outStr(",\"cfgseq\":");
    outNum(cfgSeq);
    outStr(",\"cfgver\":");
    outNum(cfgVersion);
    outStr(",\"boot\":");
    outNum(bootCount);

    outStr(",\"zbscan\":");
    outNum(zbScanMode);

    outStr(",\"heap\":");
    outNum(ESP.getFreeHeap());

    outStr(",\"oui\":");
    outNum(OUI_FULL_COUNT + OUI_MAM_COUNT + OUI_MAS_COUNT);
    outStr(",\"oui28\":");
    outNum(OUI_MAM_COUNT);
    outStr(",\"oui36\":");
    outNum(OUI_MAS_COUNT);

    outStr(",\"clients\":");
    outNum(countWebClients());

    outStr(",\"sensor\":");
    outJSON(sensorName);

    // What the scanner is doing right now, ready to display
    outStr(",\"scan\":");
    {
        char buffer[32];

        // Fixed width so the badge does not change length
        if (scanMode == MODE_BLE)
        {
            snprintf(buffer, sizeof(buffer), "BLE %-7s",
                     bleActiveScan ? "active" : "passive");
        }
        else if (scanMode == MODE_ZB)
        {
            snprintf(buffer, sizeof(buffer), "802.15.4 ch %3u",
                     (unsigned)zbChannelNumber(zbChannelIndex));
        }
        else
        {
            snprintf(buffer, sizeof(buffer), "ch %3u",
                     (unsigned)getChannelNumber(currentChannelIndex));
        }

        outJSON(buffer);
    }

    outStr(",\"ip\":");
    outJSON(ethIpText().c_str());

    outStr(",\"clock\":");
    outNum(pcapClockSource);

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

    outStr(",\"static\":");
    outNum(ethStaticActive ? 1 : 0);

    outStr(",\"link\":");
    outNum(ethLinkUp ? 1 : 0);

#if ENABLE_ETHERNET
    outStr(",\"speed\":");
    outNum(ETH.linkSpeed());

    outStr(",\"duplex\":");
    outNum(ETH.fullDuplex() ? 1 : 0);
#endif

    outStr("}");

    outEnd();
}
