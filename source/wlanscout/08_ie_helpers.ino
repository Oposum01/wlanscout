// SPDX-License-Identifier: GPL-2.0-only WITH AdditionRef-wlanscout-linking-exception
// SPDX-FileCopyrightText: 2026 Dr.-Ing. Simon Wunderlich Systementwicklung & Beratung

// ============================================================
// IE parser helpers
// ============================================================

const uint8_t *findIE(const uint8_t *ies, int ieLength,
                      uint8_t wantedID, int &foundLength)
{
    int pos = 0;

    while (pos + 2 <= ieLength)
    {
        uint8_t id  = ies[pos];
        uint8_t len = ies[pos + 1];

        pos += 2;

        if (pos + len > ieLength)
            break;

        if (id == wantedID)
        {
            foundLength = len;
            return &ies[pos];
        }

        pos += len;
    }

    foundLength = 0;
    return nullptr;
}

const uint8_t *findExtendedIE(const uint8_t *ies, int ieLength,
                              uint8_t extensionID, int &foundLength)
{
    int pos = 0;

    while (pos + 2 <= ieLength)
    {
        uint8_t id  = ies[pos];
        uint8_t len = ies[pos + 1];

        pos += 2;

        if (pos + len > ieLength)
            break;

        if (id == 255 && len >= 1 && ies[pos] == extensionID)
        {
            foundLength = len - 1;
            return &ies[pos + 1];
        }

        pos += len;
    }

    foundLength = 0;
    return nullptr;
}

const uint8_t *findVendorIE(const uint8_t *ies, int ieLength,
                            uint8_t o0, uint8_t o1, uint8_t o2,
                            uint8_t type, int &foundLength)
{
    int pos = 0;

    while (pos + 2 <= ieLength)
    {
        uint8_t id  = ies[pos];
        uint8_t len = ies[pos + 1];

        pos += 2;

        if (pos + len > ieLength)
            break;

        if (id == 221 && len >= 4 &&
            ies[pos + 0] == o0 &&
            ies[pos + 1] == o1 &&
            ies[pos + 2] == o2 &&
            ies[pos + 3] == type)
        {
            foundLength = len - 4;
            return &ies[pos + 4];
        }

        pos += len;
    }

    foundLength = 0;
    return nullptr;
}

// ============================================================
// Cipher and AKM names
// ============================================================

const char *cipherName(uint8_t type, bool wpa1)
{
    switch (type)
    {
        case 0:  return "use-group";
        case 1:  return "WEP-40";
        case 2:  return "TKIP";
        case 4:  return "CCMP-128";
        case 5:  return "WEP-104";
        case 6:  return "BIP-CMAC-128";
        case 8:  return "GCMP-128";
        case 9:  return "GCMP-256";
        case 10: return "CCMP-256";
        case 11: return "BIP-GMAC-128";
        case 12: return "BIP-GMAC-256";
        case 13: return "BIP-CMAC-256";
        default: break;
    }

    // A suite this table does not know yet: say which one it is,
    // so it can be looked up instead of guessed at
    static char buffer[16];

    snprintf(buffer, sizeof(buffer), "cipher %u", type);

    return buffer;
}

/*
 * AKM suites of OUI 00-0F-AC, 802.11-2024 table 9-190, checked
 * against Wireshark's ieee80211_rsn_keymgmt_vals. Names follow
 * the short forms hostapd and the WFA use.
 */
/*
 * Suites under a vendor's own OUI that have a name. Nintendo's
 * turned up in a network without an SSID, most likely a Switch's
 * local play (October 2026); what they do is not public. Cisco's
 * 00-40-96 type 0 is CCKM, Cisco's fast roaming over 802.1X, seen
 * beside 802.1X in a Cisco guest network (November 2026).
 */
struct VendorSuite
{
    uint32_t    suite;
    bool        akm;
    const char *name;
};

static const VendorSuite vendorSuites[] = {
    { 0xA4C0E102u, true,  "Nintendo" },
    { 0xA4C0E104u, false, "Nintendo" },
    { 0x00409600u, true,  "CCKM" }
};

const char *vendorSuiteName(uint32_t suite, bool akm)
{
    for (const VendorSuite &v : vendorSuites)
        if (v.suite == suite && v.akm == akm)
            return v.name;

    return nullptr;
}

