"""The Graphs tab of a pinned network tooltip: recorded by the sensor
only while it shows, fetched in steps, stopped on closing."""


def samples(n, start=0, heard=True):
    # [min, avg, max, util, stations, seen, heard]
    return [[-62 - i % 3, -58, -55, 30 + i, 4, 2, 3 if heard else 0]
            for i in range(start, start + n)]


def record_calls(app):
    app.js("""() => { window.watchCalls = [];
        const orig = api;
        api = (url, opt) => { if (url.startsWith('/api/watch'))
            watchCalls.push(url); return orig(url, opt); }; }""")


def calls(app):
    return app.js("watchCalls")


def pin_first(app):
    app.page.locator("#networkTable tr").first.click()


def tabs(app):
    return app.js("""() => [...document.querySelectorAll('#tip .tipTab')]
        .map(t => t.innerText + (t.classList.contains('active') ? '*' : ''))""")


def test_tabs_shown_under_the_mouse_usable_once_pinned(opened):
    opened.page.locator("#networkTable tr").first.hover()

    # Already there, so Graphs can be found, but only a preview
    assert tabs(opened) == ["Details*", "Graphs"]
    assert opened.js("!!document.querySelector('#tip .tipTabs.preview')")
    assert "then Graphs works" in opened.js(
        "document.querySelector('#tip .tipHint').innerText")
    assert opened.js("document.querySelector('#tip .tipGraphs')") is None

    pin_first(opened)

    assert tabs(opened) == ["Details*", "Graphs"]
    assert not opened.js("!!document.querySelector('#tip .tipTabs.preview')")
    assert opened.js("!!document.querySelector('#tip .tipGraphs')")


def test_nothing_recorded_until_the_tab(app):
    app.api["watch"] = {"ok": 1, "step": 10, "next": 6, "first": 0,
                        "epoch": 0, "up": 100, "s": samples(6)}
    app.open()
    record_calls(app)
    pin_first(app)
    app.page.wait_for_timeout(300)

    assert calls(app) == []

    bssid = app.js("document.querySelector('#tip .tipDetails').dataset.bssid")
    app.page.locator("#tip .tipTab", has_text="Graphs").click()
    app.page.wait_for_function("() => graphWatch && graphWatch.next === 6")

    assert calls(app)[0] == "/api/watch?bssid=" + bssid + "&since=0"
    assert tabs(app) == ["Details", "Graphs*"]

    head = app.js("document.querySelector('#tip .graphHead').innerText")

    assert head.startswith("Recording for 1 min")
    assert "10 s steps" in head and "6 points" in head
    assert app.js("document.querySelectorAll('#tip .tipGraph canvas').length") == 3
    assert not app.js("""getComputedStyle(document.querySelector(
        '#tip .tipDetails')).display !== 'none'""")

    # Further polls ask for what came since
    app.js("fetchGraphWatch()")
    app.page.wait_for_function("() => watchCalls.length >= 2")

    assert calls(app)[-1].endswith("&since=6")


def test_closing_stops_the_recording(app):
    app.api["watch"] = {"ok": 1, "step": 10, "next": 2, "first": 0,
                        "epoch": 0, "up": 0, "s": samples(2)}
    app.open()
    record_calls(app)
    pin_first(app)
    app.page.locator("#tip .tipTab", has_text="Graphs").click()
    app.page.wait_for_function("() => graphWatch && graphWatch.next === 2")

    app.page.keyboard.press("Escape")
    app.page.wait_for_function("() => watchCalls.some(u => u.endsWith('&stop=1'))")

    assert app.js("graphWatch") is None

    # Back to Details stops it as well
    pin_first(app)
    app.page.locator("#tip .tipTab", has_text="Graphs").click()
    app.page.wait_for_function("() => graphWatch !== null")
    app.page.locator("#tip .tipTab", has_text="Details").click()

    assert app.js("graphWatch") is None
    assert sum(u.endswith("&stop=1") for u in calls(app)) == 2


def test_gaps_and_the_hint_to_lock_the_channel(app):
    app.api["watch"] = {"ok": 1, "step": 10, "next": 4, "first": 0,
                        "epoch": 0, "up": 0,
                        "s": samples(2) + samples(2, 2, heard=False)}
    app.open()
    pin_first(app)
    app.page.locator("#tip .tipTab", has_text="Graphs").click()
    app.page.wait_for_function("() => graphWatch && graphWatch.next === 4")

    head = app.js("document.querySelector('#tip .graphHead').innerText")

    assert "2 without a beacon" in head
    assert "scan only its channel, with the button in its tooltip" in head


def test_refused_says_why(app):
    app.api["watch"] = {"ok": 0, "error": "too many graphs open"}
    app.open()
    pin_first(app)
    app.page.locator("#tip .tipTab", has_text="Graphs").click()
    app.page.wait_for_function("() => graphWatch && graphWatch.error")

    assert app.js("document.querySelector('#tip .graphHead').innerText") == \
        "Not recorded: too many graphs open"


