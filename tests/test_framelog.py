"""The frame logs of the capture: frameLogGrow(), frameLogAppend(),
frameLogClear(), the cursor and the time-ordered merge of the export,
compiled on the PC from the firmware with a PSRAM of chosen size.

Each run of the harness reads commands from stdin and prints what the
logs hold, so the tests can drive growth, reuse of the oldest chunk,
clearing and the merge directly."""

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
#include <algorithm>
using std::min;

#define MALLOC_CAP_SPIRAM 1

%(defines)s

// PSRAM of a chosen size: allocations count against it
static size_t psramFree = 8u << 20;
static int    allocs    = 0;

size_t heap_caps_get_free_size(int) { return psramFree; }
size_t heap_caps_get_largest_free_block(int) { return psramFree; }

void *heap_caps_malloc(size_t n, int)
{
    if (n > psramFree)
        return nullptr;
    psramFree -= n;
    allocs++;
    return malloc(n);
}

void heap_caps_free(void *p)
{
    // Every chunk is FRAME_CHUNK_BYTES
    psramFree += FRAME_CHUNK_BYTES;
    allocs--;
    free(p);
}

static size_t listed = 0;
void setAllocation(const char *, size_t bytes) { listed = bytes; }

%(code)s

static MgmtFrameCopy e;

static void frame(uint64_t us, int len, int ta)
{
    memset(&e, 0, sizeof(e));
    e.us = us;
    e.length = len;
    e.rssi = -50;
    e.channel = 6;
    e.data[0] = 0x40;            // probe request
    for (int i = 0; i < 6; i++)
        e.data[10 + i] = (uint8_t)ta;
    e.data[len - 1] = (uint8_t)(us & 0xFF);
}

static void dump(FrameLog &L)
{
    FrameCursor c;
    PcapView    v;
    uint32_t    n = 0, bytes = 0;
    uint64_t    last = 0;
    bool        ordered = true, intact = true;

    frameCursorStart(c, L);
    while (frameCursorNext(c, v))
    {
        n++;
        bytes += v.length;
        if (v.us < last)
            ordered = false;
        if (v.data[v.length - 1] != (uint8_t)(v.us & 0xFF))
            intact = false;
        last = v.us;
    }

    printf("n=%%u count=%%u bytes=%%u/%%u chunks=%%u total=%%u dropped=%%u "
           "ordered=%%d intact=%%d oldest=%%llu listed=%%zu allocs=%%d\n",
           n, L.frameCount, bytes, L.frameBytes, L.chunkCount, L.framesStored,
           L.framesDropped, ordered,
           intact, (unsigned long long)frameLogOldest(L), listed, allocs);
}