const char *akmName(uint8_t type, bool wpa1)
{
    if (wpa1)
    {
        switch (type)
        {
            case 0:  return "none";     // reserved, offers nothing
            case 1:  return "802.1X";
            case 2:  return "PSK";
            default: break;
        }
    }
    else
    {
        switch (type)
        {
            case 1:  return "802.1X";
            case 2:  return "PSK";
            case 3:  return "FT-802.1X";
            case 4:  return "FT-PSK";
            case 5:  return "802.1X-SHA256";
            case 6:  return "PSK-SHA256";
            case 7:  return "TDLS";
            case 8:  return "SAE";
            case 9:  return "FT-SAE";
            case 10: return "AP-PeerKey";
            case 11: return "802.1X-SuiteB";
            case 12: return "802.1X-SuiteB-192";
            case 13: return "FT-802.1X-SHA384";
            case 14: return "FILS-SHA256";
            case 15: return "FILS-SHA384";
            case 16: return "FT-FILS-SHA256";
            case 17: return "FT-FILS-SHA384";
            case 18: return "OWE";
            case 19: return "FT-PSK-SHA384";
            case 20: return "PSK-SHA384";
            case 21: return "PASN";
            case 24: return "SAE-EXT-KEY";
            case 25: return "FT-SAE-EXT-KEY";
            case 29: return "EPPKE";
            default: break;
        }
    }

    static char buffer[16];

    snprintf(buffer, sizeof(buffer), "AKM %u", type);

    return buffer;
}


// ============================================================
// RSN (IE 48) or WPA (vendor IE 221 / 00:50:F2 type 1)
// ============================================================

struct RsnInfo
{
    bool present;
    bool psk;
    bool sae;
    bool eap;
    bool owe;
    bool osen;
    bool mfpCapable;
    bool mfpRequired;

    /*
     * RSN Capabilities bit 0. Pre-authentication lets a
     * station authenticate with an AP it is not associated
     * with, over the wired side, to shorten a later roam. It
     * is the predecessor of 802.11r and only works with
     * WPA2-EAP, so its presence says something about the age
     * of the deployment.
     */
    bool preAuth;

    char group[18];
    char pairwise[36];
    char mgmt[18];
    char akm[64];

    // First suite under an OUI that has no place here, see badOui
    bool     badSeen;
    uint32_t badSuite;
};

/*
 * A suite whose OUI does not belong in this element: 00:00:00
 * anywhere, 00:0F:AC in the WPA element, 00:50:F2 in the RSN
 * element. Some cable routers list 00-0F-AC-02 and 00-00-00-00
 * among the WPA pairwise ciphers; the AP's firmware wrote them
 * wrong. Vendor suites such as Cisco's CCKM are fine.
 */
void noteBadSuite(const uint8_t *q, bool wpa1, struct RsnInfo &info)
{
    uint32_t oui = ((uint32_t)q[0] << 16) | (q[1] << 8) | q[2];

    bool bad = oui == 0 || oui == (wpa1 ? 0x000FACu : 0x0050F2u);

    if (bad && !info.badSeen)
    {
        info.badSeen  = true;
        info.badSuite = (oui << 8) | q[3];
    }
}

/*
 * OSEN, the OSU Server-only authenticated L2 Encryption
 * Network of Hotspot 2.0.
 *
 * Carried in a WFA vendor element (50:6F:9A type 0x12) rather
 * than in an RSN element, and laid out like RSN but without
 * the leading version field. The ciphers use the usual
 * 00:0F:AC suites, only the AKM is a WFA one, so this needs
 * its own reader instead of the RSN path.
 *
 * A BSS like this is an onboarding network: the client
 * authenticates the server by certificate, not the other way
 * round, and uses it only to fetch a subscription.
 */
void parseOsen(const uint8_t *p, int len, struct RsnInfo &info)
{
    if (p == nullptr || len < 6)
        return;

    info.present = true;
    info.osen    = true;

    int pos = 0;

    // Group cipher
    if (p[0] == 0x00 && p[1] == 0x0F && p[2] == 0xAC)
    {
        strncpy(info.group,
                cipherName(p[3], false),
                sizeof(info.group) - 1);
    }

    pos += 4;

    if (pos + 2 > len)
        return;

    uint16_t pairwiseCount = p[pos] | (p[pos + 1] << 8);
    pos += 2;

    for (uint16_t i = 0; i < pairwiseCount; i++)
    {
        if (pos + 4 > len)
            return;

        if (p[pos] == 0x00 && p[pos + 1] == 0x0F && p[pos + 2] == 0xAC)
        {
            appendName(info.pairwise,
                       sizeof(info.pairwise),
                       cipherName(p[pos + 3], false));
        }

        pos += 4;
    }

    if (pos + 2 > len)
        return;

    uint16_t akmCount = p[pos] | (p[pos + 1] << 8);
    pos += 2;

    for (uint16_t i = 0; i < akmCount; i++)
    {
        if (pos + 4 > len)
            return;

        // 50:6F:9A type 1 is the OSEN AKM
        if (p[pos] == 0x50 && p[pos + 1] == 0x6F &&
            p[pos + 2] == 0x9A && p[pos + 3] == 0x01)
        {
            appendName(info.akm, sizeof(info.akm), "OSEN");
        }

        pos += 4;
    }

    if (pos + 2 > len)
        return;

    uint16_t caps = p[pos] | (p[pos + 1] << 8);

    info.preAuth     = (caps & 0x0001) != 0;
    info.mfpRequired = (caps & 0x0040) != 0;
    info.mfpCapable  = (caps & 0x0080) != 0;
}


