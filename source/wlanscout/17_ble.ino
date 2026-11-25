// SPDX-License-Identifier: GPL-2.0-only WITH AdditionRef-wlanscout-linking-exception
// SPDX-FileCopyrightText: 2026 Dr.-Ing. Simon Wunderlich Systementwicklung & Beratung

// ============================================================
// BLE
//
// The scanner is exclusive: 2.4 GHz Wi-Fi promiscuous mode
// and BLE share the same radio. Running both at once would
// slow each other down and skew the RSSI readings.
//
// Only the advertising channels 37/38/39 are captured. Data
// channels of established connections cannot be sniffed
// with this controller.
//
// CORE VERSION NOTE
// Written for arduino-esp32 3.x (required for the C5
// anyway). On 2.x two spots need adjusting, both marked
// "CORE 2.x" below.
// ============================================================

// A 128-bit UUID as text is 36 characters
#define BLE_SVC_TEXT 37

struct BleDevice
{
    bool     valid;
    uint8_t  addr[6];
    uint8_t  addrType;      // 0 public, 1 random (the PDU's TxAdd)
    char     name[25];
    char     svc[BLE_SVC_TEXT];
    int8_t   rssi;
    int8_t   rssiPeak;
    int8_t   txPower;       // 127 = not advertised
    uint16_t company;       // 0xFFFF = no manufacturer data
    uint32_t adverts;
    uint32_t firstSeen;
    uint32_t lastSeen;
};

BleDevice *bleDevices = nullptr;      // PSRAM

struct BleAdvInfo
{
    uint64_t us;
    uint8_t  addr[6];
    uint8_t  addrType;
    uint8_t  advType;       // the report's event type, 0..4
    int8_t   rssi;
    int8_t   txPower;
    uint16_t company;
    char     name[25];
    char     svc[BLE_SVC_TEXT];
    uint8_t  payloadLen;
    uint8_t  payload[BLE_ADV_MAX];   // advertising data + scan response
};

BleAdvInfo *bleQueue = nullptr;       // PSRAM

volatile uint8_t bleWrite = 0;
volatile uint8_t bleRead  = 0;

volatile uint32_t bleDrops  = 0;
volatile uint32_t bleFrames = 0;

uint8_t scanMode = MODE_WIFI;

bool bleInitialised = false;
bool bleActiveScan  = false;

uint32_t bleLastAdvert  = 0;
uint32_t bleScanStarted = 0;

BLEScan *bleScan = nullptr;

// ------------------------------------------------------------
// Company Identifier
//
// Small deliberate subset. The full list is maintained by
// the Bluetooth SIG under "Assigned Numbers".
// ------------------------------------------------------------

struct CompanyEntry
{
    uint16_t    id;
    const char *name;
};

static const CompanyEntry companyTable[] = {
    { 0x0001, "Nokia" },
    { 0x0002, "Intel" },
    { 0x0006, "Microsoft" },
    { 0x000D, "Texas Instruments" },
    { 0x000F, "Broadcom" },
    { 0x004C, "Apple" },
    { 0x0059, "Nordic Semiconductor" },
    { 0x0075, "Samsung" },
    { 0x0087, "Garmin" },
    { 0x009E, "Bose" },
    { 0x00E0, "Google" },
    { 0x0157, "Huami / Amazfit" },
    { 0x02E5, "Espressif" }
};

#define COMPANY_TABLE_SIZE \
    (sizeof(companyTable) / sizeof(companyTable[0]))

const char *lookupCompany(uint16_t id)
{
    for (size_t i = 0; i < COMPANY_TABLE_SIZE; i++)
    {
        if (companyTable[i].id == id)
            return companyTable[i].name;
    }

    return "";
}

// ------------------------------------------------------------
// Address type
//
// The advertisement says whether the address is public (an
// IEEE OUI) or random (TxAdd). Only a random one's top two
// bits tell its kind:
//
// 0b11 = random static
// 0b01 = resolvable private
// 0b00 = non-resolvable private
// ------------------------------------------------------------

