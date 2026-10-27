"""noteSixGhz() of the firmware names a 6 GHz BSS on every report, so a
renamed network does not keep its old name. Compiled on the PC."""

import pathlib
import shutil
import subprocess
import sys
import zlib

import pytest

ROOT = pathlib.Path(__file__).resolve().parent.parent
SKETCH = ROOT / "source" / "wlanscout" / "wlanscout.ino"

sys.path.insert(0, str(ROOT / "tools"))
import check_against_tshark as tshark  # noqa: E402

STUBS = r"""
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string>

#define MAX_SIXG_BSS 40
#define MAX_NETWORKS 4

struct WiFiNetwork { bool valid; char ssid[33]; };
WiFiNetwork networks_[MAX_NETWORKS];
WiFiNetwork *networks = networks_;

uint32_t millis() { return 0; }
uint16_t sixGhzFreq(uint8_t ch) { return 5950 + 5 * ch; }
std::string macToString(const uint8_t *) { return ""; }
struct { int printf(const char *, ...) { return 0; } } Serial;
"""

MAIN = r"""
static SixGhzBss list[MAX_SIXG_BSS];

static const uint8_t BSS[6] = { 2, 0, 0, 0, 0, 0x22 };
static const uint8_t REP[6] = { 2, 0, 0, 0, 0, 0x02 };

// One command per line: report SAME REPORTER_SSID SHORT (hex, 0 none),
// known I SSID, forget I, dump
int main()
{
    char cmd[16];

    sixGhz = list;

    while (scanf("%15s", cmd) == 1)
    {
        if (!strcmp(cmd, "report"))
        {
            int same;
            char rep[40];
            unsigned shortSsid;

            scanf("%d %39s %x", &same, rep, &shortSsid);
            noteSixGhz(BSS, 131, 1, shortSsid, shortSsid != 0,
                       same ? 0x02 : 0, true, 127, REP, rep, 1);
        }
        else if (!strcmp(cmd, "known"))
        {
            int i;
            char ssid[40];

            scanf("%d %39s", &i, ssid);
            networks[i].valid = true;
            strcpy(networks[i].ssid, ssid);
        }
        else if (!strcmp(cmd, "forget"))
        {
            int i;

            scanf("%d", &i);
            networks[i].valid = false;
        }
        else if (!strcmp(cmd, "dump"))
            printf("'%s'\n", list[0].ssid);
    }
}
"""


def crc(name):
    return "%x" % zlib.crc32(name.encode())


@pytest.fixture(scope="module")
def six(tmp_path_factory):
    if not shutil.which("g++"):
        pytest.skip("no g++")

    src = SKETCH.read_text(encoding="utf-8")
    text = "\n".join([
        STUBS,
        tshark.cut(src, "struct SixGhzBss"),
        "SixGhzBss *sixGhz = nullptr;",
        tshark.cut(src, "uint32_t crc32ssid("),
        tshark.cut(src, "void noteSixGhz("),
    ]) + MAIN

    d = tmp_path_factory.mktemp("sixname")
    (d / "t.cpp").write_text(text)
    subprocess.run(["g++", "-w", "-o", str(d / "t"), str(d / "t.cpp")],
                   check=True)

    return lambda script: subprocess.run(
        [str(d / "t")], input=script, capture_output=True, text=True,
        check=True).stdout.splitlines()


def test_same_ssid_follows_a_rename(six):
    # As .219: renamed from OpenWrt-PAX9500 to OpenWrt-PAX9500-Test
    old, new = "OpenWrt-PAX9500", "OpenWrt-PAX9500-Test"

    assert six("report 1 %s %s dump report 1 %s %s dump" %
               (old, crc(old), new, crc(new))) == ["'%s'" % old, "'%s'" % new]


def test_short_ssid_name_dropped_when_it_no_longer_matches(six):
    out = six("known 0 Lab report 0 Office %s dump "
              "forget 0 known 1 Lab2 report 0 Office %s dump "
              "report 0 Office %s dump" % (crc("Lab"), crc("Lab2"),
                                           crc("Gone")))

    # Lab, then the short SSID changed to Lab2's, then to one nobody has
    assert out == ["'Lab'", "'Lab2'", "''"]


def test_hidden_reporter_keeps_the_short_ssid_name(six):
    assert six("known 0 Lab report 1 <hidden> %s dump" % crc("Lab")) == \
        ["'Lab'"]
