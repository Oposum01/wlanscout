"""Snapshots: the list in Settings, viewing one, back to live."""

import copy
import json
import zipfile

from conftest import STATIC_BASE, App, load_fixtures


def snapshot_doc(fixtures, ssid="SnapNet"):
    """A snapshot as the sensor writes it, from the fixtures, with
    the first network renamed so it can be told from live data."""
    networks = copy.deepcopy(fixtures["networks"])
    networks["networks"][0]["ssid"] = ssid

    history_short = copy.deepcopy(fixtures["history"])
    history_long = copy.deepcopy(fixtures["history"])
    history_long["marker"] = "long"

    return {
        "wlanscout_snapshot": 1, "seq": 7, "epoch": 0, "uptime": 3725,
        "fw": "1.69.0", "sensor": "test-sensor",
        "sensorid": "02:00:00:00:00:99",
        "answers": {
            "/api/status": fixtures["status"],
            "/api/plan": fixtures["plan"],
            "/api/live": fixtures["live"],
            "/api/networks": networks,
            "/api/clients": fixtures["clients"],
            "/api/assochistory": fixtures["assochistory"],
            "/api/catalog": fixtures["catalog"],
            "/api/history?range=120000": history_short,
            "/api/history?range=1800000": history_long,
            "/api/ble": None,
        },
    }


LIST = {"keep": 5, "fs": 1, "parts": 127, "snapshots": [
    {"seq": 6, "epoch": 0, "uptime": 60, "bytes": 90000, "name": ""},
    {"seq": 7, "epoch": 0, "uptime": 3725, "bytes": 95000,
     "name": "Keller"}]}


def route_snapshots(app, doc):
    """/api/snapshot: the list, ?get= the document, ?take=1 a new one."""
    asked = []

    def answer(route):
        url = route.request.url
        asked.append(url[url.index("/api/"):])

        if "get=" in url:
            body = doc
        elif "clear=1" in url:
            body = dict(LIST, snapshots=[])
        elif "parts=" in url:
            body = dict(LIST, parts=int(url.split("parts=")[1]))
        elif "take=1" in url:
            body = dict(LIST, taken=8, snapshots=LIST["snapshots"] + [
                {"seq": 8, "epoch": 0, "uptime": 4000, "bytes": 96000}])
        else:
            body = LIST

        route.fulfill(content_type="application/json", body=json.dumps(body))

    app.page.route(app.base + "/api/snapshot*", answer)

    return asked


def open_file(app, doc, tmp_path):
    path = tmp_path / "wlanscout-snapshot-7.json"
    path.write_text(json.dumps(doc))
    app.page.set_input_files("#snapFile", str(path))
    app.page.wait_for_function("() => snapshotView !== null")


def ssids(app):
    return app.js("networks.map(n => n.ssid)")


def test_open_file_shows_the_snapshot(opened, tmp_path):
    open_file(opened, snapshot_doc(opened.api), tmp_path)

    opened.page.evaluate("updateNetworks()")
    opened.page.wait_for_function("() => networks.some(n => n.ssid === 'SnapNet')")

    bar = opened.js("document.getElementById('snapshotBar').innerText")

    assert "Snapshot 7 of sensor test-sensor" in bar
    assert "after power-on" in bar
    assert "Back to live" in bar

    # Served from the snapshot, not from the sensor
    before = list(opened.http_api)
    opened.page.evaluate("updateNetworks()")
    opened.page.evaluate("updateClients()")
    assert [p for p in opened.http_api[len(before):]
            if p in ("/api/networks", "/api/clients")] == []


def test_badge_says_snapshot(opened, tmp_path):
    open_file(opened, snapshot_doc(opened.api), tmp_path)
    opened.page.evaluate("updateStatus()")

    assert "snapshot" in opened.js("document.getElementById('version').innerText")


