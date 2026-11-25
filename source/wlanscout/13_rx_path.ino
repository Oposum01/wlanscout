// SPDX-License-Identifier: GPL-2.0-only WITH AdditionRef-wlanscout-linking-exception
// SPDX-FileCopyrightText: 2026 Dr.-Ing. Simon Wunderlich Systementwicklung & Beratung

// ============================================================
// Management frame dispatch
// ============================================================

/*
 * Beacons, probes and (re)association frames are never encrypted;
 * one with the Protected bit is something else in their shape (a
 * ZTE device sends such "beacons", 8 bytes of its own and then
 * ciphertext). Its body would read as elements of random IDs, so
 * it is neither parsed nor catalogued, only captured.
 */
bool protectedNonsense(const uint8_t *frame, int length)
{
    if (length < 2 || (frame[0] & 0x0F) != 0 || !(frame[1] & 0x40))
        return false;

    uint8_t subtype = (frame[0] >> 4) & 0x0F;

    return subtype <= 5 || subtype == 8;
}

void parseManagementFrame(const uint8_t *frame, uint16_t length,
                          int8_t rssi, uint8_t channelHint)
{
    if (length < 24 || protectedNonsense(frame, length))
        return;

    uint16_t frameControl = frame[0] | (frame[1] << 8);
    uint8_t  subtype      = (frameControl >> 4) & 0x0F;

    if (subtype == 4)
    {
        parseProbeRequest(frame, length, rssi, channelHint);
        return;
    }

    // 0 = Association Request, 2 = Reassociation Request
    if (subtype == 0 || subtype == 2)
    {
        parseAssocRequest(frame, length, rssi, channelHint,
                          subtype == 2);
        return;
    }

    // 1 = Association Response, 3 = Reassociation Response
    if (subtype == 1 || subtype == 3)
    {
        parseAssocResponse(frame, length);
        return;
    }

    // 8 = Beacon, 5 = Probe Response
    if ((subtype == 8 || subtype == 5) &&
        (length <= 36 || elementsPlausible(frame + 36, length - 36)))
        parseBeacon(frame, length, rssi, channelHint);

    // 13 = Action, of which AWDL and Fine Timing Measurement
    if (subtype == 13)
    {
        parseAwdl(frame, length, rssi, channelHint);
        parseFtm(frame, length, rssi, channelHint);
    }
}

// ============================================================
// Frame queues
//
// Single producer (Wi-Fi task) / single consumer (loop).
// ============================================================

struct MgmtFrameCopy
{
    uint64_t us;
    uint16_t length;
    int8_t   rssi;
    uint8_t  channel;
    int8_t   noise;
    uint8_t  rxRate;
    uint8_t  data[MGMT_PARSE_MAX];
};

MgmtFrameCopy *mgmtQueue = nullptr;   // PSRAM

volatile uint8_t mgmtWrite = 0;
volatile uint8_t mgmtRead  = 0;

// Highest fill of the queue, in the current statistics window and
// since the start; MGMT_QUEUE_SIZE - 1 means it was full
volatile uint8_t mgmtFillWin  = 0;
volatile uint8_t mgmtFillBoot = 0;

#define DATA_QUEUE_SIZE 64

struct DataFrameInfo
{
    uint8_t  bssid[6];
    uint8_t  sta[6];
    uint8_t  peer[6];
    int8_t   rssi;
    uint8_t  channel;
    bool     uplink;
    uint16_t length;
};

DataFrameInfo *dataQueue = nullptr;   // PSRAM

volatile uint8_t dataWrite = 0;
volatile uint8_t dataRead  = 0;

// Frames dropped on a full queue
volatile uint32_t mgmtDrops = 0;
volatile uint32_t dataDrops = 0;

/*
 * For the heap probe: frames handed to the callback by kind, and
 * the longest the callback took, so a snapshot says how many frames
 * came in just before and whether the callback kept up.
 */
