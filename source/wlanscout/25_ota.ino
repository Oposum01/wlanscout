// SPDX-License-Identifier: GPL-2.0-only WITH AdditionRef-wlanscout-linking-exception
// SPDX-FileCopyrightText: 2026 Dr.-Ing. Simon Wunderlich Systementwicklung & Beratung

#if ENABLE_OTA

// ============================================================
// Firmware update over the network (OTA)
//
// partitions.csv provides two firmware slots, app0 and app1. An
// upload to /api/ota goes into the slot not running, is checked
// and then booted. The previous firmware stays in its slot:
//
//   - the bootloader starts a new firmware once, "pending". It
//     confirms itself as soon as either Ethernet has held an IP
//     address for OTA_STABLE_MS (DHCP or the static fallback),
//     or it has answered an HTTP request, whichever comes first.
//     The second covers a sensor reached some other way than
//     over the W5500. If it resets before that, the bootloader
//     goes back to the previous one; if neither happens within
//     OTA_CONFIRM_MS, it rolls back on its own
//   - /api/otarollback boots the previous firmware by hand
//
// Without a second slot (partitions_xiao.csv, or the old "Huge
// APP" table) an upload is refused; firmware then goes over USB.
// ============================================================

#define OTA_CONFIRM_MS 300000
#define OTA_STABLE_MS  30000

// FW_MARKER_PREFIX, BOARD_MARKER_PREFIX and the markers: see
// FW_VERSION in wlanscout.ino

// Finds the marker in data that arrives in pieces
struct MarkerScanner
{
    const char *prefix = FW_MARKER_PREFIX;
    uint8_t matched;
    uint8_t vlen;
    bool    found;
    char    version[16];     // or the board id

    void reset()
    {
        matched = vlen = 0;
        found = false;
        version[0] = 0;
    }

    void feed(const uint8_t *data, size_t length)
    {
        const uint8_t plen = strlen(prefix);

        for (size_t i = 0; i < length && !found; i++)
        {
            char c = (char)data[i];

            if (matched < plen)
            {
                matched = (c == prefix[matched]) ? matched + 1
                        : (c == prefix[0]) ? 1 : 0;
                vlen = 0;
                continue;
            }

            // The prefix alone, as the search above keeps it,
            // is followed by a 0 and not by a version
            if (c == '\x02' && vlen > 0)
            {
                version[vlen] = 0;
                found = true;
            }
            else if (((c >= '0' && c <= '9') || c == '.' ||
                      (c >= 'a' && c <= 'z')) &&
                     vlen < sizeof(version) - 1)
                version[vlen++] = c;
            else
                matched = (c == prefix[0]) ? 1 : 0;
        }
    }
};

// Keep a new firmware pending until otaLoop() decides
extern "C" bool verifyRollbackLater()
{
    return true;
}

bool          otaRunning;
size_t        otaWritten;
char          otaError[96];
MarkerScanner otaScan;
MarkerScanner otaBoardScan;
uint32_t      otaRestartAt;      // millis(), 0 = no restart due
bool          otaPending;        // running firmware not confirmed
bool          otaIpSeen;         // ethGotIP, since otaIpSince
uint32_t      otaIpSince;        // millis()
char          otaPrevVersion[16];
bool          otaPrevChecked;

