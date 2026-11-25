// SPDX-License-Identifier: GPL-2.0-only WITH AdditionRef-wlanscout-linking-exception
// SPDX-FileCopyrightText: 2026 Dr.-Ing. Simon Wunderlich Systementwicklung & Beratung

volatile bool ethGotIP = false;

volatile bool ethLinkUp = false;

uint32_t dhcpAttemptStart = 0;
uint8_t  dhcpAttempts     = 0;
bool     ethStaticActive  = false;

volatile bool ethLinkChanged = false;

// The address dropped by lwIP, taken over by ethLoop()
volatile bool ethIpLost = false;

uint8_t  reinitCount = 0;
uint32_t lastReinit  = 0;

// "ethreinit" on the serial console, carried out by ethLoop()
bool ethReinitRequested = false;

// When setupEthernet() last started the W5500, for w5500WasReset()
uint32_t ethStartedAt = 0;

uint32_t ethStaticSince = 0;

/*
 * The DHCP lease as lwIP holds it, followed in loop() by
 * dhcpLeaseWatch(). A renewal that keeps the address raises no
 * event in ESP-IDF, so the client's state is read instead.
 */
struct DhcpLease
{
    uint8_t  state;       // DHCP_STATE_*, as last seen
    uint32_t seconds;     // lease time handed out
    uint32_t ackAt;       // millis() of the last acknowledgement
    uint32_t renewals;    // renewals since boot
    uint32_t stateSince;  // millis() when state was entered
    uint32_t server;      // server address, network order
    uint32_t used;        // lwIP's ticks since the last ACK
    uint32_t renewIn;     // lwIP's ticks to the next renewal
};

DhcpLease dhcpLease = {};

/*
 * DHCP hostname: "wlanscout-" plus the sensor name, cut down to
 * what a hostname may hold (lower case letters, digits, hyphens),
 * or plus the last three bytes of the Ethernet MAC when there is
 * no usable name. With several sensors on one network each shows
 * up under its own name in the DHCP server. A changed name takes
 * effect with the next lease, that is after a restart.
 */
char dhcpHostname[48] = "";

#if ENABLE_ETHERNET

void buildHostname()
{
    char part[33];
    int  k = 0;

    for (const char *p = sensorName; *p && k < (int)sizeof(part) - 1; p++)
    {
        char c = *p;

        if (c >= 'A' && c <= 'Z')
            c = c - 'A' + 'a';

        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');

        if (ok)
            part[k++] = c;
        else if (k > 0 && part[k - 1] != '-')
            part[k++] = '-';               // spaces and the like
    }

    while (k > 0 && part[k - 1] == '-')
        k--;

    part[k] = 0;

    if (k > 0)
    {
        snprintf(dhcpHostname, sizeof(dhcpHostname), "wlanscout-%s", part);
        return;
    }

    uint8_t mac[6];

    ETH.macAddress(mac);

    snprintf(dhcpHostname, sizeof(dhcpHostname), "wlanscout-%02x%02x%02x",
             mac[3], mac[4], mac[5]);
}

// ============================================================
// Ethernet events
// ============================================================

