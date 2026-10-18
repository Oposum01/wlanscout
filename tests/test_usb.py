"""The page as a static copy talking to the sensor over WebUSB."""


def connect(usb):
    usb.page.click("#usbBtn")
    usb.page.wait_for_function("() => usbLink !== null")
    usb.page.wait_for_function("() => networks.length > 0")


def test_usb_button_only_on_a_static_copy(opened, usb):
    # The sensor's own http page has no WebUSB, so no button
    assert opened.js("getComputedStyle(usbBtn).display") == "none"
    assert usb.js("getComputedStyle(usbBtn).display") != "none"


def test_nothing_asked_over_http_without_a_link(usb):
    usb.page.wait_for_timeout(1500)

    assert usb.http_api == []
    assert "Not connected" in usb.js("document.getElementById('status').innerText")


def test_tables_filled_over_usb(usb):
    connect(usb)

    assert usb.js("networks.length") == len(usb.api["networks"]["networks"])
    assert "GET /api/networks" in usb.js("window.__usbRequests")
    assert usb.http_api == []
    assert usb.js("usbBtn.innerText") == "USB ✓"

    # Let a few poll rounds run over USB
    usb.page.wait_for_timeout(3000)

    assert usb.errors == []
    assert usb.http_api == []


def test_device_filter_and_no_reset(usb):
    connect(usb)

    assert usb.js("window.__usbFilters") == [{"vendorId": 0x303a}]
    # DTR on, RTS off: RTS would reset the chip
    assert usb.js("window.__usbControl.request") == 0x22
    assert usb.js("window.__usbControl.value") == 1


def test_query_arguments_reach_the_sensor(usb):
    connect(usb)

    usb.js("api('/api/history?range=60000')")
    usb.page.wait_for_function("""() => window.__usbRequests
        .includes('GET /api/history?range=60000')""")


def test_answers_in_order_and_errors(usb):
    connect(usb)

    result = usb.js("""async () => {
        const [a, b, c] = await Promise.all([
            api('/api/status'), api('/api/nope'), api('/api/plan')]);
        return [a.ok, (await a.json()).version, b.status,
                await b.text(), (await c.json()).b24.length];
    }""")

    assert result[0] is True
    assert result[1] == usb.api["status"]["version"]
    assert result[2:4] == [404, "Not found"]
    assert result[4] > 0


def test_pcap_download_over_usb(usb):
    connect(usb)

    with usb.page.expect_download() as info:
        usb.js("apiDownload('/api/pcap')")

    download = info.value

    assert download.suggested_filename == "capture.pcap"
    assert open(download.path(), "rb").read(4) == b"\xd4\xc3\xb2\xa1"


def test_unplugging_drops_the_link(usb):
    connect(usb)

    usb.js("window.__usbUnplug()")

    assert usb.js("usbLink") is None
    assert usb.js("usbBtn.innerText") == "USB"
    assert "unplugged" in usb.js("usbBtn.title")


def choose_firmware(usb, tmp_path, first_byte, size=4096):
    path = tmp_path / "wlanscout.ino.bin"
    path.write_bytes(bytes([first_byte]) + bytes(size - 1))
    usb.page.set_input_files("#otaFile", str(path))


def test_firmware_update_over_usb(usb, tmp_path):
    connect(usb)
    usb.js("showSettings()")
    usb.js("window.__notReloaded = true")
    choose_firmware(usb, tmp_path, 0xE9)

    usb.page.once("dialog", lambda d: d.accept())
    usb.js("uploadFirmware()")

    # Written, the sensor restarts, comes back on USB and is taken up
    # again without asking; then the new version is shown, no reload
    usb.page.wait_for_function(
        "() => document.getElementById('otaStatus').innerText"
        " === 'v9.9.9 running'", timeout=15000)

    puts = usb.js("window.__usbPuts")

    assert puts == [{"line": "PUT /api/ota?len=4096", "size": 4096,
                     "first": 0xE9}]
    assert not usb.js("window.__usbOverrun === true")
    assert usb.js("usbLink !== null")
    assert usb.js("window.__notReloaded") is True
    assert "/api/ota" not in usb.http_api


def test_browser_time_sent_after_an_update_over_usb(usb, tmp_path):
    connect(usb)
    usb.js("showSettings()")
    choose_firmware(usb, tmp_path, 0xE9)

    def times():
        return [r for r in usb.js("window.__usbRequests")
                if r.startswith("GET /api/time?epoch=")]

    usb.page.wait_for_function("""() => window.__usbRequests
        .some(r => r.startsWith('GET /api/time?epoch='))""")
    before = len(times())

    usb.page.once("dialog", lambda d: d.accept())
    usb.js("uploadFirmware()")
    usb.page.wait_for_function(
        "() => document.getElementById('otaStatus').innerText"
        " === 'v9.9.9 running'", timeout=15000)

    # The restarted sensor has no clock: the page hands it over again
    usb.page.wait_for_function("""n => window.__usbRequests
        .filter(r => r.startsWith('GET /api/time?epoch=')).length > n""",
        arg=before)
    assert len(times()) == before + 1


