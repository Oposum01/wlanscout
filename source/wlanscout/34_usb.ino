// SPDX-License-Identifier: GPL-2.0-only WITH AdditionRef-wlanscout-linking-exception
// SPDX-FileCopyrightText: 2026 Dr.-Ing. Simon Wunderlich Systementwicklung & Beratung

// ============================================================
// The USB port: requests in, answers out (the protocol is
// described at "Requests over Ethernet or USB")
// ============================================================

char   usbLine[512];
size_t usbLineLen = 0;
bool   usbLineTooLong = false;

void setupUsbApi()
{
    // Room for one PUT piece and then some, see USB_PUT_PIECE
    usbPort.setRxBufferSize(4096);
    usbPort.setTxBufferSize(4096);
    usbPort.begin();

    // Without a host nothing is read; do not stall on a full buffer
    usbPort.setTxTimeoutMs(200);
}

int hexValue(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;

    return -1;
}

// Query strings come URL encoded, as from a browser
String urlDecode(const char *text, size_t length)
{
    String out;

    for (size_t i = 0; i < length; i++)
    {
        char c = text[i];

        if (c == '+')
        {
            out += ' ';
        }
        else if (c == '%' && i + 2 < length &&
                 hexValue(text[i + 1]) >= 0 && hexValue(text[i + 2]) >= 0)
        {
            out += (char)(hexValue(text[i + 1]) * 16 + hexValue(text[i + 2]));
            i += 2;
        }
        else
        {
            out += c;
        }
    }

    return out;
}

void usbParseQuery(const char *query)
{
    usbArgCount = 0;

    while (*query && usbArgCount < USB_ARGS_MAX)
    {
        const char *end = strchr(query, '&');
        size_t      len = end ? (size_t)(end - query) : strlen(query);
        const char *eq  = (const char *)memchr(query, '=', len);

        if (len > 0)
        {
            size_t nameLen = eq ? (size_t)(eq - query) : len;

            usbArgNames[usbArgCount]  = urlDecode(query, nameLen);
            usbArgValues[usbArgCount] =
                eq ? urlDecode(eq + 1, len - nameLen - 1) : String("");

            usbArgCount++;
        }

        if (!end)
            break;

        query = end + 1;
    }
}

/*
 * A file coming in over USB: "PUT /api/snapshotimport?len=N&..." for
 * a snapshot, "PUT /api/ota?len=N" for a firmware, then N bytes,
 * read by usbApiLoop() before any further request line. A stall of
 * USB_PUT_TIMEOUT_MS gives up.
 */
#define USB_PUT_TIMEOUT_MS  10000

/*
 * USB has no flow control here: once the receive queue is full, the
 * core drops what comes on (HWCDC.cpp), and writing the flash holds
 * up the loop for milliseconds while the host sends at 1 MB/s. So
 * the page sends the bytes in pieces of USB_PUT_PIECE and waits for
 * USB_PUT_ACK after each but the last, which the sensor sends once
 * it has written the piece away. The receive queue holds two.
 */
#define USB_PUT_PIECE       2048
#define USB_PUT_ACK         0x06

enum UsbPutKind { USB_PUT_IMPORT, USB_PUT_OTA };

// A firmware sent to a build without OTA is read to its end and refused
#define OTA_NOT_BUILT "this build has one firmware slot, so no update " \
                      "while it runs (upload or from GitHub): use " \
                      "'Update in download mode' (the flasher) " \
                      "or esptool"

uint32_t   usbPutLeft = 0;
uint32_t   usbPutLast = 0;
uint32_t   usbPutPiece = 0;          // bytes of the piece read so far
UsbPutKind usbPutKind = USB_PUT_IMPORT;

void usbPutFinish()
{
    outToUsb = true;

    if (usbPutKind == USB_PUT_OTA)
    {
#if ENABLE_OTA
        otaFinish();
        handleOtaDone();
#else
        logRequest("firmware upload refused, not built in");
        reqSend(200, "application/json",
                "{\"ok\":0,\"error\":\"" OTA_NOT_BUILT "\"}");
#endif
    }
    else
    {
        importAnswer(!importError[0] && importEnd());
    }

    outToUsb    = false;
    usbArgCount = 0;
}

void usbPutWrite(const uint8_t *data, size_t length)
{
    if (usbPutKind == USB_PUT_OTA)
    {
#if ENABLE_OTA
        otaWrite(data, length);
#endif
    }
    else
        importWrite(data, length);
}

void usbPutStalled()
{
    if (usbPutKind == USB_PUT_OTA)
    {
#if ENABLE_OTA
        otaFail("USB stalled");
#endif
    }
    else
        importFail("USB stalled");
}

