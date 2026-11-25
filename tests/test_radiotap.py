"""The radiotap header in front of every exported frame: rxRateOf(),
radiotapRateOf(), pcapBuildRadiotap() and radiotapLenOf() of the
firmware compiled on the PC, the header read back by a radiotap parser
written here from the specification (alignment, present bits)."""

import pathlib
import re
import shutil
import struct
import subprocess

import pytest
from sketch import sketch_text

ROOT = pathlib.Path(__file__).resolve().parent.parent
SKETCH = ROOT / "source" / "wlanscout" / "wlanscout.ino"

HARNESS = r"""
#include <cstdio>
#include <cstdint>
#include <cstring>

#define IRAM_ATTR

%s

int main()
{
    char cmd[8];

    while (scanf("%%7s", cmd) == 1)
    {
        if (!strcmp(cmd, "rt"))
        {
            // channel rssi noise bbFormat rate
            int ch, rssi, noise, fmt, rate;

            if (scanf("%%d %%d %%d %%d %%d", &ch, &rssi, &noise, &fmt,
                      &rate) != 5)
                return 1;

            uint8_t rt[RADIOTAP_LEN];

            pcapBuildRadiotap(rt, ch, rssi, noise, rxRateOf(fmt, rate));

            for (int i = 0; i < RADIOTAP_LEN; i++)
                printf("%%02x", rt[i]);
            printf("\n");
        }
        else
        {
            // len hex: radiotapLenOf() of the first octets of a frame
            unsigned len;
            char     hex[64];

            if (scanf("%%u %%63s", &len, hex) != 2)
                return 1;

            uint8_t b[32] = {};

            for (int i = 0; hex[2 * i] && hex[2 * i + 1]; i++)
                sscanf(hex + 2 * i, "%%2hhx", &b[i]);

            printf("%%u\n", radiotapLenOf(b, len));
        }
    }
}
"""


def cut(src, start):
    a = src.index(start)
    return src[a:src.index("\n}\n", a) + 3]


@pytest.fixture(scope="module")
def radiotap(tmp_path_factory):
    if not shutil.which("g++"):
        pytest.skip("no g++")

    src = sketch_text()

    parts = [re.search(r"^#define RADIOTAP_LEN \d+$", src, re.M).group(0),
             re.search(r"^#define RX_RATE_UNKNOWN 0xFF$", src, re.M).group(0),
             cut(src, "uint8_t IRAM_ATTR rxRateOf("),
             cut(src, "uint16_t pcapChannelFreq("),
             cut(src, "uint8_t radiotapRateOf("),
             cut(src, "void pcapBuildRadiotap("),
             cut(src, "uint16_t radiotapLenOf(")]

    d = tmp_path_factory.mktemp("radiotap")
    (d / "t.cpp").write_text(HARNESS % "\n".join(parts))
    subprocess.run(["g++", "-Wall", "-Werror", "-o", str(d / "t"),
                    str(d / "t.cpp")], check=True)

    return d / "t"


def run(binary, lines):
    return subprocess.run([str(binary)], input="\n".join(lines) + "\n",
                          text=True, capture_output=True,
                          check=True).stdout.split()


# bit: (alignment, size, name) for the fields the firmware may write
FIELDS = {1: (1, 1, "flags"), 2: (1, 1, "rate"), 3: (2, 4, "channel"),
          5: (1, 1, "signal"), 6: (1, 1, "noise")}


def parse(hexed):
    """The fields of one radiotap header, as Wireshark reads them."""
    b = bytes.fromhex(hexed)
    version, _, length, present = struct.unpack_from("<BBHI", b)

    assert version == 0 and length == len(b)
    assert present & ~sum(1 << k for k in FIELDS) == 0

    out, off = {}, 8
    for bit in sorted(FIELDS):
        if not present & (1 << bit):
            continue
        align, size, name = FIELDS[bit]
        off = (off + align - 1) // align * align
        raw = b[off:off + size]
        off += size
        if name == "channel":
            out[name] = struct.unpack("<HH", raw)
        elif name in ("signal", "noise"):
            out[name] = struct.unpack("<b", raw)[0]
        else:
            out[name] = raw[0]

    assert off <= length
    return out


def header(binary, ch, rssi, noise, fmt, rate):
    return parse(run(binary, ["rt %d %d %d %d %d" %
                              (ch, rssi, noise, fmt, rate)])[0])


def test_length_is_sixteen(radiotap):
    out = run(radiotap, ["rt 6 -60 -95 1 11", "rt 36 -60 0 4 0"])
    assert [len(h) // 2 for h in out] == [16, 16]


def test_ofdm_beacon_on_5ghz(radiotap):
    # 6 Mbit/s, L-SIG code 0xB
    h = header(radiotap, 36, -61, -96, 1, 0x0B)

    assert h == {"flags": 0, "rate": 12, "channel": (5180, 0x0140),
                 "signal": -61, "noise": -96}


def test_cck_beacon_on_24ghz(radiotap):
    # 1 Mbit/s, long preamble
    h = header(radiotap, 6, -70, -92, 0, 0)

    assert h == {"flags": 0, "rate": 2, "channel": (2437, 0x00A0),
                 "signal": -70, "noise": -92}


def test_short_preamble(radiotap):
    h = header(radiotap, 1, -50, -90, 0, 7)

    assert h["flags"] == 0x02 and h["rate"] == 22
    assert h["channel"] == (2412, 0x00A0)


@pytest.mark.parametrize("fmt, code, units", [
    (0, 0, 2), (0, 1, 4), (0, 2, 11), (0, 3, 22), (0, 5, 4), (0, 6, 11),
    (1, 0x0B, 12), (1, 0x0F, 18), (1, 0x0A, 24), (1, 0x0E, 36),
    (1, 0x09, 48), (1, 0x0D, 72), (1, 0x08, 96), (1, 0x0C, 108)])
def test_every_legacy_rate(radiotap, fmt, code, units):
    assert header(radiotap, 11, -40, -90, fmt, code)["rate"] == units


@pytest.mark.parametrize("fmt, code", [
    (2, 0x0B),      # HT: the L-SIG rate says nothing
    (4, 0x0B),      # HE SU
    (0, 4),         # no such 802.11b code
    (0, 9),         # an OFDM code in an 802.11b frame
    (1, 3)])        # an 802.11b code in an OFDM frame
def test_no_rate_without_a_legacy_one(radiotap, fmt, code):
    h = header(radiotap, 149, -55, -97, fmt, code)

    assert "rate" not in h
    # Still OFDM, as before 2.8.27, and the noise where it belongs
    assert h["channel"] == (5745, 0x0140)
    assert h["noise"] == -97 and h["signal"] == -55


def test_no_noise_reading(radiotap):
    h = header(radiotap, 1, -45, 0, 1, 0x0B)

    assert "noise" not in h and h["signal"] == -45


# Before 2.8.27: 15 octets, flags, channel 1 OFDM, -60 dBm
OLD = "00000f002a0000000000" "6c09c000" "c4"


@pytest.mark.parametrize("length, first, expect", [
    (100, "000010002e000000", 16),
    (100, OLD, 15),
    (15, OLD, 15),
    (14, OLD, 0),                    # longer than the frame
    (7, "000010002e000000", 0),      # not even the fixed part
    (100, "000040002e000000", 0),    # longer than any of ours
    (100, "000004002e000000", 0)])   # shorter than the fixed part
def test_header_length_read_back(radiotap, length, first, expect):
    assert run(radiotap, ["len %d %s" % (length, first)]) == [str(expect)]
