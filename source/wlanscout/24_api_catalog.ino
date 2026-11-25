// SPDX-License-Identifier: GPL-2.0-only WITH AdditionRef-wlanscout-linking-exception
// SPDX-FileCopyrightText: 2026 Dr.-Ing. Simon Wunderlich Systementwicklung & Beratung

// ============================================================
// API: IE catalog
//
// /api/catalog lists the entries; ?enable=0|1 switches the
// recording, which is kept across reboots.
// ============================================================

void outByteList(const char *name, const uint8_t *list, size_t n)
{
    outStr(",\"");
    outStr(name);
    outStr("\":[");

    for (size_t i = 0; i < n; i++)
    {
        if (i)
            outStr(",");

        outNum(list[i]);
    }

    outStr("]");
}

void outKeyList(const char *name, const uint32_t *list, size_t n)
{
    outStr(",\"");
    outStr(name);
    outStr("\":[");

    for (size_t i = 0; i < n; i++)
    {
        if (i)
            outStr(",");

        outNum64(list[i]);
    }

    outStr("]");
}

/*
 * The AKM or cipher suites with a name, [key, name] each, under
 * the IEEE OUI and the WPA (version 1) one.
 */
void outSuiteList(const char *name, bool akm)
{
    outStr(",\"");
    outStr(name);
    outStr("\":[");

    bool first = true;

    for (uint32_t oui : { 0x000FACu, 0x0050F2u })
    {
        bool wpa1 = oui == 0x0050F2u;

        for (uint32_t t = 0; t < 256; t++)
        {
            uint32_t key = (oui << 8) | t;

            if (akm ? !akmKnown(key, wpa1) : !cipherKnown(key))
                continue;

            outStr(first ? "[" : ",[");
            outNum64(key);
            outStr(",");
            outJSON(akm ? akmName(t, wpa1) : cipherName(t, wpa1));
            outStr("]");

            first = false;
        }
    }

    for (const VendorSuite &v : vendorSuites)
    {
        if (v.akm != akm)
            continue;

        outStr(",[");
        outNum64(v.suite);
        outStr(",");
        outJSON(v.name);
        outStr("]");
    }

    if (!akm)
        outStr(",[0,\"zero suite (broken WPA element)\"]");

    outStr("]");
}

/*
 * "reads": what the sensor decodes, for the list below the
 * catalog. Element numbers only, the page has the names.
 */
void sendCatalogReads()
{
    outStr(",\"reads\":{\"v\":1");

    outByteList("dec", decodedIes, sizeof(decodedIes));
    outByteList("decx", decodedExt, sizeof(decodedExt));
    outByteList("part", partialIes, sizeof(partialIes));
    outByteList("pass", passedIes, sizeof(passedIes));
    outByteList("passx", passedExt, sizeof(passedExt));
    outKeyList("vend", decodedVendor,
               sizeof(decodedVendor) / sizeof(decodedVendor[0]));
    // None decoded in part at the moment; the page still reads it
    outKeyList("vpart", nullptr, 0);
    outKeyList("vrev", reviewedVendor,
               sizeof(reviewedVendor) / sizeof(reviewedVendor[0]));
    outSuiteList("akm", true);
    outSuiteList("cipher", false);

    outStr("}");
}

void sendCatalogJSON()
{
    if (reqHasArg("enable"))
    {
        bool on = reqArg("enable").toInt() != 0;

        if (on != catalogEnabled)
        {
            catalogEnabled = on;

            logRequest("IE catalog %s", on ? "on" : "off");

            saveSettings();

            // So nothing counted so far is lost by switching off
            if (!on && catalogDirty)
                catalogSave();
        }
    }

    outBegin();

    outStr("{\"enabled\":");
    outNum(catalogEnabled ? 1 : 0);

    outStr(",\"fs\":");
    outNum(catalogFsOk ? 1 : 0);

    outStr(",\"boot\":");
    outNum(bootCount);

    outStr(",\"max\":");
    outNum(CATALOG_MAX);

    outStr(",\"lost\":");
    outNum(catalogFull);

    outStr(",\"used\":");
    outNum(catalogFsOk ? (long)LittleFS.usedBytes() : 0);

    outStr(",\"total\":");
    outNum(catalogFsOk ? (long)LittleFS.totalBytes() : 0);

    long pcapSize = 0;

    if (catalogFsOk && LittleFS.exists(CATALOG_PCAP))
    {
        File f = LittleFS.open(CATALOG_PCAP, "r");

        if (f)
        {
            pcapSize = (long)f.size();
            f.close();
        }
    }

    outStr(",\"pcap\":");
    outNum(pcapSize);

    outStr(",\"pcapmax\":");
    outNum(CATALOG_PCAP_MAX);

    sendCatalogReads();

    outStr(",\"entries\":[");

    for (uint16_t i = 0; i < catalogCount; i++)
    {
        const CatalogEntry &e = catalog[i];

        if (i)
            outStr(",");

        outStr("{\"k\":");
        outNum(e.kind);

        outStr(",\"id\":");
        outNum64(e.key);

        outStr(",\"n\":");
        outNum64(e.count);

        outStr(",\"in\":");
        outNum(e.seenIn);

        outStr(",\"ch\":");
        outNum(e.channel);

        // Distinct senders; beyond CATALOG_SENDERS only "more"
        outStr(",\"snd\":");
        outNum(e.senderCount);

        if (e.sendersMore)
            outStr(",\"sndmore\":1");

        outStr(",\"fr\":");
        outNum(e.frames);

        outStr(",\"f\":[");
        outNum64(e.firstAt);
        outStr(",");
        outNum(e.firstClock);
        outStr(",");
        outNum(e.firstBoot);

        outStr("],\"l\":[");
        outNum64(e.lastAt);
        outStr(",");
        outNum(e.lastClock);
        outStr(",");
        outNum(e.lastBoot);

        outStr("],\"mac\":");
        outMac(e.mac);

        outStr(",\"ssid\":");
        outJSON(e.ssid);

        // Who owns the OUI of a vendor element or suite
        if (e.kind >= CAT_VENDOR)
        {
            uint8_t oui[6] = { (uint8_t)(e.key >> 24),
                               (uint8_t)(e.key >> 16),
                               (uint8_t)(e.key >> 8), 0, 0, 0 };

            outStr(",\"vn\":");
            outJSON(lookupOui(oui));
        }

        outStr(",\"len\":");
        outNum(e.fullLen);

        outStr(",\"raw\":\"");

        for (uint8_t b = 0; b < e.sampleLen; b++)
        {
            char hex[3];

            snprintf(hex, sizeof(hex), "%02x", e.sample[b]);

            outStr(hex);
        }

        outStr("\"}");
    }

    outStr("]}");

    outEnd();
}

