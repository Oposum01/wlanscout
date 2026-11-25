"""BLE capture and hits: the link layer packets written for the pcap
(bleLogAdvert()) and the hit counter (bleHitsNote()) compiled on the
PC from the firmware; the address type; the Seen most table and the
capture row in the page."""

import re
import shutil
import subprocess

import pytest
from sketch import sketch_text
from test_settings_tabs import settings

HARNESS = r"""
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>

static uint32_t clockMs = 1000;
static uint32_t epochNow = 0;
uint32_t millis() { return clockMs; }
uint32_t deviceEpoch() { return epochNow; }
int16_t tzOffsetAt(uint32_t) { return 60; }        // UTC+1
size_t strlcpy(char *d, const char *s, size_t n)
{ size_t l = strlen(s); if (n) { size_t c = l < n - 1 ? l : n - 1;
  memcpy(d, s, c); d[c] = 0; } return l; }

%(defines)s

%(code)s

// The log prints what it is given
struct FrameLog { int n; } bleLog;
void frameLogAppendBytes(FrameLog &, uint64_t, const uint8_t *data,
                         uint16_t length, int8_t rssi, uint8_t, int8_t,
                         uint8_t)
{
    printf("pdu rssi=%%d ", rssi);
    for (int i = 0; i < length; i++)
        printf("%%02x", data[i]);
    printf("\n");
}

BleHit   *bleHits      = nullptr;
uint32_t *bleHitHeard  = nullptr;
bool      bleHitsDirty = false;

%(capture)s

static int hexval(char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; }

int main()
{
    static BleHit   hits[BLE_HITS_MAX];
    static uint32_t heard[BLE_HITS_MAX];
    bleHits = hits;
    bleHitHeard = heard;

    char cmd[16], hex[200], addr[20];
    BleAdvInfo e;

    while (scanf("%%15s", cmd) == 1)
    {
        if (!strcmp(cmd, "at"))
            scanf("%%u %%u", &clockMs, &epochNow);
        else if (!strcmp(cmd, "adv"))
        {
            // adv ADDR ADDRTYPE ADVTYPE PAYLOADHEX|-
            unsigned at, vt;
            memset(&e, 0, sizeof(e));
            scanf("%%19s %%u %%u %%199s", addr, &at, &vt, hex);
            for (int i = 0; i < 6; i++)
                e.addr[i] = hexval(addr[2 * i]) << 4 | hexval(addr[2 * i + 1]);
            e.addrType = at;
            e.advType = vt;
            e.rssi = -60;
            e.company = 0xFFFF;
            if (strcmp(hex, "-"))
            {
                e.payloadLen = strlen(hex) / 2;
                for (int i = 0; i < e.payloadLen; i++)
                    e.payload[i] = hexval(hex[2 * i]) << 4 | hexval(hex[2 * i + 1]);
            }
            bleLogAdvert(e);
            bleHitsNote(e);
        }
        else if (!strcmp(cmd, "type"))
        {
            unsigned at;
            scanf("%%19s %%u", addr, &at);
            uint8_t a[6];
            for (int i = 0; i < 6; i++)
                a[i] = hexval(addr[2 * i]) << 4 | hexval(addr[2 * i + 1]);
            printf("type %%s stable=%%d\n", bleAddrType(a, at), bleStableAddr(a, at));
        }
        else if (!strcmp(cmd, "hits"))
        {
            for (int i = 0; i < BLE_HITS_MAX; i++)
            {
                if (!hits[i].used)
                    continue;
                printf("hit %%02x%%02x%%02x%%02x%%02x%%02x hits=%%u adv=%%u first=%%u last=%%u h=",
                       hits[i].addr[0], hits[i].addr[1], hits[i].addr[2],
                       hits[i].addr[3], hits[i].addr[4], hits[i].addr[5],
                       hits[i].hits, hits[i].adverts, hits[i].firstEpoch,
                       hits[i].lastEpoch);
                for (int k = 0; k < 24; k++)
                    if (hits[i].hours[k])
                        printf("%%d:%%u,", k, hits[i].hours[k]);
                printf("\n");
            }
            printf("count %%u\n", countBleHits());
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
        for n in ["BLE_ADV_MAX", "BLE_SVC_TEXT", "BLE_HIT_GAP_MS"]) + \
        "\n#define BLE_HITS_MAX 4\n"

    code = "\n".join([
        cut(src, "struct BleAdvInfo\n{", "BleAdvInfo *bleQueue"),
        cut(src, "const char *bleAddrType(", "// ----"),
        cut(src, "struct BleHit\n{", "BleHit   *bleHits")])

    capture = "\n".join([
        cut(src, "#define BLE_ACCESS_ADDRESS", "void sendBlePcap()"),
        cut(src, "static int bleHitSlot()", "void clearBleHits()")])

    d = tmp_path_factory.mktemp("ble")
    (d / "t.cpp").write_text(HARNESS % {"defines": defines, "code": code,
                                        "capture": capture})
    subprocess.run(["g++", "-Wall", "-Werror", "-Wno-unused-result",
                    "-fsanitize=address,undefined", "-g",
                    "-o", str(d / "t"), str(d / "t.cpp")], check=True)

    return d / "t"


def run(binary, *cmds):
    return subprocess.run([str(binary)], input="\n".join(cmds) + "\n",
                          text=True, capture_output=True,
                          check=True).stdout.splitlines()


PUBLIC = "a4c138001122"
STATIC = "c4aabbccddee"          # random, top bits 11
RPA = "5a1122334455"             # random, top bits 01

AA = "d6be898e"


def crc24(pdu_hex):
    """The BLE link layer CRC, written here from the spec, not the
    firmware: LFSR x^24+x^10+x^9+x^6+x^4+x^3+x+1, preset 0x555555,
    bits least significant first; as sent, low octet first."""
    state = int(format(0x555555, "024b")[::-1], 2)
    for b in bytes.fromhex(pdu_hex):
        for j in range(8):
            fb = (state ^ (b >> j)) & 1
            state >>= 1
            if fb:
                state ^= 0x800000 | 0x5A6000
    return state.to_bytes(3, "little").hex()


def ad(kind, body):
    return "%02x%02x%s" % (len(body) // 2 + 1, kind, body)


FLAGS = ad(0x01, "06")
NAME = ad(0x09, b"Watch".hex())


def test_a_pdu_as_on_the_air(harness):
    out = run(harness, "adv %s 0 0 %s" % (PUBLIC, FLAGS + NAME))

    payload = FLAGS + NAME
    # ADV_IND, TxAdd 0, length AdvA + data; AdvA reversed; the CRC
    pdu = "00%02x" % (6 + len(payload) // 2) + "221100" + "38c1a4" + payload
    assert out == ["pdu rssi=-60 " + AA + pdu + crc24(pdu)]


def test_tx_add_and_the_pdu_type(harness):
    out = run(harness, "adv %s 1 3 %s" % (STATIC, FLAGS))

    # ADV_NONCONN_IND (2) with TxAdd
    assert out[0].split()[2].startswith(AA + "42")


def test_scan_response_cut_apart(harness):
    adv = FLAGS + ad(0xFF, "4c00" + "00" * 24)       # 3 + 27 = 30 octets
    rsp = NAME + ad(0x0A, "08")                     # a scan response
    out = run(harness, "adv %s 0 0 %s" % (PUBLIC, adv + rsp))

    assert len(out) == 2
    first, second = (o.split()[2] for o in out)
    assert first[8:10] == "00" and first[:-6].endswith(adv)
    assert second[8:10] == "04" and second[:-6].endswith(rsp)
    assert first[-6:] == crc24(first[8:-6])
    assert second[-6:] == crc24(second[8:-6])


def test_address_types(harness):
    out = run(harness, "type %s 0" % PUBLIC, "type %s 1" % STATIC,
              "type %s 1" % RPA, "type 0a1122334455 1",
              "type %s 0" % RPA)

    assert out == ["type public stable=1", "type random static stable=1",
                   "type resolvable private stable=0",
                   "type non-resolvable stable=0",
                   # A public address whose top bits look like a random one's
                   "type public stable=1"]


def hits(out):
    return {l.split()[1]: dict(kv.split("=") for kv in l.split()[2:])
            for l in out if l.startswith("hit ")}


def test_a_hit_after_five_minutes_of_silence(harness):
    t0 = 1762430400              # 2025-11-06 12:00 UTC, 13 h local
    out = run(harness,
              "at 1000 %d" % t0, "adv %s 0 0 -" % PUBLIC,
              "at 60000 %d" % (t0 + 59), "adv %s 0 0 -" % PUBLIC,
              "at 290000 %d" % (t0 + 289), "adv %s 0 0 -" % PUBLIC,
              # Six minutes without it, then back: a second hit
              "at 660000 %d" % (t0 + 659), "adv %s 0 0 -" % PUBLIC,
              "hits")

    h = hits(out)[PUBLIC]
    assert h["hits"] == "2" and h["adv"] == "4"
    assert h["first"] == str(t0) and h["last"] == str(t0 + 659)
    assert h["h"] == "13:2,"


def test_rotating_addresses_are_not_counted(harness):
    out = run(harness, "adv %s 1 0 -" % RPA, "adv 0a1122334455 1 0 -",
              "adv %s 1 0 -" % STATIC, "hits")

    assert set(hits(out)) == {STATIC}


def test_no_hours_without_a_clock(harness):
    out = run(harness, "at 1000 0", "adv %s 0 0 -" % PUBLIC, "hits")

    h = hits(out)[PUBLIC]
    assert h["hits"] == "1" and h["first"] == "0" and h["h"] == ""


def test_full_the_fewest_hits_give_way(harness):
    cmds = []
    # Device 1 with three hits, devices 2..4 with one each
    for i in range(3):
        cmds += ["at %d 0" % (1000 + i * 400000), "adv a4c138000001 0 0 -"]
    for n in (2, 3, 4):
        cmds += ["adv a4c13800000%d 0 0 -" % n]
    cmds += ["adv a4c138000005 0 0 -", "hits"]

    got = hits(run(harness, *cmds))

    assert len(got) == 4
    assert got["a4c138000001"]["hits"] == "3"
    assert "a4c138000005" in got


# ---- the page ----

DEVICES = {"max": 512, "gap": 300, "devices": [
    {"addr": "A4:C1:38:00:11:22", "type": "public", "name": "Watch",
     "company": 65535, "companyName": "", "vendor": "Telink",
     "hits": 3, "adv": 40, "first": 1762430400, "last": 1762434000,
     "ago": -1, "hours": [0] * 13 + [2, 1] + [0] * 9},
    {"addr": "C4:AA:BB:CC:DD:EE", "type": "random static", "name": "",
     "company": 76, "companyName": "Apple", "vendor": "",
     "hits": 9, "adv": 300, "first": 1762400000, "last": 1762434000,
     "ago": 5, "hours": [1] * 24}]}


def hit_rows(app):
    return app.js("""[...document.querySelectorAll('#bleHitsTable tr')]
        .map(r => [...r.cells].map(c => c.innerText))""")


def test_seen_most_by_hits(opened):
    opened.js("(d) => { document.getElementById('bleHitsBox')"
              ".style.display = ''; fillBleHits(d); }", DEVICES)

    rows = hit_rows(opened)

    assert [r[0] for r in rows] == ["C4:AA:BB:CC:DD:EE", "A4:C1:38:00:11:22"]
    assert rows[0][2] == "Apple" and rows[0][6] == "now"
    assert rows[1][2] == "Watch, Telink" and rows[1][3] == "3"
    # Two hours with hits, the rest dots
    assert len(rows[1][7]) == 24 and rows[1][7].count("·") == 22
    assert rows[1][8] == "pcap"
    assert opened.js("document.querySelector('#bleHitsTable a.dl')"
                     ".getAttribute('href')") == \
        "/api/pcap?src=ble&ta=C4:AA:BB:CC:DD:EE"


def test_no_table_with_older_firmware(opened):
    # The fake sensor answers {"ok":1} for what it does not know
    opened.js("bleHitsAt = 0; updateBleHits()")
    opened.page.wait_for_timeout(300)

    assert opened.js("document.getElementById('bleHitsBox')"
                     ".style.display") == "none"


def test_table_with_firmware_that_counts(app):
    app.api["blehits"] = DEVICES
    app.open()
    app.js("bleHitsAt = 0; updateBleHits()")
    app.page.wait_for_function(
        "() => document.querySelectorAll('#bleHitsTable tr').length === 2")

    assert app.js("document.getElementById('bleHitsBox').style.display") == ""


def test_capture_row_for_ble(app):
    app.open()
    app.api["system"]["pcap"].update(
        {"ble": 7, "blebytes": 400, "blemem": 65536, "blemax": 1048576,
         "bletotal": 7, "bledropped": 0})
    settings(app, "capture")
    app.page.wait_for_function("""() => [...document.querySelectorAll(
        '#storeTable tr')].some(r => r.cells[0].textContent ===
        'BLE advertisements')""")

    row = app.js("""[...document.querySelectorAll('#storeTable tr')]
        .find(r => r.cells[0].textContent === 'BLE advertisements')
        .innerText""")
    assert "7 advertisements" in row and "download all" in row