def test_back_to_live(opened, tmp_path):
    open_file(opened, snapshot_doc(opened.api), tmp_path)
    opened.page.evaluate("updateNetworks()")
    opened.page.wait_for_function("() => networks.some(n => n.ssid === 'SnapNet')")

    opened.page.locator("#snapshotBar button", has_text="Back to live").click()

    assert opened.js("snapshotView") is None
    assert not opened.js("document.getElementById('snapshotBar')"
                         ".classList.contains('on')")

    opened.page.evaluate("updateNetworks()")
    opened.page.wait_for_function("() => networks.length > 0")

    assert "SnapNet" not in ssids(opened)


def test_history_by_range(opened, tmp_path):
    open_file(opened, snapshot_doc(opened.api), tmp_path)

    marker = """async url => (await (await api(url)).json()).marker || null"""

    assert opened.js(marker, "/api/history?range=1800000") == "long"
    assert opened.js(marker, "/api/history?range=120000") is None
    # A range the snapshot lacks: another answer of the same endpoint
    assert opened.js("async () => (await api('/api/history?range=999')).ok")


def test_missing_answer_is_404(opened, tmp_path):
    open_file(opened, snapshot_doc(opened.api), tmp_path)

    assert opened.js("async () => (await api('/api/ble')).status") == 404


def test_commands_blocked_in_a_snapshot(opened, tmp_path):
    open_file(opened, snapshot_doc(opened.api), tmp_path)

    dialogs = []
    opened.page.on("dialog", lambda d: (dialogs.append(d.message), d.accept()))

    opened.js("queueCommand('/api/mode?fast=1')")

    assert dialogs and "Back to live" in dialogs[0]
    assert opened.js("pendingCommands.length") == 0

    # The clock still goes to the sensor
    opened.js("queueCommand('/api/time?epoch=1800000000')")
    opened.page.wait_for_function(
        "() => pendingCommands.length === 0")


def test_not_a_snapshot(opened, tmp_path):
    dialogs = []
    opened.page.on("dialog", lambda d: (dialogs.append(d.message), d.accept()))

    path = tmp_path / "other.json"
    path.write_text(json.dumps({"networks": []}))
    opened.page.set_input_files("#snapFile", str(path))
    opened.page.wait_for_function("() => true")
    opened.page.wait_for_timeout(300)

    assert dialogs and "not a wlanscout+ snapshot" in dialogs[0]
    assert opened.js("snapshotView") is None


def test_list_in_settings_and_view(app):
    doc = snapshot_doc(app.api)
    asked = route_snapshots(app, doc)
    app.open()

    app.js("showSettings()")
    app.page.wait_for_function(
        "() => document.querySelectorAll('#snapTable tr').length === 2")

    rows = app.js("""[...document.querySelectorAll('#snapTable tr')]
        .map(r => r.cells[0].textContent + ' ' + r.cells[3].textContent)""")

    # Newest first
    assert rows == ["7 93 kB", "6 88 kB"]
    assert "(2 of 5)" in app.js("document.getElementById('snapInfo').innerText")

    app.page.locator("#snapTable tr").first.locator(
        "button", has_text="View").click()
    app.page.wait_for_function("() => snapshotView !== null")

    assert "/api/snapshot?get=7" in asked
    # Back on the view of the snapshot's mode
    assert app.js("uiPage") == "wifi"


def test_take_snapshot(app):
    asked = route_snapshots(app, snapshot_doc(app.api))
    app.open()
    app.js("showSettings()")

    app.page.locator("button", has_text="Take snapshot").click()
    app.page.wait_for_function(
        "() => document.querySelectorAll('#snapTable tr').length === 3")

    assert "/api/snapshot?take=1" in asked


def test_static_copy_without_sensor(browser, tmp_path):
    # The installed app, no sensor plugged in: a downloaded snapshot
    # still opens with all its tables
    context = browser.new_context(viewport={"width": 1400, "height": 900})
    page = context.new_page()
    fixtures = load_fixtures()
    app = App(page, fixtures, base=STATIC_BASE)

    page.goto(STATIC_BASE + "/")
    page.wait_for_function("() => typeof networks !== 'undefined'")

    open_file(app, snapshot_doc(fixtures), tmp_path)
    page.evaluate("updateNetworks()")
    page.wait_for_function("() => networks.some(n => n.ssid === 'SnapNet')")

    assert app.http_api == []
    assert app.errors == []

    context.close()