void onEvent(arduino_event_id_t event, arduino_event_info_t info)
{
    switch (event)
    {
        case ARDUINO_EVENT_ETH_START:
            Serial.println("ETH START");
            buildHostname();
            ETH.setHostname(dhcpHostname);
            Serial.printf("DHCP hostname: %s\r\n", dhcpHostname);
            break;

        case ARDUINO_EVENT_ETH_CONNECTED:
            /*
             * Speed and duplex say whether auto-negotiation
             * actually ran. A W5500 module pinned to a fixed
             * mode links to a switch through parallel
             * detection but often not to a PC NIC, and
             * 10 Mbps half duplex is the tell-tale.
             */
            Serial.printf("ETH CONNECTED: %d Mbps, %s\r\n",
                          (int)ETH.linkSpeed(),
                          ETH.fullDuplex() ? "full duplex"
                                           : "half duplex");

            ethLinkUp = true;

            // The work is done in loop(), calling into the
            // network stack from the event task is asking for
            // trouble
            ethLinkChanged = true;

            logEventFrom("system", "link up, %d Mbps %s",
                         (int)ETH.linkSpeed(),
                         ETH.fullDuplex() ? "full duplex"
                                          : "half duplex");

            break;

        case ARDUINO_EVENT_ETH_GOT_IP:
            ethGotIP = true;

            // A real lease means the interface is healthy
            if (!ethStaticActive)
                reinitCount = 0;

            Serial.printf("ETH GOT IP (%s)\r\n",
                          ethStaticActive ? "static" : "DHCP");

            Serial.print("IP: ");
            Serial.println(ETH.localIP());

            Serial.print("Gateway: ");
            Serial.println(ETH.gatewayIP());

            Serial.print("Subnet: ");
            Serial.println(ETH.subnetMask());

            Serial.println();
            Serial.print("Web UI: http://");
            Serial.println(ETH.localIP());
            break;

        case ARDUINO_EVENT_ETH_LOST_IP:
            /*
             * Raised two minutes after lwIP dropped the address,
             * usually because the lease ran out. ethLoop() clears
             * ethGotIP and starts the DHCP watchdog's count, which
             * reinitialises the interface if no lease follows.
             * Done there rather than here: from this task the
             * watchdog could count an attempt between clearing
             * ethGotIP and resetting the count.
             */
            ethIpLost = true;

            Serial.println("ETH LOST IP");

            break;

        case ARDUINO_EVENT_ETH_DISCONNECTED:
            ethGotIP  = false;
            ethLinkUp = false;

            Serial.println("ETH DISCONNECTED");

            logEventFrom("system", "link down");

            // Start over with DHCP once the link is back
            dhcpAttempts     = 0;
            dhcpAttemptStart = 0;

            break;

        case ARDUINO_EVENT_ETH_STOP:
            ethGotIP  = false;
            ethLinkUp = false;

            Serial.println("ETH STOP");
            break;

        default:
            break;
    }
}

#endif

#if ENABLE_ETHERNET

/*
 * Bring the W5500 up.
 *
 * Separate from setup() so the interface can be taken down
 * and started again when it stops answering, without
 * touching the SPI bus or the event handler, which only need
 * to be set up once.
 */
void setupEthernet()
{
    pinMode(W5500_RST, OUTPUT);

    digitalWrite(W5500_RST, LOW);
    delay(10);

    digitalWrite(W5500_RST, HIGH);
    delay(100);

    Serial.println();
    Serial.println("Starting W5500 Ethernet...");

#if HAVE_DHCP_NTP
    // Before the first lease, its option 42 would be lost otherwise
    esp_netif_init();
    esp_sntp_servermode_dhcp(true);
#endif

    ethStartedAt = millis();

    ethPresent = ETH.begin(ETH_PHY_W5500,
                           1,
                           W5500_CS,
                           W5500_INT,
                           W5500_RST,
                           SPI);

    Serial.println(ethPresent ? "ETH.begin(): OK"
                              : "ETH.begin(): FAILED, no W5500? "
                                "Running on USB only");
}

/*
 * Restart the DHCP client properly.
 *
 * ETH.config() with empty addresses was supposed to do this,
 * but its result was never checked, and after a static
 * address had been set the client stayed down: no discover
 * ever went out on the wire. Going through esp_netif directly
 * makes each step visible - stop the client, drop the address
 * that is still configured, start the client.
 */
bool restartDhcpClient(const char *why)
{
    esp_netif_t *netif = ETH.netif();

    if (netif == nullptr)
    {
        bool ok = ETH.config(IPAddress((uint32_t)0),
                             IPAddress((uint32_t)0),
                             IPAddress((uint32_t)0));

        Serial.printf("ETH: no netif handle, ETH.config(dhcp) %s\r\n",
                      ok ? "OK" : "FAILED");

        return ok;
    }

    esp_err_t stopErr = esp_netif_dhcpc_stop(netif);

    esp_netif_ip_info_t zero;

    memset(&zero, 0, sizeof(zero));

    esp_err_t clearErr = esp_netif_set_ip_info(netif, &zero);
    esp_err_t startErr = esp_netif_dhcpc_start(netif);

    Serial.printf("ETH: DHCP restart (%s): stop 0x%X, clear 0x%X, "
                  "start 0x%X\r\n",
                  why, stopErr, clearErr, startErr);

    bool ok = (startErr == ESP_OK ||
               startErr == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED);

    if (!ok)
    {
        logEventFrom("system",
                     "DHCP client would not start, error 0x%X",
                     startErr);
    }

    return ok;
}

