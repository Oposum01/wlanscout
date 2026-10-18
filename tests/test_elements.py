"""The smaller AP elements: Quiet, Measurement Pilot, Admission
Capacity, WMM ACM, beacon protection, Advertisement Protocol, AP
name, NAN clusters."""

import pytest


def tooltip_text(app, ssid):
    return app.js("""ssid => {
        const d = document.createElement('div');
        d.innerHTML = networkTooltip(networks.find(n => n.ssid === ssid));
        return d.innerText;
    }""", ssid)


def matches(app, text):
    return app.js("""t => networks
        .filter(n => { filterText = t; return matchesFilter(n); })
        .map(n => n.bssid)""", text)


def test_quiet(opened):
    assert opened.js("quietText({quiet: [200, 30, 20], bi: 100})") \
        == "31 ms every 200 beacons (≈ 20 s), 20 TU after the beacon"
    assert opened.js("quietText({quiet: [0, 10, 0], bi: 100})") \
        == "10 ms once, 0 TU after the beacon"
    assert opened.js("quietText({})") == ""


@pytest.mark.parametrize("hex_body, text", [
    # Aruba: video and voice, both above a second
    ("000C68F568F5", "video, voice — above 1 s/s, not limited"),
    # Voice only, 3125 × 32 µs = 100 ms
    ("0008350C", "voice 100 ms/s"),
    # Mixed
    ("000C68F5350C", "video not limited, voice 100 ms/s"),
    # UP 6 (bit 6)
    ("4000E803", "UP6 32 ms/s"),
    ("", ""),
])
def test_admission_capacity(opened, hex_body, text):
    assert opened.js("h => aacText({aac: h})", hex_body) == text


def test_acm_note_only_when_set(opened):
    notes = "n => networkNotes(n).map(x => x[1]).join(' | ')"

    assert "Admission control" not in opened.js(notes, {"acm": 0})
    assert "Admission control required for voice, video" in \
        opened.js(notes, {"acm": 12})


def test_tooltip_rows(opened):
    text = tooltip_text(opened, "Hall")

    assert "AP name: Hall-3" in text
    assert "beacon protection" in text
    assert "ANQP" in text
    assert "measurement pilot: none (interval 0)" in text
    assert "31 ms every 200 beacons" in text
    assert "video, voice — above 1 s/s, not limited" in text
    assert "Manufacturer: Aruba (HPE)" in text
    # No blue note for beacon protection
    assert opened.js("""networkNotes(networks.find(n => n.ssid === 'Hall'))
        .filter(x => /beacon prot/i.test(x[1])).length""") == 0


def test_mpt_interval(opened):
    rows = "n => { const d = document.createElement('div'); " \
           "d.innerHTML = networkTooltip(Object.assign({}, networks[0], n)); " \
           "return d.innerText; }"

    assert "measurement pilot every 5 TU" in opened.js(rows, {"mpt": 5})


def test_nan_cluster(opened):
    nan = opened.js("networks.find(n => n.bssid === '50:6F:9A:01:CD:8F')")

    assert opened.js("n => cellText(n, 'ssid')", nan) == \
        "NAN cluster (Wi-Fi Aware)"
    assert tooltip_text(opened, "<hidden>").startswith(
        "NAN cluster (Wi-Fi Aware)50:6F:9A:01:CD:8F")
    assert opened.js("isNan({bssid: '50:6F:9A:02:00:00'})") is False


def test_nan_element(opened):
    text = tooltip_text(opened, "<hidden>")

    assert "master preference 128, random factor 37 \u00b7 anchor master " \
        "2 hops away (preference 254, random factor 9)" in text

    nan = "n => nanText({nan: n})"

    assert opened.js(nan, [255, 3, 255, 3, 0]) == \
        "master preference 255, random factor 3 \u00b7 is the anchor master"
    assert opened.js(nan, [0, 5, -1, 0, 0]) == \
        "master preference 0 (not master capable), random factor 5"
    assert opened.js(nan, [-1, 0, 200, 1, 1]) == \
        "anchor master 1 hop away (preference 200, random factor 1)"

    # Without the element, or from older firmware: no line
    assert opened.js("nanText({})") == ""
    assert "NAN" not in opened.js(
        "n => { const d = document.createElement('div'); "
        "d.innerHTML = networkTooltip(n); return d.innerText; }",
        opened.js("networks.find(n => n.ssid === 'Plain')"))


