"""The heap snapshot of the heap probe: used blocks counted by size
and the "[heapsnap]" line with what grew against the baseline.
Compiled on the PC from the sketch with a made-up heap."""

import pathlib
import shutil
import subprocess
import sys

import pytest

from sketch import sketch_text

ROOT = pathlib.Path(__file__).resolve().parent.parent

sys.path.insert(0, str(ROOT / "tools"))
import check_against_tshark as tshark  # noqa: E402

HEAD = r"""
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstddef>

struct walker_heap_into_t { intptr_t start, end; };
struct walker_block_info_t { void *ptr; size_t size; bool used; };

struct { void println(const char *s) { printf("%s\n", s); } } Serial;
"""

MAIN = r"""
static void block(HeapCensus &c, size_t size, bool used)
{
    heapSnapCount({0, 0}, {nullptr, size, used}, &c);
}

int main()
{
    // Baseline: a few small blocks and one large
    for (int i = 0; i < 10; i++)
        block(heapSnap.base, 100, true);
    block(heapSnap.base, 20000, true);
    block(heapSnap.base, 50000, false);         // free, not counted

    heapSnap.census = heapSnap.base;

    // The dip: 28 receive buffers of 1.6 kB and one small block more
    for (int i = 0; i < 28; i++)
        block(heapSnap.census, 1664, true);
    block(heapSnap.census, 100, true);

    heapSnap.freeNow  = 4300;
    heapSnap.channel    = 40;
    heapSnap.mgmtPerSec = 120;
    heapSnap.dataPerSec = 3400;
    heapSnap.mgmtQueue  = 3;
    heapSnap.dataQueue  = 63;
    heapSnap.cbMaxUs    = 850;
    heapSnap.ctrlPerSec  = 5200;
    heapSnap.fcsPerSec   = 310;
    heapSnap.cbUsPerSec  = 41000;
    heapSnap.switchAgoMs = 17;
    heapSnap.stalePerSec = 90;
    heapSnap.staleMaxMs  = 12;
    heapSnap.maxBlock = 3072;
    heapSnap.atMillis = 3723000;
    heapSnap.baseFree = 80700;
    strcpy(heapSnap.label, "idle");
    strcpy(heapSnap.ready, "wifi tiT");
    heapSnap.fresh = true;

    printf("base %u %u %u\n", heapSnap.base.count[0],
           heapSnap.base.count[6], heapSnap.base.bytes[6]);

    heapSnapPrint();
    heapSnapPrint();            // printed once only
    return 0;
}
"""


@pytest.fixture(scope="module")
def output(tmp_path_factory):
    if not shutil.which("g++"):
        pytest.skip("no g++")

    src = sketch_text()
    parts = [HEAD]

    for start in ("#define HEAP_SNAP_BUCKETS",
                  "static const uint32_t heapSnapLimit[",
                  "static const char *const heapSnapName[",
                  "struct HeapCensus",
                  "struct HeapSnapRecord"):
        parts.append(tshark.cut(src, start))

    parts.append("static HeapSnapRecord heapSnap = {};")
    parts.append(tshark.cut(src, "static bool heapSnapCount("))
    parts.append(tshark.cut(src, "static void heapSnapPrint("))

    d = tmp_path_factory.mktemp("heapsnap")
    (d / "t.cpp").write_text("\n".join(parts) + MAIN)
    subprocess.run(["g++", "-O1", "-w", "-o", str(d / "t"), str(d / "t.cpp")],
                   check=True)

    return subprocess.run([str(d / "t")], capture_output=True, text=True,
                          check=True).stdout


def test_free_blocks_are_not_counted(output):
    assert output.startswith("base 10 1 20000\n")


def test_line_names_what_grew(output):
    line = [l for l in output.splitlines() if l.startswith("[heapsnap]")]

    assert len(line) == 1
    line = line[0]

    assert "free 4.2 kB  maxblock 3.0 kB  at 01:02:03" in line
    assert ("label 'idle'  ch 40  rx/s mgmt 120 data 3400 ctrl 5200 fcs 310  "
            "queues 3 63  cb max 850 us, 41000 us/s  switch 17 ms ago, "
            "stale 90/s up to 12 ms after  vs 78.8 kB free:") in line
    assert "vs 78.8 kB free:" in line
    assert "<=1.8k +28 (+45.5 kB)" in line
    assert line.endswith("ready: wifi tiT")


def test_small_growth_left_out(output):
    # One block of 100 bytes more is below the 1 kB that is shown
    assert "<=128" not in output


def test_probe_above_the_wifi_task():
    src = sketch_text()
    start = tshark.cut(src, "static void startHeapProbe(")

    assert '"heapprobe", 4096, nullptr,\n                configMAX_PRIORITIES - 1' in start