/*
 * One word about the Ethernet state for the heap line.
 *
 * The settings page is no help while the device has no
 * address, which is exactly when this matters, so the state
 * goes where it can always be read.
 */
const char *ethStatusText()
{
    static char buffer[40];

    if (ethGotIP)
    {
        snprintf(buffer, sizeof(buffer), "up/%s",
                 ethStaticActive ? "static" : "dhcp");

        return buffer;
    }

    snprintf(buffer, sizeof(buffer), "down/link %s/dhcp %s/try %u",
             ethLinkUp ? "up" : "down",
             dhcpStatusText(),
             (unsigned)dhcpAttempts);

    return buffer;
}

// What the DHCP client is doing, for the settings page
const char *dhcpStatusText()
{
    esp_netif_t *netif = ETH.netif();

    if (netif == nullptr)
        return "no interface";

    esp_netif_dhcp_status_t status;

    if (esp_netif_dhcpc_get_status(netif, &status) != ESP_OK)
        return "unknown";

    switch (status)
    {
        case ESP_NETIF_DHCP_INIT:    return "idle";
        case ESP_NETIF_DHCP_STARTED: return "running";
        case ESP_NETIF_DHCP_STOPPED: return "stopped";
        default:                     break;
    }

    return "unknown";
}

// lwIP's DHCP client state, nullptr without a client
const char *dhcpLeaseStateText(uint8_t state)
{
    switch (state)
    {
        case DHCP_STATE_REQUESTING:  return "requesting";
        case DHCP_STATE_INIT:        return "init";
        case DHCP_STATE_REBOOTING:   return "rebooting";
        case DHCP_STATE_REBINDING:   return "rebinding";
        case DHCP_STATE_RENEWING:    return "renewing";
        case DHCP_STATE_SELECTING:   return "selecting";
        case DHCP_STATE_INFORMING:   return "informing";
        case DHCP_STATE_CHECKING:    return "checking";
        case DHCP_STATE_BOUND:       return "bound";
        case DHCP_STATE_BACKING_OFF: return "backing off";
    }

    return nullptr;
}

/*
 * Follow the lease once a second and log what deserves it:
 * the first lease, a renewal the server did not answer at once,
 * rebinding and the end of a lease. A renewal that goes through
 * on the first try is only counted, every 45 minutes in the
 * event log would push everything else out.
 */