@pytest.mark.parametrize("term, bssid", [
    ("aruba", "02:00:00:00:00:04"),
    ("beacon protection", "02:00:00:00:00:04"),
    ("hall-3", "02:00:00:00:00:04"),
    ("anqp", "02:00:00:00:00:04"),
    ("nan cluster", "50:6F:9A:01:CD:8F"),
    ("anchor master", "50:6F:9A:01:CD:8F"),
])
def test_filter(opened, term, bssid):
    assert matches(opened, term) == [bssid]


def test_override(opened):
    text = "SAE, SAE-EXT-KEY \u00b7 GCMP-256, CCMP-128 \u00b7 PMF required \u00b7 H2E"

    assert opened.js("overrideText(networks.find(n => n.ssid === 'Wifi7'))") == text
    assert text in tooltip_text(opened, "Wifi7")


def test_wifi_direct_and_printer(opened):
    text = tooltip_text(opened, "DIRECT-xx-HP Printer")

    assert "group owner, persistent group, cross connection, " \
        "concurrent operation \u00b7 \u201cLobby printer\u201d \u00b7 " \
        "Miracast sink, available" in text
    assert "DeskJet 2600 series (2630)" in text


def test_printer_never_shows_a_serial(opened):
    # The firmware sends model and number only; nothing else is read
    assert opened.js("printerText({hp: ['Color Laser MFP 178 179', '480']})") \
        == "Color Laser MFP 178 179 (480)"
    assert opened.js("printerText({hp: ['Photosmart 6520 series', '6520']})") \
        == "Photosmart 6520 series"


def test_mbo(opened):
    assert opened.js("mboText(networks.find(n => n.ssid === 'Wifi7'))") == \
        "MBO \u00b7 cellular data aware \u00b7 OCE \u00b7 " \
        "non-OCE AP on the channel \u00b7 avoid channel 52, 56 \u00b7 " \
        "no new associations"


def test_notes_for_mbo_and_cross_connection(opened):
    notes = """ssid => networkNotes(networks.find(n => n.ssid === ssid))
        .map(x => [x[0], x[1]])"""

    wifi7 = opened.js(notes, "Wifi7")
    printer = opened.js(notes, "DIRECT-xx-HP Printer")

    assert ["#d08080", "AP refuses new associations (MBO): maximum "
            "number of stations reached"] in wifi7
    assert any("cross connection" in t for c, t in printer if c == "#c8a050")


@pytest.mark.parametrize("term, ssid", [
    ("miracast", "DIRECT-xx-HP Printer"),
    ("deskjet", "DIRECT-xx-HP Printer"),
    ("lobby printer", "DIRECT-xx-HP Printer"),
    ("sae-ext-key", "Wifi7"),
    ("oce", "Wifi7"),
])
def test_filter_new(opened, term, ssid):
    names = opened.js("""t => networks
        .filter(n => { filterText = t; return matchesFilter(n); })
        .map(n => n.ssid)""", term)

    assert ssid in names


@pytest.mark.parametrize("omn, text", [
    (0x00, "20 MHz, 1 stream"),
    (0x11, "40 MHz, 2 streams"),
    (0x32, "80 MHz, 4 streams"),
    (0x16, "160 MHz, 2 streams"),
])
def test_operating_mode(opened, omn, text):
    assert opened.js("v => omnText({omn: v})", omn) == text


def test_operating_mode_and_wps_name_in_tooltip(opened):
    text = tooltip_text(opened, "My SEAT 0001")

    assert "20 MHz, 1 stream" in text
    assert "BMW 00001" in text
    assert opened.js("omnText({})") == ""


def test_filter_wps_name(opened):
    assert matches(opened, "bmw 00001") == ["02:00:00:00:00:01"]


def test_vendor_firmware_version(opened):
    opened.js("""networks.find(n => n.ssid === 'Hall').vfw =
        'firmware-test-2026-01-1-g0000000'""")

    text = tooltip_text(opened, "Hall")

    # Under the AP name
    assert "AP name: Hall-3Firmware: firmware-test-2026-01-1-g0000000" \
        in text
    assert matches(opened, "g0000000") == \
        opened.js("networks.filter(n => n.ssid === 'Hall').map(n => n.bssid)")

    # Nothing sent, no line
    assert "Firmware:" not in tooltip_text(opened, "<hidden>")