int main()
{
    char cmd[16];

    beaconSlots = (BeaconSlot *)calloc(MAX_BEACON_SLOTS, sizeof(BeaconSlot));

    while (scanf("%%15s", cmd) == 1)
    {
        if (!strcmp(cmd, "psram"))
        {
            unsigned kb;
            scanf("%%u", &kb);
            psramFree = (size_t)kb * 1024;
        }
        else if (!strcmp(cmd, "fill"))
        {
            // fill LOG COUNT LEN TA: frames 1 us apart, after the last
            char log[8];
            unsigned count, len, ta;
            static uint64_t us = 1000;
            scanf("%%7s %%u %%u %%u", log, &count, &len, &ta);
            FrameLog &L = log[0] == 'r' ? ringLog : assocLog;
            for (unsigned i = 0; i < count; i++)
            {
                frame(us++, len, ta);
                frameLogAppend(L, e);
            }
        }
        else if (!strcmp(cmd, "beacon"))
        {
            // beacon SLOT US TA
            unsigned slot, ta;
            unsigned long long us;
            scanf("%%u %%llu %%u", &slot, &us, &ta);
            BeaconSlot &b = beaconSlots[slot];
            b.valid = true;
            memset(b.bssid, ta, 6);
            b.rec.us = us;
            b.rec.length = 24;
            memset(b.rec.data, 0, 24);
            memset(&b.rec.data[10], ta, 6);
            b.rec.data[23] = (uint8_t)(us & 0xFF);
        }
        else if (!strcmp(cmd, "clear"))
        {
            char log[8];
            scanf("%%7s", log);
            frameLogClear(log[0] == 'r' ? ringLog : assocLog);
        }
        else if (!strcmp(cmd, "dump"))
        {
            char log[8];
            scanf("%%7s", log);
            dump(log[0] == 'r' ? ringLog : assocLog);
        }
        else if (!strcmp(cmd, "merge"))
        {
            // merge TA (0 for all): times in export order
            unsigned ta;
            scanf("%%u", &ta);
            static PcapMerge m;
            m.haveTa = ta != 0;
            memset(m.ta, ta, 6);
            pcapMergeStart(m, true, true, true, false);
            PcapView v;
            unsigned n = 0;
            uint64_t last = 0;
            bool ordered = true;
            while (pcapMergeNext(m, v))
            {
                if (v.us < last)
                    ordered = false;
                last = v.us;
                n++;
            }
            printf("merged=%%u ordered=%%d\n", n, ordered);
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

    names = ["FRAME_CHUNK_BYTES", "FRAME_CHUNKS_MAX", "RING_CHUNKS_MAX",
             "RING_RESERVE", "ASSOC_CHUNKS_MAX", "ASSOC_RESERVE",
             "MGMT_FRAME_MAX", "MGMT_PARSE_MAX", "MAX_NETWORKS",
             "MAX_BEACON_SLOTS", "BV_CHUNKS_MAX", "BV_RESERVE", "BV_KEEP",
             "BV_ELEMS", "BV_VOLATILE", "BLE_CHUNKS_MAX", "BLE_RESERVE"]
    defines = "\n".join(
        re.search(r"^#define %s\b.*$" % n, src, re.M).group(0)
        for n in names)

    code = "\n".join([
        cut(src, "// The newest beacon of a BSSID, see BeaconSlot",
            "// ============================================================\n"
            "// Frame logs"),
        cut(src, "static_assert(RING_CHUNKS_MAX", "struct BeaconSlot\n{"),
        cut(src, "struct BeaconSlot\n{", "uint32_t beaconTotal"),
        cut(src, "struct MgmtFrameCopy\n{", "MgmtFrameCopy *mgmtQueue"),
        "void frameLogAppendBytes(struct FrameLog &log, uint64_t us, "
        "const uint8_t *data, uint16_t length, int8_t rssi, "
        "uint8_t channel, int8_t noise, uint8_t rxRate);",
        func(src, "void frameLogAppend("),
        func(src, "void frameLogAppendBytes("),
        cut(src, "// The elaborated \"struct\" is needed again",
            "// Size of one frame in the file")])

    d = tmp_path_factory.mktemp("framelog")
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


CHUNK = 64 * 1024
# A 150-octet probe request takes its 16-octet head and padding to 8
REC = (16 + 150 + 7) // 8 * 8


def test_starts_with_one_chunk_and_grows(harness):
    per_chunk = CHUNK // REC
    got = run(harness, "fill ring 1 150 1", "dump ring",
              "fill ring %d 150 1" % per_chunk, "dump ring")

    assert got[0]["chunks"] == "1" and got[0]["n"] == "1"
    assert got[1]["chunks"] == "2"
    assert got[1]["count"] == got[1]["n"] == str(per_chunk + 1)
    assert got[1]["bytes"] == "%d/%d" % ((per_chunk + 1) * 150,
                                         (per_chunk + 1) * 150)
    assert got[1]["ordered"] == got[1]["intact"] == "1"
    assert got[1]["listed"] == str(2 * CHUNK)


def test_three_times_the_frames_of_fixed_slots(harness):
    # 3.2 MB held 6000 slots of 528 octets
    chunks = 3168000 // CHUNK
    got = run(harness, "fill ring %d 150 1" % (chunks * (CHUNK // REC)),
              "dump ring")[0]

    assert int(got["n"]) >= 3 * 6000


def test_stops_at_the_reserve_and_reuses_the_oldest(harness):
    # Room for the first chunk and three more above the 1 MB reserve
    per_chunk = CHUNK // REC
    got = run(harness, "psram %d" % (1024 + 4 * 64 + 63),
              "fill ring %d 150 1" % (10 * per_chunk), "dump ring")[0]

    assert got["chunks"] == "4"
    assert got["total"] == str(10 * per_chunk)
    assert got["dropped"] == str(6 * per_chunk)
    assert got["count"] == got["n"] == str(4 * per_chunk)
    assert got["ordered"] == got["intact"] == "1"
    # The oldest frame left is the first of the seventh chunk
    assert got["oldest"] == str(1000 + 6 * per_chunk)


def test_never_past_its_own_maximum(harness):
    # The association ring takes at most ASSOC_CHUNKS_MAX chunks
    got = run(harness, "fill assoc %d 2000 2" % (30 * (CHUNK // 2016)),
              "dump assoc")[0]

    assert got["chunks"] == "24"
    assert got["ordered"] == got["intact"] == "1"


def test_full_size_frames_never_span_chunks(harness):
    got = run(harness, "fill ring 200 2304 3", "dump ring")[0]

    assert got["n"] == "200" and got["intact"] == "1"
    assert got["chunks"] == str(-(-200 // (CHUNK // (16 + 2304))))


def test_clear_gives_back_all_but_one_chunk(harness):
    got = run(harness, "fill ring 2000 150 1", "dump ring", "clear ring",
              "dump ring", "fill ring 3 150 1", "dump ring")

    assert int(got[0]["chunks"]) > 1
    assert got[1]["chunks"] == "1" and got[1]["n"] == "0"
    assert got[1]["total"] == "0" and got[1]["oldest"] == "0"
    assert got[1]["listed"] == str(CHUNK)
    # One chunk per log left: the ring's and the association ring's
    assert got[1]["allocs"] == "1"
    assert got[2]["n"] == "3" and got[2]["ordered"] == "1"


def test_merge_is_in_time_order(harness):
    got = run(harness,
              "fill ring 500 150 1", "fill assoc 20 600 2",
              "fill ring 500 150 3",
              "beacon 5 1200 4", "beacon 9 10 4", "beacon 2 1700 5",
              "merge 0", "merge 2", "merge 4", "merge 7")

    assert got[0] == {"merged": str(1000 + 20 + 3), "ordered": "1"}
    assert got[1] == {"merged": "20", "ordered": "1"}
    assert got[2] == {"merged": "2", "ordered": "1"}
    assert got[3] == {"merged": "0", "ordered": "1"}