void dhcpLeaseWatch()
{
    static uint32_t lastCheck = 0;

    if (!ethPresent || millis() - lastCheck < 1000)
        return;

    lastCheck = millis();

    esp_netif_t *esp = ETH.netif();
    struct netif *lw = esp ? (struct netif *)esp_netif_get_netif_impl(esp)
                           : nullptr;

    uint8_t  state   = DHCP_STATE_OFF;
    uint32_t seconds = 0;
    uint32_t server  = 0;
    uint32_t addr    = 0;
    uint32_t used    = 0;
    uint32_t renewIn = 0;

    if (lw != nullptr)
    {
        LOCK_TCPIP_CORE();

        struct dhcp *d = netif_dhcp_data(lw);

        if (d != nullptr)
        {
            state   = d->state;
            seconds = d->offered_t0_lease;
            server  = ip4_addr_get_u32(ip_2_ip4(&d->server_ip_addr));
            addr    = ip4_addr_get_u32(ip_2_ip4(&lw->ip_addr));
            used    = d->lease_used;
            renewIn = d->t1_renew_time;
        }

        UNLOCK_TCPIP_CORE();
    }

    uint8_t  prev = dhcpLease.state;

    /*
     * A renewal the server answers at once passes through
     * RENEWING within milliseconds, between two looks. lwIP
     * restarts its count of the lease on every ACK, so a count
     * that went back is a renewal.
     */
    bool quietRenewal = state == DHCP_STATE_BOUND &&
                        prev == DHCP_STATE_BOUND &&
                        used < dhcpLease.used;

    dhcpLease.used    = used;
    dhcpLease.renewIn = renewIn;

    if (quietRenewal)
    {
        dhcpLease.renewals++;
        dhcpLease.ackAt   = millis();
        dhcpLease.seconds = seconds;
        dhcpLease.server  = server;

        Serial.printf("DHCP: lease renewed (%lu since start)\r\n",
                      (unsigned long)dhcpLease.renewals);
    }

    if (state == prev)
        return;

    uint32_t inPrev = (millis() - dhcpLease.stateSince) / 1000;

    dhcpLease.state      = state;
    dhcpLease.stateSince = millis();

    Serial.printf("DHCP: %s -> %s\r\n",
                  dhcpLeaseStateText(prev) ? dhcpLeaseStateText(prev) : "off",
                  dhcpLeaseStateText(state) ? dhcpLeaseStateText(state) : "off");

    char ip[16];
    char srv[16];
    IPAddress(addr).toString().toCharArray(ip, sizeof(ip));
    IPAddress(server).toString().toCharArray(srv, sizeof(srv));

    bool held = prev == DHCP_STATE_BOUND ||
                prev == DHCP_STATE_RENEWING ||
                prev == DHCP_STATE_REBINDING;

    if (state == DHCP_STATE_BOUND)
    {
        uint32_t left = 0;

        if (dhcpLease.ackAt != 0 && dhcpLease.seconds != 0)
        {
            uint32_t used = (millis() - dhcpLease.ackAt) / 1000;
            left = used < dhcpLease.seconds ? dhcpLease.seconds - used : 0;
        }

        dhcpLease.seconds = seconds;
        dhcpLease.server  = server;
        dhcpLease.ackAt   = millis();

        if (prev == DHCP_STATE_RENEWING)
        {
            dhcpLease.renewals++;

            // lwIP sends the first request at once and repeats
            // it after a while, so a slow answer means a lost one
            if (inPrev > 20)
                logEventFrom("system", "lease renewed after %lu s, "
                             "%lu min left before",
                             (unsigned long)inPrev,
                             (unsigned long)(left / 60));
        }
        else if (prev == DHCP_STATE_REBINDING)
        {
            dhcpLease.renewals++;

            logEventFrom("system", "lease kept by rebinding, %s "
                         "answered with %lu min left",
                         srv, (unsigned long)(left / 60));
        }
        else
        {
            logEventFrom("system", "lease %s from %s for %lu min",
                         ip, srv, (unsigned long)(seconds / 60));
        }
    }
    else if (state == DHCP_STATE_REBINDING)
    {
        logEventFrom("system", "%s did not answer the renewal, "
                     "asking any DHCP server (rebinding)",
                     IPAddress(dhcpLease.server).toString().c_str());
    }
    else if (held && state != DHCP_STATE_RENEWING && ethLinkUp)
    {
        logEventFrom("system", "lease ended (%s), DHCP client %s",
                     prev == DHCP_STATE_REBINDING ? "not renewed in time"
                                                  : "refused by the server",
                     dhcpLeaseStateText(state) ? dhcpLeaseStateText(state)
                                               : "off");
    }
}

// The W5500 on its SPI bus, once from setup()
void ethSetup()
{
    // --------------------------------------------------------
    // W5500 reset
    // --------------------------------------------------------

    Serial.println();
    Serial.println("W5500 reset...");

    pinMode(W5500_RST, OUTPUT);

    digitalWrite(W5500_RST, LOW);
    delay(10);

    digitalWrite(W5500_RST, HIGH);
    delay(100);

    // --------------------------------------------------------
    // SPI
    // --------------------------------------------------------

    Serial.println("Initialising SPI...");

    SPI.begin(W5500_SCLK, W5500_MISO, W5500_MOSI, W5500_CS);

    // --------------------------------------------------------
    // Ethernet
    // --------------------------------------------------------

    Network.onEvent(onEvent);

    setupEthernet();
}

/*
 * Takes the interface down and up again and starts over with DHCP;
 * recovers a W5500 whose transmit path is wedged while the link
 * stays up.
 */