def test_flash_writes_timed():
    """Every write to the flash in loop() says how long it took and
    where the heap's low stood before and after."""
    src = sketch_text()

    for start, name in (("void catalogSave(", "catalog save"),
                        ("void catalogWriteFrame(", "catalog frame"),
                        ("void logPersist(", "event log entry"),
                        ("void logRewrite(", "event log rewrite"),
                        ("bool takeSnapshot(", "snapshot"),
                        ("void saveSettings(", "settings (NVS)")):
        assert 'FLASH_TIMED("%s");' % name in tshark.cut(src, start)

    timer = src[src.index("struct FlashTimer"):src.index("#define FLASH_TIMED")]
    assert '"[flash] %s %lu ms, low %.1f -> %.1f kB\\r\\n"' in timer


def test_snapshot_relative_to_the_baseline():
    """30 kB below what the baseline had free, whatever the board:
    the XIAO without Ethernet has 25 kB more to begin with."""
    src = sketch_text()
    loop = tshark.cut(src, "static void heapSnapLoop(")

    assert "#define HEAP_SNAP_DROP     (30 * 1024)" in src
    assert "freeNow + HEAP_SNAP_DROP >= heapSnapBaseFree" in loop
    assert "HEAP_SNAP_BELOW" not in src


def test_callback_counts_and_times_itself():
    src = sketch_text()
    frame = tshark.cut(src, "void IRAM_ATTR wifiPromiscuousFrame(")
    head = frame[:frame.index("Drop frames that failed the FCS")]

    assert "cbMgmtFrames += 1;" in head and "cbDataFrames += 1;" in head

    cb = tshark.cut(src, "void IRAM_ATTR wifiPromiscuousCallback(")
    assert "wifiPromiscuousFrame(buf, type);" in cb
    assert "if (us > cbMaxUs)" in cb


def test_everything_the_driver_got_counted():
    """Test build: control frames and FCS failures reach the callback,
    which counts them and goes no further with a control frame, so the
    channel load stays as it was; a frame from before the last switch
    is counted with how long after it came."""
    src = sketch_text()
    frame = tshark.cut(src, "void IRAM_ATTR wifiPromiscuousFrame(")

    assert "cbCtrlFrames += 1;" in frame
    assert "pkt->rx_ctrl.channel != cbChannel" in frame
    ctrl_out = frame.index("if (type == WIFI_PKT_CTRL)\n        return;")
    assert frame.index("fcsErrors += 1;") < ctrl_out < frame.index("rxPackets += 1;")

    switch = tshark.cut(src, "void switchChannel(")
    assert "cbSwitchUs = esp_timer_get_time();" in switch

    flt = tshark.cut(src, "void applyPromiscuousFilter(")
    assert "WIFI_PROMIS_FILTER_MASK_FCSFAIL" in flt


def test_rx_line_every_15_seconds():
    """Test build: a "[rx]" line after each "[heap]" line with what the
    callback got per second, including both reception states the C5
    reports, so the counts are there without waiting for a dip."""
    src = sketch_text()
    loop = src[src.index("lastHeapLog = millis();"):]
    loop = loop[:loop.index("heapSnapPrint();")]
    assert "heapProbeRxLine();" in loop

    line = tshark.cut(src, "static void heapProbeRxLine(")
    assert '"[rx] per s  mgmt %lu data %lu ctrl %lu  fcs %lu "' in line

    frame = tshark.cut(src, "void IRAM_ATTR wifiPromiscuousFrame(")
    assert frame.index("cbRxEndBad += 1;") < frame.index("fcsErrors += 1;")


def test_receive_buffers_capped():
    """The driver is started before WiFi.mode() does it, with 16
    dynamic receive buffers instead of Arduino's 32, which caps the
    heap dips under bursts of traffic."""
    src = sketch_text()
    assert "#define WIFI_RX_BUFFERS 16" in src
    assert "#define HEAP_PROBE_RX_ALL 0" in src

    setup = tshark.cut(src, "void setupWiFiScanner(")
    init = setup.index("cfg.dynamic_rx_buf_num = WIFI_RX_BUFFERS;")
    assert "#if WIFI_RX_BUFFERS" not in setup
    assert init < setup.index("esp_wifi_init(&cfg);") < setup.index(
        "WiFi.mode(WIFI_STA);")


def test_one_channel_not_retuned():
    """With one channel selected the scanner does not set the same
    channel again after every dwell."""
    src = sketch_text()
    hop = src[src.index("bool sweepFinished = (next == firstSelected);"):]
    hop = hop[:hop.index("lastChannelSwitch = now;")]
    assert "if (!sameChannel)\n        switchChannel(currentChannelIndex);" in hop


def test_all_data_frames_reach_callback():
    """The filter lets every data frame through, aggregated or not: the
    stations and their access points are found from them."""
    src = sketch_text()
    assert "WIFI_AMPDU_RX" not in src
    assert "WIFI_DATA_MPDU_ONLY" not in src

    flt = tshark.cut(src, "void applyPromiscuousFilter(")
    assert ("WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA;"
            in flt)
