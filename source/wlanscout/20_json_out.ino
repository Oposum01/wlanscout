// SPDX-License-Identifier: GPL-2.0-only WITH AdditionRef-wlanscout-linking-exception
// SPDX-FileCopyrightText: 2026 Dr.-Ing. Simon Wunderlich Systementwicklung & Beratung

// ============================================================
// Chunked JSON output
//
// Responses used to be built as one String on the heap.
// With 33 channels and a full history that was more than
// 100 kB in one block, which fragments the heap until
// allocation fails and the W5500 driver runs out of
// buffers: link flapping and a stalled device.
//
// Everything now goes out in chunks of about 1 kB.
// ============================================================

String outBuf;

void outBegin()
{
    lastRequestServed = millis();

#if ENABLE_ETHERNET
    if (!REQ_OWN_ARGS)
        noteWebClient((uint32_t)server.client().remoteIP());
#endif

    outBuf = "";
    outBuf.reserve(1400);

    reqStart(200, "application/json", -1, nullptr);
}

void outFlush()
{
    if (outBuf.length() == 0)
        return;

    reqContent(outBuf.c_str(), outBuf.length());

    outBuf = "";
}

void outStr(const char *text)
{
    outBuf += text;

    if (outBuf.length() >= 1024)
        outFlush();
}

void outNum(long value)
{
    outBuf += value;

    if (outBuf.length() >= 1024)
        outFlush();
}

/*
 * Operating classes as a JSON list of class numbers. As a single
 * number the mask would need 57 bits, and a browser holds only 53
 * exactly: class 134 alone lifts the value past that and the low
 * bits, 2.4 GHz among them, were rounded away.
 */
void outOpClasses(uint64_t mask)
{
    outStr("[");

    bool first = true;

    for (int i = 0; i < 57; i++)
    {
        if (!((mask >> i) & 1))
            continue;

        if (!first)
            outStr(",");

        outNum(81 + i);

        first = false;
    }

    outStr("]");
}

// "unk": the element codes, "unkin": the SEEN_* bits of each
void outUnknownElements(const uint16_t *list, const uint8_t *seenIn,
                        uint8_t count)
{
    outStr(",\"unk\":[");

    for (uint8_t i = 0; i < count; i++)
    {
        if (i)
            outStr(",");

        outNum(list[i]);
    }

    outStr("],\"unkin\":[");

    for (uint8_t i = 0; i < count; i++)
    {
        if (i)
            outStr(",");

        outNum(seenIn[i]);
    }

    outStr("]");
}

// 6 GHz channels as a list, for the same reason as the classes
void outSixGhzChannels(uint64_t mask)
{
    outStr("[");

    bool first = true;

    for (int i = 0; i < 59; i++)
    {
        if (!((mask >> i) & 1))
            continue;

        if (!first)
            outStr(",");

        outNum(1 + i * 4);

        first = false;
    }

    outStr("]");
}

// For bit fields wider than 32 bits; JSON numbers hold 53 exactly
void outNum64(uint64_t value)
{
    char buffer[24];

    snprintf(buffer, sizeof(buffer), "%llu",
             (unsigned long long)value);

    outBuf += buffer;

    if (outBuf.length() >= 1024)
        outFlush();
}

void outFixed(float value, int digits)
{
    outBuf += String(value, digits);

    if (outBuf.length() >= 1024)
        outFlush();
}

// JSON string including the quotes
/*
 * Length of the UTF-8 sequence at p, 0 when it is none: a lead byte
 * with as many continuation bytes as it announces, not overlong, no
 * surrogate, nothing above U+10FFFF.
 */
