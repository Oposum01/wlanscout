"""The waterfall history: finishSweep() with its rows per sweep and per
HISTORY_STEP_MS, historyFor() picking the rows that answer a range and
historyFirstAfter(), compiled on the PC from the firmware with a clock
the test sets."""

import pathlib
import re
import shutil
import subprocess

import pytest
from sketch import sketch_text

ROOT = pathlib.Path(__file__).resolve().parent.parent
SKETCH = ROOT / "source" / "wlanscout" / "wlanscout.ino"

HARNESS = r"""
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#define TOTAL_CHANNELS 42

static uint32_t clockMs = 0;
uint32_t millis() { return clockMs; }

bool     channelSelected[TOTAL_CHANNELS];
bool     channelVisited[TOTAL_CHANNELS];
uint32_t sweepCount = 0;
void     buildScanPlan() {}

%(defines)s

%(code)s

static void ring(HistoryRing &h, uint32_t rows)
{
    h.rows = (WaterfallEntry *)calloc(rows, sizeof(WaterfallEntry));
    h.capacity = rows;
}

int main()
{
    char cmd[16];

    while (scanf("%%15s", cmd) == 1)
    {
        if (!strcmp(cmd, "rings"))
        {
            // rings FINE COARSE
            unsigned fine, coarse;
            scanf("%%u %%u", &fine, &coarse);
            ring(historyFine, fine);
            if (coarse)
                ring(historyCoarse, coarse);
            historyReset();
        }
        else if (!strcmp(cmd, "sweeps"))
        {
            // sweeps COUNT EVERY_MS VALUE: channels 0-2 measured
            unsigned count, every;
            int value;
            scanf("%%u %%u %%d", &count, &every, &value);
            for (unsigned i = 0; i < count; i++)
            {
                clockMs += every;
                for (int c = 0; c < TOTAL_CHANNELS; c++)
                {
                    channelSelected[c] = c < 3;
                    channelVisited[c]  = c < 3;
                    currentMeasurement[c].rssiSum   = value * 2;
                    currentMeasurement[c].rssiCount = c < 3 ? 2 : 0;
                }
                finishSweep();
            }
        }
        else if (!strcmp(cmd, "query"))
        {
            // query RANGE_MS: which rows, from where, how many
            unsigned range;
            scanf("%%u", &range);
            uint32_t now = millis();
            uint32_t start = now > range ? now - range : 0;
            const HistoryRing &h = historyFor(start);
            uint32_t first = historyFirstAfter(h, start);
            printf("rows=%%s first=%%u count=%%u t0=%%u v0=%%d v3=%%d "
                   "fine=%%u coarse=%%u sweeps=%%u\n",
                   &h == &historyFine ? "fine" : "coarse", first, h.count,
                   first < h.count ? historyRow(h, first).timestamp : 0,
                   first < h.count ? historyRow(h, first).rssi[0] : 0,
                   first < h.count ? historyRow(h, first).rssi[3] : 0,
                   historyFine.count, historyCoarse.count, sweepCount);
        }
        else if (!strcmp(cmd, "coarse"))
        {
            // coarse I: timestamp and channel 0 of row I from the oldest
            unsigned i;
            scanf("%%u", &i);
            const WaterfallEntry &r = historyRow(historyCoarse, i);
            printf("t=%%u v=%%d\n", r.timestamp, r.rssi[0]);
        }
    }
}
"""


def cut(src, start, end):
    a = src.index(start)
    return src[a:src.index(end, a)]


def func(src, start):
    a = src.index(start)
    return src[a:src.index("\n}\n", a) + 3]