def test_camera_button_on_every_view(app):
    asked = route_snapshots(app, snapshot_doc(app.api))
    app.open()

    for show in ("applyModeUI()", "showSettings()"):
        app.js(show)
        assert app.page.locator("#snapBtn").is_visible()

    app.js("uiPage = 'wifi'; applyModeUI()")
    app.page.locator("#snapBtn").click()
    app.page.wait_for_function(
        "() => document.getElementById('snapBtn').classList.contains('ok')")

    assert "/api/snapshot?take=1" in asked
    assert "Snapshot 8 kept on the sensor" in app.js(
        "document.getElementById('status').innerText")


def test_snapshot_message_outlasts_a_poll_round(app):
    """The live poll empties the status line; the message after a
    snapshot stays its few seconds all the same."""
    route_snapshots(app, snapshot_doc(app.api))
    app.open()

    app.page.locator("#snapBtn").click()
    app.page.wait_for_function(
        "() => document.getElementById('snapBtn').classList.contains('ok')")
    app.page.evaluate("updateLive()")

    assert "Snapshot 8 kept on the sensor" in app.js(
        "document.getElementById('status').innerText")

    # Held no longer, the next round clears it
    app.js("statusHeldUntil = 0")
    app.page.evaluate("updateLive()")

    assert app.js("document.getElementById('status').innerText") == ""


def test_camera_button_failure_is_red(opened):
    opened.page.route(opened.base + "/api/snapshot*", lambda r: r.fulfill(
        content_type="application/json",
        body=json.dumps(dict(LIST, taken=0))))

    opened.page.locator("#snapBtn").click()
    opened.page.wait_for_function(
        "() => document.getElementById('snapBtn').classList.contains('bad')")

    assert "could not be written" in opened.js(
        "document.getElementById('status').innerText")


def test_camera_button_fits_a_phone(phone):
    # Next to the badge, icon only, nothing past the edge
    phone.page.set_viewport_size({"width": 360, "height": 700})

    box = phone.js("""(() => {
        const r = document.getElementById('snapBtn').getBoundingClientRect();
        return [r.right, r.height,
                document.documentElement.scrollWidth,
                getComputedStyle(document.querySelector('#snapBtn .wideLabel'))
                    .display]; })()""")

    assert box[0] <= 360
    assert box[2] <= 360
    assert box[3] == "none"


def test_parts_from_the_sensor_and_back(app):
    asked = route_snapshots(app, snapshot_doc(app.api))
    app.open()
    app.js("showSettings()")
    app.page.wait_for_function(
        "() => document.querySelectorAll('#snapTable tr').length === 2")

    boxes = "[...document.querySelectorAll('#snapParts input')].map(b => b.checked)"

    assert app.js(boxes) == [True] * 7

    # Stations off: 127 - 2
    app.page.locator("#snapParts label", has_text="stations").locator(
        "input").uncheck()
    app.page.wait_for_function(
        "() => !document.querySelector('#snapParts input[data-bit=\"2\"]').checked")

    assert "/api/snapshot?parts=125" in asked
    assert app.js(boxes) == [True, False, True, True, True, True, True]


def test_parts_disabled_with_older_firmware(app):
    old = {k: v for k, v in LIST.items() if k != "parts"}
    app.page.route(app.base + "/api/snapshot*", lambda r: r.fulfill(
        content_type="application/json", body=json.dumps(old)))
    app.open()
    app.js("showSettings()")
    app.page.wait_for_function(
        "() => document.querySelectorAll('#snapTable tr').length === 2")

    assert app.js("[...document.querySelectorAll('#snapParts input')]"
                  ".every(b => b.disabled && b.checked)")


def test_bar_names_what_was_left_out(opened, tmp_path):
    doc = snapshot_doc(opened.api)
    doc["parts"] = 1 | 8

    open_file(opened, doc, tmp_path)

    assert ("(without stations, association history, IE catalog, BLE, "
            "802.15.4)") in opened.js(
        "document.getElementById('snapshotBar').innerText")