void parseSuites(const uint8_t *p, int len, bool wpa1,
                 struct RsnInfo &info)
{
    if (p == nullptr || len < 8)
        return;

    info.present = true;

    const uint8_t oui0 = 0x00;
    const uint8_t oui1 = wpa1 ? 0x50 : 0x0F;
    const uint8_t oui2 = wpa1 ? 0xF2 : 0xAC;

    int pos = 2;                  // Version

    if (pos + 4 <= len)
    {
        noteBadSuite(p + pos, wpa1, info);

        if (p[pos] == oui0 && p[pos + 1] == oui1 && p[pos + 2] == oui2)
        {
            strncpy(info.group,
                    cipherName(p[pos + 3], wpa1),
                    sizeof(info.group) - 1);
        }
        else if (vendorSuiteName(suiteAt(p + pos), false))
        {
            strncpy(info.group,
                    vendorSuiteName(suiteAt(p + pos), false),
                    sizeof(info.group) - 1);
        }
    }

    pos += 4;

    if (pos + 2 > len)
        return;

    uint16_t pairwiseCount = p[pos] | (p[pos + 1] << 8);
    pos += 2;

    for (uint16_t i = 0; i < pairwiseCount; i++)
    {
        if (pos + 4 > len)
            return;

        noteBadSuite(p + pos, wpa1, info);

        if (p[pos] == oui0 && p[pos + 1] == oui1 && p[pos + 2] == oui2)
        {
            appendName(info.pairwise,
                       sizeof(info.pairwise),
                       cipherName(p[pos + 3], wpa1));
        }
        else if (vendorSuiteName(suiteAt(p + pos), false))
        {
            appendName(info.pairwise,
                       sizeof(info.pairwise),
                       vendorSuiteName(suiteAt(p + pos), false));
        }

        pos += 4;
    }

    if (pos + 2 > len)
        return;

    uint16_t akmCount = p[pos] | (p[pos + 1] << 8);
    pos += 2;

    for (uint16_t i = 0; i < akmCount; i++)
    {
        if (pos + 4 > len)
            return;

        bool known =
            (p[pos] == oui0 && p[pos + 1] == oui1 && p[pos + 2] == oui2);

        uint8_t type = p[pos + 3];

        noteBadSuite(p + pos, wpa1, info);

        if (!known && vendorSuiteName(suiteAt(p + pos), true))
            appendName(info.akm, sizeof(info.akm),
                       vendorSuiteName(suiteAt(p + pos), true));

        // CCKM roams within an 802.1X network
        if (suiteAt(p + pos) == 0x00409600u)
            info.eap = true;

        if (known)
        {
            appendName(info.akm,
                       sizeof(info.akm),
                       akmName(type, wpa1));

            if (wpa1)
            {
                if (type == 1) info.eap = true;
                if (type == 2) info.psk = true;
            }
            else
            {
                switch (type)
                {
                    case 1:  case 3:  case 5:
                    case 11: case 12: case 13:
                        info.eap = true;
                        break;

                    // FILS authenticates through 802.1X as well
                    case 14: case 15: case 16: case 17:
                        info.eap = true;
                        break;

                    case 2: case 4: case 6: case 19: case 20:
                        info.psk = true;
                        break;

                    // 24/25: SAE with a group-dependent hash
                    // (SAE-EXT-KEY), the WPA3 variant for groups
                    // beyond 19 and the default with Wi-Fi 7
                    case 8: case 9: case 24: case 25:
                        info.sae = true;
                        break;

                    case 18:
                        info.owe = true;
                        break;

                    default:
                        break;
                }
            }
        }

        pos += 4;
    }

    if (wpa1)
        return;

    if (pos + 2 > len)
        return;

    uint16_t caps = p[pos] | (p[pos + 1] << 8);
    pos += 2;

    info.preAuth     = (caps & 0x0001) != 0;   // Pre-Auth
    info.mfpRequired = (caps & 0x0040) != 0;   // MFPR
    info.mfpCapable  = (caps & 0x0080) != 0;   // MFPC

    if (pos + 2 > len)
        return;

    uint16_t pmkidCount = p[pos] | (p[pos + 1] << 8);
    pos += 2 + 16 * pmkidCount;

    if (pos + 4 <= len)
    {
        if (p[pos] == oui0 && p[pos + 1] == oui1 && p[pos + 2] == oui2)
        {
            strncpy(info.mgmt,
                    cipherName(p[pos + 3], false),
                    sizeof(info.mgmt) - 1);
        }
    }
}
