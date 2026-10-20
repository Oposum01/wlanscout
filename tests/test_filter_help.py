"""The "?" beside both filters: the list of terms opens and closes, and
every term in it finds a BSS or station that has what it says."""

import pytest

# A BSS or station with what each term stands for, besides the fixture's
NETWORK = {
    "ch:36": {"channel": 36},
    "80mhz": {"bandwidth": 80},
    "2.4ghz": {"channel": 6},
    "wifi6": {"std": 6},
    "wpa3": {"sec": "WPA3"},
    "sae": {"akm": "SAE"},
    "ccmp": {"pair": "CCMP-128"},
    "pmf:required": {"mfpr": 1},
    "wps": {"wps": 1},
    "de": {"cc": "DE"},
    "2ss": {"nss": 2},
    "mesh": {"mesh": 1},
    "ibss": {"ibss": 1},
    "mlo": {"mld": "02:00:00:00:00:01"},
    "drone": {"rid": {"ua": 2, "sid": "ABC"}},
    "changed": {"cfgn": 1},
    "fastbeacons": {"bi": 100, "bgap": [40, 18, 2300]},
    "probetsf0": {"ptsf": [12, 0], "tsfup": 100},
    "dtim:3": {"dtim": [3, 0, 0]},
    "mdid:4a2b": {"r": 1, "mdid": [0x4A2B, 0]},
    "ranging": {"ftm": [1, 0, []]},
    "ftm responder": {"v": 1, "wnm": 1 << 8},
    "colour change": {"bcc": [12, 3, 1]},
    "broken suite": {"suitebad": [1, 0x000FAC04]},
    "basic rates 1": {"basicrates": [2, 4]},
    "headunit": {"voui": [["04DF69", 10, "Car Connectivity Con"]]},
    "broadcom chip": {"voui": [["001018", 2, "Broadcom"]]},
}

STATION = {
    "ch:36": {"ch": 36},
    "associated": {"assoc": 1},
    "wifi6": {"std": 6},
    "sae": {"akm": "SAE"},
    "pmf": {"pmf": 1},
    "wmm": {"wmm": 1},
    "11k": {"k": 1},
    "ft roam": {"roamsft": 1},
    "ranging": {"ftm": [1, 0, []]},
    "mlo": {"mld": "02:00:00:00:00:01"},
    "awdl": {"awdlonly": {"cls": 1}},
    "broadcom chip": {"voui": [["001018", 2, "Broadcom"]]},
}


def test_lists_name_exactly_the_tested_terms(opened):
    assert [t for t, _ in opened.js("FILTER_HELP")] == list(NETWORK)
    assert [t for t, _ in opened.js("STATION_FILTER_HELP")] == list(STATION)


@pytest.mark.parametrize("term", list(NETWORK))
def test_network_term_finds(opened, term):
    assert opened.js("""([term, extra]) => {
        const n = Object.assign({}, networks[0], extra);
        filterText = term;
        const hit = matchesFilter(n);
        filterText = '';
        return hit; }""", [term, NETWORK[term]])


@pytest.mark.parametrize("term", list(STATION))
def test_station_term_finds(opened, term):
    opened.page.evaluate("updateClients()")
    opened.page.wait_for_function("() => stations.length > 0")

    assert opened.js("""([term, extra]) => {
        const c = Object.assign({}, stations[0], extra);
        stationFilterText = term;
        const hit = stationMatches(c);
        stationFilterText = '';
        return hit; }""", [term, STATION[term]])


def test_question_mark_opens_and_closes(opened):
    box = opened.page.locator("#filterHelp")

    assert not box.is_visible()

    opened.page.locator("button.helpBtn").first.click()
    assert box.is_visible()
    assert "ranging" in box.inner_text()
    assert "Every word has to match" in box.inner_text()

    opened.page.locator("button.helpBtn").first.click()
    assert not box.is_visible()


def test_headunit_only_where_it_says(opened):
    # One word of its own: "car" alone was in "carry" of a note
    assert opened.js("""() => { filterText = 'headunit';
        const hits = networks.filter(n => !n.acc && !(n.voui || [])
            .some(v => v[0] === '04DF69')).filter(matchesFilter).length;
        filterText = ''; return hits; }""") == 0
    assert opened.js("""() => { filterText = 'headunit';
        const hit = matchesFilter(Object.assign({}, networks[0],
            {acc: 1, accname: 'MBUX'}));
        filterText = ''; return hit; }""")


def test_pmf_required_not_the_notes_about_pmf_off(opened):
    assert not opened.js("""() => { filterText = 'pmf:required';
        const hit = matchesFilter(Object.assign({}, networks[0],
            {mfpr: 0, mfpc: 0}));
        filterText = ''; return hit; }""")


def matches(app, term, extra):
    return app.js("""([term, extra]) => {
        filterText = term;
        const hit = matchesFilter(Object.assign({}, networks[0], extra));
        filterText = ''; return hit; }""", [term, extra])


def test_key_value_terms_are_exact(opened):
    assert matches(opened, "ch:1", {"channel": 1})
    assert not matches(opened, "ch:1", {"channel": 11})
    assert not matches(opened, "ch:1", {"channel": 100})
    assert matches(opened, "dtim:1", {"dtim": [1, 0, 0]})
    assert not matches(opened, "dtim:1", {"dtim": [10, 0, 0]})
    assert matches(opened, "pmf:capable", {"mfpr": 0, "mfpc": 1})
    assert matches(opened, "pmf:off", {"mfpr": 0, "mfpc": 0})
    assert not matches(opened, "pmf:off", {"mfpr": 1, "mfpc": 1})
    assert matches(opened, "mdid:0x4A2B", {"r": 1, "mdid": [0x4A2B, 0]})
    # With another word, and a piece of a MAC address stays text
    assert matches(opened, "ch:36 80mhz", {"channel": 36, "bandwidth": 80})
    assert matches(opened, "c4:xx", {"bssid": "00:83:C4:XX:00:01"})


def test_station_channel_exact(opened):
    opened.page.evaluate("updateClients()")
    opened.page.wait_for_function("() => stations.length > 0")

    assert not opened.js("""() => { stationFilterText = 'ch:1';
        const hit = stationMatches(Object.assign({}, stations[0], {ch: 11}));
        stationFilterText = ''; return hit; }""")
