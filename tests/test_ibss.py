"""Ad-hoc networks (IBSS): Capabilities column, tooltip and filter."""

from test_tooltip import tooltip


def with_ibss(app, **fields):
    """The Plain network turned into an IBSS before the page loads."""
    for n in app.api["networks"]["networks"]:
        if n["ssid"] == "Plain":
            n.update(ibss=1, **fields)

    return app.open()


def caps_of(app, ssid):
    return app.js("""s => apCapsText(networks.find(n => n.ssid === s))""",
                  ssid)


def test_ad_hoc_in_the_capabilities(app):
    with_ibss(app, atim=0)

    assert caps_of(app, "Plain").startswith("Ad-hoc")
    assert "Ad-hoc" not in caps_of(app, "Office")


def test_tooltip_explains_ibss_and_atim(opened):
    text = tooltip(opened, {"ibss": 1, "atim": 0})

    assert "IBSS, no access point" in text
    assert "ATIM window 0 (no power save)" in text

    assert "ATIM window 10 TU" in tooltip(opened, {"ibss": 1, "atim": 10})

    # Firmware before 1.77.0 sends neither
    assert "IBSS" not in tooltip(opened, {})


def test_filter_finds_ad_hoc(app):
    with_ibss(app, atim=0)

    for word in ["ibss", "adhoc", "ad-hoc"]:
        app.page.fill("#filter", word)
        app.page.wait_for_timeout(100)

        assert app.js("networks.filter(matchesFilter).map(n => n.ssid)") \
            == ["Plain"], word