def test_name_in_list_and_bar(app):
    route_snapshots(app, dict(snapshot_doc(app.api), name="Keller"))
    app.open()
    app.js("showSettings()")
    app.page.wait_for_function(
        "() => document.querySelectorAll('#snapTable tr').length === 2")

    assert app.js("document.querySelector('#snapTable tr').cells[1]"
                  ".textContent") == "Keller"

    app.page.locator("#snapTable tr").first.locator(
        "button", has_text="View").click()
    app.page.wait_for_function("() => snapshotView !== null")

    assert "Snapshot 7 \u201cKeller\u201d" in app.js(
        "document.getElementById('snapshotBar').innerText")


def test_rename(app):
    asked = route_snapshots(app, snapshot_doc(app.api))
    app.open()
    app.js("showSettings()")
    app.page.wait_for_function(
        "() => document.querySelectorAll('#snapTable tr').length === 2")

    offered = []

    def answer(dialog):
        offered.append(dialog.default_value)
        dialog.accept("Halle Nord & S\u00fcd")

    app.page.on("dialog", answer)

    app.page.locator("#snapTable tr").first.locator(
        "button", has_text="Rename").click()
    app.page.wait_for_timeout(500)

    # The old name to start from, the new one URL encoded
    assert offered == ["Keller"]
    assert "/api/snapshot?rename=7&name=Halle%20Nord%20%26%20S%C3%BCd" in asked


def test_rename_cancelled(app):
    asked = route_snapshots(app, snapshot_doc(app.api))
    app.open()
    app.js("showSettings()")
    app.page.wait_for_function(
        "() => document.querySelectorAll('#snapTable tr').length === 2")

    app.page.on("dialog", lambda d: d.dismiss())

    app.page.locator("#snapTable tr").first.locator(
        "button", has_text="Rename").click()
    app.page.wait_for_timeout(300)

    assert not [a for a in asked if "rename=" in a]


def test_bar_default_parts(opened, tmp_path):
    # The default since 1.70.1: all but BLE and 802.15.4
    doc = snapshot_doc(opened.api)
    doc["parts"] = 127 & ~(32 | 64)

    open_file(opened, doc, tmp_path)

    assert "(without BLE, 802.15.4)" in opened.js(
        "document.getElementById('snapshotBar').innerText")


# ---- export and import ------------------------------------------------

def test_each_snapshot_has_export(app):
    route_snapshots(app, snapshot_doc(app.api))
    app.open()
    app.js("showSettings()")
    app.page.wait_for_function(
        "() => document.querySelectorAll('#snapTable tr').length === 2")

    labels = app.js("""[...document.querySelector('#snapTable tr')
        .querySelectorAll('button')].map(b => b.textContent)""")

    assert labels == ["View", "Export", "Rename", "Delete"]


def two_snapshot_routes(app):
    first = dict(snapshot_doc(app.api), seq=6, name="")
    second = dict(snapshot_doc(app.api), seq=7, name="Keller")

    def answer(route):
        url = route.request.url
        body = (first if "get=6" in url else second if "get=7" in url
                else LIST)
        route.fulfill(content_type="application/json", body=json.dumps(body))

    app.page.route(app.base + "/api/snapshot*", answer)


def test_export_all_as_a_zip(app):
    two_snapshot_routes(app)
    app.open()
    app.js("showSettings()")

    with app.page.expect_download() as info:
        app.page.locator("button", has_text="Export all").click()

    download = info.value

    assert download.suggested_filename.startswith(
        "wlanscout-snapshots-test-sensor-")
    assert download.suggested_filename.endswith(".zip")

    with zipfile.ZipFile(download.path()) as z:
        # CRCs and sizes as any ZIP tool checks them
        assert z.testzip() is None
        assert z.namelist() == ["wlanscout-snapshot-6.json",
                                "wlanscout-snapshot-7-Keller.json"]
        assert all(i.compress_type == zipfile.ZIP_DEFLATED
                   for i in z.infolist())

        doc = json.loads(z.read("wlanscout-snapshot-7-Keller.json"))

        assert doc["name"] == "Keller"
        assert doc["answers"]["/api/networks"]["networks"]

        # Packed to a fraction
        info7 = z.getinfo("wlanscout-snapshot-7-Keller.json")
        assert info7.compress_size < info7.file_size / 3