void otaInit()
{
    otaBoardScan.prefix = BOARD_MARKER_PREFIX;

    esp_ota_img_states_t state;

    otaPending =
        esp_ota_get_state_partition(esp_ota_get_running_partition(),
                                    &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY;

    if (otaPending)
        Serial.println("OTA: new firmware, waiting for confirmation");
}

/*
 * The other slot holds a firmware that did not confirm itself and
 * was rolled back from (or never finished writing): not one to go
 * back to.
 */
bool otaOtherFailed()
{
    const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
    esp_ota_img_states_t state;

    return other != NULL &&
           esp_ota_get_state_partition(other, &state) == ESP_OK &&
           (state == ESP_OTA_IMG_INVALID || state == ESP_OTA_IMG_ABORTED);
}

// Called once the log exists: say so after a rollback
void otaLogRollback()
{
    if (otaOtherFailed())
        logEventFrom("system", "firmware v%s did not confirm itself, "
                     "back on v%s", otaPreviousVersion(), FW_VERSION);
}

void otaFail(const char *text)
{
    if (otaError[0])
        return;

    snprintf(otaError, sizeof(otaError), "%s", text);

    if (otaRunning)
        Update.abort();

    otaRunning = false;
}

/*
 * The three steps of an update, the same over HTTP (a multipart
 * upload to /api/ota) and over USB ("PUT /api/ota?len=N" and the
 * bytes, see usbApiRequest()). The first piece has to hold the
 * image header.
 */
void otaStart()
{
    otaError[0] = 0;
    otaWritten  = 0;
    otaRunning  = false;
    otaScan.reset();
    otaBoardScan.reset();

    if (esp_ota_get_next_update_partition(NULL) == NULL)
    {
        otaFail("no second firmware slot in this partition table: "
                "a full install with the flasher (flash.html) or esptool");
        return;
    }

    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH))
    {
        otaFail(Update.errorString());
        return;
    }

    otaRunning = true;

    logRequest("firmware upload started");
}

void otaWrite(const uint8_t *data, size_t length)
{
    if (!otaRunning)
        return;

    // Image header: magic 0xE9, chip ID at offset 12
    if (otaWritten == 0 &&
        (length < 16 || data[0] != 0xE9 ||
         (data[12] | (data[13] << 8)) != CONFIG_IDF_FIRMWARE_CHIP_ID))
    {
        otaFail("not an ESP32-C5 firmware image");
        return;
    }

    // Its flash size in the header, 1 MB << the upper nibble: an
    // image for more flash than there is would not start
    if (otaWritten == 0 &&
        (1048576UL << (data[3] >> 4)) > ESP.getFlashChipSize())
    {
        otaFail("built for a board with more flash than this one");
        return;
    }

    otaScan.feed(data, length);
    otaBoardScan.feed(data, length);

    if (Update.write((uint8_t *)data, length) != length)
    {
        otaFail(Update.errorString());
        return;
    }

    otaWritten += length;
}

void otaFinish()
{
    if (!otaRunning)
        return;

    if (!otaScan.found)
    {
        otaFail("not a wlanscout+ firmware");
        return;
    }

    const char *board = otaBoardScan.found ? otaBoardScan.version : "kit";

    if (strcmp(board, BOARD_ID) != 0)
    {
        char text[sizeof(otaError)];

        snprintf(text, sizeof(text),
                 "firmware for the %s build, this sensor takes the %s "
                 "build", board, BOARD_ID);
        otaFail(text);
        return;
    }

    // Checks length and SHA-256 and makes the slot the next one to
    // boot
    if (!Update.end(true))
    {
        otaFail(Update.errorString());
        return;
    }

    otaRunning = false;
}

#if ENABLE_ETHERNET
void handleOtaUpload()
{
    HTTPUpload &up = server.upload();

    if (up.status == UPLOAD_FILE_START)
        otaStart();
    else if (up.status == UPLOAD_FILE_WRITE)
        otaWrite(up.buf, up.currentSize);
    else if (up.status == UPLOAD_FILE_END)
        otaFinish();
    else if (up.status == UPLOAD_FILE_ABORTED)
        otaFail("upload aborted");
}
#endif

