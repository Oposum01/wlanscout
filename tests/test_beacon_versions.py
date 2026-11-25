"""Beacon versions: beaconVersionFrame() compiled on the PC from the
firmware with a clock the test sets. The first beacon of a BSS is kept,
a lasting change of content adds a version, what changes by itself
does not, a version seen before is not stored twice."""

import re
import shutil
import subprocess

import pytest
from sketch import sketch_text

HARNESS = r"""
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>

static uint32_t clockMs = 0;
uint32_t millis() { return clockMs; }

#define MAX_BEACON_SLOTS 4

%(defines)s

%(code)s

// The log only counts what it is given
struct FrameLog { unsigned frames; } versionLog;
void frameLogAppend(FrameLog &log, const MgmtFrameCopy &) { log.frames++; }

%(versions)s

static MgmtFrameCopy entry;

static int hexval(char c)
{
    return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10;
}

int main()
{
    static BeaconTrack tracks[MAX_BEACON_SLOTS];
    beaconTracks = tracks;

    char cmd[16];
    static char hex[2 * MGMT_PARSE_MAX + 1];

    while (scanf("%%15s", cmd) == 1)
    {
        if (!strcmp(cmd, "at"))
            scanf("%%u", &clockMs);
        else if (!strcmp(cmd, "frame"))
        {
            // frame HEX: the whole frame; then the state of its BSS
            scanf("%%s", hex);
            entry.length = strlen(hex) / 2;
            for (unsigned i = 0; i < entry.length; i++)
                entry.data[i] = hexval(hex[2 * i]) << 4 | hexval(hex[2 * i + 1]);
            entry.us = (uint64_t)clockMs * 1000;

            beaconVersionFrame(entry);

            const BeaconTrack *t = bvTrackOf(&entry.data[10]);
            if (t == nullptr)
                printf("none stored=%%u\n", versionLog.frames);
            else
                printf("v=%%u cur=%%u ch=%%u vol=%%u stored=%%u\n",
                       t->versionCount, t->versions[t->current].seq,
                       t->changes, t->volatileCount, versionLog.frames);
        }
    }
}
"""


def cut(src, start, end):
    a = src.index(start)
    return src[a:src.index(end, a)]


@pytest.fixture(scope="module")
def harness(tmp_path_factory):
    if not shutil.which("g++"):
        pytest.skip("no g++")

    src = sketch_text()

    defines = "\n".join(
        re.search(r"^#define %s\b.*$" % n, src, re.M).group(0)
        for n in ["MGMT_PARSE_MAX", "BV_KEEP", "BV_ELEMS", "BV_VOLATILE",
                  "BV_VOLATILE_CHANGES", "BV_VOLATILE_MS"])

    code = "\n".join([
        cut(src, "struct MgmtFrameCopy\n{", "MgmtFrameCopy *mgmtQueue"),
        cut(src, "struct BvElem\n{", "BeaconTrack *beaconTracks = nullptr;"),
        "BeaconTrack *beaconTracks = nullptr;"])

    versions = cut(src, "static uint32_t bvFnv(", "void clearBeaconVersions()")

    d = tmp_path_factory.mktemp("bv")
    (d / "t.cpp").write_text(HARNESS % {"defines": defines, "code": code,
                                        "versions": versions})
    subprocess.run(["g++", "-Wall", "-Werror", "-Wno-unused-result",
                    "-fsanitize=address,undefined", "-g",
                    "-o", str(d / "t"), str(d / "t.cpp")], check=True)

    return d / "t"


BSSID = "02aabbccdd01"