void handleCatalogClear()
{
    logRequest("IE catalog cleared, %u entries discarded",
               (unsigned)catalogCount);

    catalogClear();

    reqSend(200, "application/json", "{\"ok\":1}");
}
/*
 * "element-ie73", "element-ext107", "element-vendor-506F9A-41",
 * "element-akm-000FAC-30": what a download of one entry is called.
 * The page adds the element's name when it offers the link.
 */
void catalogFileName(uint8_t kind, uint32_t key, char *out, size_t size)
{
    if (kind == CAT_ELEMENT || kind == CAT_EXT)
    {
        snprintf(out, size, "element-%s%lu",
                 kind == CAT_ELEMENT ? "ie" : "ext",
                 (unsigned long)key);
        return;
    }

    snprintf(out, size, "element-%s-%06lX-%lu",
             kind == CAT_VENDOR ? "vendor"
                 : (kind == CAT_AKM ? "akm" : "cipher"),
             (unsigned long)(key >> 8), (unsigned long)(key & 0xFF));
}

/*
 * The frames of one catalog entry (?k=kind&id=key), picked out
 * of the pcap file by looking at each record again. Two passes:
 * the first only adds up the length, so the reply needs no
 * chunked encoding.
 */
void sendCatalogPcapFor(uint8_t kind, uint32_t key)
{
    File f = LittleFS.open(CATALOG_PCAP, "r");

    if (!f || f.size() < 24)
    {
        reqSend(404, "text/plain", "No frames for this entry.");
        return;
    }

    static uint8_t buf[16 + RADIOTAP_LEN + MGMT_PARSE_MAX];

    uint8_t hdr[24];

    f.read(hdr, sizeof(hdr));

    catalogMatching  = true;
    catalogMatchKind = kind;
    catalogMatchKey  = key;

    uint32_t total = 24;
    uint32_t count = 0;

    for (int pass = 0; pass < 2; pass++)
    {
        f.seek(24);

        while (true)
        {
            if (f.read(buf, 16) != 16)
                break;

            uint32_t len;

            memcpy(&len, &buf[8], 4);

            if (len > RADIOTAP_LEN + MGMT_PARSE_MAX ||
                f.read(buf + 16, len) != len)
            {
                break;
            }

            // 15 octets from firmware before 2.8.27, then 16
            uint16_t rtLen = radiotapLenOf(buf + 16, len);

            if (rtLen == 0)
                break;

            CatalogFrame cf = {};

            catalogMatched = false;

            catalogScan(buf + 16 + rtLen, len - rtLen, 0, cf);

            if (!catalogMatched)
                continue;

            if (pass == 0)
            {
                total += 16 + len;
                count++;
            }
            else
            {
                reqContent(buf, 16 + len);
            }
        }

        if (pass == 0)
        {
            if (count == 0)
                break;

            char base[32];

            catalogFileName(kind, key, base, sizeof(base));

            char name[48];

            snprintf(name, sizeof(name), "%s.pcap", base);

            reqStart(200, "application/vnd.tcpdump.pcap", total, name);
            reqContent(hdr, sizeof(hdr));
        }
    }

    catalogMatching = false;

    f.close();

    if (count == 0)
        reqSend(404, "text/plain", "No frames for this entry.");
    else
        reqDone();

    lastRequestServed = millis();
}

void sendCatalogPcap()
{
    if (!catalogFsOk || !LittleFS.exists(CATALOG_PCAP))
    {
        reqSend(404, "text/plain",
                    "No frames in the IE catalog yet.");
        return;
    }

    if (reqHasArg("k") && reqHasArg("id"))
    {
        sendCatalogPcapFor((uint8_t)reqArg("k").toInt(),
                           (uint32_t)strtoul(reqArg("id").c_str(),
                                             nullptr, 10));
        return;
    }

    File f = LittleFS.open(CATALOG_PCAP, "r");

    if (!f)
    {
        reqSend(500, "text/plain", "cannot open the catalog pcap");
        return;
    }

    reqStart(200, "application/vnd.tcpdump.pcap", f.size(),
             "element-catalog.pcap");

    static uint8_t part[1024];

    while (true)
    {
        size_t got = f.read(part, sizeof(part));

        if (got == 0)
            break;

        reqContent(part, got);
    }

    reqDone();

    f.close();

    lastRequestServed = millis();
}
