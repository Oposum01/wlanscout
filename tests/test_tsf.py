"""tsfWatch() of the firmware: when the beacon timestamp counts as a
restart of the AP. Compiled on the PC from the sketch."""

import pathlib
import shutil
import subprocess

import pytest

ROOT = pathlib.Path(__file__).resolve().parent.parent
SKETCH = ROOT / "source" / "wlanscout" / "wlanscout.ino"

HARNESS = r"""
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>

enum { CFG_RESTART, CFG_CLONE };

struct WiFiNetwork
{
    uint64_t tsf;
    uint32_t tsfAt;
    uint8_t  tsfResets;
    uint32_t tsfResetAt;
    int64_t  clkOff[2];
    uint16_t clkHits[2];
    uint16_t clkSwitches;
    uint8_t  clkLast;
    bool     clkReported;
};

uint64_t mgmtFrameUs;

void cfgRecord(WiFiNetwork &, uint8_t field, const char *from, const char *)
{
    printf("%%s %%s\n", field == CFG_RESTART ? "restart" : "clone", from);
}

%s

%s

// Lines of "<capture time µs> <tsf µs>"
int main()
{
    WiFiNetwork n;
    unsigned long long at, tsf;

    memset(&n, 0, sizeof(n));

    while (scanf("%%llu %%llu", &at, &tsf) == 2)
    {
        mgmtFrameUs = at;
        tsfWatch(n, tsf);
    }

    printf("resets %%u\n", n.tsfResets);
}
"""


def cut(src, start, end):
    a = src.index(start)
    return src[a:src.index(end, a)]


@pytest.fixture(scope="module")
def tsfwatch(tmp_path_factory):
    if not shutil.which("g++"):
        pytest.skip("no g++")

    src = SKETCH.read_text(encoding="utf-8")
    clock = cut(src, "// Two devices clocking as one",
                "// After a beacon: the gap")
    watch = cut(src, "// The timestamp of a beacon or probe response",
                "\n}\n") + "\n}\n"

    d = tmp_path_factory.mktemp("tsfwatch")
    (d / "t.cpp").write_text(HARNESS % (clock, watch))
    subprocess.run(["g++", "-o", str(d / "t"), str(d / "t.cpp")],
                   check=True)

    return d / "t"


def run(binary, frames):
    lines = "".join("%d %d\n" % f for f in frames)
    return subprocess.run([str(binary)], input=lines, text=True,
                          capture_output=True, check=True).stdout


UP = 800_000_000          # the AP has been up 800 s
S = 1_000_000


def beacons(start, count, step=10 * S):
    return [(start + i * step, UP + start + i * step) for i in range(count)]


def test_running_timer_is_no_restart(tsfwatch):
    assert run(tsfwatch, beacons(5 * S, 20)) == "resets 0\n"


def test_timer_back_is_a_restart(tsfwatch):
    frames = beacons(5 * S, 5)
    frames.append((60 * S, 2 * S))      # up again for 2 s
    frames += [(70 * S, 12 * S), (80 * S, 22 * S)]

    assert run(tsfwatch, frames) == "restart 860\nresets 1\n"


def test_probe_responses_with_timestamp_0(tsfwatch):
    # As an OpenWrt AP sends them: 0 in every probe response,
    # the timer running in the beacons
    frames = []

    for t, tsf in beacons(5 * S, 30):
        frames.append((t, tsf))
        frames.append((t + 3 * S, 0))

    assert run(tsfwatch, frames) == "resets 0\n"
