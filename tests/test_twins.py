"""Evil twins: two beacon timers under one BSSID, and a BSS weaker
than the others of its SSID on other devices."""


def notes(app, bssid):
    return app.js("""b => networkNotes(networks.find(n => n.bssid === b))
        .map(n => n[1])""", bssid)


def add(app, nets):
    """Networks of one SSID, besides the fixture's."""
    app.js("""list => list.forEach(x => networks.push(Object.assign(
        {}, networks[0], {mld: undefined, mesh: 0, cc: 'DE', mfpr: 0}, x)))""",
           nets)


def test_two_clocks_under_one_bssid(opened):
    opened.js("networks[0].clocks = [40, 35, 30, 4200000000]")
    bssid = opened.js("networks[0].bssid")

    assert any("Two devices send as this BSSID" in n and "4200.0 s apart" in n
               and "alternating 30 times" in n for n in notes(opened, bssid))


def test_a_restart_is_no_twin(opened):
    # The old timer seen often, the new one a few times, one switch
    opened.js("networks[0].clocks = [300, 12, 1, 9000000]")

    assert not any("Two devices" in n
                   for n in notes(opened, opened.js("networks[0].bssid")))


def test_weaker_security_than_the_other_devices(opened):
    add(opened, [
        {"bssid": "0A:10:00:00:00:01", "ssid": "Corp", "sec": "WPA2-EAP"},
        {"bssid": "0A:20:00:00:00:01", "ssid": "Corp", "sec": "WPA2-EAP"},
        {"bssid": "0A:30:00:00:00:01", "ssid": "Corp", "sec": "WPA2-EAP"},
        {"bssid": "0A:40:00:00:00:01", "ssid": "Corp", "sec": "WPA2"}])

    weak = notes(opened, "0A:40:00:00:00:01")

    assert any("Weaker security than the other 3 BSSs of “Corp”" in n
               and "WPA2 here, WPA2-EAP there" in n for n in weak)
    assert not any("Weaker security" in n
                   for n in notes(opened, "0A:10:00:00:00:01"))


def test_country_and_pmf_outlier(opened):
    add(opened, [
        {"bssid": "0A:10:00:00:00:02", "ssid": "Shop", "sec": "WPA3", "mfpr": 1},
        {"bssid": "0A:20:00:00:00:02", "ssid": "Shop", "sec": "WPA3", "mfpr": 1},
        {"bssid": "0A:30:00:00:00:02", "ssid": "Shop", "sec": "WPA3", "mfpr": 0,
         "cc": "US"}])

    odd = notes(opened, "0A:30:00:00:00:02")

    assert any("PMF not required here" in n for n in odd)
    assert any("Country element US here, DE on the other 2 BSSs" in n
               for n in odd)


def test_same_device_and_too_few_peers_are_left_alone(opened):
    # Next to each other: one device, its own note covers that
    add(opened, [
        {"bssid": "0A:50:00:00:00:10", "ssid": "Home", "sec": "WPA2/WPA3"},
        {"bssid": "0A:50:00:00:00:11", "ssid": "Home", "sec": "WPA2"},
        {"bssid": "0A:60:00:00:00:10", "ssid": "Lab", "sec": "WPA3"},
        {"bssid": "0A:70:00:00:00:10", "ssid": "Lab", "sec": "WPA2"}])

    assert not any("Weaker security" in n
                   for n in notes(opened, "0A:50:00:00:00:11"))
    assert not any("Weaker security" in n
                   for n in notes(opened, "0A:70:00:00:00:10"))


def test_gap_row_only_when_it_says_something(opened):
    text = opened.js("""() => { const d = document.createElement('div');
        d.innerHTML = networkTooltip(Object.assign({}, networks[0],
            {bi: 100, bgap: [8, 0, 102300]}));
        return d.innerText; }""")

    assert "gaps under half" not in text
