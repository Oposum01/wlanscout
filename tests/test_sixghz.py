"""The 6 GHz list: which AP announced a BSS, also after it aged out."""


def first_network(app):
    return app.js("({bssid: networks[0].bssid, ssid: networks[0].ssid, "
                  "channel: networks[0].channel})")


def text(app, html):
    return app.js("""h => { const d = document.createElement('div');
        d.innerHTML = h; return d.textContent; }""", html)


def test_reporter_from_the_network_table(opened):
    n = first_network(opened)
    entry = {"reporter": n["bssid"], "repssid": "Old name", "repch": 99}

    cell = text(opened, opened.js("e => sixReporterCell(e)", entry))

    # The table wins while the AP is in it
    assert cell.startswith(opened.js("b => apNameFor(b)", n["bssid"]))
    assert "ch %d" % n["channel"] in cell
    assert "(gone)" not in cell


def test_reporter_kept_after_it_left_the_table(opened):
    entry = {"reporter": "02:00:00:00:66:01", "repssid": "Adventureland",
             "repch": 36, "ssid": "Six", "bssid": "02:00:00:00:66:02",
             "ch": 37, "freq": 6135, "opclass": 131, "psd": 127}

    cell = text(opened, opened.js("e => sixReporterCell(e)", entry))
    tip = text(opened, opened.js("e => sixTooltip(e)", entry))

    assert cell == "Adventureland · 5 GHz ch 36 (gone)"
    assert "Announced by Adventureland (02:00:00:00:66:01), 5 GHz ch 36" \
        in tip
    assert "no longer in the network table" in tip


def test_older_firmware_leaves_the_address(opened):
    entry = {"reporter": "02:00:00:00:66:01", "ssid": "Six",
             "bssid": "02:00:00:00:66:02", "ch": 37, "freq": 6135,
             "opclass": 131, "psd": 127}

    cell = text(opened, opened.js("e => sixReporterCell(e)", entry))

    assert cell == "02:00:00:00:66:01 (gone)"