def test_firmware_over_usb_refused(usb, tmp_path):
    connect(usb)
    usb.js("showSettings()")
    choose_firmware(usb, tmp_path, 0x00)

    usb.page.once("dialog", lambda d: d.accept())
    usb.js("uploadFirmware()")

    usb.page.wait_for_function(
        "() => document.getElementById('otaStatus').innerText"
        ".startsWith('refused')")

    assert usb.js("document.getElementById('otaStatus').innerText") == \
        "refused: not an ESP32-C5 firmware image"
    assert usb.js("otaBusy") is False


def test_no_reconnect_without_an_update(usb):
    # A sensor plugged in by itself is not taken without a tap on USB
    connect(usb)
    usb.js("window.__usbUnplug()")
    usb.js("window.__usbPlugIn()")
    usb.page.wait_for_timeout(300)

    assert usb.js("usbLink") is None


def test_not_connected_message_fits_a_phone(browser):
    import json, pathlib
    from conftest import App, STATIC_BASE, load_fixtures

    context = browser.new_context(viewport={"width": 390, "height": 800},
                                  has_touch=True)
    fixtures = load_fixtures()
    context.add_init_script(
        script="window.__usbFixtures = " + json.dumps(fixtures) + ";")
    context.add_init_script(
        path=str(pathlib.Path(__file__).resolve().parent / "fake_usb.js"))

    page = context.new_page()
    App(page, fixtures, base=STATIC_BASE)
    page.goto(STATIC_BASE + "/")
    page.wait_for_function(
        "() => document.getElementById('status').innerText.startsWith('Not connected')")

    right = page.evaluate(
        "document.getElementById('status').getBoundingClientRect().right")

    assert right <= 390
    assert page.evaluate("document.documentElement.scrollWidth") <= 390

    context.close()


def test_not_connected_line_goes_on_connect(usb):
    usb.page.wait_for_function(
        "() => document.getElementById('status').innerText.startsWith('Not connected')")

    connect(usb)

    assert usb.js("document.getElementById('status').innerText") == ""

    # And stays away while the polls run over USB
    usb.page.wait_for_timeout(2500)
    assert usb.js("document.getElementById('status').innerText") == ""


def test_browser_time_sent_on_connect(usb):
    # Before connecting there is nobody to send it to
    assert not any("/api/time" in r for r in usb.js("window.__usbRequests"))

    connect(usb)

    usb.page.wait_for_function("""() => window.__usbRequests
        .some(r => r.startsWith('GET /api/time?epoch='))""")

    line = [r for r in usb.js("window.__usbRequests") if "/api/time" in r][0]
    epoch = int(line.split("epoch=")[1])
    now = usb.js("Math.floor(Date.now() / 1000)")

    assert abs(now - epoch) < 30


def test_sensor_page_still_sends_time_at_load(opened):
    opened.page.wait_for_timeout(1500)

    assert "/api/time" in opened.http_api


def test_badge_says_usb_instead_of_web_clients(usb, opened):
    connect(usb)
    usb.page.wait_for_function(
        "() => document.getElementById('version').innerText.includes('via USB')")

    badge = usb.js("document.getElementById('version').innerText")

    assert "clients" not in badge
    # Over Ethernet the count of web clients stays
    opened.page.wait_for_function(
        "() => document.getElementById('version').innerText.includes('client')")


def test_put_waits_for_the_go_ahead(usb, tmp_path):
    # A firmware of 2.5 MB-like size in pieces: never more in flight
    # than the sensor's receive queue holds
    connect(usb)
    usb.js("showSettings()")
    choose_firmware(usb, tmp_path, 0xE9, size=50000)

    usb.page.once("dialog", lambda d: d.accept())
    usb.js("uploadFirmware()")

    usb.page.wait_for_function(
        "() => document.getElementById('otaStatus').innerText"
        " === 'v9.9.9 running'", timeout=15000)

    assert usb.js("window.__usbPuts")[0]["size"] == 50000
    assert not usb.js("window.__usbOverrun === true")


def test_parser_takes_the_go_ahead_between_answers(usb):
    acks = usb.js("""() => { let n = 0;
        const p = new UsbAnswerParser(() => { }, () => n++);
        p.feed(new Uint8Array([0x06, 0x06]));
        // Inside an answer a 0x06 is data, not a go-ahead
        p.feed(new TextEncoder().encode('\\x02200 text/plain\\n'));
        p.feed(new Uint8Array([1, 0, 0x06, 0, 0]));
        return n; }""")

    assert acks == 2


def test_usb_request_gives_up_with_its_signal(usb):
    # A request to a sensor that went away must not hold up whoever
    # waits for it, e.g. the wait for a new firmware
    connect(usb)
    result = usb.js("""async () => {
        usbLink.request = () => new Promise(() => { });
        try { await api('/api/system', {signal: timeoutSignal(300)}); }
        catch (e) { return e.message; }
        return 'answered'; }""")

    assert result == "aborted"


def test_closed_link_refuses_requests(usb):
    connect(usb)
    result = usb.js("""async () => { const link = usbLink;
        window.__usbUnplug();
        try { await link.request('/api/system'); }
        catch (e) { return e.message; }
        return 'answered'; }""")

    assert result == "USB link closed"