def test_export_one_as_a_zip(app):
    two_snapshot_routes(app)
    app.open()
    app.js("showSettings()")
    app.page.wait_for_function(
        "() => document.querySelectorAll('#snapTable tr').length === 2")

    with app.page.expect_download() as info:
        app.page.locator("#snapTable tr").first.locator(
            "button", has_text="Export").first.click()

    assert info.value.suggested_filename == "wlanscout-snapshot-7-Keller.zip"

    with zipfile.ZipFile(info.value.path()) as z:
        assert z.testzip() is None
        assert z.namelist() == ["wlanscout-snapshot-7-Keller.json"]


def import_files(app, tmp_path, docs, accept=True):
    paths = []

    for i, doc in enumerate(docs):
        if isinstance(doc, (str, bytes)) or hasattr(doc, "__fspath__"):
            paths.append(str(doc))
            continue

        path = tmp_path / ("import-%d.json" % i)
        path.write_text(json.dumps(doc))
        paths.append(str(path))

    dialogs = []

    def answer(dialog):
        dialogs.append(dialog.message)
        if accept:
            dialog.accept()
        else:
            dialog.dismiss()

    app.page.on("dialog", answer)
    app.page.set_input_files("#snapImport", paths)

    return dialogs


def route_import(app, answer=None):
    posts = []

    def handle(route):
        posts.append({"url": route.request.url,
                      "body": route.request.post_data_buffer or b""})
        route.fulfill(content_type="application/json",
                      body=json.dumps(answer or {"ok": 1, "seq": 30}))

    app.page.route(app.base + "/api/snapshotimport*", handle)

    return posts


def test_import_one_over_http(app, tmp_path):
    route_snapshots(app, snapshot_doc(app.api))
    posts = route_import(app)
    app.open()
    app.js("showSettings()")

    doc = dict(snapshot_doc(app.api), name="Halle Nord", epoch=1790000000)
    dialogs = import_files(app, tmp_path, [doc])

    app.page.wait_for_function(
        "() => document.getElementById('status').innerText"
        ".includes('1 snapshot imported')")

    assert "Import 1 snapshot onto the sensor?" in dialogs[0]
    assert len(posts) == 1

    url = posts[0]["url"]

    assert "/api/snapshotimport?len=" in url
    assert "&epoch=1790000000&uptime=3725&name=Halle%20Nord" in url

    # A multipart upload; the file starts as the firmware checks,
    # without the name, which went as an argument
    body = posts[0]["body"].decode()
    payload = body[body.index('{"wlanscout_snapshot":1'):]
    payload = payload[:payload.rindex("}") + 1]

    assert '"name"' not in payload
    assert int(url.split("len=")[1].split("&")[0]) == len(payload.encode())
    assert json.loads(payload)["answers"]["/api/networks"]


def test_import_a_whole_export_oldest_first(app, tmp_path):
    route_snapshots(app, snapshot_doc(app.api))
    posts = route_import(app)
    app.open()
    app.js("showSettings()")

    bundle = {"wlanscout_snapshots": 1, "sensor": "x", "snapshots": [
        dict(snapshot_doc(app.api), seq=2, epoch=1790000200),
        dict(snapshot_doc(app.api), seq=1, epoch=1790000100)]}

    import_files(app, tmp_path, [bundle])
    app.page.wait_for_function(
        "() => document.getElementById('status').innerText"
        ".includes('2 snapshots imported')")

    assert [p["url"].split("epoch=")[1].split("&")[0] for p in posts] == \
        ["1790000100", "1790000200"]


