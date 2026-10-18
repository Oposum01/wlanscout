"""The bell in the header: configuration changes of all BSSs, the
count of unread ones, the list, read on opening, a click filters."""

import re

CHANGES = {"boot": 7, "seq": 3, "changes": [
    [3, 30, "02:00:00:00:00:01", "Hall", "channel", "36", "52"],
    [2, 600, "02:00:00:00:00:02", "", "restart", "273600", ""],
    [1, 900, "02:00:00:00:00:01", "Hall", "WPS", "off", "on"]]}


def with_bell(app, seq=3):
    app.api["status"] = dict(app.api["status"], cfgseq=seq, boot=7)
    app.api["changes"] = CHANGES
    app.open()
    app.js("updateStatus()")
    app.page.wait_for_function(
        "() => document.getElementById('bellCount').innerText !== ''")
    return app


def test_no_bell_on_older_firmware(opened):
    opened.js("updateStatus()")
    opened.page.wait_for_timeout(300)

    assert not opened.page.locator("#bellBtn").is_visible()


def test_count_of_unread(app):
    with_bell(app)

    assert app.page.locator("#bellBtn").is_visible()
    assert app.page.locator("#bellCount").inner_text() == "3"


def test_open_lists_and_marks_read(app):
    with_bell(app)
    app.page.locator("#bellBtn").click()

    panel = app.page.locator("#changesPanel")
    text = panel.inner_text()

    assert panel.is_visible()
    assert "channel 36 → 52" in text
    assert "AP restarted, was up 3 d 4 h" in text
    assert "<hidden>" in text
    assert app.page.locator("#changesPanel .chg.new").count() == 3

    # Read once opened, also after a reload of the page
    assert not app.page.locator("#bellCount").is_visible()

    app.page.reload()
    app.js("updateStatus()")
    app.page.wait_for_function(
        "() => document.getElementById('bellCount').innerText !== ''")
    assert not app.page.locator("#bellCount").is_visible()


def test_click_filters_to_the_bss(app):
    with_bell(app)
    app.page.locator("#bellBtn").click()
    app.page.locator("#changesPanel .chg").first.click()

    assert app.js("filterText") == "02:00:00:00:00:01"
    assert not app.page.locator("#changesPanel").is_visible()


def test_panel_stays_inside_a_phone_screen(browser):
    import conftest
    ctx = browser.new_context(viewport={"width": 412, "height": 900})
    app = conftest.App(ctx.new_page(), conftest.load_fixtures())
    with_bell(app)
    app.page.locator("#bellBtn").click()

    box = app.page.locator("#changesPanel").bounding_box()

    assert box["x"] >= 0 and box["x"] + box["width"] <= 412
    ctx.close()


def test_flapping_entry_and_refetch_on_update(app):
    with_bell(app)

    # The same entry updated: cfgver moves, cfgseq does not
    app.api["changes"] = dict(CHANGES, ver=9, changes=[
        [3, 5, "02:00:00:00:00:01", "Hall", "flapping",
         "width, standard", "6 switches"]] + CHANGES["changes"][1:])
    app.api["status"] = dict(app.api["status"], cfgver=9)
    app.js("updateStatus()")
    app.page.wait_for_function("() => cfgList.ver === 9")

    app.page.locator("#bellBtn").click()
    text = app.page.locator("#changesPanel").inner_text()

    assert "flapping between two configurations (width, standard), " \
        "6 switches" in text


def test_list_scrolls(app):
    # Forty BSSs, so none of them is grouped with another
    many = [[i, i, "02:00:00:00:00:%02X" % i, "Hall", "WPS", "off", "on"]
            for i in range(40, 0, -1)]
    app.api["status"] = dict(app.api["status"], cfgseq=40, boot=7)
    app.api["changes"] = {"boot": 7, "seq": 40, "changes": many}
    app.open()
    app.js("updateStatus()")
    app.page.wait_for_function(
        "() => document.getElementById('bellCount').innerText === '40'")
    app.page.locator("#bellBtn").click()

    assert app.page.locator("#changesPanel .chg").count() == 40
    assert app.js("""(() => { const p = document.getElementById('changesPanel');
        return p.scrollHeight > p.clientHeight &&
               getComputedStyle(p).overflowY === 'auto'; })()""")


def test_changes_of_one_bss_at_once_are_one_entry(app):
    app.api["status"] = dict(app.api["status"], cfgseq=5, boot=7)
    app.api["changes"] = {"boot": 7, "seq": 5, "changes": [
        [5, 10, "02:00:00:00:00:01", "Hall", "group cipher", "TKIP", "CCMP-128"],
        [4, 10, "02:00:00:00:00:01", "Hall", "AKM", "PSK, SAE", "SAE"],
        [3, 11, "02:00:00:00:00:01", "Hall", "security", "WPA2/WPA3", "WPA3"],
        [2, 11, "02:00:00:00:00:02", "Lab", "WPS", "off", "on"],
        [1, 300, "02:00:00:00:00:01", "Hall", "channel", "36", "52"]]}
    app.open()
    app.js("updateStatus()")
    app.page.wait_for_function(
        "() => document.getElementById('bellCount').innerText !== ''")

    # Three entries for Hall at once are one, then Lab, then Hall later
    assert app.page.locator("#bellCount").inner_text() == "3"

    app.page.locator("#bellBtn").click()
    entries = app.page.locator("#changesPanel .chg")

    assert entries.count() == 3
    first = entries.first.inner_text()
    assert "security WPA2/WPA3 \u2192 WPA3" in first
    assert "AKM PSK, SAE \u2192 SAE" in first
    assert "group cipher TKIP \u2192 CCMP-128" in first


