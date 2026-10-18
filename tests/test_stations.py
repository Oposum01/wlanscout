"""The station view: its filter and what it searches."""

import pytest


@pytest.fixture
def stationview(opened):
    opened.tab("Stations")
    opened.page.evaluate("updateClients()")
    opened.page.wait_for_function("() => stations.length >= 2")

    return opened


def shown(app):
    """MACs in the probe table, the fixtures' stations only probe."""
    return app.js("""[...document.querySelectorAll('#clientTableProbe tr')]
        .map(r => r.cells[0] && r.cells[0].textContent)
        .filter(t => t && /^02:00/.test(t))""")


def type_filter(app, text):
    app.page.fill("#stationFilter", text)
    app.page.wait_for_timeout(100)


@pytest.mark.parametrize("text, macs", [
    ("", ["02:00:00:00:01:01", "02:00:00:00:01:02"]),
    ("apple", ["02:00:00:00:01:01"]),
    ("broadcom chip", ["02:00:00:00:01:01"]),
    ("samsung", ["02:00:00:00:01:02"]),
    ("qualcomm chip", ["02:00:00:00:01:02"]),
    ("home", ["02:00:00:00:01:02"]),
    ("01:02", ["02:00:00:00:01:02"]),
    ("nothing-like-this", []),
])
def test_station_filter(stationview, text, macs):
    type_filter(stationview, text)

    assert sorted(shown(stationview)) == macs


def test_station_filter_info_and_clear(stationview):
    type_filter(stationview, "apple")

    assert stationview.page.locator("#clientInfo").inner_text().startswith("1 of 2 stations")

    stationview.page.locator("#stationFilter + button").click()

    assert stationview.page.input_value("#stationFilter") == ""
    assert len(shown(stationview)) == 2


def test_beacon_name_clears_the_station_guess(opened):
    # Stations loaded before the networks: the AP name comes from
    # the station first, the beacon later must drop the hint
    text = opened.js("""() => {
        const n = networks[0];
        delete apNameCache[n.bssid];
        rememberStationApNames([{bssid: n.bssid, apssid: n.ssid}]);
        const before = !!apNameFromStation[n.bssid];
        rememberApNames();
        return [before, !!apNameFromStation[n.bssid]];
    }""")

    assert text == [True, False]


def test_default_order_by_address(opened):
    opened.page.evaluate("updateClients()")
    opened.page.wait_for_function("() => stations.length > 0")

    assert opened.js("clientSortFor(clientColumns, false)") == ["mac", 1]
    # The history starts with the newest association
    assert opened.js("clientSortFor(histColumns, true)") == ["last", -1]

    rows = opened.js("""[...document.querySelectorAll(
        '#clientTable tr, #clientTableProbe tr')]
        .map(r => r.cells[0].textContent)""")

    assert rows == sorted(rows)


def test_clicked_column_sorts_the_history_too(opened):
    opened.js("setClientSort('rssi')")

    assert opened.js("clientSortFor(histColumns, true)")[0] == "rssi"


def test_one_station_table_at_a_time(stationview):
    def visible():
        return stationview.js("""['clientTable', 'clientTableProbe',
            'assocHistTable'].filter(id =>
                document.getElementById(id).offsetParent !== null)""")

    assert visible() == ["clientTable"]

    stationview.page.locator(".subTab", has_text="Probing only").click()
    assert visible() == ["clientTableProbe"]

    stationview.page.locator(".subTab", has_text="History").click()
    assert visible() == ["assocHistTable"]


def test_tab_counts_follow_the_filter(stationview):
    def counts():
        return stationview.js("""['assocTabCount', 'probeTabCount']
            .map(id => document.getElementById(id).innerText)""")

    total = counts()

    stationview.page.fill("#stationFilter", "02:00:00:00:00:99")
    stationview.page.dispatch_event("#stationFilter", "input")

    assert counts() == ["(0)", "(0)"]
    assert total != counts()


def test_chosen_tab_is_kept(stationview):
    stationview.page.locator(".subTab", has_text="History").click()
    stationview.page.reload()
    stationview.page.wait_for_function("() => typeof networks !== 'undefined'")

    assert stationview.js("""document.getElementById('histPane')
        .classList.contains('active')""")
