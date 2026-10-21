"""ehtCapsOf(), mlRemovalOf(), heTwtOf(), broadcastTwtOf() and vhtCapsOf() of the
firmware, compiled on the PC,
against elements Wireshark decodes the same way."""

import pathlib
import shutil
import struct
import subprocess
import sys

import pytest

ROOT = pathlib.Path(__file__).resolve().parent.parent
SKETCH = ROOT / "source" / "wlanscout" / "wlanscout.ino"

sys.path.insert(0, str(ROOT / "tools"))
import check_against_tshark as tshark  # noqa: E402

MAIN = r"""
static int hexval(char c)
{
    return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10;
}

int main(int argc, char **argv)
{
    static uint8_t ies[2000];
    int n = 0;
    const char *hex = argv[1];

    for (int i = 0; hex[i] && hex[i + 1]; i += 2)
        ies[n++] = hexval(hex[i]) << 4 | hexval(hex[i + 1]);

    uint16_t mac = 0, links = 0, tbtts = 0;
    uint8_t  phy = 0;
    bool     qam = false;

    bool caps = ehtCapsOf(ies, n, argv[2][0] == '1', mac, phy, qam);
    bool rm   = mlRemovalOf(ies, n, links, tbtts);

    if (argv[2][0] == 'w')
    {
        printf("8080 %d\n", caps8080Of(ies, n, argv[2][1] == '2'));
        return 0;
    }

    if (argv[2][0] == 'v')
    {
        printf("vht %u\n", vhtCapsOf(ies, n, argv[2][1] == '2'));
        return 0;
    }

    if (argv[2][0] == 'h')
    {
        printf("he %u\n", heCapsOf(ies, n));
        return 0;
    }

    if (argv[2][0] == 't')
    {
        uint8_t ids[2], recs[2];
        uint32_t iv[2], du[2];
        uint8_t k = broadcastTwtOf(ies, n, ids, recs, iv, du, 2);

        printf("twt %u", heTwtOf(ies, n));
        for (uint8_t i = 0; i < k; i++)
            printf(" [%u %u %u %u]", ids[i], recs[i], iv[i], du[i]);
        printf("\n");
        return 0;
    }

    printf("%d %u %u %d | %d %u %u\n", caps, mac, phy, qam, rm, links, tbtts);
}
"""


@pytest.fixture(scope="module")
def parse(tmp_path_factory):
    if not shutil.which("g++"):
        pytest.skip("no g++")

    src = SKETCH.read_text(encoding="utf-8")
    text = "\n".join(["#include <cstdint>", "#include <cstring>",
                      "#include <cstdio>",
                      tshark.cut(src, "const uint8_t *findExtendedIE("),
                      tshark.cut(src, "bool ehtCapsOf("),
                      tshark.cut(src, "bool mlRemovalOf("),
                      tshark.cut(src, "const uint8_t *findIE("),
                      tshark.cut(src, "uint8_t heTwtOf("),
                      tshark.cut(src, "uint8_t heCapsOf("),
                      tshark.cut(src, "uint8_t vhtCapsOf("),
                      tshark.cut(src, "bool caps8080Of("),
                      tshark.cut(src, "uint8_t broadcastTwtOf(")]) + MAIN

    d = tmp_path_factory.mktemp("ehtcaps")
    (d / "t.cpp").write_text(text)
    subprocess.run(["g++", "-o", str(d / "t"), str(d / "t.cpp")], check=True)

    return lambda ies, twenty=False, mode=None: subprocess.run(
        [str(d / "t"), ies.hex(), mode or ("1" if twenty else "0")],
        capture_output=True, text=True, check=True).stdout.strip()


def ie(i, body):
    return bytes([i, len(body)]) + body


def test_eht_capabilities(parse):
    # EPCS, restricted TWT and SCS; 320 MHz; MCS 12/13 on two streams
    eht = bytes([108]) + struct.pack("<H", 0x0031) + bytes([0x02]) + \
        bytes(8) + bytes([0x22, 0x22, 0x22])

    assert parse(ie(255, eht)) == "1 49 2 1 | 0 0 0"

    # Without MCS 12/13 no 4096-QAM; a 20 MHz-only station's set has
    # MCS 0-7 first, so 12/13 is its fourth octet
    no4k = bytes([108]) + bytes(2) + bytes(9) + bytes([0x22, 0x22, 0x00])
    assert parse(ie(255, no4k)) == "1 0 0 0 | 0 0 0"
    twenty = bytes([108]) + bytes(2) + bytes(9) + bytes([0x22, 0x22, 0x22, 0x00])
    assert parse(ie(255, twenty), twenty=True) == "1 0 0 0 | 0 0 0"


