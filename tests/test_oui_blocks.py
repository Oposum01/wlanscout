"""The vendor table with the IEEE's small blocks: an address in an
MA-M (28 bit) or MA-S/IAB (36 bit) block is named after the block's
holder, not after "IEEE Registration Authority" holding the OUI
around it, and the page says which block. oui_table.h and the
sketch's lookups are compiled on the PC."""

import pathlib
import shutil
import subprocess

import pytest

ROOT = pathlib.Path(__file__).resolve().parent.parent
SOURCE = ROOT / "source" / "wlanscout"
SKETCH = SOURCE / "wlanscout.ino"

HARNESS = r"""
#include <cstdio>
#include <cstring>
#include <cstdint>

#include "oui_table.h"

struct MloInfo { bool present; uint8_t mldMac[6]; };

%s

%s

%s

// Lines of six hex octets: the vendor, the BSSID's vendor, the size
// of the prefix each came from, the owner of the first three octets
// as a vendor element's OUI
int main()
{
    unsigned b[6];

    while (scanf("%%x:%%x:%%x:%%x:%%x:%%x", &b[0], &b[1], &b[2], &b[3],
                 &b[4], &b[5]) == 6)
    {
        uint8_t mac[6];
        MloInfo mlo = { false, { 0 } };

        for (int i = 0; i < 6; i++)
            mac[i] = (uint8_t)b[i];

        uint8_t bits = 99, bssBits = 99;
        const char *vendor = lookupVendor(mac, &bits);
        const char *bss = lookupBssidVendor(mac, mlo, &bssBits);

        printf("%%s|%%s|%%u|%%u|%%s\n", vendor, bss, bits, bssBits,
               lookupOui(mac));
    }
}
"""


@pytest.fixture(scope="module")
def lookup(tmp_path_factory):
    if not shutil.which("g++"):
        pytest.skip("no g++")

    src = SKETCH.read_text(encoding="utf-8")
    a = src.index("const char *lookupVendor(")
    vendor = src[a:src.index("\n}\n", a) + 3]
    b = src.index("const char *lookupBssidVendor(")
    bssid = src[b:src.index("\n}\n", b) + 3]
    c = src.index("const char *lookupOui(")
    oui = src[c:src.index("\n}\n", c) + 3]

    d = tmp_path_factory.mktemp("oui")
    (d / "Arduino.h").write_text("#include <cstdint>\n")
    (d / "t.cpp").write_text(HARNESS % (vendor, oui, bssid))
    subprocess.run(["g++", "-O1", "-I", str(d), "-I", str(SOURCE),
                    "-o", str(d / "t"), str(d / "t.cpp")], check=True)

    return d / "t"


def lookups(binary, *macs):
    out = subprocess.run([str(binary)], input="\n".join(macs) + "\n",
                         capture_output=True, text=True, check=True).stdout

    return [line.split("|") for line in out.splitlines()]


def vendors(binary, *macs):
    return [x[:2] for x in lookups(binary, *macs)]


def test_ma_l_as_before(lookup):
    assert vendors(lookup, "00:26:B4:EF:6B:60") == \
        [["Ford Motor Company", "Ford Motor Company"]]

    # Runs of spaces closed up: "MAGNETI MARELLI   S.E. S.p.A."
    assert vendors(lookup, "60:56:99:22:F0:4B")[0][0] == "MAGNETI MARELLI S.E."


def test_ma_m_block(lookup):
    # A car's hotspot in Flextronics' block of F8:2B:E6
    assert vendors(lookup, "F8:2B:E6:20:4A:2A")[0][0] == "Flextronics Computin"
    assert vendors(lookup, "28:F8:C6:30:00:01")[0][0] == "PANASONIC AUTOMOTIVE"


def test_ma_s_and_iab_blocks(lookup):
    assert vendors(lookup, "70:B3:D5:00:1F:FF")[0][0] == "SOREDI touch systems"
    assert vendors(lookup, "00:1B:C5:00:00:01")[0][0] == "Converging Systems"


def test_local_bssid_from_a_block(lookup):
    # The local bit cleared, the block's holder is looked up
    assert vendors(lookup, "FA:2B:E6:20:4A:2A") == \
        [["randomised (LAA)", "Flextronics Computin (local)"]]


def test_unassigned_block_keeps_the_ieee(lookup):
    # An OUI IEEE holds, a block not in the tables
    got = vendors(lookup, "F8:2B:E6:F0:00:00")[0][0]

    assert got == "IEEE Registration Au"


def test_prefix_size(lookup):
    got = lookups(lookup, "00:26:B4:EF:6B:60", "F8:2B:E6:20:4A:2A",
                  "70:B3:D5:00:1F:FF", "FA:2B:E6:20:4A:2A",
                  "02:11:22:33:44:55", "F8:2B:E6:F0:00:00")

    assert [x[2:4] for x in got] == [["24", "24"], ["28", "28"],
                                     ["36", "36"], ["0", "28"],
                                     ["0", "24"], ["24", "24"]]

    # A local BSSID whose OUI is not assigned: randomised, no prefix
    assert lookups(lookup, "06:06:49:0A:C2:70")[0][1:4] == \
        ["randomised (LAA)", "0", "0"]


def test_element_oui_is_24_bit_only(lookup):
    # A vendor element's OUI with zeros after it is no address in a
    # block: 70:B3:D5:00:0 is an MA-S block
    assert lookups(lookup, "70:B3:D5:00:00:00")[0][4] == \
        "IEEE Registration Au"
    assert lookups(lookup, "00:26:B4:00:00:00")[0][4] == "Ford Motor Company"
    assert lookups(lookup, "02:26:B4:00:00:00")[0][4] == ""


FLEX = {"bssid": "F8:2B:E6:20:4A:2A", "vendor": "Flextronics Computin",
        "vblk": 28}


def test_block_in_the_network_tooltip(opened):
    from test_tooltip import tooltip

    text = tooltip(opened, FLEX)
    assert "(Flextronics Computin \u00b7 MA-M F8:2B:E6:2)" in text

    # The local bit cleared, as the firmware looked it up
    local = tooltip(opened, dict(FLEX, bssid="FA:2B:E6:20:4A:2A",
                                 vendor="Flextronics Computin (local)"))
    assert "MA-M F8:2B:E6:2)" in local

    sas = tooltip(opened, {"bssid": "70:B3:D5:00:1F:FF",
                           "vendor": "SOREDI touch systems", "vblk": 36})
    assert "SOREDI touch systems \u00b7 MA-S 70:B3:D5:00:1" in sas

    # A plain OUI says nothing more
    assert "MA-" not in tooltip(opened, {"vendor": "Ford Motor Company"})


def test_block_in_the_station_tooltip(opened):
    text = opened.js("""() => { const d = document.createElement('div');
        d.innerHTML = clientTooltip({mac: '70:B3:D5:00:1F:FF',
            vendor: 'SOREDI touch systems', vblk: 36,
            bssid: '00:00:00:00:00:00'});
        return d.innerText; }""")

    assert "SOREDI touch systems \u00b7 MA-S 70:B3:D5:00:1" in text


def test_filter_by_block(opened):
    found = opened.js("""n => { filterText = 'ma-m';
        const plain = networks.find(x => x.ssid === 'Plain');
        const hit = matchesFilter(Object.assign({}, plain, n));
        const miss = matchesFilter(plain);
        filterText = '';
        return [hit, miss]; }""", FLEX)

    assert found == [True, False]
