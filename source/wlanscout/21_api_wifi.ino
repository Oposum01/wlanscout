// SPDX-License-Identifier: GPL-2.0-only WITH AdditionRef-wlanscout-linking-exception
// SPDX-FileCopyrightText: 2026 Dr.-Ing. Simon Wunderlich Systementwicklung & Beratung

// ============================================================
// API: live data
// ============================================================

/*
 * The channel plan, so the browser does not have to keep a
 * copy of channels24[] and channels5[] in step with this
 * file. Fetched once when the page loads.
 */
void sendPlanJSON()
{
    outBegin();

    outStr("{\"b24\":[");

    for (uint16_t i = 0; i < NUM_CHANNELS_24; i++)
    {
        if (i)
            outStr(",");

        outNum(channels24[i]);
    }

    outStr("],\"b5\":[");

    for (uint16_t i = 0; i < NUM_CHANNELS_5; i++)
    {
        if (i)
            outStr(",");

        outNum(channels5[i]);
    }

    // The default channel plan, see saveDefaultChannels()
    outStr("],\"def\":[");

    bool first = true;

    for (uint16_t i = 0; i < TOTAL_CHANNELS && i < 64; i++)
    {
        if (!(defaultMask & (1ULL << i)))
            continue;

        if (!first)
            outStr(",");

        outNum(getChannelNumber(i));

        first = false;
    }

    outStr("]}");

    outEnd();
}