def test_import_cancelled_sends_nothing(app, tmp_path):
    route_snapshots(app, snapshot_doc(app.api))
    posts = route_import(app)
    app.open()
    app.js("showSettings()")

    import_files(app, tmp_path, [snapshot_doc(app.api)], accept=False)
    app.page.wait_for_timeout(500)

    assert posts == []


def test_import_refused_by_the_sensor(app, tmp_path):
    route_snapshots(app, snapshot_doc(app.api))
    route_import(app, {"ok": 0, "error": "no room on the flash"})
    app.open()
    app.js("showSettings()")

    import_files(app, tmp_path, [snapshot_doc(app.api)])
    app.page.wait_for_function(
        "() => document.getElementById('status').innerText"
        ".includes('Import stopped after 0 of 1: no room on the flash')")


def test_import_not_a_snapshot(app, tmp_path):
    route_snapshots(app, snapshot_doc(app.api))
    posts = route_import(app)
    app.open()
    app.js("showSettings()")

    dialogs = import_files(app, tmp_path, [{"networks": []}])
    app.page.wait_for_timeout(500)

    assert dialogs and "not a wlanscout+ snapshot" in dialogs[0]
    assert posts == []


def test_view_shows_the_number_on_this_sensor(app):
    # An imported snapshot still carries its old number inside
    route_snapshots(app, dict(snapshot_doc(app.api), seq=3))
    app.open()
    app.js("showSettings()")
    app.page.wait_for_function(
        "() => document.querySelectorAll('#snapTable tr').length === 2")

    app.page.locator("#snapTable tr").first.locator(
        "button", has_text="View").click()
    app.page.wait_for_function("() => snapshotView !== null")

    assert app.js("document.getElementById('snapshotBar').innerText") \
        .startswith("Snapshot 7")


def test_import_over_usb(usb, tmp_path):
    usb.page.click("#usbBtn")
    usb.page.wait_for_function("() => usbLink !== null")
    usb.js("showSettings()")

    docs = [dict(snapshot_doc(usb.api), name="Feld", epoch=1790000000)]
    import_files(usb, tmp_path, docs)

    usb.page.wait_for_function(
        "() => document.getElementById('status').innerText"
        ".includes('1 snapshot imported')")

    puts = usb.js("window.__usbPuts")

    assert len(puts) == 1
    assert puts[0]["line"].startswith("PUT /api/snapshotimport?len=")
    assert "&name=Feld" in puts[0]["line"]
    assert puts[0]["body"].startswith('{"wlanscout_snapshot":1')
    assert len(puts[0]["body"].encode()) == int(
        puts[0]["line"].split("len=")[1].split("&")[0])
    # Nothing over HTTP
    assert "/api/snapshotimport" not in usb.http_api


def test_left_out_part_does_not_come_live(opened, tmp_path):
    # Stations left out by Keep: the view shows none, not today's
    doc = snapshot_doc(opened.api)
    del doc["answers"]["/api/clients"]
    doc["parts"] = 127 & ~2

    open_file(opened, doc, tmp_path)

    before = list(opened.http_api)
    status = opened.js("async () => (await api('/api/clients')).status")
    opened.page.evaluate("updateClients()")
    opened.page.wait_for_timeout(300)

    assert status == 404
    assert "/api/clients" not in opened.http_api[len(before):]
    assert opened.js("stations.length") == 0


def test_left_out_spectrum_says_no_connection_lost(opened, tmp_path):
    # Kept without the spectrum: /api/live answers 404 in the view.
    # That is no lost connection, and the status line must not come
    # and go with every poll round, moving the page by a line
    doc = snapshot_doc(opened.api)
    doc["answers"].pop("/api/live", None)

    open_file(opened, doc, tmp_path)

    opened.js("""() => { window.__status = [];
        new MutationObserver(() => window.__status.push(
            document.getElementById('status').innerText))
            .observe(document.getElementById('status'),
                     {childList: true, characterData: true, subtree: true}); }""")

    for _ in range(3):
        opened.page.evaluate("updateLive()")
        opened.page.evaluate("updateStatus()")
        opened.page.wait_for_timeout(200)

    assert opened.js("document.getElementById('status').innerText") == ""
    assert not any(opened.js("window.__status"))