volatile uint32_t cbMgmtFrames = 0;
volatile uint32_t cbDataFrames = 0;
volatile uint32_t cbCtrlFrames = 0;
volatile uint32_t cbMaxUs      = 0;
volatile uint32_t cbTotalUs    = 0;

/*
 * Frames that came with the channel before the last switch, and how
 * long after the switch the latest of them came (cbChannel and
 * cbSwitchUs, set in switchChannel())
 */
volatile uint32_t cbStaleFrames = 0;

/*
 * Frames whose end-of-reception state says failed. The C5 has this
 * second state next to rx_state; which of the two marks a bad FCS is
 * not documented, so the test build counts both.
 */
volatile uint32_t cbRxEndBad = 0;
volatile uint32_t cbStaleMaxUs  = 0;

uint8_t mgmtQueueFill()
{
    return (mgmtWrite + MGMT_QUEUE_SIZE - mgmtRead) % MGMT_QUEUE_SIZE;
}

uint8_t dataQueueFill()
{
    return (dataWrite + DATA_QUEUE_SIZE - dataRead) % DATA_QUEUE_SIZE;
}

// Frames discarded because the FCS was wrong
volatile uint32_t fcsErrors = 0;

// ============================================================
// Promiscuous callback
// ============================================================

/*
 * The rate code kept for the radiotap Rate field, or
 * RX_RATE_UNKNOWN. Only the code is checked here, the conversion
 * to 500 kbit/s units waits for the export (radiotapRateOf()): this
 * runs for every frame and stays free of tables in flash.
 */
uint8_t IRAM_ATTR rxRateOf(unsigned bbFormat, unsigned rate)
{
    // RX_BB_FORMAT_11B: 1, 2, 5.5, 11 Mbit/s, long or short preamble
    if (bbFormat == 0 && rate <= 7 && rate != 4)
        return rate;

    // RX_BB_FORMAT_11G (also 11A): the L-SIG rate, 6 to 54 Mbit/s
    if (bbFormat == 1 && rate >= 8 && rate <= 15)
        return rate;

    return RX_RATE_UNKNOWN;
}

