"""802.11r in detail: the Mobility Domain ID and FT over the DS in the
tooltip and the filter, the amber note for a BSS in another domain than
the others of its SSID, and the domain a station asked for."""

from test_twins import add, notes


def tooltip_text(app, js_obj):
    return app.js("""o => { const d = document.createElement('div');
        d.innerHTML = networkTooltip(Object.assign({}, networks[0], o));
        return d.innerText; }""", js_obj)


def test_roaming_row(opened):
    text = tooltip_text(opened, {"r": 1, "mdid": [0x4A2B, 1]})
    assert "fast transition, mobility domain 0x4A2B, over the air and the DS" \
        in text

    text = tooltip_text(opened, {"r": 1, "mdid": [0x00AB, 0]})
    assert "mobility domain 0x00AB, over the air" in text
    assert "and the DS" not in text

    # An older sensor sends no ID
    assert "fast transition (mobility domain)" in \
        tooltip_text(opened, {"r": 1, "mdid": None})


def test_filter_by_domain(opened):
    opened.js("networks[0].r = 1; networks[0].mdid = [0x4A2B, 0]")
    found = opened.js("""networks.filter(n => {
        filterText = 'mdid:4a2b'; return matchesFilter(n); }).length""")

    assert found == 1


def test_other_domain_than_the_rest_of_the_ssid(opened):
    add(opened, [
        {"bssid": "0A:10:00:00:00:03", "ssid": "Roam", "r": 1, "mdid": [0x1111, 0]},
        {"bssid": "0A:20:00:00:00:03", "ssid": "Roam", "r": 1, "mdid": [0x1111, 0]},
        {"bssid": "0A:30:00:00:00:03", "ssid": "Roam", "r": 1, "mdid": [0x1111, 0]},
        {"bssid": "0A:40:00:00:00:03", "ssid": "Roam", "r": 1, "mdid": [0x2222, 0]}])

    odd = notes(opened, "0A:40:00:00:00:03")

    assert any("Mobility domain 0x2222 here, 0x1111 on the other 3 BSSs with "
               "802.11r of “Roam”" in n and "no fast transition" in n
               for n in odd)
    assert not any("Mobility domain" in n
                   for n in notes(opened, "0A:10:00:00:00:03"))

    # Amber
    assert opened.js("""networkNotes(networks.find(n => n.bssid ===
        '0A:40:00:00:00:03')).some(n => n[0] === '#c8a050' &&
        n[1].startsWith('Mobility domain'))""")


def test_station_domain(opened):
    opened.page.evaluate("updateClients()")
    opened.page.wait_for_function("() => stations.length > 0")

    text = opened.js("""() => { const d = document.createElement('div');
        d.innerHTML = clientTooltip(Object.assign({}, stations[0],
            {r: 1, mdid: 0x4A2B}));
        return d.innerText; }""")

    assert "fast transition, mobility domain 0x4A2B" in text
