"""The graphs' recording in the firmware: watchFrame(), watchClose() and
watchLoop() compiled on the PC, with millis() and countClients() of
the test's own."""

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

STUBS = r"""
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>

static uint32_t nowMs = 0;
static uint16_t seenNow = 0;

uint32_t millis() { return nowMs; }
uint16_t countClients(const uint8_t *bssid) { return seenNow; }
"""

MAIN = r"""
static const uint8_t BSS[6] = { 2, 0, 0, 0, 0, 1 };

// One command per line: start, frame RSSI STATIONS UTIL, seen N,
// at MS, renew, keep, dump
int main()
{
    char cmd[16];

    while (scanf("%15s", cmd) == 1)
    {
        if (!strcmp(cmd, "start"))
        {
            BssWatch &w = watches[0];

            memset(&w, 0, sizeof(w));
            memcpy(w.bssid, BSS, 6);
            w.used      = true;
            w.ring      = (WatchSample *)malloc(sizeof(WatchSample) *
                                                WATCH_SAMPLES);
            w.stepStart = nowMs;
            w.renewed   = nowMs;
            watchStepReset(0);
        }
        else if (!strcmp(cmd, "frame"))
        {
            int r, s, u;

            scanf("%d %d %d", &r, &s, &u);
            watchFrame(BSS, (int8_t)r, (int16_t)s, (int16_t)u);
        }
        else if (!strcmp(cmd, "seen"))
        {
            int n;

            scanf("%d", &n);
            seenNow = (uint16_t)n;
        }
        else if (!strcmp(cmd, "at"))
        {
            unsigned ms;

            scanf("%u", &ms);
            nowMs = ms;
            watchLoop();
        }
        else if (!strcmp(cmd, "renew"))
            watches[0].renewed = nowMs;
        else if (!strcmp(cmd, "keep"))
            watches[0].keep = true;
        else if (!strcmp(cmd, "dump"))
        {
            const BssWatch &w = watches[0];

            printf("used %d next %u\n", w.used, w.used ? w.next : 0);

            uint32_t first = w.next > WATCH_SAMPLES ? w.next - WATCH_SAMPLES : 0;

            for (uint32_t k = first; w.used && k < w.next && k < first + 5; k++)
            {
                const WatchSample &s = w.ring[k % WATCH_SAMPLES];

                printf("%u: %d %d %d %u %u %u %u\n", k, s.rssiMin, s.rssiAvg,
                       s.rssiMax, s.util, s.stations, s.seen, s.heard);
            }
        }
    }
}
"""


def piece(src, start, end):
    a = src.index(start)
    return src[a:src.index(end, a) + len(end)]


@pytest.fixture(scope="module")
def watch(tmp_path_factory):
    if not shutil.which("g++"):
        pytest.skip("no g++")

    src = sketch_text()
    text = "\n".join([
        STUBS,
        # The sizes and both structs, up to the end of BssWatch
        piece(src, "#define WATCH_MAX", "int16_t      util, stations;\n};"),
        "BssWatch watches[WATCH_MAX];",
        tshark.cut(src, "int watchIndex("),
        tshark.cut(src, "void watchStepReset("),
        tshark.cut(src, "void watchFrame("),
        tshark.cut(src, "void watchClose("),
        tshark.cut(src, "void watchStop("),
        tshark.cut(src, "void watchLoop("),
    ]) + MAIN

    d = tmp_path_factory.mktemp("watch")
    (d / "t.cpp").write_text(text)
    subprocess.run(["g++", "-w", "-o", str(d / "t"), str(d / "t.cpp")],
                   check=True)

    return lambda script: subprocess.run(
        [str(d / "t")], input=script, capture_output=True, text=True,
        check=True).stdout.splitlines()


def test_one_step(watch):
    out = watch("start seen 3 "
                "frame -60 12 40 frame -50 12 41 frame -55 13 42 "
                "at 4999 dump at 5000 dump")

    # Nothing before the 5 s step is over, then min, avg, max and the last
    # BSS Load, the stations the sensor sees, three beacons
    assert out[0] == "used 1 next 0"
    assert out[1] == "used 1 next 1"
    assert out[2] == "0: -60 -55 -50 42 13 3 3"


def test_step_without_a_beacon_and_without_bss_load(watch):
    out = watch("start frame -70 -1 -1 at 5000 at 10000 dump")

    assert out[0] == "used 1 next 2"
    assert out[1] == "0: -70 -70 -70 255 65535 0 1"
    # The sweep elsewhere: an empty step, not a long one
    assert out[2] == "1: 0 0 0 255 65535 0 0"


def test_stalled_loop_gives_empty_steps(watch):
    out = watch("start frame -60 -1 -1 at 12500 renew dump")

    assert out[0] == "used 1 next 2"


def test_dropped_without_renewal(watch):
    # 3 min, so a background tab polling once a minute keeps it
    assert watch("start at 179000 dump")[0].startswith("used 1")
    assert watch("start at 180001 dump")[0] == "used 0 next 0"
    assert watch("start at 120000 renew at 290000 dump")[0].startswith(
        "used 1")


def test_ring_wraps_after_a_day(watch):
    # 17280 steps of 5 s kept, the oldest go
    script = "start " + " ".join("at %d renew" % (i * 5000)
                                 for i in range(1, 17291)) + " dump"
    out = watch(script)

    assert out[0] == "used 1 next 17290"
    assert out[1].startswith("10: ")


def test_kept_one_not_dropped(watch):
    # Recorded without a viewer: no renewal for an hour, still there
    out = watch("start keep at 3600000 dump")

    assert out[0] == "used 1 next 720"