def test_a_recording_started_anew_replaces_the_old_one(app):
    # 30 points, then the sensor dropped it and began again at 0
    app.api["watch"] = {"ok": 1, "step": 5, "next": 30, "first": 0,
                        "epoch": 0, "up": 100, "s": samples(30)}
    app.open()
    pin_first(app)
    app.page.locator("#tip .tipTab", has_text="Graphs").click()
    app.page.wait_for_function("() => graphWatch && graphWatch.next === 30")

    app.api["watch"] = {"ok": 1, "step": 5, "next": 4, "first": 0,
                        "epoch": 0, "up": 400, "s": samples(4)}
    app.js("fetchGraphWatch()")
    app.page.wait_for_function("() => graphWatch.next === 4")

    # Only the new ones, numbered from 0, not 34 points out of order
    assert app.js("graphWatch.samples.map(x => x.k)") == [0, 1, 2, 3]
    assert "Started anew: the sensor had dropped the recording" in app.js(
        "document.querySelector('#tip .graphHead').innerText")

    # A start earlier than the one before: the sensor restarted
    app.api["watch"] = dict(app.api["watch"], up=20, next=2, s=samples(2))
    app.js("fetchGraphWatch()")
    app.page.wait_for_function("() => graphWatch.up === 20")

    assert "Started anew: the sensor restarted" in app.js(
        "document.querySelector('#tip .graphHead').innerText")


def test_times_with_a_clock(opened):
    # 2026-10-21 12:00:00 UTC, sample 6 starts a minute later
    text = opened.js("""graphTimeText({epoch: 1792584000, step: 10,
        next: 7}, 6)""")
    expected = opened.js("""new Date((1792584000 + 60) * 1000)
        .toLocaleTimeString([], {hour: '2-digit', minute: '2-digit'})""")

    assert text == expected
    assert opened.js("graphTimeText({epoch: 0, step: 10, next: 7}, 4)") == \
        "−30 s"


def test_starts_with_what_the_tooltip_says(app):
    app.api["watch"] = {"ok": 1, "step": 10, "next": 0, "first": 0,
                        "epoch": 0, "up": 0, "s": []}
    app.open()
    pin_first(app)
    app.page.locator("#tip .tipTab", has_text="Graphs").click()
    app.page.wait_for_function("() => graphWatch && graphWatch.seed")

    seed, net = app.js("""() => { const b = graphWatch.bssid;
        return [graphWatch.seed, networks.find(n => n.bssid === b)]; }""")

    # The RSSI, BSS Load and stations of the network table, a step
    # before the first one
    assert seed["k"] == -1
    assert seed["s"][:3] == [net["rssi"]] * 3
    assert seed["s"][3] == (net["util"] if net.get("util", -1) >= 0 else 255)
    assert seed["s"][4] == (net["sta"] if net.get("sta", -1) >= 0 else 65535)
    assert "so far what the tooltip says" in app.js(
        "document.querySelector('#tip .graphHead').innerText")


def test_keep_recording_without_the_tab(app):
    app.api["watch"] = {"ok": 1, "keep": 0, "step": 5, "next": 2,
                        "first": 0, "epoch": 0, "up": 0, "s": samples(2)}
    app.open()
    record_calls(app)
    pin_first(app)
    app.page.locator("#tip .tipTab", has_text="Graphs").click()
    app.page.wait_for_function("() => graphWatch && graphWatch.answered")

    box = app.page.locator("#tip .graphKeep")

    assert not box.is_checked()

    app.api["watch"] = dict(app.api["watch"], keep=1)
    box.check()
    app.page.wait_for_function("() => graphWatch.keep === true")

    assert any(u.endswith("&keep=1") for u in calls(app))
    assert box.is_checked()


def test_graph_csv(app):
    app.api["watch"] = {"ok": 1, "keep": 0, "step": 5, "next": 2,
                        "first": 0, "epoch": 0, "up": 0, "s": samples(2)}
    app.open()
    pin_first(app)
    app.page.locator("#tip .tipTab", has_text="Graphs").click()
    app.page.wait_for_function("() => graphWatch && graphWatch.answered")
    app.js("""() => { window.downloads = [];
        apiDownload = url => downloads.push(url); }""")
    app.page.locator("#tip .graphCsv").click()

    bssid = app.js("graphWatch.bssid")

    assert app.js("downloads") == ["/api/watch?bssid=" + bssid + "&csv=1"]


def test_kept_marked_in_the_network_table(opened):
    html = opened.js("n => noteBadges(n)", {"bssid": "02:00:00:00:00:01",
                                           "wk": 1})

    assert "wkBadge" in html and "<svg" in html
    assert "wkBadge" not in opened.js("n => noteBadges(n)",
                                      {"bssid": "02:00:00:00:00:01"})


def test_recorded_graphs_in_settings(opened):
    bssid = opened.js("networks[0].bssid")
    opened.api["system"] = dict(opened.api["system"], watch=[
        [bssid, 1, 1792584000, 100, 240],
        ["02:00:00:00:00:99", 0, 0, 50, 12]])
    opened.js("showSettings('capture')")
    opened.js("updateSystem()")
    opened.page.wait_for_function(
        "() => document.querySelectorAll('#watchTable tr').length === 2")

    rows = opened.js("""[...document.querySelectorAll('#watchTable tr')]
        .map(r => r.innerText)""")

    assert "kept, also without a viewer" in rows[0] and "240 points" in rows[0]
    assert "while its tab is open" in rows[1]
    # Without a clock the start as time since boot
    assert "after the start" in rows[1]
    assert opened.js("document.getElementById('watchInfo').innerText") == \
        "(2 of 4)"

    record_calls(opened)
    opened.page.locator("#watchTable tr").first.locator(
        "button", has_text="stop").click()
    opened.page.wait_for_function("() => watchCalls.length > 0")

    assert opened.js("watchCalls")[0] == \
        "/api/watch?bssid=" + bssid + "&keep=0&stop=1"


def test_no_graphs_recorded(opened):
    opened.js("showSettings('capture')")
    opened.js("updateSystem()")
    opened.page.wait_for_function(
        "() => document.querySelectorAll('#watchTable tr').length === 1")

    assert "none" in opened.js(
        "document.querySelector('#watchTable').innerText")
