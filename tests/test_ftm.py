"""Fine Timing Measurement: ftmElementsOf() of the firmware compiled on
the PC, and in the page the FTM section of a BSS with what it granted,
its position and address, the station's row, the note and `ranging`."""

import pathlib
import shutil
import subprocess

import pytest

ROOT = pathlib.Path(__file__).resolve().parent.parent
SKETCH = ROOT / "source" / "wlanscout" / "wlanscout.ino"

HARNESS = r"""
#include <cstdio>
#include <cstdint>
#include <cstring>

%s
%s

static int hexval(char c)
{
    return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10;
}

int main()
{
    static char hex[4000];
    static uint8_t ies[2000];
    int n = 0;

    if (!fgets(hex, sizeof(hex), stdin))
        return 1;

    for (int i = 0; hex[i] > ' ' && hex[i + 1] > ' '; i += 2)
        ies[n++] = hexval(hex[i]) << 4 | hexval(hex[i + 1]);

    FtmInfo f;
    memset(&f, 0, sizeof(f));
    ftmElementsOf(ies, n, f);

    printf("par %%d ", f.paramSeen);
    for (int i = 0; f.paramSeen && i < 9; i++) printf("%%02x", f.param[i]);
    printf("\nask %%d %%d\nlci %%d ", f.askLci, f.askCivic, f.lciSeen);
    for (int i = 0; f.lciSeen && i < 16; i++) printf("%%02x", f.lci[i]);
    printf("\nciv ");
    for (int i = 0; i < f.civicLen; i++) printf("%%02x", f.civic[i]);
    printf("\n");
}
"""


def lci(lat, lon, alt, alttype=1):
    def tw(x, bits, frac):
        return int(round(x * 2 ** frac)) & ((1 << bits) - 1)

    v = (21 | tw(lat, 34, 25) << 6 | 21 << 40 | tw(lon, 34, 25) << 46 |
         alttype << 80 | 10 << 84 | tw(alt, 30, 8) << 90 | 1 << 120 |
         1 << 126)
    return v.to_bytes(16, "little")


LCI = lci(50.49512, -12.13780, 372.5)
CIVIC = (b"DE" + bytes([3, 6]) + b"Plauen" + bytes([34, 7]) + b"Hauptst" +
         bytes([19, 2]) + b"12" + bytes([27, 1]) + b"2")
PARAM = bytes([1, 0x21, 0x0A, 0, 0, 0x44, 13 << 2, 20, 0])


def report_lci(mode=0):
    return bytes([39, 3 + 2 + 16, 1, mode, 8, 0, 16]) + LCI


def report_civic():
    return bytes([39, 4 + 2 + len(CIVIC), 2, 0, 11, 0, 0, len(CIVIC)]) + CIVIC


@pytest.fixture(scope="module")
def ftmelements(tmp_path_factory):
    if not shutil.which("g++"):
        pytest.skip("no g++")

    src = SKETCH.read_text(encoding="utf-8")
    a = src.index("struct FtmInfo\n{")
    struct = src[a:src.index("\n};", a) + 3]
    a = src.index("void ftmElementsOf(")
    func = src[a:src.index("\n}\n", a) + 3]

    d = tmp_path_factory.mktemp("ftm")
    (d / "t.cpp").write_text(HARNESS % (struct, func))
    subprocess.run(["g++", "-o", str(d / "t"), str(d / "t.cpp")], check=True)

    return d / "t"


def elements(binary, data):
    out = subprocess.run([str(binary)], input=data.hex() + "\n", text=True,
                         capture_output=True, check=True).stdout
    return dict(line.split(" ", 1) for line in out.splitlines())


def test_request_asks_for_position_and_address(ftmelements):
    req = (bytes([38, 3, 1, 0, 8]) + bytes([38, 3, 2, 0, 11]) +
           bytes([206, 9]) + PARAM)
    got = elements(ftmelements, req)

    assert got["ask"] == "1 1"
    assert got["par"] == "1 " + PARAM.hex()


def test_responder_reports(ftmelements):
    got = elements(ftmelements, bytes([206, 9]) + PARAM + report_lci() +
                   report_civic())

    assert got["lci"] == "1 " + LCI.hex()
    assert got["civ"] == CIVIC.hex()


def test_refused_report_is_left_out(ftmelements):
    # Mode bit 2: refused
    assert elements(ftmelements, report_lci(mode=4))["lci"] == "0 "


def tip(app, extra):
    return app.js("""o => { const d = document.createElement('div');
        d.innerHTML = networkTooltip(Object.assign({}, networks[0], o));
        return d.innerText; }""", extra)


AP = {"ftm": [12, 40, ["02:00:00:00:00:01", "02:00:00:00:00:02"]],
      "ftmpar": PARAM.hex(), "ftmlci": LCI.hex(), "ftmciv": CIVIC.hex()}


def test_ftm_section(opened):
    text = tip(opened, AP)

    assert "12 requests from 2 stations, 40 FTM frames sent" in text
    assert "8 FTM frames per burst, 2 bursts, every 2 s, ASAP, VHT 80 MHz" in text
    assert "50.49512° N, 12.13780° W, 373 m (LCI, as configured)" in text
    assert "Hauptst 12, Plauen, DE, floor 2 (Location Civic)" in text


def test_position_links_to_openstreetmap(opened):
    href = opened.js("""o => { const d = document.createElement('div');
        d.innerHTML = networkTooltip(Object.assign({}, networks[0], o));
        const a = [...d.querySelectorAll('a')].find(a =>
            a.href.includes('openstreetmap'));
        return a ? a.getAttribute('href') : null; }""", AP)

    assert href == ("https://www.openstreetmap.org/?mlat=50.495120"
                    "&mlon=-12.137800#map=18/50.495120/-12.137800")


def test_no_section_without_frames(opened):
    assert "Ranging" not in tip(opened, {"ftm": None})


def test_note_and_filter(opened):
    notes = opened.js("""o => networkNotes(Object.assign({}, networks[0], o))
        .map(n => n[0] + ' ' + n[1])""", AP)

    assert any(n.startswith("#7aaed0 Gives its position and address in FTM")
               for n in notes)

    opened.js("networks[0].ftm = [1, 0, []]")
    found = opened.js("""networks.filter(n => {
        filterText = 'ranging'; return matchesFilter(n); }).length""")

    assert found == 1


def test_station_row(opened):
    opened.page.evaluate("updateClients()")
    opened.page.wait_for_function("() => stations.length > 0")

    text = opened.js("""() => { const d = document.createElement('div');
        d.innerHTML = clientTooltip(Object.assign({}, stations[0],
            {ftm: [3, 24, ['02:00:00:00:00:09']], ftmask: 1}));
        return d.innerText; }""")

    assert "ranging with 02:00:00:00:00:09: 3 requests, 24 FTM frames back, " \
        "asks for the position" in text
