"""Wi-Fi Alliance Capabilities (QoS Management) and RSN Selection."""

from test_tooltip import tooltip


def station_tooltip(app, overrides):
    app.js("updateClients()")
    app.page.wait_for_function("() => stations.length > 0")

    return app.js("""o => {
        const d = document.createElement('div');
        d.innerHTML = clientTooltip(Object.assign({}, stations[0], o));
        return d.innerText;
    }""", overrides)


def test_ap_qos_management(opened):
    assert "SCS traffic description" in tooltip(opened, {"wfacapa": 4})
    assert "DSCP policy, unsolicited DSCP policy" in \
        tooltip(opened, {"wfacapa": 3})
    assert "element without capabilities" in tooltip(opened, {"wfacapa": 0})

    # Without the element, or from older firmware: no line
    assert "QoS Mgmt" not in tooltip(opened, {})


def test_station_qos_management_and_rsn_selection(opened):
    text = station_tooltip(opened, {"wfacapa": 1, "rsnsel": 1})

    assert "DSCP policy" in text
    assert "RSNE Override (RSN selection, from assoc request)" in text

    assert "RSNE Override 2" in station_tooltip(opened, {"rsnsel": 2})
    assert "unknown value 7" in station_tooltip(opened, {"rsnsel": 7})

    plain = station_tooltip(opened, {})

    assert "QoS Mgmt" not in plain
    assert "RSN selection" not in plain


def test_filters(app):
    for n in app.api["networks"]["networks"]:
        if n["ssid"] == "Plain":
            n["wfacapa"] = 4

    app.api["clients"]["clients"][0].update(wfacapa=1, rsnsel=1)
    app.open()

    app.page.fill("#filter", "qos management")
    app.page.wait_for_timeout(100)

    assert app.js("networks.filter(matchesFilter).map(n => n.ssid)") == \
        ["Plain"]

    app.js("updateClients()")
    app.page.wait_for_function("() => stations.length > 0")

    mac = app.api["clients"]["clients"][0]["mac"]

    for word in ["dscp", "rsne override"]:
        app.js("stationFilterText = arguments[0]".replace(
            "arguments[0]", repr(word)))

        assert app.js("stations.filter(stationMatches).map(c => c.mac)") \
            == [mac], word


def test_catalog_name(opened):
    assert opened.js("""catalogName({k: 2, id: 0x506F9A23 >>> 0,
        vn: 'Wi-Fi Alliance'})[1]""") == \
        "50:6F:9A type 35 Wi-Fi Alliance Capabilities (QoS Management)" \
        " (Wi-Fi Alliance)"