void IRAM_ATTR wifiPromiscuousFrame(void *buf,
                                    wifi_promiscuous_pkt_type_t type)
{
    if (buf == nullptr)
        return;

    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;

    if (type == WIFI_PKT_MGMT)
        cbMgmtFrames += 1;
    else if (type == WIFI_PKT_DATA)
        cbDataFrames += 1;
    else if (type == WIFI_PKT_CTRL)
        cbCtrlFrames += 1;

    if (cbChannel != 0 && pkt->rx_ctrl.channel != cbChannel)
    {
        uint32_t after = (uint32_t)(esp_timer_get_time() - cbSwitchUs);

        cbStaleFrames += 1;

        if (after > cbStaleMaxUs)
            cbStaleMaxUs = after;
    }

    /*
     * Drop frames that failed the FCS.
     *
     * Without this a single flipped bit in an address turns
     * into a phantom BSSID, a phantom station or a bogus
     * roaming event. The BSSIDs a dual-band AP uses on 2.4 and
     * 5 GHz differ in a single bit often enough for that to
     * look entirely plausible.
     *
     * rx_state is 0 when the frame was received cleanly.
     */
#if HEAP_PROBE_RX_ALL && CONFIG_SOC_WIFI_MAC_VERSION_NUM == 3
    if (pkt->rx_ctrl.rxend_state != 0)
        cbRxEndBad += 1;
#endif

    if (pkt->rx_ctrl.rx_state != 0)
    {
        fcsErrors += 1;
        return;
    }

#if HEAP_PROBE_RX_ALL
    // Counted above for the probe only; the channel load stays as it was
    if (type == WIFI_PKT_CTRL)
        return;
#endif

    if (type == WIFI_PKT_MGMT ||
        type == WIFI_PKT_DATA ||
        type == WIFI_PKT_CTRL)
    {
        int8_t rssi = pkt->rx_ctrl.rssi;

        portENTER_CRITICAL_ISR(&measurementMux);

        rxPackets += 1;
        rxRSSISum += rssi;
        rxRSSICount += 1;

        if (rssi > rxPeakRSSI)
            rxPeakRSSI = rssi;

        portEXIT_CRITICAL_ISR(&measurementMux);
    }

    const uint8_t *frame = pkt->payload;

    /*
     * A transmitter address with the group bit set cannot come
     * from a real sender. Such a frame is noise that got past the
     * FCS check, and would put made-up elements into the catalog
     * and a phantom station into the tables. Only for management
     * and data frames: in an RTS or CF-End the bit signals the
     * bandwidth (802.11ac). It still counted for the channel load
     * above, the air was busy all the same.
     */
    if ((type == WIFI_PKT_MGMT || type == WIFI_PKT_DATA) &&
        pkt->rx_ctrl.sig_len >= 16 && (frame[10] & 0x01))
        return;

    // --------------------------------------------------------
    // Data frames: derive BSSID, station and peer
    //
    // ToDS=1: Addr1 BSSID, Addr2 STA (SA), Addr3 DA
    // FromDS=1: Addr1 STA (DA), Addr2 BSSID, Addr3 SA
    // --------------------------------------------------------

    if (type == WIFI_PKT_DATA)
    {
        if (pkt->rx_ctrl.sig_len < 24)
            return;

        bool toDS   = (frame[1] & 0x01) != 0;
        bool fromDS = (frame[1] & 0x02) != 0;

        const uint8_t *a1 = &frame[4];
        const uint8_t *a2 = &frame[10];
        const uint8_t *a3 = &frame[16];

        const uint8_t *bssid = nullptr;
        const uint8_t *sta   = nullptr;
        const uint8_t *peer  = a3;

        bool uplink = false;

        /*
         * Only the two infrastructure cases are evaluated.
         *
         * ToDS=1:   Addr1 = RA = BSSID, Addr2 = TA = station
         * FromDS=1: Addr1 = RA = station, Addr2 = TA = BSSID
         *
         * ToDS=0 and FromDS=0 used to be treated as IBSS with
         * Addr3 as the BSSID. That was wrong far more often
         * than it was right: TDLS direct links, Wi-Fi Direct
         * and mesh traffic all carry those bits, and the
         * result was a station whose BSSID ended up being its
         * own address or that of another client. Those frames
         * are ignored now.
         */
        if (toDS && !fromDS)
        {
            bssid  = a1;
            sta    = a2;
            uplink = true;
        }
        else if (!toDS && fromDS)
        {
            bssid = a2;
            sta   = a1;
        }
        else
        {
            return;
        }

        if ((sta[0] & 0x01) || (bssid[0] & 0x01))
            return;

        // A station cannot be its own access point
        if (memcmp(sta, bssid, 6) == 0)
            return;

        uint8_t next = (dataWrite + 1) % DATA_QUEUE_SIZE;

        if (next == dataRead)
        {
            dataDrops += 1;
            return;
        }

        DataFrameInfo &d = dataQueue[dataWrite];

        memcpy(d.bssid, bssid, 6);
        memcpy(d.sta, sta, 6);
        memcpy(d.peer, peer, 6);

        d.rssi    = pkt->rx_ctrl.rssi;
        d.channel = pkt->rx_ctrl.channel;
        d.uplink  = uplink;
        d.length  = pkt->rx_ctrl.sig_len;

        dataWrite = next;

        return;
    }

    // --------------------------------------------------------
    // Beacon, probe response, probe request
    // --------------------------------------------------------

    if (type != WIFI_PKT_MGMT)
        return;

    uint16_t frameControl = frame[0] | (frame[1] << 8);
    uint8_t  subtype      = (frameControl >> 4) & 0x0F;

    /*
     * 8 beacon, 5 probe response, 4 probe request,
     * 0 assoc request, 2 reassoc request, 1 and 3 their
     * responses: these are parsed, whatever the capture keeps.
     *
     * Everything else is only queued when it is wanted for
     * the capture, since the parser ignores it anyway.
     */
    bool parsed =
        (subtype == 8 || subtype == 5 || subtype == 4 ||
         subtype == 0 || subtype == 2 || subtype == 1 || subtype == 3);

    /*
     * 13 action: only AWDL (category 127, Apple's OUI, type 8) and
     * Fine Timing Measurement (category 4, actions 32 and 33) are
     * parsed, the rest goes to the capture if wanted. Checked here
     * so that 802.11k/v and block ack traffic does not fill the
     * queue.
     */
    if (subtype == 13 && pkt->rx_ctrl.sig_len >= 24 + 16 + 4 &&
        frame[24] == 0x7F && frame[25] == 0x00 && frame[26] == 0x17 &&
        frame[27] == 0xF2 && frame[28] == 0x08)
        parsed = true;

    if (subtype == 13 && pkt->rx_ctrl.sig_len >= 24 + 3 + 4 &&
        frame[24] == 4 && (frame[25] == 32 || frame[25] == 33))
        parsed = true;

    bool wanted =
        parsed ||
        (pcapEnabled && (pcapMask & PCAP_OTHER));

    if (!wanted)
        return;

    uint16_t length = pkt->rx_ctrl.sig_len;

    // Strip the FCS
    if (length >= 4)
        length -= 4;

    if (length > MGMT_PARSE_MAX)
        length = MGMT_PARSE_MAX;

    uint8_t next = (mgmtWrite + 1) % MGMT_QUEUE_SIZE;

    uint8_t fill = (mgmtWrite + MGMT_QUEUE_SIZE - mgmtRead) %
                   MGMT_QUEUE_SIZE + 1;

    if (fill > mgmtFillWin)
        mgmtFillWin = fill;

    if (fill > mgmtFillBoot)
        mgmtFillBoot = fill;

    if (next == mgmtRead)
    {
        mgmtDrops += 1;
        return;
    }

    MgmtFrameCopy &entry = mgmtQueue[mgmtWrite];

    entry.us      = esp_timer_get_time();
    entry.length  = length;
    entry.rssi    = pkt->rx_ctrl.rssi;
    entry.channel = pkt->rx_ctrl.channel;
    entry.noise   = pkt->rx_ctrl.noise_floor;
    entry.rxRate  = rxRateOf(pkt->rx_ctrl.cur_bb_format,
                             pkt->rx_ctrl.rate);

    memcpy(entry.data, frame, length);

    mgmtWrite = next;
}