void ethReinit()
{
    ETH.end();

    delay(200);

    setupEthernet();

    dhcpLease.state = DHCP_STATE_OFF;

    dhcpAttempts     = 0;
    dhcpAttemptStart = millis();
}

/*
 * Gateway check. A W5500 whose transmit or receive path wedges keeps
 * its link, and the lease only runs out up to 90 minutes later. So
 * once a minute, while the link is up and DHCP gave an address, the
 * interface's ARP cache is cleared and the gateway asked again; the
 * next check looks whether it answered. After ARP_CHECK_FAILURES
 * checks without an answer the interface is reinitialised, as with
 * "ethreinit". Not on the static fallback: a sensor wired straight
 * to a PC may have no gateway there, and a reinit would take it back
 * to DHCP.
 */
#define ARP_CHECK_MS       60000
#define ARP_CHECK_FAILURES 3

/*
 * What the W5500 itself says, for telling a wedged chip from a wedged
 * driver on the ESP side: printed with every missed ARP answer, every
 * DHCP attempt without a lease, every "no link" line and by "w5500"
 * on the serial console. Read over the same SPI object and
 * chip select as the driver; beginTransaction() keeps the two apart.
 *
 * Common block: version (0x04, else the SPI reads garbage), the MAC
 * address (zero after a chip reset nobody noticed), interrupt flags
 * and PHYCFGR (bit 0 link, bit 1 100M, bit 2 full duplex). Socket 0,
 * which the driver runs in MACRAW mode: status (0x42 when open), the
 * free transmit space and both pointers of each ring. Between two
 * dumps RX_WR moving means frames come in, RX_RD moving that the
 * driver takes them; TX_WR moving that the driver hands frames over,
 * TX_RD catching up that the chip sends them.
 */
#if ENABLE_ETHERNET
static void w5500Read(uint16_t addr, uint8_t block, uint8_t *buf,
                      size_t len)
{
    SPI.beginTransaction(SPISettings(10000000, MSBFIRST, SPI_MODE0));
    digitalWrite(W5500_CS, LOW);

    SPI.write16(addr);
    SPI.write(block << 3);
    SPI.transferBytes(nullptr, buf, len);

    digitalWrite(W5500_CS, HIGH);
    SPI.endTransaction();
}

void w5500Dump(const char *why)
{
    if (!ethPresent)
    {
        Serial.printf("[w5500] %s: not started\r\n", why);
        return;
    }

    uint8_t c[0x3A];
    uint8_t s[0x30];

    w5500Read(0x0000, 0, c, sizeof(c));     // common registers
    w5500Read(0x0000, 1, s, sizeof(s));     // socket 0 registers

    auto u16 = [](const uint8_t *p) { return (unsigned)(p[0] << 8 | p[1]); };

    uint8_t phy = c[0x2E];

    Serial.printf("[w5500] %s: ver %02X mr %02X mac %02X:%02X:%02X:%02X:"
                  "%02X:%02X ir %02X imr %02X sir %02X simr %02X "
                  "phy %02X (link %u %s %s)\r\n",
                  why, c[0x39], c[0x00], c[0x09], c[0x0A], c[0x0B],
                  c[0x0C], c[0x0D], c[0x0E], c[0x15], c[0x16], c[0x17],
                  c[0x18], phy, phy & 1, (phy & 2) ? "100M" : "10M",
                  (phy & 4) ? "full" : "half");

    Serial.printf("[w5500] %s: s0 mr %02X cr %02X ir %02X sr %02X "
                  "imr %02X  tx free %u rd %u wr %u  rx size %u "
                  "rd %u wr %u\r\n",
                  why, s[0x00], s[0x01], s[0x02], s[0x03], s[0x2C],
                  u16(&s[0x20]), u16(&s[0x22]), u16(&s[0x24]),
                  u16(&s[0x26]), u16(&s[0x28]), u16(&s[0x2A]));
}

/*
 * The W5500 sometimes resets on its own (seen on the kit, cause not
 * known yet): every register back to its default, the MAC address
 * zero, the socket closed, and the PHY negotiating again, so the
 * switch sees the link go down for two seconds. The ESP-IDF driver
 * does not notice; it goes on as if the socket were open and nothing
 * gets through until a reinit, which DHCP or the gateway check only
 * asked for half a minute to three minutes later. So once a second
 * the MAC address is read back: the driver writes it once at start,
 * nothing else touches it. Along with the version register, which a
 * reset leaves at 0x04: a module that is gone reads all zeros.
 */