void usbApiRequest(char *line)
{
    outToUsb = true;

    if (strncmp(line, "PUT /api/ota", 12) == 0 &&
        (line[12] == '?' || line[12] == 0))
    {
        usbParseQuery(line[12] == '?' ? line + 13 : "");

        lastRequestServed = millis();

        uint32_t length = reqArg("len").toInt();

        usbPutKind  = USB_PUT_OTA;
        usbPutPiece = 0;

#if ENABLE_OTA
        const esp_partition_t *slot = esp_ota_get_next_update_partition(NULL);

        // As for an import, the bytes are read in any case and an
        // update that could not start answers with its error
        otaStart();

        if (slot != NULL && length > slot->size)
            otaFail("larger than a firmware slot");

        if (length == 0)
            otaFail("no file received");
#endif

        usbPutLeft = length;
        usbPutLast = millis();

        if (usbPutLeft == 0)
            usbPutFinish();

        outToUsb = false;

        return;
    }

    if (strncmp(line, "PUT /api/snapshotimport", 23) == 0 &&
        (line[23] == '?' || line[23] == 0))
    {
        usbParseQuery(line[23] == '?' ? line + 24 : "");

        lastRequestServed = millis();

        usbPutKind  = USB_PUT_IMPORT;
        usbPutPiece = 0;

        // The bytes follow in any case; when the start failed they
        // are read and dropped, then the error is the answer
        importBegin();

        usbPutLeft = importLen;
        usbPutLast = millis();

        if (usbPutLeft == 0 || usbPutLeft > SNAP_IMPORT_MAX)
        {
            usbPutLeft = 0;
            usbPutFinish();
        }

        outToUsb = false;

        return;
    }

    if (strncmp(line, "GET ", 4) != 0)
    {
        reqSend(405, "text/plain", "GET only");
        outToUsb = false;

        return;
    }

    char *path  = line + 4;
    char *query = strchr(path, '?');

    if (query)
        *query++ = 0;

    usbParseQuery(query ? query : "");

    const ApiRoute *route = nullptr;

    for (const ApiRoute &r : apiRoutes)
        if (strcmp(path, r.path) == 0)
            route = &r;

    if (route)
    {
        lastRequestServed = millis();
        runRoute(*route);
    }
    else
    {
        reqSend(404, "text/plain", "Not found");
    }

    outToUsb    = false;
    usbArgCount = 0;
}

void usbApiLoop()
{
    // The body of a PUT comes first
    while (usbPutLeft > 0)
    {
        int avail = usbPort.available();

        if (avail <= 0)
        {
            if (millis() - usbPutLast > USB_PUT_TIMEOUT_MS)
            {
                usbPutLeft = 0;
                usbPutStalled();
                usbPutFinish();
            }

            return;
        }

        // A firmware's first piece has to hold its 16-byte header
#if ENABLE_OTA
        if (usbPutKind == USB_PUT_OTA && otaRunning && otaWritten == 0 &&
            (uint32_t)avail < min((uint32_t)16, usbPutLeft) &&
            millis() - usbPutLast <= USB_PUT_TIMEOUT_MS)
            return;
#endif

        uint8_t buffer[512];

        size_t n = usbPort.read(buffer,
            min((size_t)avail, min(sizeof(buffer), (size_t)usbPutLeft)));

        usbPutWrite(buffer, n);

        usbPutLeft -= n;
        usbPutLast  = millis();

        // A whole piece written away: the page may send the next one
        usbPutPiece += n;

        if (usbPutPiece >= USB_PUT_PIECE && usbPutLeft > 0)
        {
            usbPutPiece -= USB_PUT_PIECE;
            // No flush(): the core's empties the whole transmit buffer
            // when its connection check flickers, as it does around
            // flash writes, and the lost go-ahead stalled the upload
            usbPort.write((uint8_t)USB_PUT_ACK);
        }

        if (usbPutLeft == 0)
            usbPutFinish();
    }

    while (usbPort.available())
    {
        int c = usbPort.read();

        if (c == '\r')
            continue;

        if (c == '\n')
        {
            usbLine[usbLineLen] = 0;

            if (usbLineLen && !usbLineTooLong)
                usbApiRequest(usbLine);

            usbLineLen     = 0;
            usbLineTooLong = false;

            /*
             * A PUT: the bytes right behind its line are the file,
             * usually in the same transfer. Read on here and they
             * were taken for request lines, the file arriving
             * without its start ("not a wlanscout+ snapshot")
             */
            if (usbPutLeft > 0)
                return;
        }
        else if (usbLineLen < sizeof(usbLine) - 1)
        {
            usbLine[usbLineLen++] = (char)c;
        }
        else
        {
            usbLineTooLong = true;
        }
    }
}
