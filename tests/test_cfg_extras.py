"""cfgExtrasOf() and cfgDiff() of the firmware: what the bell reports
from a beacon's elements beyond the network entry. Compiled on the PC
from the sketch, with the parsers check_against_tshark.py takes."""

import pathlib
import shutil
import subprocess
import sys

import pytest

ROOT = pathlib.Path(__file__).resolve().parent.parent
SKETCH = ROOT / "source" / "wlanscout" / "wlanscout.ino"

sys.path.insert(0, str(ROOT / "tools"))
import check_against_tshark as tshark  # noqa: E402

# Before cfgDiff(): what it records into, printed instead
RECORD = r"""
struct WiFiNetwork { int unused; };

void cfgRecord(WiFiNetwork &, uint8_t field, const char *from,
               const char *to)
{
    printf("%s|%s|%s\n", cfgFieldNames[field], from, to);
}
"""

MAIN = r"""
static int hexval(char c)
{
    return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10;
}

static int unhex(const char *hex, uint8_t *out)
{
    int n = 0;

    for (int i = 0; hex[i] && hex[i + 1]; i += 2)
        out[n++] = hexval(hex[i]) << 4 | hexval(hex[i + 1]);

    return n;
}

// Two lines of hex elements: the state before and after
int main(int argc, char **argv)
{
    static char line[2][4000];

    // The flapping entry's list of names, cut to its 40 octets
    if (argc > 1)
    {
        char out[40];

        shortList(out, sizeof(out), argv[1]);
        printf("%s\n", out);
        return 0;
    }

    static uint8_t ies[2][2000];
    CfgSnap snap[2];

    for (int k = 0; k < 2; k++)
    {
        if (!fgets(line[k], sizeof(line[k]), stdin))
            return 1;

        line[k][strcspn(line[k], "\r\n")] = 0;

        memset(&snap[k], 0, sizeof(snap[k]));
        cfgExtrasOf(ies[k], unhex(line[k], ies[k]), snap[k]);
    }

    WiFiNetwork n;

    cfgDiff(n, snap[0], snap[1], nullptr, 0);
}
"""


def piece(src, start, end):
    a = src.index(start)
    return src[a:src.index(end, a) + len(end)]


@pytest.fixture(scope="module")
def differ(tmp_path_factory):
    if not shutil.which("g++"):
        pytest.skip("no g++")

    src = SKETCH.read_text(encoding="utf-8")
    parsers = [tshark.cut(src, p) for p in tshark.PIECES]

    text = "\n".join(
        ["#include <cstdint>", "#include <cstring>", "#include <cstdio>",
         "#include <cstdlib>"] + parsers[:2] +
        ["#define NUM_CHANNELS_24 (sizeof(channels24) / sizeof(channels24[0]))",
         "#define NUM_CHANNELS_5  (sizeof(channels5) / sizeof(channels5[0]))"] +
        parsers[2:] + [
            tshark.cut(src, "const uint8_t *findVendorIE("),
            tshark.cut(src, "void mergeNames("),
            tshark.cut(src, "bool rsnOverridesOf("),
            piece(src, "struct CfgSnap\n{", "\n};"),
            piece(src, "enum\n{\n    CFG_RESTART", "\n};"),
            piece(src, "const char *const cfgFieldNames[] = {", "\n};"),
            piece(src, "const uint8_t cfgRates[]", ";"),
            tshark.cut(src, "bool ehtOperationOf("),
            tshark.cut(src, "bool mlRemovalOf("),
            tshark.cut(src, "uint8_t broadcastTwtOf("),
            tshark.cut(src, "void puncturedText("),
            tshark.cut(src, "void cfgExtrasOf("),
            RECORD,
            tshark.cut(src, "void cfgDiff("),
            tshark.cut(src, "void shortList("),
        ]) + MAIN

    d = tmp_path_factory.mktemp("cfgextras")
    (d / "t.cpp").write_text(text)
    subprocess.run(["g++", "-O1", "-w", "-o", str(d / "t"), str(d / "t.cpp")],
                   check=True)

    return d / "t"


def ie(id_, body):
    body = bytes.fromhex(body) if isinstance(body, str) else bytes(body)
    return bytes([id_, len(body)]) + body


def vendor(oui_type, body=""):
    return ie(221, bytes.fromhex(oui_type) + bytes.fromhex(body))


SSID = ie(0, b"Office")
RATES = ie(1, "8c129824b048606c")       # 6, 12, 24 basic
RSN_SAE = "0100000fac040100000fac040100000fac08c000"


def diff(binary, before, after):
    out = subprocess.run([str(binary)], text=True, capture_output=True,
                         input=before.hex() + "\n" + after.hex() + "\n",
                         check=True).stdout
    return [tuple(line.split("|")) for line in out.splitlines()]


def test_nothing_changed(differ):
    beacon = SSID + RATES + ie(48, RSN_SAE)
    assert diff(differ, beacon, beacon) == []