bool w5500WasReset()
{
    static uint32_t lastCheck = 0;

    if (!ethPresent || millis() - ethStartedAt < 3000 ||
        millis() - lastCheck < 1000)
        return false;

    lastCheck = millis();

    uint8_t mac[6];
    uint8_t version;

    w5500Read(0x0009, 0, mac, sizeof(mac));
    w5500Read(0x0039, 0, &version, 1);

    // Zeros from a chip that is gone are no reset: no reinit loop
    if (version != 0x04)
        return false;

    for (uint8_t b : mac)
    {
        if (b != 0)
            return false;
    }

    return true;
}
#endif

uint8_t  arpFailures  = 0;
uint32_t arpLastCheck = 0;
bool     arpAsked     = false;

// True when the gateway did not answer often enough: reinitialise
bool arpCheck()
{
    if (!ethLinkUp || !ethGotIP || ethStaticActive)
    {
        arpFailures  = 0;
        arpAsked     = false;
        arpLastCheck = millis();
        return false;
    }

    if (millis() - arpLastCheck < ARP_CHECK_MS)
        return false;

    arpLastCheck = millis();

    esp_netif_t *esp = ETH.netif();
    struct netif *lw = esp ? (struct netif *)esp_netif_get_netif_impl(esp)
                           : nullptr;

    if (lw == nullptr)
        return false;

    bool       answered = false;
    ip4_addr_t gw;

    LOCK_TCPIP_CORE();

    ip4_addr_copy(gw, *netif_ip4_gw(lw));

    if (!ip4_addr_isany_val(gw))
    {
        struct eth_addr  *mac;
        const ip4_addr_t *ip;

        answered = arpAsked && etharp_find_addr(lw, &gw, &mac, &ip) >= 0;

        etharp_cleanup_netif(lw);
        etharp_request(lw, &gw);
    }

    UNLOCK_TCPIP_CORE();

    if (ip4_addr_isany_val(gw))
    {
        arpAsked = false;
        return false;
    }

    if (arpAsked)
    {
        if (answered)
        {
            arpFailures = 0;
        }
        else
        {
            arpFailures++;

            Serial.printf("ETH: gateway %s did not answer ARP "
                          "(%u of %u)\r\n",
                          ip4addr_ntoa(&gw), (unsigned)arpFailures,
                          (unsigned)ARP_CHECK_FAILURES);

            w5500Dump("no ARP answer");
        }
    }

    arpAsked = true;

    if (arpFailures < ARP_CHECK_FAILURES)
        return false;

    logEventFrom("system", "gateway %s did not answer ARP %u times, "
                 "reinitialising the Ethernet interface",
                 ip4addr_ntoa(&gw), (unsigned)ARP_CHECK_FAILURES);

    arpFailures = 0;
    arpAsked    = false;

    return true;
}

/*
 * Link, DHCP and the static fallback, from loop(). False when the
 * interface was just reinitialised, which ends this loop() round.
 */
