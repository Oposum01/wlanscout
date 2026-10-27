"""Settings in tabs: Sensor, Network, Capture, Snapshots, Event log and
Diagnostics, each with a short status; every row of before still there."""

# Every label the page had before the tabs, now somewhere in a tab
LABELS = ["Firmware", "Source", "Author", "Uptime", "Boot", "Scanner mode",
          "Clock source", "Device time", "Last clock sync", "NTP server",
          "Clock corrections", "OUI entries", "Address", "Gateway",
          "Subnet mask", "DNS", "MAC", "Hostname", "Link",
          "Failed DHCP attempts", "DHCP client", "Internal heap free",
          "Internal heap minimum since boot", "Largest free internal block",
          "PSRAM free", "Sensor id", "Sensor name", "Export schema",
          "Sweeps completed", "Channels selected",
          "Channels in the current plan", "Management frames dropped",
          "Data frames dropped", "Frames with a bad FCS", "Capture",
          "Captured frame types", "Frames in the ring",
          "Beacons held, newest per AP",
          "Association requests in their own ring",
          "Frames captured in total", "Buffer reserved", "IE catalog"]


def settings(app, tab=None):
    app.js("showSettings(%s)" % ("'%s'" % tab if tab else ""))
    app.js("updateSystem()")
    app.page.wait_for_function(
        "() => document.querySelectorAll('#sysTable tr').length > 0")


def shown_pane(app):
    return app.js("""[...document.querySelectorAll('.setPane')]
        .filter(p => p.classList.contains('active')).map(p => p.id)""")


def test_sensor_first_then_the_tab_picked(opened):
    settings(opened)

    assert shown_pane(opened) == ["setSensor"]

    opened.page.locator("#setTabs button", has_text="Capture").click()

    assert shown_pane(opened) == ["setCapture"]
    assert opened.page.locator("#storeTable").is_visible()
    assert not opened.page.locator("#sysTable").is_visible()

    # Kept across a reload
    opened.page.reload()
    opened.page.wait_for_function("() => typeof networks !== 'undefined'")
    settings(opened)

    assert shown_pane(opened) == ["setCapture"]


def test_no_row_lost(opened):
    settings(opened)
    labels = opened.js("""[...document.querySelectorAll(
        '#settingsMode table.kv tr, #storeTable tr')]
        .map(r => r.cells[0].textContent)""")

    assert [x for x in LABELS if x not in labels] == []


def test_status_beside_the_tabs(opened):
    opened.api["log"] = {"entries": [], "total": 3}
    settings(opened)
    opened.js("updateLog()")
    opened.page.wait_for_function(
        "() => document.getElementById('hintLog').innerText !== ''")

    hints = opened.js("""() => Object.fromEntries([...document.querySelectorAll(
        '#setTabs button')].map(b => [b.dataset.tab,
        b.querySelector('.setHint').innerText]))""")

    assert hints["sensor"] == "v9.99.9"
    assert hints["network"] == "192.0.2.10"
    assert hints["capture"].endswith(" frames")
    assert hints["log"] == "3 since start"


def test_a_crash_shows_on_the_log_tab(opened):
    opened.api["system"] = dict(opened.api["system"], crash={
        "task": "loopTask", "cause": "load access fault", "pc": "0x1",
        "ra": "0x2", "mtval": "0x0", "mcause": 5, "size": 9216,
        "elf": "3fa1c0de", "same": 1})
    opened.api["log"] = {"entries": [], "total": 3}
    settings(opened)
    opened.js("updateLog()")
    opened.page.wait_for_timeout(300)

    hint = opened.page.locator("#hintLog")

    assert hint.inner_text() == "crash"
    assert "setAlert" in hint.get_attribute("class")


def test_capture_stores_with_their_buttons(opened):
    settings(opened, "capture")

    rows = opened.js("""[...document.querySelectorAll('#storeTable tr')].map(r =>
        [r.cells[0].textContent, [...r.querySelectorAll('button')]
         .map(b => b.textContent)])""")

    assert rows == [
        ["Frames in the ring", ["download all", "clear"]],
        ["Beacons held, newest per AP", ["download all", "clear"]],
        ["Association requests in their own ring", ["download all", "clear"]],
        ["Association history", ["clear"]]]

    # The inventory in Diagnostics has no buttons, the largest first
    assert opened.js("document.querySelectorAll('#allocTable button').length") \
        == 0
    sizes = opened.js("""[...document.querySelectorAll('#allocTable tr')]
        .slice(0, -1).map(r => parseFloat(r.cells[1].textContent))""")

    assert sizes == sorted(sizes, reverse=True)


def test_help_behind_the_question_mark(opened):
    settings(opened, "snapshots")
    help = opened.page.locator("#setSnapshots .setHelp").first

    assert not help.is_visible()

    opened.page.locator("#setSnapshots .setHelpBtn").first.click()

    assert help.is_visible()
    assert "A snapshot keeps what the page shows" in help.inner_text()


def test_tabs_on_top_on_a_phone(opened):
    opened.page.set_viewport_size({"width": 390, "height": 800})
    settings(opened)

    assert opened.js("""getComputedStyle(document.getElementById('setTabs'))
        .flexDirection""") == "row"
    assert opened.js("document.documentElement.scrollWidth") <= 390


def test_boxes_one_below_the_other(opened):
    settings(opened, "diag")

    # CPU, Memory, Tasks, Allocated buffers: same left edge, each lower
    boxes = opened.js("""[...document.querySelectorAll('#setDiag .section')]
        .map(b => { const r = b.getBoundingClientRect();
                    return [Math.round(r.left), Math.round(r.top)]; })""")

    assert len(boxes) == 4
    assert len({left for left, _ in boxes}) == 1
    assert [top for _, top in boxes] == sorted(top for _, top in boxes)