void sendLiveJSON()
{
    outBegin();

    outStr("{\"version\":");
    outJSON(FW_VERSION);

    outStr(",\"oui\":");
    outNum(OUI_FULL_COUNT + OUI_MAM_COUNT + OUI_MAS_COUNT);
    outStr(",\"oui28\":");
    outNum(OUI_MAM_COUNT);
    outStr(",\"oui36\":");
    outNum(OUI_MAS_COUNT);

    outStr(",\"currentChannel\":");
    outNum(getChannelNumber(currentChannelIndex));

    outStr(",\"netcap\":");
    outNum(MAX_NETWORKS);

    // What the device actually scans, so the checkboxes can
    // show the truth after a reload instead of all-selected
    outStr(",\"chans\":[");

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

    outStr("]");

    outStr(",\"pcapcount\":");
    outNum(ringLog.frameCount);

    outStr(",\"pcapbytes\":");
    outNum(ringLog.frameBytes);

    outStr(",\"pcaptotal\":");
    outNum(ringLog.framesStored);

    outStr(",\"beacons\":");
    outNum(countBeaconSlots());

    outStr(",\"beacontotal\":");
    outNum(beaconTotal);

    outStr(",\"assoc\":");
    outNum(assocLog.frameCount);

    outStr(",\"assocbytes\":");
    outNum(assocLog.frameBytes);

    outStr(",\"capture\":");
    outNum(pcapEnabled ? 1 : 0);

    outStr(",\"capmask\":");
    outNum(pcapMask);

    outStr(",\"fast\":");
    outNum(fastSweep ? 1 : 0);

    outStr(",\"plan\":");
    outNum(planCount);

    outStr(",\"heap\":");
    outNum(ESP.getFreeHeap());

    outStr(",\"psram\":");
    outNum(heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    outStr(",\"drops\":");
    outNum(mgmtDrops + dataDrops);

    outStr(",\"fcs\":");
    outNum(fcsErrors);

    outStr(",\"channels\":[");

    bool first = true;

    for (uint16_t i = 0; i < TOTAL_CHANNELS; i++)
    {
        if (!channelSelected[i])
            continue;

        if (!first)
            outStr(",");

        first = false;

        ChannelMeasurement &m = currentMeasurement[i];

        outStr("{\"ch\":");
        outNum(getChannelNumber(i));

        outStr(",\"band\":\"");
        outStr((i < NUM_CHANNELS_24) ? "2.4" : "5");
        outStr("\"");

        outStr(",\"packets\":");
        outNum(m.packets);

        outStr(",\"peak\":");
        outNum(m.peakRSSI);

        outStr(",\"avg\":");

        if (m.rssiCount > 0)
            outFixed((float)m.rssiSum / m.rssiCount, 1);
        else
            outStr("-127");

        outStr("}");
    }

    outStr("]}");

    outEnd();
}

// ============================================================
// API: history
// ============================================================

void sendHistoryJSON()
{
    uint32_t requestedMs = 120000;

    if (reqHasArg("range"))
        requestedMs = reqArg("range").toInt();

    if (requestedMs < 10000)
        requestedMs = 10000;

    uint32_t now = millis();

    uint32_t startTime = (now > requestedMs) ? (now - requestedMs) : 0;

    const HistoryRing &h = historyFor(startTime);

    uint32_t first =
        h.capacity ? historyFirstAfter(h, startTime) : h.count;

    uint32_t available = (first < h.count) ? (h.count - first) : 0;

    uint32_t step = 1;

    if (available > MAX_HISTORY_POINTS)
    {
        step = (available + MAX_HISTORY_POINTS - 1) / MAX_HISTORY_POINTS;
    }

    outBegin();

    outStr("{\"points\":[");

    bool firstOutput = true;

    for (uint32_t i = first; i < h.count; i += step)
    {
        const WaterfallEntry &row = historyRow(h, i);

        if (!firstOutput)
            outStr(",");

        firstOutput = false;

        outStr("{\"t\":");
        outNum(row.timestamp);

        outStr(",\"v\":[");

        bool firstValue = true;

        for (uint16_t c = 0; c < TOTAL_CHANNELS; c++)
        {
            if (!channelSelected[c])
                continue;

            if (!firstValue)
                outStr(",");

            firstValue = false;

            outNum(row.rssi[c]);
        }

        outStr("]}");
    }

    outStr("]}");

    outEnd();
}

// ============================================================
// API: networks
// ============================================================

void sendNetworksJSON()
{
    cleanupNetworks();
    cleanupClients();

    refreshPcapCounts();

    uint32_t now = millis();

    outBegin();

    outStr("{\"networks\":[");

    bool first = true;

    for (int i = 0; i < MAX_NETWORKS; i++)
    {
        if (!networks[i].valid)
            continue;

        if (!first)
            outStr(",");

        first = false;

        WiFiNetwork &n = networks[i];

        outStr("{\"ssid\":");
        outJSON(n.ssid);

        outStr(",\"bssid\":");
        outMac(n.bssid);

        uint8_t vblk;

        outStr(",\"vendor\":");
        outJSON(lookupBssidVendor(n.bssid, n.mlo, &vblk));
        outVendorBlock(vblk);

        outStr(",\"resolved\":");
        outNum(n.ssidResolved ? 1 : 0);

        // Its graphs recorded without a viewer
        if (watchKept(n.bssid))
            outStr(",\"wk\":1");

        outStr(",\"pcapf\":");
        outNum(n.pcapFrames);

        outStr(",\"pcapb\":");
        outNum(n.pcapBeacon ? 1 : 0);

        outStr(",\"vht24\":");
        outNum(n.vht24 ? 1 : 0);

        outStr(",\"mesh\":");
        outNum(n.mesh ? 1 : 0);

        outStr(",\"meshpeers\":");
        outNum(n.meshPeerings);

        outStr(",\"meshgate\":");
        outNum(n.meshGate ? 1 : 0);

        outStr(",\"meshaccept\":");
        outNum(n.meshAccepting ? 1 : 0);

        outStr(",\"meshfwd\":");
        outNum(n.meshForwarding ? 1 : 0);

        outStr(",\"meshpath\":");
        outNum(n.meshPathProto);

        outStr(",\"meshmetric\":");
        outNum(n.meshMetric);

        outStr(",\"meshauth\":");
        outNum(n.meshAuth);

        outStr(",\"channel\":");
        outNum(n.channel);

        outStr(",\"bandwidth\":");
        outNum(n.bandwidth);

        outStr(",\"center\":");
        outNum(n.centerChannel);

        if (n.centerChannel2)
        {
            outStr(",\"center2\":");
            outNum(n.centerChannel2);
        }

        // EHT Operation: [width MHz, centre channel, punctured bitmap]
        if (n.ehtWidth)
        {
            outStr(",\"eht\":[");
            outNum(n.ehtWidth);
            outStr(",");
            outNum(n.ehtCenter);
            outStr(",");
            outNum(n.punctured);
            outStr("]");
        }

        outStr(",\"rssi\":");
        outNum(n.rssi);

        outStr(",\"sec\":");
        outJSON(n.security);

        outStr(",\"pair\":");
        outJSON(n.pairwise);

        outStr(",\"group\":");
        outJSON(n.groupCipher);

        outStr(",\"mgmt\":");
        outJSON(n.mgmtCipher);

        outStr(",\"akm\":");
        outJSON(n.akm);

        outStr(",\"std\":");
        outNum(n.standard);

        outStr(",\"modes\":");
        outNum(n.phyModes);

        outStr(",\"gf\":");
        outNum(n.htGreenfield ? 1 : 0);

        outStr(",\"intol\":");
        outNum(n.fortyIntolerant ? 1 : 0);

        outStr(",\"k\":");
        outNum(n.rm11k ? 1 : 0);

        outStr(",\"v\":");
        outNum(n.bss11v ? 1 : 0);

        outStr(",\"rrm\":");
        outNum64(n.rrmCaps);

        outStr(",\"nss\":");
        outNum(n.streams);

        // TWT: 1 requester, 2 responder, 4 broadcast; the broadcast
        // schedules as [ID, recommendation, interval µs, duration µs]
        if (n.heTwt)
        {
            outStr(",\"twt\":");
            outNum(n.heTwt);
        }

        // HE: capability bits (heCapsOf()) and HE Operation flags
        if (n.heCaps)
        {
            outStr(",\"hecap\":");
            outNum(n.heCaps);
        }

        // VHT: capability bits (vhtCapsOf())
        if (n.vhtCaps)
        {
            outStr(",\"vhtcap\":");
            outNum(n.vhtCaps);
        }

        if (n.heOpFlags)
        {
            outStr(",\"heop\":");
            outNum(n.heOpFlags);
        }

        if (n.btwtCount)
        {
            outStr(",\"btwt\":[");

            for (uint8_t i = 0; i < n.btwtCount; i++)
            {
                outStr(i ? ",[" : "[");
                outNum(n.btwtId[i]);
                outStr(",");
                outNum(n.btwtRec[i]);
                outStr(",");
                outNum(n.btwtInterval[i]);
                outStr(",");
                outNum(n.btwtDuration[i]);
                outStr("]");
            }

            outStr("]");
        }

        // Links being removed: [bit per link ID, beacons left]
        if (n.mlRemoving)
        {
            outStr(",\"mlrm\":[");
            outNum(n.mlRemoving);
            outStr(",");
            outNum(n.mlRemoveIn);
            outStr("]");
        }

        // EHT Capabilities: [MAC capabilities, first PHY octet, 4096-QAM]
        if (n.ehtMac || n.ehtPhy)
        {
            outStr(",\"ehtcap\":[");
            outNum(n.ehtMac);
            outStr(",");
            outNum(n.ehtPhy);
            outStr(",");
            outNum(n.ehtQam4k ? 1 : 0);
            outStr("]");
        }

        outMlo(n.mlo);

        outUnknownElements(n.unknownIes, n.unknownSeen, n.unknownCount);

        // "iw": "1A0A01": the Interworking element's body in hex
        if (n.iwLen)
        {
            char hex[sizeof(n.iw) * 2 + 1];

            for (uint8_t i = 0; i < n.iwLen; i++)
                snprintf(hex + i * 2, 3, "%02X", n.iw[i]);

            outStr(",\"iw\":");
            outJSON(hex);
        }

        // The smaller elements, only when seen, see parseApExtras()
        if (n.quietSeen)
        {
            outStr(",\"quiet\":[");
            outNum(n.quietPeriod);
            outStr(",");
            outNum(n.quietDuration);
            outStr(",");
            outNum(n.quietOffset);
            outStr("]");
        }

        if (n.mptInterval >= 0)
        {
            outStr(",\"mpt\":");
            outNum(n.mptInterval);
        }

        if (n.ibss)
            outStr(",\"ibss\":1");

        if (n.atimWindow >= 0)
        {
            outStr(",\"atim\":");
            outNum(n.atimWindow);
        }

        if (n.aacLen)
        {
            char hex[sizeof(n.aac) * 2 + 1];

            for (uint8_t i = 0; i < n.aacLen; i++)
                snprintf(hex + i * 2, 3, "%02X", n.aac[i]);

            outStr(",\"aac\":");
            outJSON(hex);
        }

        if (n.beaconProt)
            outStr(",\"bprot\":1");

        if (n.advProtoCount)
        {
            outStr(",\"adv\":[");

            for (uint8_t i = 0; i < n.advProtoCount; i++)
            {
                if (i)
                    outStr(",");

                outNum(n.advProto[i]);
            }

            outStr("]");
        }

        if (n.apName[0])
        {
            outStr(",\"apname\":");
            outJSON(n.apName);
        }

        static const uint8_t noUuid[16] = {0};

        if (memcmp(n.lancomUuid, noUuid, 16) != 0)
        {
            const uint8_t *u = n.lancomUuid;
            char uuid[37];

            snprintf(uuid, sizeof(uuid),
                     "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
                     "%02x%02x%02x%02x%02x%02x",
                     u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7],
                     u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
            outStr(",\"uuid\":");
            outJSON(uuid);
        }

        if (n.vendorFirmware[0])
        {
            outStr(",\"vfw\":");
            outJSON(n.vendorFirmware);
        }

        if (n.camKit[0])
        {
            outStr(",\"camkit\":");
            outJSON(n.camKit);
        }

        if (n.acmMask >= 0)
        {
            outStr(",\"acm\":");
            outNum(n.acmMask);
        }

        // Wi-Fi Alliance elements and HP's, see parseWfaElements()
        if (n.ovrSeen)
        {
            outStr(",\"ovr\":{\"akm\":");
            outJSON(n.ovrAkm);
            outStr(",\"pair\":");
            outJSON(n.ovrPairwise);
            outStr(",\"pmf\":");
            outNum(n.ovrPmf);
            outStr(",\"h2e\":");
            outNum(n.ovrH2E ? 1 : 0);
            outStr("}");
        }

        if (n.wfaCapaSeen)
        {
            outStr(",\"wfacapa\":");
            outNum(n.wfaCapa);
        }

        // [colour, disabled]
        if (n.bssColor >= 0)
        {
            outStr(",\"color\":[");
            outNum(n.bssColor);
            outStr(",");
            outNum(n.bssColorOff ? 1 : 0);
            outStr("]");
        }

        // [new colour, seconds since announced, changes seen]
        if (n.colorNext >= 0)
        {
            outStr(",\"bcc\":[");
            outNum(n.colorNext);
            outStr(",");
            outNum((now - n.colorChangeAt) / 1000);
            outStr(",");
            outNum(n.colorChanges);
            outStr("]");
        }

        // Seconds the TSF has run by now; resets, seconds since one
        if (n.tsfAt != 0)
        {
            outStr(",\"tsfup\":");
            outNum((long)(n.tsf / 1000000ULL + (now - n.tsfAt) / 1000));

            if (n.tsfResets)
            {
                outStr(",\"tsfreset\":[");
                outNum(n.tsfResets);
                outStr(",");
                outNum((now - n.tsfResetAt) / 1000);
                outStr("]");
            }
        }

        // Its configuration changes kept out of the bell
        if (cfgIsMuted(n.bssid))
            outStr(",\"cfgmute\":1");

        // [frames of clock 1, of clock 2, switches, µs apart]
        if (n.clkHits[1])
        {
            outStr(",\"clocks\":[");
            outNum(n.clkHits[0]);
            outStr(",");
            outNum(n.clkHits[1]);
            outStr(",");
            outNum(n.clkSwitches);
            outStr(",");
            outNum64((uint64_t)llabs(n.clkOff[0] - n.clkOff[1]));
            outStr("]");
        }

        // [DTIM period, group traffic buffered, stations waiting]
        if (n.dtimPeriod)
        {
            outStr(",\"dtim\":[");
            outNum(n.dtimPeriod);
            outStr(",");
            outNum(n.timMcast ? 1 : 0);
            outStr(",");
            outNum(n.timWaiting);
            outStr("]");
        }

        // Probe responses: [with timestamp 0, with one]
        if (n.probeTsfZero)
        {
            outStr(",\"ptsf\":[");
            outNum(n.probeTsfZero);
            outStr(",");
            outNum(n.probeTsfSet);
            outStr("]");
        }

        // [gaps measured, of them under half an interval, shortest µs]
        if (n.gapCount)
        {
            outStr(",\"bgap\":[");
            outNum(n.gapCount);
            outStr(",");
            outNum(n.gapShort);
            outStr(",");
            outNum(n.gapMinUs);
            outStr("]");
        }

        // Configuration changes, newest first:
        // [[seconds ago, what, from, to], ...] and how many in all
        if (n.cfgTotal)
        {
            outStr(",\"cfgn\":");
            outNum(n.cfgTotal);
            outStr(",\"cfgch\":[");

            int kept = n.cfgTotal < CFG_CHANGES ? n.cfgTotal : CFG_CHANGES;

            for (int i = 0; i < kept; i++)
            {
                const CfgChange &c = n.cfgChanges[
                    (n.cfgHead + CFG_CHANGES - 1 - i) % CFG_CHANGES];

                outStr(i ? ",[" : "[");
                outNum((now - c.at) / 1000);
                outStr(",");
                outJSON(cfgFieldNames[c.field]);
                outStr(",");
                outJSON(c.from);
                outStr(",");
                outJSON(c.to);
                outStr("]");
            }

            outStr("]");
        }

        // Beacon versions kept, see /api/beaconversions
        uint8_t bvn = bvCountOf(n.bssid);

        if (bvn)
        {
            outStr(",\"bvn\":");
            outNum(bvn);
        }

        if (n.maxIdle)
        {
            outStr(",\"maxidle\":[");
            outNum(n.maxIdle);
            outStr(",");
            outNum(n.maxIdleOpt);
            outStr("]");
        }

        /*
         * Drone Remote ID, raw where the page converts: altitudes
         * and height as sent (value / 2 - 1000 m, 0 unknown),
         * positions in 1e-7 degrees, speeds in cm/s
         */
        if (n.drone.seen)
        {
            const DroneInfo &d = n.drone;

            outStr(",\"rid\":{\"ua\":");
            outNum(d.uaType);
            outStr(",\"mk\":");
            outJSON(d.maker);
            outStr(",\"ago\":");
            outNum((now - d.at) / 1000);

            if (d.haveLocation)
            {
                outStr(",\"st\":");
                outNum(d.status);
                outStr(",\"lat\":");
                outNum(d.lat);
                outStr(",\"lon\":");
                outNum(d.lon);
                outStr(",\"ab\":");
                outNum(d.altBaro);
                outStr(",\"ag\":");
                outNum(d.altGeo);
                outStr(",\"h\":");
                outNum(d.height);
                outStr(",\"hagl\":");
                outNum(d.heightAgl ? 1 : 0);
                outStr(",\"spd\":");
                outNum(d.speed);
                outStr(",\"vs\":");
                outNum(d.vspeed);
                outStr(",\"trk\":");
                outNum(d.track);
            }

            if (d.haveOperator)
            {
                outStr(",\"olat\":");
                outNum(d.opLat);
                outStr(",\"olon\":");
                outNum(d.opLon);
                outStr(",\"oalt\":");
                outNum(d.opAlt);
                outStr(",\"eu\":");
                outNum(d.euClass);
            }

            outStr(",\"sid\":");
            outJSON(d.selfId);
            outStr("}");
        }

        if (n.p2pSeen)
        {
            outStr(",\"p2p\":[");
            outNum(n.p2pDevCap);
            outStr(",");
            outNum(n.p2pGroupCap);
            outStr(",");
            outJSON(n.p2pName);
            outStr("]");
        }

        if (n.nanPref >= 0 || n.nanAmPref >= 0)
        {
            outStr(",\"nan\":[");
            outNum(n.nanPref);
            outStr(",");
            outNum(n.nanRandom);
            outStr(",");
            outNum(n.nanAmPref);
            outStr(",");
            outNum(n.nanAmRandom);
            outStr(",");
            outNum(n.nanHops);
            outStr("]");
        }

        if (n.pdtCat)
        {
            outStr(",\"pdt\":[");
            outNum(n.pdtCat);
            outStr(",");
            outNum(n.pdtSub);
            outStr("]");
        }

        if (n.wfdInfo >= 0)
        {
            outStr(",\"wfd\":");
            outNum(n.wfdInfo);
        }

        if (n.mboSeen)
        {
            outStr(",\"mbo\":{\"cap\":");
            outNum(n.mboCap);
            outStr(",\"dis\":");
            outNum(n.mboDisallowed);
            outStr(",\"oce\":");
            outNum(n.oceCtrl);
            outStr(",\"npc\":[");

            for (uint8_t i = 0; i < n.mboNpcCount; i++)
            {
                if (i)
                    outStr(",");

                outNum(n.mboNpc[i]);
            }

            outStr("]}");
        }

        if (n.omn >= 0)
        {
            outStr(",\"omn\":");
            outNum(n.omn);
        }

        if (n.wpsName[0])
        {
            outStr(",\"wpsname\":");
            outJSON(n.wpsName);
        }

        // Manufacturer, model name, model number
        if (n.wpsMaker[0] || n.wpsModel[0] || n.wpsModelNo[0])
        {
            outStr(",\"wpsdev\":[");
            outJSON(n.wpsMaker);
            outStr(",");
            outJSON(n.wpsModel);
            outStr(",");
            outJSON(n.wpsModelNo);
            outStr("]");
        }

        if (n.hpModel[0])
        {
            outStr(",\"hp\":[");
            outJSON(n.hpModel);
            outStr(",");
            outJSON(n.hpNumber);
            outStr("]");
        }

        // "acc": ["My SEAT 4624","SEAT","SE3821"], Apple Device IE
        if (n.acc)
        {
            outStr(",\"acc\":[");
            outJSON(n.accName);
            outStr(",");
            outJSON(n.accMaker);
            outStr(",");
            outJSON(n.accModel);
            outStr("]");
        }

        outVendorOuis(n.vendorOui, n.vendorOuiCount);

        if (n.tpcPower != 127)
        {
            outStr(",\"tpc\":");
            outNum(n.tpcPower);
        }

        if (n.powerConstraint != 255)
        {
            outStr(",\"pwrc\":");
            outNum(n.powerConstraint);
        }

        if (n.erp != 255)
        {
            outStr(",\"erp\":");
            outNum(n.erp);
        }

        if (n.xloadPresent)
        {
            outStr(",\"xload\":[");
            outNum(n.xloadMu);

            for (uint8_t k = 0; k < 4; k++)
            {
                outStr(",");
                outNum(n.xload[k]);
            }

            outStr("]");
        }

        /*
         * The AP's clock as it would read now, and how far it is
         * from the sensor's in seconds once the sensor has one.
         */
        if (n.apClockOffsetUs != 0)
        {
            int64_t apNow = (esp_timer_get_time() +
                             n.apClockOffsetUs) / 1000000LL;

            char stamp[24];

            formatStamp((uint32_t)apNow, stamp, sizeof(stamp));

            outStr(",\"apclk\":");
            outJSON(stamp);

            if (pcapClockSource != 0)
            {
                outStr(",\"apdelta\":");
                outNum((long)((n.apClockOffsetUs - clockOffsetUs) /
                              1000000LL));
            }
        }

        if (n.apTz[0])
        {
            outStr(",\"aptz\":");
            outJSON(n.apTz);
        }

        // Transmit Power Envelope, raw octets per element
        outStr(",\"tpe\":[");

        for (uint8_t k = 0; k < 2 && n.tpeLen[k]; k++)
        {
            if (k)
                outStr(",");

            outStr("[");

            for (uint8_t b = 0; b < n.tpeLen[k]; b++)
            {
                if (b)
                    outStr(",");

                outNum(n.tpe[k][b]);
            }

            outStr("]");
        }

        outStr("]");

        outStr(",\"wnm\":");
        outNum(n.wnmCaps);

        outStr(",\"r\":");
        outNum(n.ft11r ? 1 : 0);

        // Fine Timing Measurement: [requests to it, FTM frames from
        // it, [stations ranging]], what it granted, position, address
        if (n.ftmRequests || n.ftmFrames)
        {
            outStr(",\"ftm\":[");
            outNum(n.ftmRequests);
            outStr(",");
            outNum(n.ftmFrames);
            outStr(",[");

            for (uint8_t i = 0; i < n.ftmPeerCount; i++)
            {
                if (i)
                    outStr(",");

                outMac(n.ftmPeers[i]);
            }

            outStr("]]");

            if (n.ftmParamSeen)
                outHexField("ftmpar", n.ftmParam, 9);

            if (n.ftmLciSeen)
                outHexField("ftmlci", n.ftmLci, 16);

            if (n.ftmCivicLen)
                outHexField("ftmciv", n.ftmCivic, n.ftmCivicLen);
        }

        // [Mobility Domain ID, FT Capability and Policy]
        if (n.ft11r)
        {
            outStr(",\"mdid\":[");
            outNum(n.mdid);
            outStr(",");
            outNum(n.ftPolicy);
            outStr("]");
        }

        outStr(",\"mbssid\":");
        outNum(n.multiBssid ? 1 : 0);

        outStr(",\"csach\":");
        outNum(n.csaChannel);

        outStr(",\"csacnt\":");
        outNum(n.csaCount);

        // A switch announced: its mode, the width after it, a new country
        if (n.csaChannel)
        {
            outStr(",\"csamode\":");
            outNum(n.csaMode);

            if (n.csaWidth)
            {
                outStr(",\"csaw\":[");
                outNum(n.csaWidth);
                outStr(",");
                outNum(n.csaCenter);
                outStr(",");
                outNum(n.csaCenter2);
                outStr("]");
            }

            if (n.csaCountry[0])
            {
                outStr(",\"csacc\":");
                outJSON(n.csaCountry);
            }

            if (n.csaTime)
            {
                outStr(",\"csatime\":");
                outNum(n.csaTime);
            }
        }

        outStr(",\"basic\":");
        outNum(n.basicRate);

        outStr(",\"basicrates\":[");

        for (int r = 0; r < n.basicRateCount; r++)
        {
            if (r)
                outStr(",");

            outNum(n.basicRates[r]);
        }

        outStr("]");

        outStr(",\"wpsm\":");
        outNum(n.wpsMethods);

        outStr(",\"wpslock\":");
        outNum(n.wpsLocked ? 1 : 0);

        outStr(",\"wpsstate\":");
        outNum(n.wpsState);

        outStr(",\"preauth\":");
        outNum(n.preAuth ? 1 : 0);

        if (n.badSuiteIn)
        {
            outStr(",\"suitebad\":[");
            outNum(n.badSuiteIn);
            outStr(",");
            outNum64(n.badSuite);
            outStr("]");
        }

        if (n.wpaNoAkm)
            outStr(",\"wpanoakm\":1");

        outStr(",\"hs20\":");
        outNum(n.hs20 ? 1 : 0);

        if (n.xbox)
            outStr(",\"xbox\":1");

        outStr(",\"owename\":");
        outNum(n.ssidFromOwe ? 1 : 0);

        outStr(",\"owetrans\":");
        outNum(n.oweTrans ? 1 : 0);

        outStr(",\"owebssid\":");
        outMac(n.oweBssid);

        outStr(",\"owessid\":");
        outJSON(n.oweSSID);

        outStr(",\"mfpc\":");
        outNum(n.mfpCapable ? 1 : 0);

        outStr(",\"mfpr\":");
        outNum(n.mfpRequired ? 1 : 0);

        outStr(",\"wps\":");
        outNum(n.wps ? 1 : 0);

        outStr(",\"sta\":");
        outNum(n.stations);

        outStr(",\"util\":");
        outNum(n.channelUtil);

        outStr(",\"clients\":");
        outNum(countClients(n.bssid));

        outStr(",\"bi\":");
        outNum(n.beaconInterval);

        outStr(",\"cc\":");
        outJSON(n.country);

        outStr(",\"beacons\":");
        outNum(n.beacons);

        outStr(",\"age\":");
        outNum((now - n.lastSeen) / 1000);

        outStr(",\"seen\":");
        outNum((now - n.firstSeen) / 1000);

        outStr("}");
    }

    outStr("]");

    // ---- 6 GHz networks heard about, never heard directly ----

    outStr(",\"sixghz\":[");

    if (sixGhz != nullptr)
    {
        bool firstSix = true;

        for (int i = 0; i < MAX_SIXG_BSS; i++)
        {
            if (!sixGhz[i].valid)
                continue;

            // Drop entries whose reporting AP has gone quiet, after
            // the same minute the network table gives an AP
            if (now - sixGhz[i].lastSeen > NETWORK_TIMEOUT_MS)
            {
                sixGhz[i].valid = false;
                continue;
            }

            if (!firstSix)
                outStr(",");

            firstSix = false;

            SixGhzBss &e = sixGhz[i];

            outStr("{\"bssid\":");
            outMac(e.bssid);

            outStr(",\"ssid\":");
            outJSON(e.ssid);

            outStr(",\"ch\":");
            outNum(e.channel);

            outStr(",\"freq\":");
            outNum(sixGhzFreq(e.channel));

            outStr(",\"width\":");
            outNum(sixGhzWidth(e.opClass));

            outStr(",\"opclass\":");
            outNum(e.opClass);

            outStr(",\"same\":");
            outNum(e.sameSsid ? 1 : 0);

            outStr(",\"colocated\":");
            outNum(e.coLocated ? 1 : 0);

            outStr(",\"unsolicited\":");
            outNum(e.unsolicited ? 1 : 0);

            outStr(",\"psd\":");
            outNum(e.psd);

            outStr(",\"shortssid\":");
            outNum((long)e.shortSsid);

            outStr(",\"reporter\":");
            outMac(e.reporter);

            outStr(",\"repssid\":");
            outJSON(e.reporterSsid);

            outStr(",\"repch\":");
            outNum(e.reporterChannel);

            outStr(",\"reports\":");
            outNum(e.reports);

            outStr(",\"age\":");
            outNum((now - e.lastSeen) / 1000);

            outStr("}");
        }
    }

    outStr("]}");

    outEnd();
}

// ============================================================
// API: stations
// ============================================================

/*
 * One station as JSON.
 *
 * Shared by /api/clients and /api/assochistory so the two
 * tables in the browser are rendered from the same fields and
 * cannot drift apart.
 *
 * The object is left open so a caller can append fields of
 * its own; closing it is the caller's job.
 */
/*
 * "voui": [["8CFDF0",1,"Qualcomm"], ...]: OUI, type and owner of
 * the vendor elements of an AP or a station, see
 * collectVendorOuis(). The page guesses maker and chip from them.
 */
void outVendorOuis(const uint8_t (*list)[4], uint8_t count)
{
    outStr(",\"voui\":[");

    for (uint8_t i = 0; i < count; i++)
    {
        char hex[8];

        snprintf(hex, sizeof(hex), "%02X%02X%02X", list[i][0],
                 list[i][1], list[i][2]);

        outStr(i ? ",[" : "[");
        outJSON(hex);
        outStr(",");
        outNum(list[i][3]);
        outStr(",");
        outJSON(lookupOui(list[i]));
        outStr("]");
    }

    outStr("]");
}

void outClientObject(ClientEntry &c, uint32_t now)
{
        outStr("{\"mac\":");
        outMac(c.mac);

        uint8_t vblk;

        outStr(",\"vendor\":");
        outJSON(lookupVendor(c.mac, &vblk));
        outVendorBlock(vblk);

        outStr(",\"bssid\":");
        outMac(c.bssid);

        outStr(",\"assoc\":");
        outNum(c.associated ? 1 : 0);

        // SSID of the BSS, even when no beacon was ever heard
        outStr(",\"apssid\":");
        outJSON(lookupBssName(c.bssid));

        outStr(",\"pcapf\":");
        outNum(c.pcapFrames);

        outStr(",\"prev\":");
        outMac(c.prevBssid);

        outStr(",\"roams\":");
        outNum(c.roams);

        outStr(",\"roamsok\":");
        outNum(c.roamsConfirmed);

        outStr(",\"roamlog\":[");

        for (int k = 0; k < c.roamLogCount; k++)
        {
            // Oldest first
            int idx =
                (c.roamLogWrite + MAX_ROAM_EVENTS - c.roamLogCount + k) %
                MAX_ROAM_EVENTS;

            if (k)
                outStr(",");

            outStr("{\"ago\":");
            outNum((now - c.roamLog[idx].at) / 1000);

            outStr(",\"bssid\":");
            outMac(c.roamLog[idx].bssid);

            outStr(",\"ok\":");
            outNum(c.roamLog[idx].confirmed ? 1 : 0);

            if (c.roamLog[idx].ft)
                outStr(",\"ft\":1");

            outStr("}");
        }

        outStr("]");

        outStr(",\"peer\":");
        outMac(c.peer);

        outStr(",\"rssi\":");
        outNum(c.rssi);

        outStr(",\"ch\":");
        outNum(c.channel);

        outStr(",\"up\":");
        outNum(c.toAp);

        outStr(",\"down\":");
        outNum(c.fromAp);

        outStr(",\"upb\":");
        outNum(c.toApBytes);

        outStr(",\"downb\":");
        outNum(c.fromApBytes);

        outStr(",\"status\":");
        outNum(c.assocStatusAt ? (int)c.assocStatus : -1);

        outStr(",\"statusago\":");
        outNum(c.assocStatusAt ? (now - c.assocStatusAt) / 1000 : 0);

        outStr(",\"statusbss\":");
        outMac(c.assocStatusBssid);

        outStr(",\"assocfail\":");
        outNum(c.assocFailures);

        if (c.assocComeback)
        {
            outStr(",\"comeback\":");
            outNum(c.assocComeback);
        }

        outStr(",\"probes\":");
        outNum(c.probes);

        outStr(",\"assocs\":");
        outNum(c.assocFrames);

        outStr(",\"sawassoc\":");
        outNum(c.sawAssoc ? 1 : 0);

        outStr(",\"joined\":");
        outJSON(c.assocSSID);

        outStr(",\"std\":");
        outNum(c.standard);

        outStr(",\"width\":");
        outNum(c.maxWidth);

        if (c.w8080)
            outStr(",\"w8080\":1");

        outStr(",\"nss\":");
        outNum(c.streams);

        if (c.heTwt)
        {
            outStr(",\"twt\":");
            outNum(c.heTwt);
        }

        if (c.heCaps)
        {
            outStr(",\"hecap\":");
            outNum(c.heCaps);
        }

        if (c.vhtCaps)
        {
            outStr(",\"vhtcap\":");
            outNum(c.vhtCaps);
        }

        if (c.ehtMac || c.ehtPhy)
        {
            outStr(",\"ehtcap\":[");
            outNum(c.ehtMac);
            outStr(",");
            outNum(c.ehtPhy);
            outStr(",");
            outNum(c.ehtQam4k ? 1 : 0);
            outStr("]");
        }

        outStr(",\"k\":");
        outNum(c.rm11k ? 1 : 0);

        outStr(",\"v\":");
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

        outVendorOuis(c.vendorOui, c.vendorOuiCount);

        outMlo(c.mlo);

        outStr(",\"r\":");
        outNum(c.ft11r ? 1 : 0);

        if (c.ft11r)
        {
            outStr(",\"mdid\":");
            outNum(c.mdid);
        }

        // Fine Timing Measurement: [requests, FTM frames to it,
        // [responders]], asked for 1 position, 2 address, parameters
        if (c.ftmRequests)
        {
            outStr(",\"ftm\":[");
            outNum(c.ftmRequests);
            outStr(",");
            outNum(c.ftmAnswers);
            outStr(",[");

            for (uint8_t i = 0; i < c.ftmTargetCount; i++)
            {
                if (i)
                    outStr(",");

                outMac(c.ftmTargets[i]);
            }

            outStr("]],\"ftmask\":");
            outNum((c.ftmAskLci ? 1 : 0) | (c.ftmAskCivic ? 2 : 0));

            if (c.ftmParamSeen)
                outHexField("ftmpar", c.ftmParam, 9);
        }

        outStr(",\"band6\":");
        outNum(c.band6 ? 1 : 0);

        outStr(",\"wmm\":");
        outNum(c.wmm ? 1 : 0);

        outStr(",\"pmf\":");
        outNum(c.pmf ? 1 : 0);

        outStr(",\"akm\":");
        outJSON(c.akm);

        outStr(",\"cipher\":");
        outJSON(c.cipher);

        if (c.wfaCapaSeen)
        {
            outStr(",\"wfacapa\":");
            outNum(c.wfaCapa);
        }

        if (c.rsnSelSeen)
        {
            outStr(",\"rsnsel\":");
            outNum(c.rsnSel);
        }

        // BSS Max Idle Period: [asked, options, granted, options]
        if (c.maxIdleAsked || c.maxIdleGranted)
        {
            outStr(",\"maxidle\":[");
            outNum(c.maxIdleAsked);
            outStr(",");
            outNum(c.maxIdleAskedOpt);
            outStr(",");
            outNum(c.maxIdleGranted);
            outStr(",");
            outNum(c.maxIdleGrantedOpt);
            outStr("]");
        }

        outStr(",\"roamsft\":");
        outNum(c.roamsFt);

        outStr(",\"ssids\":[");

        bool firstSSID = true;

        for (int k = 0; k < MAX_PROBE_SSIDS; k++)
        {
            if (c.probeSSID[k][0] == 0)
                continue;

            if (!firstSSID)
                outStr(",");

            firstSSID = false;

            outJSON(c.probeSSID[k]);
        }

        outStr("]");

        outStr(",\"age\":");
        outNum((now - c.lastSeen) / 1000);

        outStr(",\"seen\":");
        outNum((now - c.firstSeen) / 1000);
}


void sendClientsJSON()
{
    cleanupClients();

    refreshPcapCounts();

    uint32_t now = millis();

    outBegin();

    outStr("{\"clients\":[");

    bool first = true;

    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        if (!clients[i].valid)
            continue;

        if (!first)
            outStr(",");

        first = false;

        outClientObject(clients[i], now);

        outStr("}");
    }

    // AWDL senders, see parseAwdl()
    cleanupAwdl();

    outStr("],\"awdl\":[");

    first = true;

    for (int i = 0; i < MAX_AWDL; i++)
    {
        const AwdlEntry &d = awdlDevices[i];

        if (!d.valid)
            continue;

        if (!first)
            outStr(",");

        first = false;

        outStr("{\"mac\":");
        outMac(d.mac);
        outStr(",\"rssi\":");
        outNum(d.rssi);
        outStr(",\"ch\":");
        outNum(d.channel);
        outStr(",\"frames\":");
        outNum(d.frames);
        outStr(",\"cls\":");
        outNum(d.devClass);
        outStr(",\"ver\":");
        outNum(d.version);
        outStr(",\"cc\":");
        outJSON(d.country);

        if (d.infraSeen)
        {
            outStr(",\"bssid\":");
            outMac(d.infraBssid);
            outStr(",\"bch\":");
            outNum(d.infraChannel);
        }

        if (d.staSeen)
        {
            outStr(",\"sta\":");
            outMac(d.sta);
        }

        outStr(",\"age\":");
        outNum((now - d.lastSeen) / 1000);
        outStr(",\"seen\":");
        outNum((now - d.firstSeen) / 1000);
        outStr("}");
    }

    outStr("]}");

    outEnd();
}

// ============================================================
// API: association history
//
// Separate from /api/clients on purpose: this table does not
// age out, so it is polled rarely and must not slow the live
// view down.
// ============================================================

void sendAssocHistoryJSON()
{
    refreshPcapCounts();

    uint32_t now = millis();

    outBegin();

    outStr("{\"dropped\":");
    outNum(assocHistoryDropped);

    outStr(",\"slots\":");
    outNum(MAX_ASSOC_HISTORY);

    outStr(",\"clients\":[");

    bool first = true;

    for (int i = 0; assocHistory != nullptr &&
                    i < MAX_ASSOC_HISTORY; i++)
    {
        if (!assocHistory[i].valid)
            continue;

        if (!first)
            outStr(",");

        first = false;

        /*
         * The snapshot is frozen, so its ages would count from
         * whenever it was taken. Only the two absolute stamps
         * below are meaningful here; the browser prefers them
         * and falls back to the ages for the live table.
         */
        outClientObject(assocHistory[i].sta, now);

        outStr(",\"firstepoch\":");
        outNum(assocHistory[i].firstEpoch);

        outStr(",\"lastepoch\":");
        outNum(assocHistory[i].lastEpoch);

        outStr("}");
    }

    outStr("]}");

    outEnd();
}

// ============================================================
