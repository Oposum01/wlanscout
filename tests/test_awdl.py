"""AWDL (Apple Wireless Direct Link) in the station tooltip."""

import pytest

from test_flush import route_flush


AWDL = [
    {"mac": "AA:00:00:00:0A:01", "rssi": -48, "ch": 149, "frames": 37,
     "cls": 2, "ver": 0x31, "cc": "DE", "bssid": "02:00:00:00:00:03",
     "bch": 36, "sta": "02:00:00:00:01:01", "age": 3, "seen": 60},
    {"mac": "AA:00:00:00:0A:02", "rssi": -70, "ch": 6, "frames": 4,
     "cls": 1, "ver": 0x31, "cc": "", "sta": "02:00:00:00:09:09",
     "age": 9, "seen": 20},
    {"mac": "AA:00:00:00:0A:03", "rssi": -80, "ch": 44, "frames": 1,
     "cls": -1, "ver": 0, "cc": "", "age": 1, "seen": 1},
]


@pytest.fixture
def awdlview(app):
    app.api["clients"]["awdl"] = AWDL
    app.open()
    app.tab("Stations")
    app.page.evaluate("updateClients()")
    app.page.wait_for_function("() => awdlDevices.length === 3")

    return app


def station_tip(app, mac):
    return app.js("""mac => {
        const d = document.createElement('div');
        d.innerHTML = clientTooltip(stations.find(c => c.mac === mac));
        return d.innerText;
    }""", mac)


def test_station_tooltip(awdlview):
    assert "AWDLiPhone, iPad or Watch (iOS, watchOS) · AWDL address " \
        "AA:00:00:00:0A:01" in station_tip(awdlview, "02:00:00:00:01:01")

    # No AWDL sender names this one
    assert "AWDL" not in station_tip(awdlview, "02:00:00:00:01:02")


def probe_rows(app):
    return app.js("""[...document.querySelectorAll('#clientTableProbe tr')]
        .map(r => [r.cells[0].innerText, r.cells[2].innerText])""")


def test_no_table_of_its_own(awdlview):
    assert awdlview.js("document.querySelectorAll('.subTab').length") == 3


def test_senders_without_a_station_get_a_row(awdlview):
    # 0A:01 names a station in the table, its line is in that tooltip
    rows = probe_rows(awdlview)

    assert ["AA:00:00:00:0A:02", "AWDL only"] in rows
    assert ["AA:00:00:00:0A:03", "AWDL only"] in rows
    assert not [r for r in rows if r[0] == "AA:00:00:00:0A:01"]

    assert awdlview.js("document.getElementById('clientProbeInfo')"
                       ".innerText").endswith(", 2 of them over AWDL only")


def awdl_only_tip(app, mac):
    return app.js("""mac => {
        const d = document.createElement('div');
        d.innerHTML = clientTooltip(awdlOnlyStations()
            .find(c => c.mac === mac));
        return d.innerText;
    }""", mac)


def test_awdl_only_tooltip(awdlview):
    text = awdl_only_tip(awdlview, "AA:00:00:00:0A:02")

    assert text.startswith("AA:00:00:00:0A:02 AWDL address")
    assert "Only heard over AWDL" in text
    assert "DeviceMac (macOS)" in text
    assert "Version3.1" in text
    assert "Station02:00:00:00:09:09 (not seen)" in text
    assert "Frames4 AWDL frames, first 20 s ago" in text

    # Class and version not sent
    text = awdl_only_tip(awdlview, "AA:00:00:00:0A:03")

    assert "DeviceApple, class not sent" in text
    assert "Version" not in text


def test_awdl_only_on_no_network(app):
    """An iPhone on no Wi-Fi network: zero BSSID, its AWDL address
    given as its own."""
    app.api["clients"]["awdl"] = [
        {"mac": "AA:00:00:00:0A:09", "rssi": -90, "ch": 6, "frames": 1,
         "cls": 2, "ver": 0xA0, "cc": "DE", "bssid": "00:00:00:00:00:00",
         "bch": 116, "sta": "AA:00:00:00:0A:09", "age": 5, "seen": 5}]
    app.open()
    app.tab("Stations")
    app.page.evaluate("updateClients()")
    app.page.wait_for_function("() => awdlDevices.length === 1")

    text = awdl_only_tip(app, "AA:00:00:00:0A:09")

    assert "Version10.0" in text
    assert "Wi-Fion no network" in text
    assert "CountryDE" in text

    # Its own address is no station to point to
    assert "Station" not in text
    assert ["AA:00:00:00:0A:09", "AWDL only"] in probe_rows(app)


def test_awdl_only_rows_follow_the_filter(awdlview):
    awdlview.page.fill("#stationFilter", "macos")
    awdlview.page.wait_for_function("() => stationFilterText === 'macos'")

    assert [r[0] for r in probe_rows(awdlview)] == ["AA:00:00:00:0A:02"]


@pytest.mark.parametrize("cls, text", [
    (1, "Mac (macOS)"),
    (2, "iPhone, iPad or Watch (iOS, watchOS)"),
    (8, "Apple TV (tvOS)"),
    (5, "Apple, class 5"),
    (-1, "Apple, class not sent"),
])
def test_class_text(opened, cls, text):
    assert opened.js("c => awdlClassText(c)", cls) == text


@pytest.mark.parametrize("term", ["awdl", "iphone", "apple"])
def test_station_filter(awdlview, term):
    awdlview.page.fill("#stationFilter", term)
    awdlview.page.wait_for_timeout(100)

    assert awdlview.js("filteredStations().map(c => c.mac)") == \
        ["02:00:00:00:01:01"]


def test_flush_empties_it(awdlview):
    route_flush(awdlview, "clients", {"clients": []})

    awdlview.page.once("dialog", lambda d: d.accept())
    awdlview.page.locator("#clientView .flushBtn").click()
    awdlview.page.wait_for_function("() => stations.length === 0")

    assert awdlview.js("awdlDevices.length") == 0


def test_older_firmware(opened):
    """Without the awdl list there is no AWDL line."""
    opened.tab("Stations")
    opened.page.evaluate("updateClients()")
    opened.page.wait_for_function("() => stations.length >= 2")

    assert opened.js("awdlDevices.length") == 0
    assert "AWDL" not in station_tip(opened, "02:00:00:00:01:01")
