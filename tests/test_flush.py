"""The Flush buttons above the BSSID and the station graph."""


def route_flush(app, table, empty):
    """/api/flush: noted, and from then on the table answers empty."""
    asked = []

    def answer(route):
        url = route.request.url
        asked.append(url[url.index("/api/"):])
        app.api[table] = empty
        route.fulfill(content_type="application/json", body='{"ok":1}')

    app.page.route(app.base + "/api/flush*", answer)

    return asked


def network_rows(app):
    return app.js("document.querySelectorAll('#networkTable tr').length")


def test_network_flush_empties_the_table(opened):
    asked = route_flush(opened, "networks", {"networks": [], "sixghz": []})

    assert network_rows(opened) > 0

    opened.page.once("dialog", lambda d: d.accept())
    opened.page.locator("#networkView .flushBtn").click()
    opened.page.wait_for_function("() => networks.length === 0")
    opened.page.wait_for_timeout(300)

    assert asked == ["/api/flush?what=networks"]
    assert network_rows(opened) == 0
    assert opened.js("sixGhzList.length") == 0
    assert opened.errors == []


def test_network_flush_cancelled_sends_nothing(opened):
    asked = route_flush(opened, "networks", {"networks": [], "sixghz": []})
    before = network_rows(opened)

    opened.page.once("dialog", lambda d: d.dismiss())
    opened.page.locator("#networkView .flushBtn").click()
    opened.page.wait_for_timeout(300)

    assert asked == []
    assert network_rows(opened) == before


def test_station_flush_empties_the_table(opened):
    opened.tab("Stations")
    opened.js("updateClients()")
    opened.page.wait_for_function("() => stations.length >= 2")

    asked = route_flush(opened, "clients", {"clients": []})

    opened.page.once("dialog", lambda d: d.accept())
    opened.page.locator("#clientView .flushBtn").click()
    opened.page.wait_for_function("() => stations.length === 0")
    opened.page.wait_for_timeout(300)

    assert asked == ["/api/flush?what=stations"]
    assert opened.js("""[...document.querySelectorAll('#clientView tbody tr')]
        .filter(r => /02:00:00:00:01/.test(r.textContent)).length""") == 0
    assert opened.errors == []


def test_flush_buttons_top_right_of_the_graphs(opened):
    for view, canvas, tab in [("#networkView", "#networkGraph", "BSSIDs"),
                              ("#clientView", "#clientGraph", "Stations")]:
        opened.tab(tab)
        opened.page.wait_for_timeout(200)

        button = opened.page.locator(view + " .flushBtn").bounding_box()
        graph = opened.page.locator(canvas).bounding_box()

        # Right-aligned with the graph, just above it
        assert abs(button["x"] + button["width"] -
                   (graph["x"] + graph["width"])) < 4
        assert button["y"] + button["height"] <= graph["y"]
        assert graph["y"] - button["y"] < 80


def test_flush_button_fits_a_phone(phone):
    button = phone.page.locator("#networkView .flushBtn").bounding_box()

    assert button["x"] + button["width"] <= 390
    assert phone.js("document.documentElement.scrollWidth") <= 390


def test_flush_disabled_in_a_snapshot(opened):
    asked = route_flush(opened, "networks", {"networks": [], "sixghz": []})

    messages = []
    opened.page.on("dialog", lambda d: (messages.append(d.message),
                                        d.dismiss()))

    opened.js("snapshotView = {seq: 1, answers: {}}; showSnapshotBar()")

    for view in ["#networkView", "#clientView"]:
        button = opened.page.locator(view + " .flushBtn")

        assert button.is_disabled()
        assert button.get_attribute("title") == \
            "Not in a snapshot: Back to live first"

    # Even when called, nothing is asked and nothing sent
    opened.js("flushTable('networks')")
    opened.page.wait_for_timeout(300)

    assert asked == [] and messages == []

    # Back to live: usable again, with its own title
    opened.js("snapshotView = null; showSnapshotBar()")
    button = opened.page.locator("#networkView .flushBtn")

    assert button.is_enabled()
    assert button.get_attribute("title").startswith("Empty the network table")