bool ethLoop()
{
    if (w5500WasReset())
    {
        PROBE_MARK("eth reinit");
        w5500Dump("reset by itself");

        Serial.println("ETH: reinitialising the interface "
                       "(the W5500 reset by itself)");

        logEventFrom("system", "the W5500 reset by itself, "
                     "reinitialising the Ethernet interface");

        ethReinit();

        return false;
    }

    if (arpCheck())
    {
        PROBE_MARK("eth reinit");
        Serial.println("ETH: reinitialising the interface "
                       "(no ARP answer from the gateway)");

        ethReinit();

        return false;
    }

    if (ethReinitRequested)
    {
        ethReinitRequested = false;

        PROBE_MARK("eth reinit");
        Serial.println("ETH: reinitialising the interface "
                       "(serial console)");

        logEventFrom("system", "Ethernet interface reinitialised "
                     "from the serial console");

        ethReinit();

        return false;
    }

    /*
     * Link diagnostics.
     *
     * The event handler only reports changes, so a link that
     * never comes up produces no output at all. Ask the PHY
     * directly instead: if it reports no link while a cable is
     * plugged in, the problem is below IP, and the other end
     * is the place to look.
     */
    static uint32_t lastLinkLog = 0;
    static uint8_t  noLinkDumps = 0;

    if (ethPresent && !ethLinkUp && millis() - lastLinkLog > 5000)
    {
        lastLinkLog = millis();

        Serial.printf("ETH: no link (phy reports %s, %d Mbps, %s)\r\n",
                      ETH.linkUp() ? "up" : "down",
                      (int)ETH.linkSpeed(),
                      ETH.fullDuplex() ? "full duplex" : "half duplex");

        // The first few only: a sensor without a cable says it forever
        if (noLinkDumps < 3)
        {
            noLinkDumps++;
            w5500Dump("no link");
        }
    }

    if (ethLinkUp)
        noLinkDumps = 0;

    /*
     * A link change always returns to DHCP.
     *
     * Without this the device stayed on the static address
     * forever once it had fallen back, so unplugging and
     * replugging the cable never brought a lease back.
     */
    if (ethLinkChanged)
    {
        ethLinkChanged = false;

        if (ethLinkUp)
        {
            if (ethStaticActive)
            {
                Serial.println("ETH: link change, back to DHCP");

                logEventFrom("system",
                             "link change, leaving the static "
                             "address and trying DHCP again");

                ethStaticActive = false;
                ethGotIP        = false;
            }

            // A new lease, not the end of the old one
            dhcpLease.state = DHCP_STATE_OFF;

            ntpForgetDhcpServer();

            restartDhcpClient("link up");

            Serial.printf("ETH: waiting for a lease, dhcp client %s\r\n",
                          dhcpStatusText());

            dhcpAttempts     = 0;
            dhcpAttemptStart = millis();
        }
    }

    if (ethIpLost)
    {
        ethIpLost = false;

        ethGotIP = false;

        logEventFrom("system", "IP address lost, asking DHCP "
                               "for a new lease");

        dhcpAttempts     = 0;
        dhcpAttemptStart = millis();
    }

    /*
     * DHCP watchdog.
     *
     * While the link is up but no lease has arrived, count the
     * attempts and fall back to a static address once the
     * configured number is reached. The DHCP client is only
     * re-armed on every other attempt: restarting it every
     * time can cut off a negotiation that is still running.
     */
    if (ethLinkUp && !ethGotIP && !ethStaticActive &&
        dhcpAttemptStart != 0 &&
        millis() - dhcpAttemptStart > DHCP_TIMEOUT_MS)
    {
        dhcpAttempts++;

        Serial.printf("ETH: no DHCP lease, attempt %u of %u "
                      "(link %s, dhcp client %s, address %s)\r\n",
                      (unsigned)dhcpAttempts,
                      (unsigned)DHCP_MAX_ATTEMPTS,
                      ethLinkUp ? "up" : "down",
                      dhcpStatusText(),
                      ETH.localIP().toString().c_str());

        w5500Dump("no DHCP lease");

        /*
         * Reinitialise early. A wedged transmit path cannot be
         * waited out, and a reinit recovers it in about a
         * second. The cooldown and the counter keep it from
         * turning into a loop when the cable really is out.
         */
        if (dhcpAttempts >= DHCP_REINIT_AFTER &&
            reinitCount < DHCP_REINIT_MAX &&
            (lastReinit == 0 ||
             millis() - lastReinit > DHCP_REINIT_COOLDOWN_MS))
        {
            reinitCount++;
            lastReinit = millis();

        PROBE_MARK("eth reinit");
            Serial.printf("ETH: reinitialising the interface "
                          "(attempt %u)\r\n",
                          (unsigned)reinitCount);

            logEventFrom("system",
                         "no lease after %u attempts, "
                         "reinitialising the Ethernet interface",
                         (unsigned)dhcpAttempts);

            ethReinit();

            return false;
        }

        if (dhcpAttempts >= DHCP_MAX_ATTEMPTS)
        {
            ethStaticActive = true;
            ethStaticSince  = millis();

            bool ok = ETH.config(ETH_STATIC_IP,
                                 ETH_STATIC_GW,
                                 ETH_STATIC_MASK,
                                 ETH_STATIC_DNS);

            logEventFrom("system",
                         "no DHCP lease after %u attempts, "
                         "falling back to a static address",
                         (unsigned)DHCP_MAX_ATTEMPTS);

            Serial.print("ETH: falling back to static ");
            Serial.print(ETH_STATIC_IP);
            Serial.println(ok ? " OK" : " FAILED");

            Serial.print("Web UI: http://");
            Serial.println(ETH_STATIC_IP);
        }
        else if (dhcpAttempts % 2 == 0)
        {
            restartDhcpClient("retry");
        }

        dhcpAttemptStart = millis();
    }

    dhcpLeaseWatch();

#if DHCP_RETRY_AFTER_STATIC_MS > 0
    if (ethStaticActive && ethLinkUp &&
        millis() - ethStaticSince > DHCP_RETRY_AFTER_STATIC_MS)
    {
        Serial.println("ETH: trying DHCP again");

        logEventFrom("system", "retrying DHCP from the static address");

        ethStaticActive = false;
        ethGotIP        = false;
        ethStaticSince  = millis();

        restartDhcpClient("periodic retry");

        dhcpAttempts     = 0;
        dhcpAttemptStart = millis();
    }
#endif

    return true;
}