def test_link_removal(parse):
    common = bytes([7]) + bytes.fromhex("020000000001")
    info = bytes([3]) + struct.pack("<H", 30)
    prof = bytes([0, 2 + len(info)]) + struct.pack("<H", 2 | 1 << 6) + info
    ml = bytes([107]) + struct.pack("<H", 0x0012) + common + prof

    assert parse(ie(255, ml)) == "0 0 0 0 | 1 4 30"

    # The Basic variant is no removal
    basic = bytes([107]) + struct.pack("<H", 0x0010) + common
    assert parse(ie(255, basic)) == "0 0 0 0 | 0 0 0"


def test_twt(parse):
    # HE MAC capabilities: requester (bit 1), responder (bit 2),
    # broadcast TWT (bit 20)
    he = bytes([35, 0x06, 0x00, 0x10, 0, 0, 0]) + bytes(11)
    # As Wireshark reads it: broadcast, ID 3, recommendation 4
    # (restricted TWT), every 100 << 10 us for 32 x 256 us
    req = (4 << 1) | (1 << 5) | (4 << 7) | (10 << 10)
    twt = bytes([2 << 2]) + struct.pack("<H", req) + struct.pack("<H", 0) + \
        bytes([32]) + struct.pack("<H", 100) + struct.pack("<H", (3 << 3) | (5 << 8))

    assert parse(ie(255, he) + ie(216, twt), mode="t") == \
        "twt 7 [3 4 102400 8192]"
    # Individual TWT (type 0) is no schedule
    assert parse(ie(216, bytes([0]) + bytes(9)), mode="t") == "twt 0"


def test_he_caps_bits(parse):
    phy = bytearray(11)
    phy[0] = 0x06
    phy[2] |= 0x40
    phy[3] |= 0x01 | 0x80
    phy[4] |= 0x02
    he = bytes([35]) + bytes(6) + bytes(phy) + struct.pack("<HH", 0xfffa, 0xfffa)

    # As Wireshark reads it: UL MU-MIMO, DCM, SU and MU beamformer,
    # MCS 0-11 on two streams: all five bits
    assert parse(ie(255, he), mode="h") == "he 31"


def vht(info, rxmap):
    return ie(191, struct.pack("<I", info) + struct.pack("<HHHH", rxmap, 0, rxmap, 0))


def test_vht_caps_bits(parse):
    # As Wireshark reads them: SU beamformer bit 11, SU beamformee 12,
    # MU beamformer 19, MU beamformee 20; Rx map 2 bits per stream
    assert parse(vht(1 << 11, 0xfffe), mode="v5") == "vht %d" % (32 | 4 | 16)
    assert parse(vht(1 << 12, 0xfffd), mode="v5") == "vht %d" % (32 | 8 | 16)
    assert parse(vht(1 << 19, 0xfffa), mode="v5") == "vht %d" % (32 | 1 | 16)
    assert parse(vht(1 << 20, 0xfff0), mode="v5") == "vht %d" % (32 | 2)
    # MCS 0-8 is 256-QAM as well, MCS 0-7 on every stream is not
    assert parse(vht(0, 0xfffd), mode="v5") == "vht 48"
    assert parse(vht(0, 0xfffc), mode="v5") == "vht 32"
    # Not in 2.4 GHz, and nothing without the element
    assert parse(vht(1 << 19, 0xfffe), mode="v2") == "vht 0"
    assert parse(b"", mode="v5") == "vht 0"


def he_phy(width_set):
    # MAC capabilities, then PHY: the channel width set from bit 1
    return ie(255, bytes([35]) + bytes(6) + bytes([width_set << 1]) +
              bytes(10) + bytes.fromhex("faff"))


def test_80_plus_80_offered(parse):
    # VHT Supported Channel Width Set 2: 160 and 80+80; 1 is 160 only
    assert parse(vht(2 << 2, 0xfffe), mode="w5") == "8080 1"
    assert parse(vht(1 << 2, 0xfffe), mode="w5") == "8080 0"
    # HE: bit 3 of the width set, 160/80+80 in 5 GHz; bit 2 is 160 only
    assert parse(he_phy(0x0E), mode="w5") == "8080 1"
    assert parse(he_phy(0x06), mode="w5") == "8080 0"
    # Not in 2.4 GHz
    assert parse(vht(2 << 2, 0xfffe), mode="w2") == "8080 0"
