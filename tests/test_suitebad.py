"""A cipher or AKM suite under an OUI that does not belong in its
element (the cable routers' 00-00-00-00 in the WPA element), and a
WPA element without an AKM beside SAE."""

from test_tooltip import tooltip


def notes(app, fields):
    return app.js("n => networkNotes(Object.assign({}, networks[0], n))"
                  ".map(x => x[1]).join(' | ')", fields)


def test_zero_suite_in_wpa_is_an_amber_note(opened):
    text = notes(opened, {"suitebad": [2, 0]})

    assert "WPA element lists suite 00-00-00-00 under an OUI that " \
        "does not belong there: a firmware fault of the AP" in text

    colour = opened.js("""networkNotes(Object.assign({}, networks[0],
        {suitebad: [2, 0]})).find(x => /00-00-00-00/.test(x[1]))[0]""")

    assert colour == "#c8a050"


def test_wpa_suite_in_rsn(opened):
    text = tooltip(opened, {"suitebad": [1, 0x0050F202]})

    assert "RSN element lists suite 00-50-F2-02" in text


def test_no_note_without_the_field(opened):
    assert "under an OUI" not in notes(opened, {})


def test_filter_finds_broken_suite(app):
    for n in app.api["networks"]["networks"]:
        if n["ssid"] == "Plain":
            n["suitebad"] = [2, 0]

    app.open()

    for word in ["broken suite", "00-00-00-00"]:
        app.page.fill("#filter", word)
        app.page.wait_for_timeout(100)

        assert app.js("networks.filter(matchesFilter).map(n => n.ssid)") \
            == ["Plain"], word


def test_wpa_element_without_akm_is_a_blue_note(opened):
    found = opened.js("""networkNotes(Object.assign({}, networks[0],
        {wpanoakm: 1, sec: "WPA3"})).find(x => /without an AKM/.test(x[1]))""")

    assert found[0] == "#7aaed0"
    assert found[1] == "WPA element without an AKM: it offers neither " \
        "PSK nor 802.1X, so no station logs in over WPA; WPA3 comes " \
        "from the RSN element"

    # Only that WPA element: nothing else to point to
    text = notes(opened, {"wpanoakm": 1, "sec": "WPA"})

    assert "so no station logs in over WPA" in text
    assert "RSN element" not in text
    assert "without an AKM" not in notes(opened, {})


def test_filter_finds_wpa_without_akm(app):
    for n in app.api["networks"]["networks"]:
        if n["ssid"] == "Plain":
            n["wpanoakm"] = 1

    app.open()
    app.page.fill("#filter", "without akm")
    app.page.wait_for_function("() => filterText === 'without akm'")

    assert app.js("networks.filter(matchesFilter).map(n => n.ssid)") \
        == ["Plain"]