# ---- pcap links in a snapshot -----------------------------------------

def test_pcap_link_in_a_snapshot_explains_missing_frames(opened, tmp_path):
    open_file(opened, snapshot_doc(opened.api), tmp_path)
    opened.page.evaluate("updateNetworks()")
    opened.page.wait_for_function(
        "() => networks.some(n => n.ssid === 'SnapNet')")

    opened.page.route(opened.base + "/api/pcap*", lambda r: r.fulfill(
        status=404, content_type="text/plain",
        body="No frames from this address are in the capture.\n\n"))

    dialogs = []
    opened.page.on("dialog", lambda d: (dialogs.append(d.message),
                                        d.accept()))

    # The alert comes after the fetch; under load that takes a while
    opened.click_until(
        opened.page.locator("#networkTable a[href^='/api/pcap']").first,
        lambda: dialogs, wait_ms=5000)

    assert dialogs[0].startswith(
        "No frames from this address are in the capture.")
    assert "This is a snapshot: it keeps no frames" in dialogs[0]
    assert "from another sensor they were never there" in dialogs[0]


def test_pcap_link_in_a_snapshot_still_downloads(opened, tmp_path):
    open_file(opened, snapshot_doc(opened.api), tmp_path)
    opened.page.evaluate("updateNetworks()")
    opened.page.wait_for_function(
        "() => networks.some(n => n.ssid === 'SnapNet')")

    opened.page.route(opened.base + "/api/pcap*", lambda r: r.fulfill(
        status=200, content_type="application/vnd.tcpdump.pcap",
        headers={"Content-Disposition":
                 "attachment; filename=capture-020000000001.pcap"},
        body=b"\xd4\xc3\xb2\xa1" + b"\0" * 20))

    # Clicked again if the table was drawn anew under the click
    downloads = []
    opened.page.on("download", lambda d: downloads.append(d))
    opened.click_until(
        opened.page.locator("#networkTable a[href^='/api/pcap']").first,
        lambda: downloads, wait_ms=5000)

    assert downloads[0].suggested_filename == "capture-020000000001.pcap"


def test_pcap_link_live_stays_a_plain_link(opened):
    # Outside a snapshot over HTTP the browser follows the link itself
    assert opened.js("""() => apiLink({stopPropagation() {},
        preventDefault() { throw new Error('prevented'); }},
        document.querySelector('#networkTable a'))""") is True


# ---- ZIP ---------------------------------------------------------------

def make_zip(path, files, method=zipfile.ZIP_DEFLATED):
    with zipfile.ZipFile(path, "w", method) as z:
        for name, doc in files:
            z.writestr(name, json.dumps(doc))

    return path


def test_import_a_zip_from_elsewhere(app, tmp_path):
    # Made by another tool: deflated, a folder entry, one stored file
    route_snapshots(app, snapshot_doc(app.api))
    posts = route_import(app)
    app.open()
    app.js("showSettings()")

    path = tmp_path / "mixed.zip"

    with zipfile.ZipFile(path, "w") as z:
        z.writestr("folder/", "")
        z.writestr("folder/a.json",
                   json.dumps(dict(snapshot_doc(app.api), epoch=1790000200)),
                   compress_type=zipfile.ZIP_DEFLATED)
        z.writestr("b.json",
                   json.dumps(dict(snapshot_doc(app.api), epoch=1790000100)),
                   compress_type=zipfile.ZIP_STORED)
        z.writestr("readme.txt", "not a snapshot")

    import_files(app, tmp_path, [path])
    app.page.wait_for_function(
        "() => document.getElementById('status').innerText"
        ".includes('2 snapshots imported')")

    assert [p["url"].split("epoch=")[1].split("&")[0] for p in posts] == \
        ["1790000100", "1790000200"]


