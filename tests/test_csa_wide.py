"""csaWideOf() of the firmware: the width and the country after a
channel switch, from the Wide Bandwidth Channel Switch (194) on its
own or in the Channel Switch Wrapper (196); csaTimeOf(): the Max
Channel Switch Time (ext 52) beside it, and the note that says it.
Compiled on the PC."""

import pathlib
import shutil
import subprocess
import sys

import pytest

ROOT = pathlib.Path(__file__).resolve().parent.parent
SKETCH = ROOT / "source" / "wlanscout" / "wlanscout.ino"

sys.path.insert(0, str(ROOT / "tools"))
import check_against_tshark as tshark  # noqa: E402
from sketch import sketch_text

MAIN = r"""
static int hexval(char c)
{
    return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10;
}

int main(int argc, char **argv)
{
    static uint8_t ies[600];
    int n = 0;

    for (int i = 0; argv[1][i] && argv[1][i + 1]; i += 2)
        ies[n++] = hexval(argv[1][i]) << 4 | hexval(argv[1][i + 1]);

    uint16_t width;
    uint8_t  centre, centre2;
    char     country[3];

    bool ok = csaWideOf(ies, n, width, centre, centre2, country);

    printf("%d %u %u %u '%s'\n", ok, width, centre, centre2, country);
}
"""


@pytest.fixture(scope="module")
def wide(tmp_path_factory):
    if not shutil.which("g++"):
        pytest.skip("no g++")

    src = sketch_text()
    text = "\n".join(["#include <cstdint>", "#include <cstring>",
                      "#include <cstdio>",
                      tshark.cut(src, "const uint8_t *findIE("),
                      tshark.cut(src, "bool vhtWidthOf("),
                      tshark.cut(src, "bool csaWideOf(")]) + MAIN

    d = tmp_path_factory.mktemp("csawide")
    (d / "t.cpp").write_text(text)
    subprocess.run(["g++", "-o", str(d / "t"), str(d / "t.cpp")], check=True)

    return lambda ies: subprocess.run([str(d / "t"), ies.hex()],
                                      capture_output=True, text=True,
                                      check=True).stdout.strip()


def ie(i, body):
    b = bytes.fromhex(body)
    return bytes([i, len(b)]) + b


# As a FRITZ!Box sent them, moving from 40 to 36 within its 80 MHz:
# CSA mode 0 to 36 in 1 beacon, the wrapper with 194 width 1, CCFS0 42
CSA = ie(37, "002401")
WRAPPER = ie(196, "c203012a00")


def test_wrapper_as_captured(wide):
    # Wireshark reads it the same: new width 80 MHz, centre 42
    assert wide(CSA + WRAPPER) == "1 80 42 0 ''"


def test_wrapper_with_160_and_a_new_country(wide):
    body = ie(194, "012a32") + ie(7, "434820")    # 160 MHz on 50, CH
    assert wide(CSA + ie(196, body.hex())) == "1 160 50 0 'CH'"


def test_wide_switch_on_its_own_and_80_plus_80(wide):
    assert wide(CSA + ie(194, "012a9b")) == "1 8080 42 155 ''"


def test_nothing_without_194_or_for_20_40(wide):
    assert wide(CSA) == "0 0 0 0 ''"
    # Width 0 is 20 or 40 MHz, which 194 does not tell apart
    assert wide(CSA + ie(196, "c203000000")) == "0 0 0 0 ''"


TIME_MAIN = r"""
static int hexval(char c)
{
    return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10;
}

int main(int argc, char **argv)
{
    static uint8_t ies[600];
    int n = 0;

    for (int i = 0; argv[1][i] && argv[1][i + 1]; i += 2)
        ies[n++] = hexval(argv[1][i]) << 4 | hexval(argv[1][i + 1]);

    printf("%u\n", (unsigned)csaTimeOf(ies, n));
}
"""


@pytest.fixture(scope="module")
def switch_time(tmp_path_factory):
    if not shutil.which("g++"):
        pytest.skip("no g++")

    src = sketch_text()
    text = "\n".join(["#include <cstdint>", "#include <cstring>",
                      "#include <cstdio>",
                      tshark.cut(src, "const uint8_t *findExtendedIE("),
                      tshark.cut(src, "uint32_t csaTimeOf(")]) + TIME_MAIN

    d = tmp_path_factory.mktemp("csatime")
    (d / "t.cpp").write_text(text)
    subprocess.run(["g++", "-o", str(d / "t"), str(d / "t.cpp")], check=True)

    return lambda ies: int(subprocess.run([str(d / "t"), ies.hex()],
                                          capture_output=True, text=True,
                                          check=True).stdout)


# The Ruckus AP moving from 1 to 6: CSA and ECSA in 18 beacons, Max
# Channel Switch Time 97 TU, as Wireshark reads it
RUCKUS = ie(37, "000612") + ie(60, "00510612") + ie(255, "34610000")


def test_switch_time_as_captured(switch_time):
    assert switch_time(RUCKUS) == 97
    assert switch_time(ie(255, "34ffffff")) == 0xFFFFFF


def test_no_switch_time_without_the_element(switch_time):
    assert switch_time(CSA + WRAPPER) == 0
    # Too short to carry it
    assert switch_time(ie(255, "346100")) == 0


def test_switch_time_in_the_note(opened):
    from test_tooltip import tooltip

    text = tooltip(opened, {"csach": 6, "csacnt": 18, "csamode": 0,
                            "csatime": 97})

    assert "moving to channel 6 in 18 beacons, off the air for at most " \
           "97 TU (99 ms)." in text

    assert "off the air" not in tooltip(opened, {"csach": 6, "csacnt": 18,
                                                 "csamode": 0})