// The callback the driver calls, timed for the heap probe (cbMaxUs)
void IRAM_ATTR wifiPromiscuousCallback(void *buf,
                                       wifi_promiscuous_pkt_type_t type)
{
    int64_t start = esp_timer_get_time();

    wifiPromiscuousFrame(buf, type);

    uint32_t us = (uint32_t)(esp_timer_get_time() - start);

    cbTotalUs += us;

    if (us > cbMaxUs)
        cbMaxUs = us;
}

// ============================================================
// Process queues
// ============================================================

void frameLogAppend(struct FrameLog &log, const struct MgmtFrameCopy &frame)
{
    frameLogAppendBytes(log, frame.us, frame.data, frame.length, frame.rssi,
                        frame.channel, frame.noise, frame.rxRate);
}

// Any frame, also one that did not come through the Wi-Fi queue
void frameLogAppendBytes(struct FrameLog &log, uint64_t us,
                         const uint8_t *data, uint16_t length, int8_t rssi,
                         uint8_t channel, int8_t noise, uint8_t rxRate)
{
    uint32_t slotSize = frameSlotSize(length);

    if (log.chunkCount == 0 && !frameLogGrow(log))
        return;

    bool fits =
        log.chunks[log.writeChunk].usedBytes + slotSize <= FRAME_CHUNK_BYTES;

    if (!fits && !frameLogGrow(log))
    {
        // No memory for another chunk: reuse the oldest
        log.writeChunk = nextChunk(log, log.writeChunk);

        frameLogDropChunk(log, log.chunks[log.writeChunk]);
    }

    FrameChunk &chunk = log.chunks[log.writeChunk];
    FrameHead  *head  = (FrameHead *)(chunk.memory + chunk.usedBytes);

    head->us         = us;
    head->length     = length;
    head->origLength = length;
    head->rssi       = rssi;
    head->channel    = channel;
    head->noise      = noise;
    head->rxRate     = rxRate;

    memcpy(head + 1, data, length);

    chunk.usedBytes  += slotSize;
    chunk.frameCount += 1;
    chunk.frameBytes += length;

    log.frameCount   += 1;
    log.frameBytes   += length;
    log.framesStored += 1;
}