def test_import_an_old_export_all_json(app, tmp_path):
    route_snapshots(app, snapshot_doc(app.api))
    posts = route_import(app)
    app.open()
    app.js("showSettings()")

    bundle = {"wlanscout_snapshots": 1, "snapshots": [snapshot_doc(app.api)]}

    import_files(app, tmp_path, [bundle])
    app.page.wait_for_function(
        "() => document.getElementById('status').innerText"
        ".includes('1 snapshot imported')")

    assert len(posts) == 1


def test_view_file_takes_a_zip(opened, tmp_path):
    path = make_zip(tmp_path / "two.zip", [
        ("old.json", dict(snapshot_doc(opened.api, ssid="Older"),
                          epoch=1790000100)),
        ("new.json", dict(snapshot_doc(opened.api, ssid="Newer"),
                          epoch=1790000200))])

    opened.page.set_input_files("#snapFile", str(path))
    opened.page.wait_for_function("() => snapshotView !== null")
    opened.page.evaluate("updateNetworks()")

    # The newest of the two, and the status line says so
    opened.page.wait_for_function(
        "() => networks.some(n => n.ssid === 'Newer')")
    assert "holds 2 snapshots, showing the newest" in opened.js(
        "document.getElementById('status').innerText")


def test_zip_roundtrip_in_the_page(opened):
    # What makeZip packs readZip gives back, deflated and stored
    assert opened.js("""async () => {
        const files = [{name: 'a.json', bytes: new TextEncoder().encode('{"x":1}'.repeat(50))},
                       {name: 'Ä b.json', bytes: new TextEncoder().encode('[]')}];
        const zip = new Uint8Array(await (await makeZip(files)).arrayBuffer());
        const back = await readZip(zip);
        return back.map(f => f.name + '=' + new TextDecoder().decode(f.bytes).length);
    }""") == ["a.json=350", "Ä b.json=2"]


def test_not_a_zip(app, tmp_path):
    route_snapshots(app, snapshot_doc(app.api))
    route_import(app)
    app.open()
    app.js("showSettings()")

    path = tmp_path / "broken.zip"
    path.write_bytes(b"PK\x03\x04 broken")

    dialogs = import_files(app, tmp_path, [path])
    app.page.wait_for_timeout(500)

    assert dialogs and "Cannot import" in dialogs[0]


# ---- clear all ---------------------------------------------------------

def open_list(app):
    app.open()
    app.js("showSettings()")
    app.page.wait_for_function(
        "() => document.querySelectorAll('#snapTable tr').length === 2")


def test_clear_all(app):
    asked = route_snapshots(app, snapshot_doc(app.api))
    open_list(app)

    dialogs = []
    app.page.on("dialog", lambda d: (dialogs.append(d.message), d.accept()))

    app.page.locator("button", has_text="Clear all").click()
    app.page.wait_for_function(
        "() => document.querySelectorAll('#snapTable tr').length === 0")

    assert dialogs[0].startswith("Delete all 2 snapshots on the sensor?")
    assert "/api/snapshot?clear=1" in asked
    assert "2 snapshots deleted" in app.js(
        "document.getElementById('status').innerText")


def test_clear_all_cancelled(app):
    asked = route_snapshots(app, snapshot_doc(app.api))
    open_list(app)

    app.page.on("dialog", lambda d: d.dismiss())
    app.page.locator("button", has_text="Clear all").click()
    app.page.wait_for_timeout(400)

    assert not [a for a in asked if "clear=" in a]
    assert app.js("document.querySelectorAll('#snapTable tr').length") == 2


def test_clear_all_with_nothing_there(app):
    app.page.route(app.base + "/api/snapshot*", lambda r: r.fulfill(
        content_type="application/json",
        body=json.dumps(dict(LIST, snapshots=[]))))
    app.open()
    app.js("showSettings()")
    app.page.wait_for_timeout(500)

    dialogs = []
    app.page.on("dialog", lambda d: (dialogs.append(d.message), d.accept()))
    app.page.locator("button", has_text="Clear all").click()
    app.page.wait_for_timeout(400)

    assert dialogs == ["There are no snapshots on the sensor"]