def route_mute(app):
    asked = []

    def answer(route):
        url = route.request.url
        asked.append(url[url.index("/api/"):])
        route.fulfill(content_type="application/json", body='{"ok":1}')

    app.page.route(app.base + "/api/cfgmute*", answer)
    return asked


def test_muted_bss_stays_out_of_the_bell(app):
    app.api["status"] = dict(app.api["status"], cfgseq=3, boot=7)
    app.api["changes"] = dict(CHANGES, muted=["02:00:00:00:00:01"])
    app.open()
    app.js("updateStatus()")
    app.page.wait_for_function(
        "() => document.getElementById('bellCount').innerText !== ''")

    # Only the restart of ...:02 is left
    assert app.page.locator("#bellCount").inner_text() == "1"

    app.page.locator("#bellBtn").click()
    text = app.page.locator("#changesPanel").inner_text()

    assert "channel 36" not in text
    assert "Not notified: 02:00:00:00:00:01" in text
    assert app.page.locator("#changesPanel a.mute", has_text="unmute").count() == 1


def test_mute_from_the_list(app):
    with_bell(app)
    asked = route_mute(app)

    app.page.locator("#bellBtn").click()
    app.page.locator("#changesPanel .chg .mute").first.click()
    app.page.wait_for_function("() => (cfgList.muted || []).length === 1")

    assert asked == ["/api/cfgmute?bssid=02:00:00:00:00:01&on=1"]
    # Hall's entries are gone from the list, the restart is left
    text = app.page.locator("#changesPanel").inner_text()
    assert "channel 36" not in text and "AP restarted" in text
    assert "Not notified: 02:00:00:00:00:01" in text


def test_checkbox_in_the_tooltip(opened):
    asked = route_mute(opened)
    bssid = opened.js("networks[0].bssid")

    html = opened.js("""() => { const n = Object.assign({}, networks[0],
        {cfgn: 1, cfgch: [[5, 'WPS', 'off', 'on']]});
        return networkTooltip(n); }""")
    # In the section's title line, not a row of its own
    assert re.search(r"class='tipSec'>CHANGES <label[^>]*><input[^>]* checked", html)
    assert "notify about changes" in html

    muted = opened.js("""() => networkTooltip(Object.assign({}, networks[0],
        {cfgmute: 1}))""")
    # A muted BSS keeps the section, unticked, also without changes
    assert "notify about changes" in muted
    assert not re.search(r"CHANGES <label[^>]*><input[^>]* checked", muted)

    opened.js("b => setCfgNotify(b, false)", bssid)
    opened.page.wait_for_function("() => networks[0].cfgmute === 1")

    assert asked == ["/api/cfgmute?bssid=" + bssid + "&on=1"]


def test_list_sorted_by_last_change(app):
    # The flapping entry is old by number but updated a moment ago
    app.api["status"] = dict(app.api["status"], cfgseq=3, boot=7)
    app.api["changes"] = {"boot": 7, "seq": 3, "changes": [
        [3, 1200, "02:00:00:00:00:01", "Hall", "WPS", "off", "on"],
        [2, 1250, "02:00:00:00:00:02", "Lab", "channel", "1", "6"],
        [1, 100, "02:00:00:00:00:03", "Cal", "flapping", "width", "8 switches"]]}
    app.open()
    app.js("updateStatus()")
    app.page.wait_for_function(
        "() => document.getElementById('bellCount').innerText !== ''")
    app.page.locator("#bellBtn").click()

    first = app.page.locator("#changesPanel .chg").first.inner_text()

    assert "Cal" in first and "flapping" in first


def snapshot_with(app, changes):
    from test_snapshot import snapshot_doc

    doc = snapshot_doc(app.api)
    doc["answers"]["/api/status"] = dict(app.api["status"], cfgseq=3, boot=7)

    if changes is not None:
        doc["answers"]["/api/changes"] = changes

    app.js("d => enterSnapshot(d)", doc)
    app.js("updateStatus()")


def test_bell_in_a_snapshot_reads_as_it_was(opened):
    snapshot_with(opened, dict(CHANGES, muted=["02:00:00:00:00:09"]))
    opened.page.wait_for_function("() => cfgList.seq === 3")

    assert opened.page.locator("#bellBtn").is_visible()
    assert not opened.page.locator("#bellCount").is_visible()

    opened.page.locator("#bellBtn").click()
    panel = opened.page.locator("#changesPanel")
    text = panel.inner_text()

    assert "30 s before the snapshot" in text
    assert "Not notified: 02:00:00:00:00:09" in text
    assert panel.locator("a.mute").count() == 0
    # Looking at a snapshot marks nothing read
    assert opened.js("localStorage.getItem('wlanscout.cfgSeen')") is None


def test_snapshot_without_the_list_has_no_bell(opened):
    snapshot_with(opened, None)
    opened.page.wait_for_function(
        "() => document.getElementById('bellBtn').style.display === 'none'")


def test_time_of_a_change_on_hover(app):
    with_bell(app)
    app.page.locator("#bellBtn").click()

    title = app.page.locator("#changesPanel .chg").first \
        .locator("span[title]").first.get_attribute("title")
    expect = app.js("""(() => { const d = new Date(cfgFetchAt - 30000);
        const two = x => String(x).padStart(2, '0');
        return d.getHours() + ':' + two(d.getMinutes()) + ':' +
            two(d.getSeconds()) + ' ' + two(d.getDate()) + '.' +
            two(d.getMonth() + 1) + '.' + d.getFullYear(); })()""")

    assert title == expect