def el(eid, body):
    return "%02x%02x%s" % (eid, len(body) // 2, body)


def beacon(*elements, bssid=BSSID, tsf=0, subtype="80", addr3=None):
    """A beacon: header, timestamp, interval 100 TU, capabilities,
    then the elements given as hex."""
    header = (subtype + "00" + "0000" + "ffffffffffff" + bssid +
              (addr3 or bssid) + "0000")
    fixed = tsf.to_bytes(8, "little").hex() + "6400" + "1104"
    return header + fixed + "".join(elements)


SSID = el(0, b"Lab".hex())
HT_OP = el(61, "24" + "05" + "00" * 20)
HT_OP_2 = el(61, "28" + "05" + "00" * 20)


def tim(count):
    return el(5, "%02x030000" % count)


def run(binary, *frames):
    """frames: (ms, hex) pairs; the state after each one"""
    cmds = []
    for ms, frame in frames:
        cmds += ["at %d" % ms, "frame " + frame]
    out = subprocess.run([str(binary)], input="\n".join(cmds) + "\n",
                         text=True, capture_output=True, check=True).stdout
    return [dict(kv.split("=") for kv in line.split() if "=" in kv)
            for line in out.splitlines()]


def test_the_first_beacon_is_kept(harness):
    got = run(harness, (0, beacon(SSID, HT_OP)))

    assert got == [{"v": "1", "cur": "1", "ch": "0", "vol": "0",
                    "stored": "1"}]


def test_what_changes_with_every_beacon_is_no_version(harness):
    # Timestamp, TIM, BSS Load
    frames = [(i * 100, beacon(SSID, tim(i % 3), el(11, "%04x00ff00" % i),
                               HT_OP, tsf=i * 102400)) for i in range(20)]

    got = run(harness, *frames)

    assert got[-1]["v"] == "1" and got[-1]["stored"] == "1"


def test_a_change_counts_once_two_beacons_agree(harness):
    got = run(harness,
              (0, beacon(SSID, HT_OP)),
              (100, beacon(SSID, HT_OP_2)),
              (200, beacon(SSID, HT_OP_2)),
              (300, beacon(SSID, HT_OP_2)))

    assert [g["v"] for g in got] == ["1", "1", "2", "2"]
    assert got[-1] == {"v": "2", "cur": "2", "ch": "1", "vol": "0",
                       "stored": "2"}


def test_a_single_odd_beacon_is_no_version(harness):
    got = run(harness,
              (0, beacon(SSID, HT_OP)),
              (100, beacon(SSID, HT_OP_2)),
              (200, beacon(SSID, HT_OP)))

    assert got[-1]["v"] == "1" and got[-1]["ch"] == "0"


def test_back_to_a_version_seen_before_stores_nothing(harness):
    got = run(harness,
              (0, beacon(SSID, HT_OP)),
              (100, beacon(SSID, HT_OP_2)), (200, beacon(SSID, HT_OP_2)),
              (300, beacon(SSID, HT_OP)), (400, beacon(SSID, HT_OP)))

    assert got[-1] == {"v": "2", "cur": "1", "ch": "2", "vol": "0",
                       "stored": "2"}


def test_an_added_element_is_a_change(harness):
    country = el(7, b"DE ".hex() + "010d14")
    got = run(harness,
              (0, beacon(SSID, HT_OP)),
              (100, beacon(SSID, country, HT_OP)),
              (200, beacon(SSID, country, HT_OP)))

    assert got[-1]["v"] == "2"


def test_countdowns_are_set_aside(harness):
    # A channel switch announced, counting down: one version for the
    # announcement, none for each count
    csa = [el(37, "01%02x%02x" % (100, c)) for c in (5, 4, 3, 2, 1)]
    frames = [(0, beacon(SSID, HT_OP))] + \
        [((i + 1) * 100, beacon(SSID, c, HT_OP)) for i, c in enumerate(csa)]

    got = run(harness, *frames)

    assert got[-1]["v"] == "2" and got[-1]["vol"] == "0"


def test_dtim_count_in_a_multiple_bssid_profile_is_set_aside(harness):
    def mbssid(dtim_count):
        profile = el(0, b"Guest".hex()) + el(85, "0103%02x" % dtim_count)
        return el(71, "03" + "00%02x%s" % (len(profile) // 2, profile))

    frames = [(i * 100, beacon(SSID, mbssid(i % 3), HT_OP))
              for i in range(10)]

    got = run(harness, *frames)

    assert got[-1]["v"] == "1" and got[-1]["vol"] == "0"


def test_a_vendor_counter_is_learned_as_volatile(harness):
    def vendor(n):
        return el(221, "001122" + "07" + "%08x" % n)

    frames = [(i * 100, beacon(SSID, HT_OP, vendor(i)))
              for i in range(12)]

    got = run(harness, *frames)

    assert {g["v"] for g in got} == {"1"}
    assert got[-1]["vol"] == "1"

    # Still a change elsewhere is seen
    got = run(harness, *frames,
              (2000, beacon(SSID, HT_OP_2, vendor(50))),
              (2100, beacon(SSID, HT_OP_2, vendor(51))))

    assert got[-1]["v"] == "2"


def test_slow_changes_are_not_volatile(harness):
    # The same element changing once a minute: versions, each time
    frames = []
    for i in range(6):
        ht = HT_OP if i % 2 == 0 else HT_OP_2
        frames += [(i * 61000, beacon(SSID, ht)),
                   (i * 61000 + 100, beacon(SSID, ht))]

    got = run(harness, *frames)

    assert got[-1]["vol"] == "0" and got[-1]["ch"] == "5"


def test_at_most_bv_keep_versions(harness):
    frames = []
    for i in range(12):
        ht = el(61, "%02x05" % i + "00" * 20)
        frames += [(i * 61000, beacon(SSID, ht)),
                   (i * 61000 + 100, beacon(SSID, ht))]

    got = run(harness, *frames)

    assert got[-1]["v"] == "8" and got[-1]["cur"] == "12"
    assert got[-1]["stored"] == "12"


def test_only_beacons_from_the_bss_itself(harness):
    got = run(harness,
              (0, beacon(SSID, subtype="50")),
              (100, beacon(SSID, addr3="02aabbccdd99")))

    assert got == [{"stored": "0"}, {"stored": "0"}]


def test_a_new_bss_takes_the_one_heard_longest_ago(harness):
    frames = [(i * 100, beacon(SSID, bssid="02aabbccdd0%d" % i))
              for i in range(1, 6)]

    got = run(harness, *frames)

    # Four places: the fifth BSS gets the first one's
    assert [g["v"] for g in got] == ["1"] * 5
    assert got[-1]["stored"] == "5"