void handleOtaDone()
{
    if (otaError[0] || otaWritten == 0)
    {
        if (!otaError[0])
            snprintf(otaError, sizeof(otaError), "no file received");

        logRequest("firmware upload failed: %s", otaError);

        outBegin();
        outStr("{\"ok\":0,\"error\":");
        outJSON(otaError);
        outStr("}");
        outEnd();

        return;
    }

    logRequest("firmware %s uploaded (%u kB), restarting",
               otaScan.version, (unsigned)(otaWritten / 1024));

    outBegin();
    outStr("{\"ok\":1,\"version\":");
    outJSON(otaScan.version);
    outStr(",\"bytes\":");
    outNum((long)otaWritten);
    outStr("}");
    outEnd();

    otaRestartAt = millis() + 1500;
    otaPrevChecked = false;
}

#endif

#if ENABLE_OTA

// Boots the other slot, the firmware that ran before
void handleOtaRollback()
{
    if (otaOtherFailed() || !Update.canRollBack() || !Update.rollBack())
    {
        reqSend(200, "application/json",
                    "{\"ok\":0,\"error\":\"no previous firmware\"}");
        return;
    }

    logRequest("rollback to firmware %s, restarting",
               otaPreviousVersion());

    reqSend(200, "application/json", "{\"ok\":1}");

    otaRestartAt = millis() + 1500;
}

// The version in the other slot, read once from its marker
const char *otaPreviousVersion()
{
    if (otaPrevChecked)
        return otaPrevVersion;

    otaPrevChecked = true;
    otaPrevVersion[0] = 0;

    const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
    esp_app_desc_t desc;

    if (other == NULL ||
        esp_ota_get_partition_description(other, &desc) != ESP_OK)
        return otaPrevVersion;

    MarkerScanner scan;
    static uint8_t chunk[2048];

    scan.reset();

    for (size_t at = 0; at < other->size && !scan.found;
         at += sizeof(chunk))
    {
        if (esp_partition_read(other, at, chunk, sizeof(chunk)) != ESP_OK)
            break;

        scan.feed(chunk, sizeof(chunk));
    }

    snprintf(otaPrevVersion, sizeof(otaPrevVersion), "%s",
             scan.found ? scan.version : "?");

    return otaPrevVersion;
}

void otaLoop()
{
    if (otaPending)
    {
        if (!ethGotIP)
            otaIpSeen = false;
        else if (!otaIpSeen)
        {
            otaIpSeen  = true;
            otaIpSince = millis();
        }

        bool ipStable = otaIpSeen &&
                        millis() - otaIpSince >= OTA_STABLE_MS;

        if (ipStable || lastRequestServed != 0)
        {
            esp_ota_mark_app_valid_cancel_rollback();

            otaPending = false;

            if (ipStable)
                logEventFrom("system", "firmware v%s confirmed, "
                             "Ethernet up for %d s", FW_VERSION,
                             OTA_STABLE_MS / 1000);
            else
                logEventFrom("system", "firmware v%s confirmed, "
                             "first request answered", FW_VERSION);
        }
        else if (millis() > OTA_CONFIRM_MS)
        {
            Serial.println("OTA: neither an IP address nor a request, "
                           "rolling back");

            esp_ota_mark_app_invalid_rollback_and_reboot();
        }
    }

    if (otaRestartAt != 0 && (int32_t)(millis() - otaRestartAt) >= 0)
    {
        catalogSave();
        bleHitsSave();

        ESP.restart();
    }
}

// "ota": the slots, for Settings -> System
void outOtaJSON()
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *other   = esp_ota_get_next_update_partition(NULL);

    outStr(",\"ota\":{\"slot\":");
    outJSON(running ? running->label : "");
    outStr(",\"size\":");
    outNum(running ? (long)running->size : 0);
    outStr(",\"other\":");
    outJSON(other ? other->label : "");
    outStr(",\"prev\":");
    outJSON(other ? otaPreviousVersion() : "");
    outStr(",\"rollback\":");
    outNum(other && !otaOtherFailed() && Update.canRollBack() ? 1 : 0);
    outStr(",\"failed\":");
    outNum(otaOtherFailed() ? 1 : 0);
    outStr(",\"pending\":");
    outNum(otaPending ? 1 : 0);
    outStr("}");
}

#endif
