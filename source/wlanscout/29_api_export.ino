// SPDX-License-Identifier: GPL-2.0-only WITH AdditionRef-wlanscout-linking-exception
// SPDX-FileCopyrightText: 2026 Dr.-Ing. Simon Wunderlich Systementwicklung & Beratung

// ============================================================
// API: export
//
// Everything this sensor knows, in one document, meant to be
// merged with the output of other sensors covering the same
// area. Three things matter for that and are different from
// the endpoints the web interface uses:
//
//   - every timestamp is absolute UTC epoch seconds, not an
//     age, because ages from different sensors cannot be
//     lined up
//   - the sensor identifies itself by its Ethernet MAC, which
//     is stable across reboots, plus an optional name
//   - sections and a "since" cut-off keep the document small
//     enough to be polled often
//
// Captured frames are not part of this, they stay behind
// /api/pcap.
// ============================================================

bool wantSection(const String &sections, const char *name)
{
    if (sections.length() == 0)
        return true;

    return (sections.indexOf(name) >= 0);
}

void sendExportJSON()
{
    String sections = reqHasArg("sections")
                          ? reqArg("sections")
                          : String("");

    uint32_t since = reqHasArg("since")
                         ? (uint32_t)reqArg("since").toInt()
                         : 0;

    int64_t  upMs     = esp_timer_get_time() / 1000;
    uint32_t now      = (uint32_t)upMs;     // millis()
    uint32_t nowEpoch = deviceEpoch();
    int64_t  clockMs  = clockOffsetUs / 1000;

    /*
     * millis to epoch for a stored timestamp: its uptime plus the
     * clock's offset. Taken from the epoch now and the age, each cut
     * to whole seconds, it came out a second apart from one export
     * to the next.
     */
    #define EPOCH_OF(ms) \
        ((long)((upMs - (uint32_t)(now - (ms)) + clockMs) / 1000))

    refreshPcapCounts();

    outBegin();

    outStr("{\"schema\":");
    outNum(EXPORT_SCHEMA);

    // ---- sensor ----

    outStr(",\"sensor\":{\"id\":");
    outJSON(sensorMacText().c_str());

    outStr(",\"name\":");
    outJSON(sensorName);

    outStr(",\"ip\":");
    outJSON(ethIpText().c_str());

    outStr(",\"fw\":");
    outJSON(FW_VERSION);

    outStr(",\"uptime\":");
    outNum((long)(esp_timer_get_time() / 1000000LL));

    outStr(",\"time\":");
    outNum((long)nowEpoch);

    outStr(",\"clock\":");
    outNum(pcapClockSource);

    outStr(",\"mode\":");
    outNum(scanMode);

    outStr("}");

    // ---- scan configuration ----

    outStr(",\"scan\":{\"selected\":");
    outNum(countSelectedChannels());

    outStr(",\"plan\":");
    outNum(planCount);

    outStr(",\"fast\":");
    outNum(fastSweep ? 1 : 0);

    outStr(",\"dwell\":");
    outNum(CHANNEL_DWELL_MS);

    outStr(",\"sweeps\":");
    outNum(sweepCount);

    outStr(",\"channels\":[");

    {
        bool first = true;

        for (uint16_t i = 0; i < TOTAL_CHANNELS; i++)
        {
            if (!channelSelected[i])
                continue;

            if (!first)
                outStr(",");

            first = false;

            outNum(getChannelNumber(i));
        }
    }

    outStr("]}");

    // ---- networks ----

    if (wantSection(sections, "networks"))
    {
        outStr(",\"networks\":[");

        bool first = true;

        for (int i = 0; i < MAX_NETWORKS; i++)
        {
            WiFiNetwork &n = networks[i];

            if (!n.valid)
                continue;

            if (since && (uint32_t)EPOCH_OF(n.lastSeen) < since)
                continue;

            if (!first)
                outStr(",");

            first = false;

            outStr("{\"bssid\":");
            outMac(n.bssid);

            outStr(",\"ssid\":");
            outJSON(n.ssid);

            uint8_t vblk;

            outStr(",\"vendor\":");
            outJSON(lookupBssidVendor(n.bssid, n.mlo, &vblk));
            outVendorBlock(vblk);

            outStr(",\"ch\":");
            outNum(n.channel);

            outStr(",\"center\":");
            outNum(n.centerChannel);

            if (n.centerChannel2)
            {
                outStr(",\"center2\":");
                outNum(n.centerChannel2);
            }

            outStr(",\"bw\":");
            outNum(n.bandwidth);

            outStr(",\"freq\":");
            outNum(pcapChannelFreq(n.centerChannel));

            outStr(",\"rssi\":");
            outNum(n.rssi);

            outStr(",\"std\":");
            outNum(n.standard);

            outStr(",\"modes\":");
            outNum(n.phyModes);

            outStr(",\"sec\":");
            outJSON(n.security);

            outStr(",\"akm\":");
            outJSON(n.akm);

            outStr(",\"pair\":");
            outJSON(n.pairwise);

            outStr(",\"group\":");
            outJSON(n.groupCipher);

            outStr(",\"mfpc\":");
            outNum(n.mfpCapable ? 1 : 0);

            outStr(",\"mfpr\":");
            outNum(n.mfpRequired ? 1 : 0);

            outStr(",\"wps\":");
            outNum(n.wps ? 1 : 0);

            outStr(",\"mesh\":");
            outNum(n.mesh ? 1 : 0);

            outStr(",\"vht24\":");
            outNum(n.vht24 ? 1 : 0);

            outStr(",\"resolved\":");
            outNum(n.ssidResolved ? 1 : 0);

            outStr(",\"stations\":");
            outNum(n.stations);

            outStr(",\"util\":");
            outNum(n.channelUtil);

            outStr(",\"beaconint\":");
            outNum(n.beaconInterval);

            outStr(",\"country\":");
            outJSON(n.country);

            outStr(",\"beacons\":");
            outNum(n.beacons);

            outStr(",\"first\":");
            outNum(EPOCH_OF(n.firstSeen));

            outStr(",\"seen\":");
            outNum(EPOCH_OF(n.lastSeen));

            outStr("}");
        }

        outStr("]");
    }

    // ---- stations ----

    if (wantSection(sections, "clients"))
    {
        outStr(",\"clients\":[");

        bool first = true;

        for (int i = 0; i < MAX_CLIENTS; i++)
        {
            ClientEntry &c = clients[i];

            if (!c.valid)
                continue;

            if (since && (uint32_t)EPOCH_OF(c.lastSeen) < since)
                continue;

            if (!first)
                outStr(",");

            first = false;

            outStr("{\"mac\":");
            outMac(c.mac);

            uint8_t vblk;

            outStr(",\"vendor\":");
            outJSON(lookupVendor(c.mac, &vblk));
            outVendorBlock(vblk);

            outStr(",\"bssid\":");
            outMac(c.bssid);

            outStr(",\"apssid\":");
            outJSON(lookupBssName(c.bssid));

            outStr(",\"assoc\":");
            outNum(c.associated ? 1 : 0);

            outStr(",\"ch\":");
            outNum(c.channel);

            outStr(",\"rssi\":");
            outNum(c.rssi);

            outStr(",\"up\":");
            outNum(c.toAp);

            outStr(",\"down\":");
            outNum(c.fromAp);

            outStr(",\"upbytes\":");
            outNum(c.toApBytes);

            outStr(",\"downbytes\":");
            outNum(c.fromApBytes);

            outStr(",\"probes\":");
            outNum(c.probes);

            outStr(",\"assocs\":");
            outNum(c.assocFrames);

            outStr(",\"std\":");
            outNum(c.standard);

            outStr(",\"nss\":");
            outNum(c.streams);

            outStr(",\"width\":");
            outNum(c.maxWidth);

            if (c.w8080)
                outStr(",\"w8080\":1");

            outStr(",\"roams\":");
            outNum(c.roams);

            outStr(",\"joined\":");
            outJSON(c.assocSSID);

            outStr(",\"akm\":");
            outJSON(c.akm);

            outStr(",\"cipher\":");
            outJSON(c.cipher);

            outStr(",\"rm11k\":");
            outNum(c.rm11k ? 1 : 0);

            outStr(",\"bss11v\":");
            outNum(c.bss11v ? 1 : 0);

            outStr(",\"rrm\":");
            outNum64(c.rrmCaps);

            outStr(",\"wnm\":");
            outNum(c.wnmCaps);

            outStr(",\"opcl\":");
            outOpClasses(c.opClasses);

            outStr(",\"chans\":");
            outNum64(c.suppChans);

            outStr(",\"chans6\":");
            outSixGhzChannels(c.suppChans6);

            outUnknownElements(c.unknownIes, c.unknownSeen, c.unknownCount);

            outMlo(c.mlo);

            outStr(",\"ft11r\":");
            outNum(c.ft11r ? 1 : 0);

            outStr(",\"pmf\":");
            outNum(c.pmf ? 1 : 0);

            outStr(",\"band6\":");
            outNum(c.band6 ? 1 : 0);

            outStr(",\"ssids\":[");

            {
                bool firstSsid = true;

                for (int k = 0; k < MAX_PROBE_SSIDS; k++)
                {
                    if (c.probeSSID[k][0] == 0)
                        continue;

                    if (!firstSsid)
                        outStr(",");

                    firstSsid = false;

                    outJSON(c.probeSSID[k]);
                }
            }

            outStr("]");

            outStr(",\"first\":");
            outNum(EPOCH_OF(c.firstSeen));

            outStr(",\"seen\":");
            outNum(EPOCH_OF(c.lastSeen));

            outStr("}");
        }

        outStr("]");
    }

    // ---- 6 GHz, heard about rather than heard ----

    if (wantSection(sections, "sixghz") && sixGhz != nullptr)
    {
        outStr(",\"sixghz\":[");

        bool first = true;

        for (int i = 0; i < MAX_SIXG_BSS; i++)
        {
            if (!sixGhz[i].valid)
                continue;

            if (since && (uint32_t)EPOCH_OF(sixGhz[i].lastSeen) < since)
                continue;

            if (!first)
                outStr(",");

            first = false;

            SixGhzBss &e = sixGhz[i];

            outStr("{\"bssid\":");
            outMac(e.bssid);

            outStr(",\"ssid\":");
            outJSON(e.ssid);

            outStr(",\"ch\":");
            outNum(e.channel);

            outStr(",\"freq\":");
            outNum(sixGhzFreq(e.channel));

            outStr(",\"bw\":");
            outNum(sixGhzWidth(e.opClass));

            outStr(",\"psd\":");
            outNum(e.psd);

            outStr(",\"reporter\":");
            outMac(e.reporter);

            outStr(",\"seen\":");
            outNum(EPOCH_OF(e.lastSeen));

            outStr("}");
        }

        outStr("]");
    }

    // ---- BLE ----

    if (wantSection(sections, "ble") && bleDevices != nullptr)
    {
        outStr(",\"ble\":[");

        bool first = true;

        for (int i = 0; i < MAX_BLE_DEVICES; i++)
        {
            if (!bleDevices[i].valid)
                continue;

            if (since && (uint32_t)EPOCH_OF(bleDevices[i].lastSeen) < since)
                continue;

            if (!first)
                outStr(",");

            first = false;

            BleDevice &d = bleDevices[i];

            outStr("{\"addr\":");
            outMac(d.addr);

            outStr(",\"name\":");
            outJSON(d.name);

            outStr(",\"rssi\":");
            outNum(d.rssi);

            outStr(",\"company\":");
            outNum(d.company);

            outStr(",\"adverts\":");
            outNum(d.adverts);

            outStr(",\"seen\":");
            outNum(EPOCH_OF(d.lastSeen));

            outStr("}");
        }

        outStr("]");
    }

    // ---- 802.15.4 ----

    if (wantSection(sections, "zb") && zbNodes != nullptr)
    {
        outStr(",\"zb\":{\"channels\":[");

        for (int i = 0; i < ZB_NUM_CHANNELS; i++)
        {
            if (i)
                outStr(",");

            outStr("{\"ch\":");
            outNum(zbChannelNumber(i));

            outStr(",\"ed\":");
            outNum(zbChannels[i].edLevel);

            outStr(",\"frames\":");
            outNum(zbChannels[i].frames);

            outStr("}");
        }

        outStr("],\"nodes\":[");

        bool first = true;

        for (int i = 0; i < ZB_MAX_NODES; i++)
        {
            if (!zbNodes[i].valid)
                continue;

            if (since && (uint32_t)EPOCH_OF(zbNodes[i].lastSeen) < since)
                continue;

            if (!first)
                outStr(",");

            first = false;

            ZbNode &z = zbNodes[i];

            outStr("{\"addr\":");

            {
                char buffer[20];

                if (z.hasExt)
                {
                    snprintf(buffer, sizeof(buffer),
                             "%02X%02X%02X%02X%02X%02X%02X%02X",
                             z.extAddr[0], z.extAddr[1],
                             z.extAddr[2], z.extAddr[3],
                             z.extAddr[4], z.extAddr[5],
                             z.extAddr[6], z.extAddr[7]);
                }
                else
                {
                    snprintf(buffer, sizeof(buffer), "0x%04X",
                             z.shortAddr);
                }

                outJSON(buffer);
            }

            outStr(",\"ext\":");
            outNum(z.hasExt ? 1 : 0);

            outStr(",\"pan\":");
            outNum(z.panId);

            outStr(",\"ch\":");
            outNum(z.channel);

            outStr(",\"lqi\":");
            outNum(z.lqi);

            outStr(",\"frames\":");
            outNum(z.frames);

            outStr(",\"seen\":");
            outNum(EPOCH_OF(z.lastSeen));

            outStr("}");
        }

        outStr("]}");
    }

    // ---- counters ----

    if (wantSection(sections, "counters"))
    {
        outStr(",\"counters\":{\"mgmtdrops\":");
        outNum(mgmtDrops);

        outStr(",\"datadrops\":");
        outNum(dataDrops);

        outStr(",\"fcs\":");
        outNum(fcsErrors);

        outStr(",\"heap\":");
        outNum(ESP.getFreeHeap());

        outStr(",\"heapmin\":");
        outNum(ESP.getMinFreeHeap());

        outStr(",\"psram\":");
        outNum(heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

        outStr(",\"pcap\":");
        outNum(ringLog.frameCount);

        outStr(",\"assoc\":");
        outNum(assocLog.frameCount);

        outStr(",\"beacons\":");
        outNum(countBeaconSlots());

        outStr("}");
    }

    outStr("}");

    outEnd();

    #undef EPOCH_OF
}