def test_ssid_and_hidden(differ):
    assert diff(differ, SSID + RATES, ie(0, b"Lab") + RATES) == \
        [("SSID", "Office", "Lab")]
    assert diff(differ, SSID + RATES, ie(0, bytes(6)) + RATES) == \
        [("SSID", "Office", "hidden")]


def test_rsne_override_on_and_its_cipher(differ):
    ovr = vendor("506f9a29", RSN_SAE)
    assert diff(differ, SSID, SSID + ovr) == [("RSNE override", "off", "SAE")]

    gcmp = vendor("506f9a29", "0100000fac040100000fac090100000fac08c000")
    assert diff(differ, SSID + ovr, SSID + gcmp) == \
        [("override cipher", "CCMP-128", "GCMP-256")]


def test_association_disallowed(differ):
    mbo = vendor("506f9a16", "0101 00")
    full = vendor("506f9a16", "010100 040102")
    assert diff(differ, SSID + mbo, SSID + full) == \
        [("association", "allowed", "disallowed, stations at maximum")]


def test_power(differ):
    assert diff(differ, SSID + ie(32, "03"), SSID + ie(32, "06")) == \
        [("power constraint", "3 dB", "6 dB")]
    # Local EIRP, 20 MHz: 23 dBm, then 20 dBm
    assert diff(differ, SSID + ie(195, "002e"), SSID + ie(195, "0028")) == \
        [("Tx power envelope", "23 dBm", "20 dBm")]


def test_wps_detail(differ):
    def wps(state, locked):
        return vendor("0050f204", "104a000110" + "10440001%02x" % state +
                      ("105700010%d" % locked if locked is not None else ""))

    assert diff(differ, SSID + wps(1, None), SSID + wps(2, None)) == \
        [("WPS state", "not configured", "configured")]
    assert diff(differ, SSID + wps(2, 0), SSID + wps(2, 1)) == \
        [("WPS locked", "off", "on")]


def test_owe_hs20_interworking(differ):
    owe = vendor("506f9a1c", "0211223344550" + "4" + b"open".hex())
    assert diff(differ, SSID, SSID + owe) == [("OWE transition", "off", "on")]

    other = vendor("506f9a1c", "02aabbccddee04" + b"open".hex())
    assert diff(differ, SSID + owe, SSID + other) == \
        [("OWE partner", "02:11:22:33:44:55", "02:AA:BB:CC:DD:EE")]

    assert diff(differ, SSID, SSID + vendor("506f9a10", "00")) == \
        [("Hotspot 2.0", "off", "on")]
    assert diff(differ, SSID + ie(107, "00"), SSID + ie(107, "03")) == \
        [("interworking", "private", "free public")]


def test_basic_rates_set_beside_the_lowest(differ):
    more = ie(1, "8c121824b048e06c")     # 6, 24, 48 basic
    assert diff(differ, SSID + RATES, SSID + more) == \
        [("basic rates", "6, 12, 24", "6, 24, 48")]


def test_preauth_acm_intolerant(differ):
    pre = ie(48, RSN_SAE[:-4] + "c100")
    assert diff(differ, SSID + ie(48, RSN_SAE), SSID + pre) == \
        [("pre-authentication", "off", "on")]

    def wmm(acm_vo):
        recs = "03a40000" + "27a40000" + "42435e00" + \
            ("72" if acm_vo else "62") + "322f00"
        return vendor("0050f202", "0101" + "00" + "00" + recs)

    assert diff(differ, SSID + wmm(False), SSID + wmm(True)) == \
        [("admission control", "none", "VO")]

    assert diff(differ, SSID + ie(45, "6f00" + "00" * 24),
                SSID + ie(45, "6f40" + "00" * 24)) == \
        [("40 MHz intolerant", "off", "on")]


def test_mesh_settings(differ):
    def mesh(gate, accepting):
        return ie(0, b"") + ie(114, b"mesh") + \
            ie(113, "0101000000%02x%02x" % (gate, 0x08 | accepting))

    assert diff(differ, mesh(0, 1), mesh(1, 1)) == \
        [("mesh gate", "off", "on")]
    assert diff(differ, mesh(1, 1), mesh(1, 0)) == \
        [("accepting peerings", "on", "off")]


def test_mlo_links_from_the_rnr(differ):
    # Multi-Link element of this link (ID 0), MLD 02:00:00:00:00:01
    ml = ie(255, "6b" + "1001" + "0a" + "020000000001" + "00" + "0000")

    def rnr(link):
        tbtt = "00" + "aabbccddeeff" + "00000000" + "00" + "00" + \
            "00%02x00" % link
        return ie(201, "0010" + "8301" + tbtt)

    before = SSID + ml + rnr(1)
    after = SSID + ml + rnr(2)
    assert diff(differ, before, after) == [("MLO links", "0, 1", "0, 2")]


