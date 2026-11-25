// SPDX-License-Identifier: GPL-2.0-only WITH AdditionRef-wlanscout-linking-exception
// SPDX-FileCopyrightText: 2026 Dr.-Ing. Simon Wunderlich Systementwicklung & Beratung

// ============================================================
// Ethernet / HTTP
// ============================================================

#if ENABLE_ETHERNET
WebServer server(80);
#endif

// False without a W5500: the sensor then runs on USB alone
bool ethPresent = false;

/*
 * The sensor's id and MAC address: the W5500's address. Without
 * a W5500 the same value is derived the way its driver does, the
 * chip's Ethernet MAC made locally administered, so the id stays
 * the same either way.
 */
String sensorMacText()
{
#if ENABLE_ETHERNET
    if (ethPresent)
        return ETH.macAddress();
#endif

    uint8_t eth[6];
    uint8_t mac[6];

    esp_read_mac(eth, ESP_MAC_ETH);
    esp_derive_local_mac(mac, eth);

    char text[18];

    snprintf(text, sizeof(text), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    return String(text);
}

// The Ethernet address, "" in a build without Ethernet
String ethIpText()
{
#if ENABLE_ETHERNET
    return ETH.localIP().toString();
#else
    return String("");
#endif
}

// ============================================================
// Requests over Ethernet or USB
// ============================================================
//
// The same API answers on the web server and on the chip's own
// USB port (USB Serial/JTAG, the connector marked USB). A phone
// with the sensor on its USB-C port opens the page from a static
// site and talks to it with WebUSB, without Ethernet. The log
// stays on the UART port.
//
// Handlers take their arguments and send their answer through
// the req* functions below, which go to whichever side asked.
//
// USB protocol. A request is one line:
//
//     GET /api/networks?range=60000
//
// The answer starts with 0x02 and a header line, status code,
// content type and an optional file name, then the body in
// chunks of a two-byte length (little endian) and the bytes; a
// chunk of length 0 ends it:
//
//     0x02 "200 application/json\n" <len><bytes> ... 00 00
//
// Binary bodies (pcap) need no escaping that way.

// The core only creates HWCDCSerial with "USB CDC On Boot",
// which would move the log there
HWCDC usbPort;

// Set while a request from USB is answered
bool outToUsb = false;

// Set while a snapshot is written: answers go to this file, the
// arguments come from the same arrays as for USB
File *outFile = nullptr;

/*
 * What a snapshot keeps, see "Snapshots"; status and channel plan
 * always. Stored in the settings.
 */
#define SNAP_NETWORKS   0x01    // networks with the 6 GHz list
#define SNAP_STATIONS   0x02
#define SNAP_ASSOC      0x04    // association history
#define SNAP_SPECTRUM   0x08    // live channel data and history
#define SNAP_CATALOG    0x10
#define SNAP_BLE        0x20
#define SNAP_ZB         0x40    // 802.15.4 nodes and energy
#define SNAP_ALL        0x7F

// Without a stored choice: everything but the spectrum history, the
// biggest part, and BLE and 802.15.4, which are empty in Wi-Fi mode
#define SNAP_DEFAULT    (SNAP_ALL & ~(SNAP_SPECTRUM | SNAP_BLE | SNAP_ZB))

uint8_t snapParts = SNAP_DEFAULT;

/*
 * Remember capture mode: off, every start scans Wi-Fi. On, the mode
 * last picked (Wi-Fi, BLE or 802.15.4) is stored and the sensor
 * starts in it again; the return to Wi-Fi of an 802.15.4 sensor
 * nobody asks anything does not count as picked.
 */
bool    keepMode  = false;
uint8_t savedMode = MODE_WIFI;

// Arguments from the arrays below rather than the web server
#if ENABLE_ETHERNET
#define REQ_OWN_ARGS (outToUsb || outFile != nullptr)
#else
#define REQ_OWN_ARGS true
#endif

#define USB_ARGS_MAX 16

String  usbArgNames[USB_ARGS_MAX];
String  usbArgValues[USB_ARGS_MAX];
uint8_t usbArgCount = 0;

// The server answers with a length or chunked; chunked needs an end
bool reqChunked = false;

uint8_t reqArgs()
{
#if ENABLE_ETHERNET
    if (!REQ_OWN_ARGS)
        return (uint8_t)server.args();
#endif
    return usbArgCount;
}

String reqArgName(uint8_t i)
{
#if ENABLE_ETHERNET
    if (!REQ_OWN_ARGS)
        return server.argName(i);
#endif
    return usbArgNames[i];
}

String reqArgValue(uint8_t i)
{
#if ENABLE_ETHERNET
    if (!REQ_OWN_ARGS)
        return server.arg(i);
#endif
    return usbArgValues[i];
}

bool reqHasArg(const char *name)
{
#if ENABLE_ETHERNET
    if (!REQ_OWN_ARGS)
        return server.hasArg(name);
#endif

    for (uint8_t i = 0; i < usbArgCount; i++)
        if (usbArgNames[i] == name)
            return true;

    return false;
}

String reqArg(const char *name)
{
#if ENABLE_ETHERNET
    if (!REQ_OWN_ARGS)
        return server.arg(name);
#endif

    for (uint8_t i = 0; i < usbArgCount; i++)
        if (usbArgNames[i] == name)
            return usbArgValues[i];

    return "";
}

// Who asked, for the event log
String reqClientText()
{
#if ENABLE_ETHERNET
    if (!outToUsb)
        return server.client().remoteIP().toString();
#endif
    return String("usb");
}

/*
 * Only as much as the transmit buffer has room for: HWCDC writes what
 * does not fit over the oldest bytes queued while its connection check
 * says unplugged, and that check flickers on a healthy link (HWCDC.cpp
 * says so), around flash writes most. The interrupt sends what is
 * queued, so no flush() either, which drops the whole buffer then.
 */
#define USB_WRITE_GIVE_UP_MS 1000

void usbWriteAll(const uint8_t *data, size_t length)
{
    uint32_t moved = millis();

    while (length > 0)
    {
        int    room = usbPort.availableForWrite();
        size_t done = room > 0
                    ? usbPort.write(data, min(length, (size_t)room)) : 0;

        if (done == 0)
        {
            // Nobody reading
            if (millis() - moved > USB_WRITE_GIVE_UP_MS)
                return;

            delay(1);
            continue;
        }

        moved   = millis();
        data   += done;
        length -= done;
    }
}

#if ENABLE_ETHERNET

/*
 * What goes out over the network is gathered here and written in
 * pieces of one TCP segment, a chunk header included, with one
 * write. WebServer::sendContent() wrote every piece of about 1 kB
 * as three: chunk header, data, CRLF, and a pcap went out three
 * writes a frame, each a packet of its own over SPI to the W5500.
 * /api/networks (54 kB) took 160 packets and a quarter of a second
 * in which loop() read no frames.
 */
// One full segment of 1436 bytes (CONFIG_LWIP_TCP_MSS) with the
// chunk header ("595\r\n") and the CRLF after the data
#define TX_DATA (1436 - 7)

static char   txBuf[8 + TX_DATA + 2];   // room for "b40\r\n" in front
static size_t txLen = 0;

/*
 * A client that stops reading in the middle of an answer - a VPN
 * switched off, a laptop gone to sleep - sends no reset, its
 * packets just stop. NetworkClient::write() then waits up to 10 s
 * for room (ten selects of one second) and gives up, and the next
 * piece waited again: a 60 kB answer held loop() for almost five
 * minutes, no frames read, no other client let in. The pieces now
 * go out through txWrite(), which gives up after TX_STALL_MS
 * without progress, and after the first that does not go through
 * the answer is given up. Meanwhile the management queue fills up,
 * so the wait is kept short: a client on the LAN or over a VPN
 * makes room within milliseconds.
 */
#define TX_STALL_MS 2000
static bool     txAborted = false;
static uint32_t txSent    = 0;
static char     txWho[16] = "";    // the client, kept before a failed write can stop it
static bool     txClosed  = false; // the client closed, rather than stopped reading

#endif

/*
 * Answers given up since the start, for the CPU section in
 * Settings. They are not logged as events: a client going away is
 * not the sensor's problem and would push the rest out of the log.
 */
uint32_t txGivenUp     = 0;
uint32_t txGivenUpAt   = 0;        // millis() of the last one
char     txGivenUpWho[16] = "";

#if ENABLE_ETHERNET

static size_t txWrite(const uint8_t *p, size_t n)
{
    int fd = server.client().fd();

    if (fd < 0)
        return 0;

    size_t   sent = 0;
    uint32_t progress = millis();

    while (sent < n)
    {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(fd, &set);

        struct timeval tv = { 0, 100000 };

        int ready = select(fd + 1, nullptr, &set, nullptr, &tv);

        if (ready < 0)
        {
            txClosed = true;
            break;
        }

        if (ready > 0)
        {
            int res = send(fd, p + sent, n - sent, MSG_DONTWAIT);

            if (res > 0)
            {
                sent    += res;
                progress = millis();
                continue;
            }

            if (res < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
            {
                txClosed = true;
                break;
            }
        }

        if (millis() - progress > TX_STALL_MS)
            break;
    }

    return sent;
}

static void txFlush()
{
    if (txLen == 0)
        return;

    char  *data = txBuf + 8;
    char  *from = data;
    size_t n    = txLen;

    if (reqChunked)
    {
        char head[8];

        int h = snprintf(head, sizeof(head), "%x\r\n", (unsigned)txLen);

        from = data - h;
        memcpy(from, head, h);

        data[txLen]     = '\r';
        data[txLen + 1] = '\n';

        n = h + txLen + 2;
    }

    txLen = 0;

    if (txAborted)
        return;

    size_t done = txWrite((const uint8_t *)from, n);

    txSent += done;

    if (done == n)
        return;

    txAborted = true;

    txGivenUp++;
    txGivenUpAt = millis();
    strlcpy(txGivenUpWho, txWho, sizeof(txGivenUpWho));

    Serial.printf("[tx] answer to %s given up after %lu kB, the client %s "
                  "(%s)\r\n", txWho, (unsigned long)(txSent / 1024),
                  txClosed ? "closed the connection" : "stopped reading",
                  statsPath);

    server.client().stop();
}

#endif

/*
 * Start an answer. length -1 when it is not known up front;
 * filename makes the browser save it.
 */
void reqStart(int code, const char *type, int32_t length,
              const char *filename)
{
    // A snapshot keeps the body only
    if (outFile)
        return;

    if (outToUsb)
    {
        String head = String((char)0x02) + code + " " + type;

        if (filename)
            head += String(" ") + filename;

        head += "\n";

        usbWriteAll((const uint8_t *)head.c_str(), head.length());

        return;
    }

#if ENABLE_ETHERNET
    reqChunked = (length < 0);

    server.setContentLength(length < 0 ? CONTENT_LENGTH_UNKNOWN
                                       : (size_t)length);

    if (filename)
        server.sendHeader("Content-Disposition",
                          String("attachment; filename=") + filename);

    server.send(code, type, "");

    txLen     = 0;
    txSent    = 0;
    txAborted = false;
    txClosed  = false;

    strlcpy(txWho, reqClientText().c_str(), sizeof(txWho));
#endif
}

void reqContent(const void *data, size_t length)
{
    if (outFile)
    {
        outFile->write((const uint8_t *)data, length);
        return;
    }

#if ENABLE_ETHERNET
    if (!outToUsb)
    {
        const char *p = (const char *)data;

        while (length > 0)
        {
            size_t part = std::min(length, (size_t)TX_DATA - txLen);

            memcpy(txBuf + 8 + txLen, p, part);

            txLen  += part;
            p      += part;
            length -= part;

            if (txLen == TX_DATA)
                txFlush();
        }

        return;
    }
#endif

    const uint8_t *p = (const uint8_t *)data;

    while (length > 0)
    {
        uint16_t part = length > 4096 ? 4096 : (uint16_t)length;
        uint8_t  head[2] = { (uint8_t)(part & 0xFF), (uint8_t)(part >> 8) };

        usbWriteAll(head, 2);
        usbWriteAll(p, part);

        p      += part;
        length -= part;
    }
}

void reqDone()
{
    if (outFile)
        return;

    if (outToUsb)
    {
        uint8_t end[2] = { 0, 0 };

        usbWriteAll(end, 2);

        return;
    }

#if ENABLE_ETHERNET
    txFlush();

    if (reqChunked && !txAborted)
        server.sendContent("");
#endif

    reqChunked = false;
}

// A whole answer at once
void reqSend(int code, const char *type, const String &body)
{
    if (outFile)
    {
        outFile->write((const uint8_t *)body.c_str(), body.length());
        return;
    }

#if ENABLE_ETHERNET
    if (!outToUsb)
    {
        server.send(code, type, body);
        return;
    }
#endif

    reqStart(code, type, body.length(), nullptr);
    reqContent(body.c_str(), body.length());
    reqDone();
}