/*
 * Keep the frame as it was received.
 *
 * Everything the parser reads is a subset of what is in
 * there, so holding on to the raw bytes costs a memcpy and
 * gains a full Wireshark dissection later.
 */
void pcapStore(const struct MgmtFrameCopy &entry)
{
    if (!pcapEnabled || entry.length < 2)
        return;

    uint16_t fc      = entry.data[0] | (entry.data[1] << 8);
    uint8_t  type    = (fc >> 2) & 0x03;
    uint8_t  subtype = (fc >> 4) & 0x0F;

    if (type != 0)
        return;

    uint8_t want = 0;

    if (subtype == 8 || subtype == 5)
        want = PCAP_BEACONS;
    else if (subtype == 4)
        want = PCAP_PROBEREQ;
    else if (subtype == 0 || subtype == 2)
        want = PCAP_ASSOC;
    else
        want = PCAP_OTHER;      // auth, deauth, disassoc, action

    if (!(pcapMask & want))
        return;

    /*
     * A beacon replaces the previous one of the same BSSID
     * instead of consuming a ring slot. Probe responses stay
     * in the ring: they are rare and they are what reveals a
     * hidden SSID.
     */
    if (subtype == 8 && beaconSlots != nullptr && entry.length >= 16)
    {
        const uint8_t *ta = &entry.data[10];

        int      idx      = -1;
        int      freeSlot = -1;
        uint64_t oldest   = UINT64_MAX;
        int      oldestIx = 0;

        for (int i = 0; i < MAX_BEACON_SLOTS; i++)
        {
            if (!beaconSlots[i].valid)
            {
                if (freeSlot < 0)
                    freeSlot = i;

                continue;
            }

            if (memcmp(beaconSlots[i].bssid, ta, 6) == 0)
            {
                idx = i;
                break;
            }

            if (beaconSlots[i].rec.us < oldest)
            {
                oldest   = beaconSlots[i].rec.us;
                oldestIx = i;
            }
        }

        if (idx < 0)
            idx = (freeSlot >= 0) ? freeSlot : oldestIx;

        BeaconSlot &b = beaconSlots[idx];

        b.valid = true;

        memcpy(b.bssid, ta, 6);

        b.rec.us         = entry.us;
        b.rec.length     = min((int)entry.length, MGMT_FRAME_MAX);
        b.rec.origLength = entry.length;
        b.rec.rssi       = entry.rssi;
        b.rec.channel    = entry.channel;
        b.rec.noise      = entry.noise;
        b.rec.rxRate     = entry.rxRate;

        memcpy(b.rec.data, entry.data, b.rec.length);

        beaconTotal++;

        return;
    }

    // Association and reassociation requests have their own log
    if (subtype == 0 || subtype == 2)
    {
        frameLogAppend(assocLog, entry);
        return;
    }

    // Everything else whole, at its own length: probe responses
    // past 512 octets are no longer cut off
    frameLogAppend(ringLog, entry);
}
