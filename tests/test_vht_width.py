"""vhtWidthOf() of the firmware: channel width and centre from the VHT
Operation element. Compiled on the PC from the sketch."""

import pathlib
import shutil
import subprocess

import pytest

ROOT = pathlib.Path(__file__).resolve().parent.parent
SKETCH = ROOT / "source" / "wlanscout" / "wlanscout.ino"

HARNESS = r"""
#include <cstdio>
#include <cstdint>

%s

// Lines of "<width> <ccfs0> <ccfs1>"
int main()
{
    unsigned w, a, b;

    while (scanf("%%u %%u %%u", &w, &a, &b) == 3)
    {
        const uint8_t vht[3] = { (uint8_t)w, (uint8_t)a, (uint8_t)b };
        uint16_t bandwidth = 20;
        uint8_t  centre    = 0;
        uint8_t  centre2   = 0;

        if (vhtWidthOf(vht, bandwidth, centre, centre2) && centre2)
            printf("%%u %%u %%u\n", bandwidth, centre, centre2);
        else if (vhtWidthOf(vht, bandwidth, centre, centre2))
            printf("%%u %%u\n", bandwidth, centre);
        else
            printf("none\n");
    }
}
"""


@pytest.fixture(scope="module")
def vhtwidth(tmp_path_factory):
    if not shutil.which("g++"):
        pytest.skip("no g++")

    src = SKETCH.read_text(encoding="utf-8")
    a = src.index("bool vhtWidthOf(")
    helper = src[a:src.index("void parseBeacon(", a)]

    d = tmp_path_factory.mktemp("vhtwidth")
    (d / "t.cpp").write_text(HARNESS % helper)
    subprocess.run(["g++", "-o", str(d / "t"), str(d / "t.cpp")],
                   check=True)

    return d / "t"


def width(binary, w, ccfs0, ccfs1):
    return subprocess.run([str(binary)], input="%d %d %d\n" % (w, ccfs0, ccfs1),
                          text=True, capture_output=True,
                          check=True).stdout.strip()


def test_160_centre_is_ccfs1(vhtwidth):
    # 36/160 as an OpenWrt AP sends it: 42 the primary 80, 50 the 160
    assert width(vhtwidth, 1, 42, 50) == "160 50"
    # Primary 80 in the upper half: 58 and 50
    assert width(vhtwidth, 1, 58, 50) == "160 50"
    assert width(vhtwidth, 1, 106, 114) == "160 114"


def test_80_and_80_plus_80(vhtwidth):
    assert width(vhtwidth, 1, 42, 0) == "80 42"
    # 80+80: the second segment's centre as well
    assert width(vhtwidth, 1, 42, 106) == "8080 42 106"


def test_deprecated_widths(vhtwidth):
    assert width(vhtwidth, 2, 50, 0) == "160 50"
    assert width(vhtwidth, 3, 42, 106) == "8080 42 106"


def test_20_or_40_left_to_ht(vhtwidth):
    assert width(vhtwidth, 0, 0, 0) == "none"