const char *bleAddrType(const uint8_t *addr, uint8_t addrType)
{
    if (!(addrType & 1))
        return "public";

    uint8_t top = addr[0] >> 6;

    if (top == 0x03)
        return "random static";

    if (top == 0x01)
        return "resolvable private";

    if (top == 0x00)
        return "non-resolvable";

    return "random";
}

// An address that stays: public or random static, not one that
// changes every few minutes
bool bleStableAddr(const uint8_t *addr, uint8_t addrType)
{
    return !(addrType & 1) || (addr[0] >> 6) == 0x03;
}

// ------------------------------------------------------------
// Advertising-Callback
//
// Runs in the BLE host task, so only queue here and do the
// evaluation in loop().
// ------------------------------------------------------------

void queueBleAdvert(BLEAdvertisedDevice &dev)
{
    uint8_t next = (bleWrite + 1) % BLE_QUEUE_SIZE;

    if (next == bleRead)
    {
        bleDrops += 1;
        return;
    }

    BleAdvInfo &e = bleQueue[bleWrite];

    memset(&e, 0, sizeof(e));

    // Parse the address from its text form, that is stable
    // across core versions.
    unsigned int b[6] = { 0, 0, 0, 0, 0, 0 };

    if (sscanf(dev.getAddress().toString().c_str(),
               "%x:%x:%x:%x:%x:%x",
               &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6)
    {
        return;
    }

    for (int i = 0; i < 6; i++)
        e.addr[i] = (uint8_t)b[i];

    e.us       = esp_timer_get_time();
    e.addrType = dev.getAddressType();
    e.advType  = dev.getAdvType();

    // Advertising data and, actively scanned, the scan response
    size_t plen = dev.getPayloadLength();

    if (plen > BLE_ADV_MAX)
        plen = BLE_ADV_MAX;

    if (plen && dev.getPayload() != nullptr)
        memcpy(e.payload, dev.getPayload(), plen);

    e.payloadLen = (uint8_t)plen;

    e.rssi    = dev.getRSSI();
    e.txPower = dev.haveTXPower() ? dev.getTXPower() : 127;
    e.company = 0xFFFF;

    if (dev.haveName())
        copyPrintable(e.name, sizeof(e.name), dev.getName().c_str());

    if (dev.haveServiceUUID())
    {
        copyPrintable(e.svc,
                      sizeof(e.svc),
                      dev.getServiceUUID().toString().c_str());
    }

    if (dev.haveManufacturerData())
    {
        // Core 3.3.x returns an Arduino String here, older
        // cores returned std::string. Both expose length()
        // and operator[], so this works either way.
        String md = dev.getManufacturerData();

        if (md.length() >= 2)
        {
            e.company =
                (uint16_t)(uint8_t)md[0] |
                ((uint16_t)(uint8_t)md[1] << 8);
        }
    }

    bleWrite = next;

    bleFrames += 1;
}

// The Arduino BLE library declares
//     virtual void onResult(BLEAdvertisedDevice device) = 0;
// i.e. the device is passed by value. Some NimBLE forks use
// a pointer instead; in that case change the signature to
//     void onResult(BLEAdvertisedDevice *dev)
// and pass *dev below.
class BleScanCallbacks : public BLEAdvertisedDeviceCallbacks
{
    void onResult(BLEAdvertisedDevice dev) override
    {
        queueBleAdvert(dev);
    }
};

BleScanCallbacks bleCallbacks;

// ------------------------------------------------------------
// Drain the queue
// ------------------------------------------------------------

void processBleQueue()
{
    while (bleRead != bleWrite)
    {
        BleAdvInfo &e = bleQueue[bleRead];

        int      idx      = -1;
        int      freeSlot = -1;
        uint32_t oldest   = UINT32_MAX;
        int      oldestIx = 0;

        for (int i = 0; i < MAX_BLE_DEVICES; i++)
        {
            if (bleDevices[i].valid)
            {
                if (memcmp(bleDevices[i].addr, e.addr, 6) == 0)
                {
                    idx = i;
                    break;
                }

                if (bleDevices[i].lastSeen < oldest)
                {
                    oldest   = bleDevices[i].lastSeen;
                    oldestIx = i;
                }
            }
            else if (freeSlot < 0)
            {
                freeSlot = i;
            }
        }

        if (idx < 0)
        {
            idx = (freeSlot >= 0) ? freeSlot : oldestIx;

            memset(&bleDevices[idx], 0, sizeof(BleDevice));

            bleDevices[idx].valid     = true;
            bleDevices[idx].firstSeen = millis();
            bleDevices[idx].rssiPeak  = -127;
            bleDevices[idx].txPower   = 127;      // none until sent
            bleDevices[idx].company   = 0xFFFF;

            memcpy(bleDevices[idx].addr, e.addr, 6);
        }

        BleDevice &d = bleDevices[idx];

        d.addrType = e.addrType;

        bleLogAdvert(e);
        bleHitsNote(e);

        d.rssi = e.rssi;

        if (e.rssi > d.rssiPeak)
            d.rssiPeak = e.rssi;

        if (e.txPower != 127)
            d.txPower = e.txPower;

        if (e.company != 0xFFFF)
            d.company = e.company;

        if (e.name[0])
            strncpy(d.name, e.name, sizeof(d.name) - 1);

        if (e.svc[0])
            strncpy(d.svc, e.svc, sizeof(d.svc) - 1);

        d.adverts++;
        d.lastSeen = millis();

        bleLastAdvert = millis();

        bleRead = (bleRead + 1) % BLE_QUEUE_SIZE;
    }
}

void cleanupBleDevices()
{
    uint32_t now = millis();

    for (int i = 0; i < MAX_BLE_DEVICES; i++)
    {
        if (!bleDevices[i].valid)
            continue;

        if (now - bleDevices[i].lastSeen > BLE_TIMEOUT_MS)
            bleDevices[i].valid = false;
    }
}
// API: BLE
// ============================================================

void sendBleJSON()
{
    cleanupBleDevices();

    uint32_t now = millis();

    outBegin();

    outStr("{\"mode\":");
    outNum(scanMode);

    outStr(",\"active\":");
    outNum(bleActiveScan ? 1 : 0);

    outStr(",\"frames\":");
    outNum(bleFrames);

    outStr(",\"drops\":");
    outNum(bleDrops);

    outStr(",\"devices\":[");

    bool first = true;

    for (int i = 0; i < MAX_BLE_DEVICES; i++)
    {
        if (!bleDevices[i].valid)
            continue;

        if (!first)
            outStr(",");

        first = false;

        BleDevice &d = bleDevices[i];

        outStr("{\"addr\":");
        outMac(d.addr);

        outStr(",\"type\":");
        outJSON(bleAddrType(d.addr, d.addrType));

        outStr(",\"name\":");
        outJSON(d.name);

        outStr(",\"svc\":");
        outJSON(d.svc);

        // A random address has no OUI to look up
        outStr(",\"vendor\":");
        outJSON(d.addrType & 1 ? "" : lookupVendor(d.addr));

        outStr(",\"company\":");
        outNum(d.company);

        outStr(",\"companyName\":");
        outJSON(d.company == 0xFFFF ? "" : lookupCompany(d.company));

        outStr(",\"rssi\":");
        outNum(d.rssi);

        outStr(",\"peak\":");
        outNum(d.rssiPeak);

        outStr(",\"tx\":");
        outNum(d.txPower);

        outStr(",\"adv\":");
        outNum(d.adverts);

        uint32_t span = (now - d.firstSeen) / 1000;

        outStr(",\"rate\":");
        outFixed(span > 0 ? (float)d.adverts / span : 0.0f, 2);

        outStr(",\"age\":");
        outNum((now - d.lastSeen) / 1000);

        outStr(",\"seen\":");
        outNum(span);

        outStr("}");
    }

    outStr("]}");

    outEnd();
}