int utf8Length(const uint8_t *p)
{
    uint8_t c = p[0];

    if (c < 0x80)
        return 1;

    int      n;
    uint32_t cp;

    if (c >= 0xC2 && c <= 0xDF)
    {
        n  = 2;
        cp = c & 0x1F;
    }
    else if (c >= 0xE0 && c <= 0xEF)
    {
        n  = 3;
        cp = c & 0x0F;
    }
    else if (c >= 0xF0 && c <= 0xF4)
    {
        n  = 4;
        cp = c & 0x07;
    }
    else
        return 0;

    // The terminating 0 is no continuation byte, so this stops there
    for (int i = 1; i < n; i++)
    {
        if ((p[i] & 0xC0) != 0x80)
            return 0;

        cp = (cp << 6) | (p[i] & 0x3F);
    }

    if ((n == 3 && cp < 0x800) || (n == 4 && cp < 0x10000) ||
        (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF)
        return 0;

    return n;
}

/*
 * A JSON string. SSIDs and names come off the air as any bytes; one
 * that is not part of valid UTF-8 goes out as \u00XX, the character
 * of that value, so the answer stays valid UTF-8 for strict readers
 * and the byte still shows ("o\xa9\xf7" as "o\u00a9\u00f7").
 * Control characters are left out.
 */
void outJSON(const char *text)
{
    const uint8_t *p = (const uint8_t *)text;

    outBuf += '"';

    while (*p)
    {
        uint8_t c = *p;

        if (c == '"' || c == '\\')
        {
            outBuf += '\\';
            outBuf += (char)c;
            p++;
            continue;
        }

        if (c < 32)
        {
            p++;
            continue;
        }

        int n = utf8Length(p);

        if (n == 0)
        {
            char esc[7];

            snprintf(esc, sizeof(esc), "\\u%04x", c);
            outBuf += esc;
            p++;
            continue;
        }

        for (int i = 0; i < n; i++)
            outBuf += (char)p[i];

        p += n;
    }

    outBuf += '"';

    if (outBuf.length() >= 1024)
        outFlush();
}

// ,"name":"0A1B2C": bytes in hex, for the browser to decode
void outHexField(const char *name, const uint8_t *data, uint8_t length)
{
    char hex[3];

    outStr(",\"");
    outStr(name);
    outStr("\":\"");

    for (uint8_t i = 0; i < length; i++)
    {
        snprintf(hex, sizeof(hex), "%02X", data[i]);
        outStr(hex);
    }

    outStr("\"");
}

void outMac(const uint8_t *mac)
{
    char buffer[18];

    snprintf(buffer, sizeof(buffer),
             "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2],
             mac[3], mac[4], mac[5]);

    outStr("\"");
    outStr(buffer);
    outStr("\"");
}

// The Multi-Link fields, only for devices that sent the element
void outMlo(const struct MloInfo &m)
{
    if (!m.present)
        return;

    outStr(",\"mld\":");
    outMac(m.mldMac);

    outStr(",\"mlolink\":");
    outNum(m.linkId);

    outStr(",\"mlof\":");
    outNum(m.fields);

    outStr(",\"eml\":");
    outNum(m.eml);

    outStr(",\"mldcap\":");
    outNum(m.mldCaps);

    outStr(",\"mlolinks\":");
    outNum(m.linkMask);

    outStr(",\"mlomacs\":[");

    for (uint8_t i = 0; i < m.linkCount; i++)
    {
        if (i)
            outStr(",");

        outStr("{\"link\":");
        outNum(m.linkIds[i]);

        outStr(",\"mac\":");
        outMac(m.linkMacs[i]);

        outStr("}");
    }

    outStr("]");

    // Stations only (an AP's entry has no reqBand): the links of
    // the request, see MloInfo
    if (!m.reqBand)
        return;

    outStr(",\"mloband\":");
    outNum(m.reqBand);

    outStr(",\"mloprof\":[");

    for (uint8_t i = 0; i < m.profCount; i++)
    {
        outStr(i ? ",{\"link\":" : "{\"link\":");
        outNum(m.profLink[i]);

        outStr(",\"band\":");
        outNum(m.profBand[i]);

        outStr(",\"width\":");
        outNum(m.profWidth[i]);

        outStr(",\"nstr\":");
        outNum(m.profNstr[i]);

        outStr("}");
    }

    outStr("]");
}

void outEnd()
{
    outFlush();

    reqDone();
}
