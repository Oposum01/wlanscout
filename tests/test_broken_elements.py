"""Frames broken past the FCS check: collectUnknownElements() of the
firmware takes what catalogScan() takes, nothing after an element of a
length the standard does not allow, nor after a second SSID, and
elementsPlausible() keeps such a beacon or probe response from being
parsed at all. A PAX9500 beacon made an "Unknown element 24" note the
catalog had no row for. Compiled on the PC from the sketch."""

import pathlib
import shutil
import subprocess
import sys

import pytest

from sketch import sketch_text

ROOT = pathlib.Path(__file__).resolve().parent.parent

sys.path.insert(0, str(ROOT / "tools"))
import check_against_tshark as tshark  # noqa: E402

PIECES = [
    "static const uint8_t decodedIes[]",
    "static const uint8_t decodedExt[]",
    "static const uint8_t passedIes[]",
    "static const uint8_t passedExt[]",
    "static bool inList(",
    "#define IN_LIST",
    "bool elementKnown(",
    "bool elementLengthOk(",
    "bool elementsPlausible(",
    "void collectUnknownElements(",
]

MAIN = r"""
#include <cstdio>

// The elements in hex as the argument, the unknown codes out, then
// "ok" or "broken" from elementsPlausible()
int main(int argc, char **argv)
{
    uint8_t ies[512];
    int n = 0;

    for (const char *p = argv[1]; p[0] && p[1]; p += 2)
        sscanf(p, "%2hhx", &ies[n++]);

    uint16_t list[6];
    uint8_t seen[6];
    uint8_t count = 0;

    collectUnknownElements(ies, n, list, seen, count, 6, 1);

    for (uint8_t i = 0; i < count; i++)
        printf("%u ", list[i]);

    printf("%s", elementsPlausible(ies, n) ? "ok" : "broken");

    return 0;
}
"""


@pytest.fixture(scope="module")
def unknown(tmp_path_factory):
    if not shutil.which("g++"):
        pytest.skip("no g++")

    src = sketch_text()
    text = "\n".join(["#include <cstdint>", "#include <cstring>"] +
                     [tshark.cut(src, p) for p in PIECES]) + MAIN

    d = tmp_path_factory.mktemp("unknown")
    (d / "t.cpp").write_text(text)
    subprocess.run(["g++", "-O1", "-w", "-o", str(d / "t"), str(d / "t.cpp")],
                   check=True)

    def run(hexies):
        return subprocess.run([str(d / "t"), hexies], capture_output=True,
                              text=True, check=True).stdout

    return run


SSID = "0003616263"          # SSID "abc"
DS = "030124"                # DS Parameter Set, channel 36


def test_unknown_element_noted(unknown):
    assert unknown(SSID + DS + "1802abcd") == "24 ok"


def test_nothing_after_a_wrong_length(unknown):
    # DS Parameter Set with 2 octets instead of 1
    assert unknown(SSID + "03022400" + "1802abcd") == "broken"


def test_nothing_after_a_second_ssid(unknown):
    assert unknown(SSID + DS + "0000" + "1802abcd") == "broken"


def test_unknown_before_the_break_stays(unknown):
    assert unknown(SSID + "1802abcd" + "03022400" + "1901ff") == "24 broken"


def test_hidden_ssid_counts_once(unknown):
    # An empty SSID element is the first, not a second
    assert unknown("0000" + DS + "1802abcd") == "24 ok"


def test_cut_off_by_the_capture_is_not_broken(unknown):
    # The last element runs past the end: a short capture, not garbage
    assert unknown(SSID + DS + "3020aabb") == "ok"


def test_a_plain_beacon_passes(unknown):
    # HT Capabilities 26, HT Operation 22, VHT Capabilities 12
    assert unknown(SSID + DS + "2d1a" + "00" * 26 + "3d16" + "00" * 22 +
                   "bf0c" + "00" * 12) == "ok"