def test_mobility_domain(differ):
    assert diff(differ, SSID + ie(54, "2b4a00"), SSID + ie(54, "2c4a00")) == \
        [("mobility domain", "0x4A2B", "0x4A2C")]
    assert diff(differ, SSID + ie(54, "2b4a00"), SSID + ie(54, "2b4a01")) == \
        [("FT over the DS", "off", "on")]
    # Switched on or off is 802.11r's own entry, not this one
    assert diff(differ, SSID, SSID + ie(54, "2b4a00")) == []


def eht(width_code, ccfs0, ccfs1, bitmap=None):
    params = 0x01 | (0x02 if bitmap is not None else 0)
    body = bytes([106, params]) + bytes(4) + bytes([width_code, ccfs0, ccfs1])
    if bitmap is not None:
        body += bitmap.to_bytes(2, "little")
    return ie(255, body)


def test_puncturing(differ):
    # 36/160 (channels 36 ... 64, centre 50): 56 and 60 left out
    assert diff(differ, SSID + eht(3, 42, 50, 0), SSID + eht(3, 42, 50, 0b1100000)) == \
        [("punctured", "-", "56, 60")]
    # 80 MHz on 100 (centre 106), 100 left out, then 104 instead
    assert diff(differ, SSID + eht(2, 106, 0, 0b0001), SSID + eht(2, 106, 0, 0b0010)) == \
        [("punctured", "100", "104")]
    # Without the bitmap nothing is left out
    assert diff(differ, SSID + eht(3, 42, 50), SSID + eht(3, 42, 50, 0)) == []


def removal(link, tbtts):
    import struct
    common = bytes([7]) + bytes.fromhex("020000000001")
    info = bytes([3]) + struct.pack("<H", tbtts)
    prof = bytes([0, 2 + len(info)]) + struct.pack("<H", link | 1 << 6) + info
    return ie(255, bytes([107]) + struct.pack("<H", 0x0012) + common + prof)


def test_link_removal(differ):
    # As Wireshark reads it: link 2, AP Removal Timer 30
    assert diff(differ, SSID, SSID + removal(2, 30)) == \
        [("link removal", "-", "2")]
    # The countdown alone is no change
    assert diff(differ, SSID + removal(2, 30), SSID + removal(2, 29)) == []


def btwt(mantissa, exponent, duration, last=True):
    import struct
    req = (4 << 1) | ((1 << 5) if last else 0) | (exponent << 10)
    return struct.pack("<H", req) + struct.pack("<H", 0) + bytes([duration]) + \
        struct.pack("<H", mantissa) + struct.pack("<H", 3 << 3)


def test_broadcast_twt(differ):
    one = ie(216, bytes([2 << 2]) + btwt(100, 10, 32))
    other = ie(216, bytes([2 << 2]) + btwt(200, 10, 32))
    assert diff(differ, SSID, SSID + one) == \
        [("broadcast TWT", "-", "every 102.4 ms for 8.2 ms")]
    assert diff(differ, SSID + one, SSID + other) == \
        [("broadcast TWT", "every 102.4 ms for 8.2 ms", "every 204.8 ms for 8.2 ms")]


def he_op(twt_required):
    # Parameters (bit 3 TWT required), BSS colour, basic HE-MCS
    params = 0x000004 | (0x08 if twt_required else 0)
    return ie(255, bytes([36]) + params.to_bytes(3, "little") + bytes([10]) +
              bytes.fromhex("fcff"))


def test_twt_required(differ):
    assert diff(differ, SSID + he_op(False), SSID + he_op(True)) == \
        [("TWT required", "off", "on")]
    assert diff(differ, SSID + he_op(True), SSID + he_op(False)) == \
        [("TWT required", "on", "off")]
    assert diff(differ, SSID + he_op(True), SSID + he_op(True)) == []


def test_flapping_list_cut_at_a_whole_name(differ):
    def short(names):
        return subprocess.run([str(differ), names], text=True,
                              capture_output=True, check=True).stdout.rstrip("\n")

    assert short("width, standard") == "width, standard"
    # 70 characters into 40: whole names and an ellipsis, no "PMF, "
    assert short("width, standard, spatial streams, PMF, TWT required, BSS colour") == \
        "width, standard, spatial streams, \u2026"
    assert len(short("x" * 60).encode()) < 40


def wfa(oui_type, body):
    return ie(221, bytes.fromhex("506f9a") + bytes([oui_type]) + bytes.fromhex(body))


# Laid out as an RSNE, as an OpenWrt AP sends them: version, group
# CCMP, GCMP-256 with SAE, and GCMP-256 with SAE-EXT-KEY (24)
OVR_SAE = "0100" + "000fac04" + "0100000fac09" + "0100000fac08" + "cc00"
OVR2_EXT = "0100" + "000fac04" + "0100000fac09" + "0100000fac18" + "cc00"


def test_both_rsne_overrides(differ):
    # SAE-EXT-KEY only in Override 2: as AKM of the override all the same
    assert diff(differ, SSID + wfa(0x29, OVR_SAE),
                SSID + wfa(0x29, OVR_SAE) + wfa(0x2A, OVR2_EXT)) == \
        [("override AKM", "SAE", "SAE, SAE-EXT-KEY")]