@pytest.fixture(scope="module")
def harness(tmp_path_factory):
    if not shutil.which("g++"):
        pytest.skip("no g++")

    src = sketch_text()

    defines = "\n".join(
        re.search(r"^#define %s\b.*$" % n, src, re.M).group(0)
        for n in ["HISTORY_FINE_ROWS", "HISTORY_STEP_MS"])

    code = "\n".join([
        cut(src, "struct ChannelMeasurement\n{",
            "volatile uint32_t rxPackets"),
        cut(src, "struct WaterfallEntry\n{",
            "// ============================================================\n"
            "// Scanner state"),
        cut(src, "// The row written next, the oldest once the ring is full",
            "void finishSweep()"),
        func(src, "void finishSweep()"),
        func(src, "const struct HistoryRing &historyFor("),
        func(src, "uint32_t historyFirstAfter(")])

    d = tmp_path_factory.mktemp("history")
    (d / "t.cpp").write_text(HARNESS % {"defines": defines, "code": code})
    subprocess.run(["g++", "-Wall", "-Werror", "-Wno-unused-result",
                    "-fsanitize=address,undefined", "-g",
                    "-o", str(d / "t"), str(d / "t.cpp")], check=True)

    return d / "t"


def run(binary, *cmds):
    out = subprocess.run([str(binary)], input="\n".join(cmds) + "\n",
                         text=True, capture_output=True, check=True).stdout
    return [dict(kv.split("=") for kv in line.split())
            for line in out.splitlines()]


def test_one_row_per_step_whatever_the_sweep(harness):
    # Three channels sweep in 360 ms: 14 sweeps per 5 s step
    got = run(harness, "rings 2400 17280", "sweeps 1000 360 -60",
              "query 120000", "coarse 0", "coarse 1")

    q = got[0]
    assert q["fine"] == "1000" and q["sweeps"] == "1000"
    # 360 s of sweeps, one row per 5 s (the first after 5040 ms)
    assert int(q["coarse"]) == 360000 // 5040
    assert got[1] == {"t": "5040", "v": "-60"}
    assert int(got[2]["t"]) - int(got[1]["t"]) == 5040


def test_sweep_longer_than_the_step(harness):
    # All channels: a sweep of about 5.04 s, one row each
    got = run(harness, "rings 2400 17280", "sweeps 100 7000 -70",
              "query 120000")[0]

    assert got["coarse"] == "100"


def test_short_range_from_the_rows_per_sweep(harness):
    got = run(harness, "rings 2400 17280", "sweeps 5000 360 -55",
              "query 120000")[0]

    # 2400 sweeps reach back 864 s, more than 2 minutes
    assert got["rows"] == "fine"
    # Rows at or after now - 2 min: 4667 to 5000 of 360 ms
    assert int(got["count"]) - int(got["first"]) == 5000 - 4667 + 1
    assert got["v0"] == "-55" and got["v3"] == "-127"


def test_long_range_from_the_rows_per_step(harness):
    got = run(harness, "rings 2400 17280", "sweeps 5000 360 -55",
              "query 1800000")[0]

    assert got["rows"] == "coarse"
    # Everything since the start: 1800 s of sweeps, all within 30 min
    assert got["first"] == "0"
    assert int(got["count"]) == 1800000 // 5040


def test_rows_per_sweep_while_they_hold_everything(harness):
    got = run(harness, "rings 2400 17280", "sweeps 100 360 -50",
              "query 1800000")[0]

    assert got["rows"] == "fine" and got["first"] == "0"


def test_24_hours_whatever_the_plan(harness):
    # A day of 360 ms sweeps; the 17280 rows per step hold all of it
    sweeps = 24 * 3600 * 1000 // 360
    got = run(harness, "rings 2400 17280",
              "sweeps %d 360 -65" % sweeps, "query 86400000")[0]

    assert got["rows"] == "coarse"
    assert int(got["count"]) == 17142  # 86400 s / 5.04 s
    assert int(got["first"]) <= 2


def test_without_rows_per_step(harness):
    # No PSRAM: only the rows per sweep, also for long ranges
    got = run(harness, "rings 600 0", "sweeps 5000 360 -60",
              "query 28800000")[0]

    assert got["rows"] == "fine" and got["count"] == "600"
    assert got["coarse"] == "0"


def test_page_offers_24_hours(opened):
    ranges = opened.js("""[...document.querySelectorAll('#historyTime option')]
        .map(o => [o.value, o.text])""")

    assert ranges[-2:] == [["28800000", "8 h"], ["86400000", "24 h"]]
