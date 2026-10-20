"""Configuration changes of a BSS: the CHANGES section of the
tooltip, the Δ in the hint column and the filter term."""

CHANGES = {"cfgn": 6, "cfgch": [
    [120, "channel", "36", "52"],
    [600, "security", "WPA2", "WPA2/WPA3"],
    [3700, "restart", "273600", ""],
    [4000, "BSS colour", "6", "11"]]}


def changed(app, extra):
    """The first network with the changes added, as the page shows it."""
    return app.js("""extra => {
        const n = Object.assign({}, networks[0], extra);
        const d = document.createElement('div');
        d.innerHTML = networkTooltip(n);
        const b = document.createElement('div');
        b.innerHTML = noteBadges(n);
        return [d.innerText, b.innerText, d.innerHTML];
    }""", extra)


def test_changes_section(opened):
    text, _, html = changed(opened, CHANGES)

    assert "CHANGES" in text
    assert "2 min 0 s ago" in text and "channel 36 → 52" in text
    assert "security WPA2 → WPA2/WPA3" in text
    assert "AP restarted, was up 3 d 4 h" in text
    assert "BSS colour 6 → 11" in text
    assert "2 earlier changes no longer kept, 6 since the sensor started" in text
    # The old value is struck through
    assert "<s>36</s>" in html


def test_no_section_without_changes(opened):
    text, badge, _ = changed(opened, {})

    assert "CHANGES" not in text
    assert "Δ" not in badge


def test_delta_in_the_hint_column(opened):
    _, badge, _ = changed(opened, CHANGES)

    assert "Δ" in badge


def test_filter_finds_changed_networks(opened):
    opened.js("networks[0].cfgn = 1; networks[0].cfgch = [[5, 'WPS', 'off', 'on']]")

    found = opened.js("""networks.filter(n => {
        filterText = 'changed'; return matchesFilter(n); }).length""")

    assert found == 1


def test_flapping_in_the_tooltip(opened):
    text, _, _ = changed(opened, {"cfgn": 7, "cfgch": [
        [20, "flapping", "width, standard, spatial streams", "4 switches"]]})

    assert "flapping between two configurations (width, standard, " \
        "spatial streams), 4 switches" in text


def test_changes_at_once_share_a_row(opened):
    rows = opened.js("""configChangeRows({cfgch: [
        [10, 'AKM', 'PSK', 'SAE'], [11, 'PMF', 'capable', 'required'],
        [400, 'channel', '36', '52']]})""")

    assert len(rows) == 2
    assert "AKM" in rows[0][1] and "PMF" in rows[0][1]


def test_tooltip_takes_the_longer_list_from_the_bell(opened):
    # The sensor kept four for this BSS, the bell's list has five
    opened.js("""cfgFetchAt = Date.now(); cfgList = {boot: 1, seq: 5, changes: [
        [5, 10, networks[0].bssid, 'x', 'group cipher', 'CCMP-128', 'TKIP'],
        [4, 10, networks[0].bssid, 'x', 'AKM', 'SAE', 'PSK, SAE'],
        [3, 10, networks[0].bssid, 'x', 'PMF', 'required', 'capable'],
        [2, 10, networks[0].bssid, 'x', 'security', 'WPA3', 'WPA/WPA2/WPA3'],
        [1, 10, networks[0].bssid, 'x', 'channel', '36', '40']]}""")

    text, _, _ = changed(opened, {"cfgn": 5, "cfgch": [
        [10, "group cipher", "CCMP-128", "TKIP"], [10, "AKM", "SAE", "PSK, SAE"],
        [10, "PMF", "required", "capable"],
        [10, "security", "WPA3", "WPA/WPA2/WPA3"]]})

    assert "channel 36 \u2192 40" in text
    assert "earlier" not in text


def test_wps_methods_named(opened):
    text = opened.js("""() => { const d = document.createElement('div');
        d.innerHTML = configChangeText('WPS methods', '0x0088', '0x0080');
        return d.innerText; }""")

    assert text == "WPS methods Display, Push Button → Push Button"
    assert "→ -" in opened.js(
        "configChangeText('WPS methods', '0x0080', '-')")


def test_new_fields_shown_as_sent(opened):
    text = opened.js("""() => { const d = document.createElement('div');
        d.innerHTML = configChangeText('RSNE override', 'off', 'SAE');
        return d.innerText; }""")

    assert text == "RSNE override off → SAE"