/*
 * Network time.
 *
 * Started once an address is up, then checked regularly:
 * a later step of the system clock shifts every timestamp
 * in the capture by the same amount, which is exactly
 * what should happen.
 */
void ntpLoop()
{
    if (ethGotIP && !ntpStarted)
    {
        PROBE_MARK("ntp sync");

        // The server from DHCP first, if the lease brought one
        if (ntpTakeDhcpServer())
            configTime(0, 0, ntpDhcpServer, NTP_SERVER1, NTP_SERVER2);
        else
            configTime(0, 0, NTP_SERVER1, NTP_SERVER2);

#if HAVE_ESP_SNTP
        esp_sntp_set_sync_interval(NTP_SYNC_INTERVAL_MS);
#endif

        ntpStarted = true;

        Serial.printf("NTP: asking %s, re-sync every %lu min\r\n",
                      ntpDhcpServer[0] ? ntpDhcpServer : NTP_SERVER1,
                      (unsigned long)(NTP_SYNC_INTERVAL_MS / 60000));
    }

    if (ntpStarted && millis() - lastNtpCheck > NTP_CHECK_INTERVAL_MS)
    {
        lastNtpCheck = millis();

        // A renewed or new lease may have brought another server
        ntpTakeDhcpServer();

        struct timeval tv;

        gettimeofday(&tv, nullptr);

        // Anything before 2023 means the clock is still unset
        if (tv.tv_sec > 1700000000)
        {
            int64_t nowUs =
                (int64_t)tv.tv_sec * 1000000LL + tv.tv_usec;

            int64_t offset = nowUs - esp_timer_get_time();

            int64_t delta = offset - clockOffsetUs;

            if (delta < 0)
                delta = -delta;

            /*
             * Only a real correction counts. Below this the
             * difference is measurement noise between the two
             * clocks, not something NTP did.
             */
            if (pcapClockSource != 2 || delta > 50000)
            {
                // Against NTP, or against the AP the clock came from
                ntpLastAdjust =
                    (pcapClockSource == 2 || pcapClockSource == 3)
                        ? (int32_t)((offset - clockOffsetUs) / 1000LL)
                        : 0;

                clockOffsetUs = offset;

                ntpLastSync = (uint32_t)tv.tv_sec;
                ntpSyncCount++;

                char stamp[24];
                char tzName[12];

                formatStamp((uint32_t)tv.tv_sec, stamp, sizeof(stamp));
                formatTzLabel(tzName, sizeof(tzName));

                Serial.printf(
                    "NTP: clock set to %s %s"
                    "%s\r\n",
                    stamp, tzName,
                    (pcapClockSource == 2) ? " (adjusted)" : "");

                if (ntpSyncCount == 1)
                    logEventFrom("system", "clock synced via NTP");

                pcapClockSource = 2;
            }
        }
    }
}

#else

const char *ethStatusText()
{
    return "not built in";
}

#endif
