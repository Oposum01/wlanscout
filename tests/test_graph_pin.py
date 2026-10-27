"""A click on a network in the BSSID graph, or on an AP or station in
the station graph, pins its tooltip like a table row; restricting the
sweep to its channel is a button in it."""

from test_pin import tip_state


def network_point(app):
    """Where a network is drawn in the BSSID graph, and its channel"""
    return app.js("""() => { drawNetworkGraph();
        const c = document.getElementById('networkGraph').getBoundingClientRect();
        for (const h of graphHits) {
            const x = (h.xl + h.xr) / 2, y = h.y + 2;
            const n = networkAt(x, y);
            if (n && !bandButtonAt(x, y))
                return {x: c.left + x, y: c.top + y, ch: n.channel, bssid: n.bssid};
        }
        return null; }""")


def station_graph_point(app, kind):
    """Where an AP box (kind "ap") or a station ("sta") is drawn"""
    app.tab("Stations")
    app.js("updateClients()")
    app.page.wait_for_function("() => stations.length > 0")

    # The fixture's stations only probe: one joins the first network
    if kind == "ap":
        app.js("""() => { stations[0].bssid = networks[0].bssid;
            stations[0].ch = networks[0].channel; stations[0].assoc = true;
            drawClientGraph(); }""")

    app.page.wait_for_function("""k => clientHits.some(h => h.kind === k &&
        (k === 'sta' ? h.obj.ch : h.obj.ap))""", arg=kind)

    return app.js("""k => { const c = document.getElementById('clientGraph').getBoundingClientRect();
        const h = clientHits.find(h => h.kind === k && (k === 'sta' ? h.obj.ch : h.obj.ap));
        return {x: c.left + h.x, y: c.top + h.y,
                ch: k === 'sta' ? h.obj.ch : h.obj.ap.channel}; }""", kind)


def test_hover_says_click_and_has_no_button(opened):
    p = network_point(opened)
    opened.page.mouse.move(p["x"], p["y"])
    state = tip_state(opened)

    assert state["shown"] and not state["pinned"]
    assert "click to keep it open" in state["text"]
    assert opened.page.locator("#tip .scanOnly").count() == 0


def test_click_pins_and_keeps_the_sweep(opened):
    before = opened.js("selectedChannels()")
    p = network_point(opened)

    opened.page.mouse.move(p["x"], p["y"])
    opened.page.mouse.click(p["x"], p["y"])
    state = tip_state(opened)

    assert state["pinned"] and state["close"] == "block"
    assert "keep it open" not in state["text"]
    assert opened.js("selectedChannels()") == before

    # The real tabs, so Graphs works
    assert opened.page.locator("#tip .tipTabs:not(.preview)").count() == 1

    # The pointer moves on over the graph: the tooltip stays
    opened.page.mouse.move(p["x"] + 60, p["y"] + 30)
    opened.page.wait_for_timeout(200)
    assert tip_state(opened)["text"] == state["text"]


def test_the_button_restricts_the_sweep(opened):
    asked = []
    opened.page.on("request", lambda r: "/api/config?channels=" in r.url
                   and asked.append(r.url))

    p = network_point(opened)
    opened.page.mouse.click(p["x"], p["y"])

    button = opened.page.locator("#tip .scanOnly")
    assert button.inner_text() == "scan only channel %d" % p["ch"]

    button.click()

    assert opened.js("selectedChannels()") == [p["ch"]]
    assert not tip_state(opened)["shown"]
    opened.page.wait_for_timeout(300)
    assert any(u.endswith("channels=%d" % p["ch"]) for u in asked)


def test_no_button_in_a_snapshot(opened):
    opened.js("snapshotView = {seq: 1, answers: {}}")
    p = network_point(opened)
    opened.page.mouse.click(p["x"], p["y"])

    assert tip_state(opened)["pinned"]
    assert opened.page.locator("#tip .scanOnly").count() == 0


def test_ap_node_pins_with_the_button(opened):
    before = opened.js("selectedChannels()")
    p = station_graph_point(opened, "ap")

    opened.page.mouse.move(p["x"], p["y"])
    assert "Click to restrict" not in tip_state(opened)["text"]

    opened.page.mouse.click(p["x"], p["y"])

    assert tip_state(opened)["pinned"]
    assert opened.js("selectedChannels()") == before
    assert opened.page.locator("#tip .scanOnly").inner_text() == \
        "scan only channel %d" % p["ch"]


def test_station_node_pins_with_the_button(opened):
    before = opened.js("selectedChannels()")
    p = station_graph_point(opened, "sta")

    opened.page.mouse.click(p["x"], p["y"])

    assert tip_state(opened)["pinned"]
    assert opened.js("selectedChannels()") == before
    assert opened.page.locator("#tip .scanOnly").inner_text() == \
        "scan only channel %d" % p["ch"]


def test_a_tap_shows_the_button(phone):
    before = phone.js("selectedChannels()")
    p = network_point(phone)
    phone.page.touchscreen.tap(p["x"], p["y"])
    phone.page.wait_for_timeout(300)

    state = tip_state(phone)

    assert state["shown"] and not state["pinned"]
    assert phone.js("selectedChannels()") == before
    assert phone.page.locator("#tip .scanOnly").inner_text() == \
        "scan only channel %d" % p["ch"]
