"""The Beacons tab of a pinned network tooltip: the versions the sensor
kept, newest first, each with what changed from the one before; no tab
for firmware without beacon versions; their row in Settings, Capture."""
import base64

from test_beacon_versions import HT_OP, HT_OP_2, SSID, beacon, el, tim
from test_settings_tabs import settings


def b64(hexframe):
    return base64.b64encode(bytes.fromhex(hexframe)).decode()


def tabs(app):
    return app.js("""() => [...document.querySelectorAll('#tip .tipTab')]
        .map(t => t.innerText)""")


def with_versions(app, versions, **extra):
    # Every BSS, whichever row comes first
    for n in app.api["networks"]["networks"]:
        n["bvn"] = len(versions)
    app.api["beaconversions"] = dict(
        {"bssid": "02:00:00:00:00:01", "changes": len(versions) - 1,
         "current": versions[-1]["seq"], "volatile": [],
         "versions": versions}, **extra)
    app.open()
    app.page.locator("#networkTable tr").first.click()


def beacons_tab(app):
    app.page.locator("#tip .tipTab", has_text="Beacons").click()
    app.page.wait_for_function(
        "() => document.querySelector('#tip .tipBeacons')"
        ".innerText.includes('BEACON VERSIONS')")
    return app.js("document.querySelector('#tip .tipBeacons').innerText")


COUNTRY = el(7, b"DE ".hex() + "010d14")


def test_no_tab_without_beacon_versions(opened):
    opened.page.locator("#networkTable tr").first.click()

    assert tabs(opened) == ["Details", "Graphs"]


def test_versions_newest_first_with_what_changed(app):
    v1 = beacon(SSID, tim(0), HT_OP, tsf=1000)
    v2 = beacon(SSID, tim(2), COUNTRY, HT_OP_2, tsf=9000)

    with_versions(app, [
        {"seq": 1, "ago": 600, "since": 600, "times": 1, "len": 70,
         "frame": b64(v1)},
        {"seq": 2, "ago": 30, "since": 30, "times": 1, "len": 76,
         "frame": b64(v2)}])

    assert tabs(app) == ["Details", "Graphs", "Beacons"]

    text = beacons_tab(app)

    assert "2 kept, 1 change since the sensor started" in text
    assert text.index("v2 (current)") < text.index("v1")
    assert "+ 7 Country Information" in text
    assert "~ 61 HT Operation" in text
    # Timestamp and TIM differ too, but change with every beacon
    assert app.js("elementName(5)") not in text
    assert "beacon interval" not in text
    assert "the first beacon seen" in text

    # The download of exactly these
    href = app.js("document.querySelector('#tip .tipBeacons a.dl')"
                  ".getAttribute('href')")
    assert href.startswith("/api/pcap?src=versions&ta=")


def test_capabilities_named_apart_from_the_interval(app):
    # Only the Short Preamble bit flips, as vogtland.freifunk.net does
    v1 = beacon(SSID, HT_OP, tsf=1000)
    v2 = v1.replace("64001104", "64003104", 1)
    v3 = v1.replace("64001104", "c8001104", 1)

    with_versions(app, [
        {"seq": 1, "ago": 600, "since": 600, "times": 1, "len": 60,
         "frame": b64(v1)},
        {"seq": 2, "ago": 300, "since": 300, "times": 1, "len": 60,
         "frame": b64(v2)},
        {"seq": 3, "ago": 30, "since": 30, "times": 1, "len": 60,
         "frame": b64(v3)}])

    text = beacons_tab(app)
    v2_part = text[text.index("v2"):text.index("v1")]
    v3_part = text[text.index("v3 (current)"):text.index("v2")]

    assert "~ capabilities" in v2_part
    assert "beacon interval" not in v2_part
    assert "~ beacon interval" in v3_part
    assert "capabilities" in v3_part     # v2's back to v1's


def test_a_version_back_again_and_a_frame_gone(app):
    v1 = beacon(SSID, HT_OP)
    v2 = beacon(SSID, HT_OP_2)

    with_versions(app, [
        {"seq": 1, "ago": 900, "since": 20, "times": 2, "len": 60,
         "frame": b64(v1)},
        {"seq": 3, "ago": 300, "since": 300, "times": 1, "len": 60,
         "frame": b64(v2)},
        {"seq": 4, "ago": 100, "since": 100, "times": 1, "len": 60}],
        current=1, changes=4)

    text = beacons_tab(app)

    assert "v1 (current)" in text
    assert "current again" in text and "2 times" in text
    assert "its frame is no longer kept" in text


def test_volatile_elements_named_and_left_out(app):
    def vendor(n):
        return el(221, "001122" + "07" + "%08x" % n)

    v1 = beacon(SSID, HT_OP, vendor(1))
    v2 = beacon(SSID, HT_OP_2, vendor(9))

    with_versions(app, [
        {"seq": 1, "ago": 60, "since": 60, "times": 1, "len": 70,
         "frame": b64(v1)},
        {"seq": 2, "ago": 10, "since": 10, "times": 1, "len": 70,
         "frame": b64(v2)}])

    key = app.js("bvKeyOf(221, [0x00, 0x11, 0x22, 7])")
    app.api["beaconversions"]["volatile"] = [key]

    text = beacons_tab(app)

    assert "left out as changing by themselves: 221 vendor 001122 type 7" \
        in text
    assert "~ 61 HT Operation" in text
    assert "~ 221" not in text


def test_the_page_computes_the_sensors_keys(opened):
    # The same folding as bvKeyOf() in the firmware
    oui, kind = 0x0050F2, 2
    x = oui * 31 + kind
    assert opened.js("bvKeyOf(221, [0x00, 0x50, 0xF2, 2])") == \
        0x8000 | ((x ^ (x >> 15)) & 0x7FFF)
    assert opened.js("bvKeyOf(255, [107])") == 0x100 | 107
    assert opened.js("bvKeyOf(61, [])") == 61


def test_capture_row_only_with_firmware_that_keeps_them(app):
    app.open()
    settings(app, "capture")
    labels = app.js("""[...document.querySelectorAll('#storeTable tr')]
        .map(r => r.cells[0].textContent)""")
    assert "Beacon versions, each content kept" not in labels

    app.api["system"]["pcap"].update(
        {"bv": 3, "bvbytes": 1200, "bvmem": 65536, "bvmax": 1048576,
         "bvtotal": 3, "bvdropped": 0})
    app.js("updateSystem()")
    app.page.wait_for_function("""() => [...document.querySelectorAll(
        '#storeTable tr')].some(r => r.cells[0].textContent
        .startsWith('Beacon versions'))""")

    row = app.js("""[...document.querySelectorAll('#storeTable tr')]
        .find(r => r.cells[0].textContent.startsWith('Beacon versions'))
        .innerText""")
    assert "3 beacons" in row and "download all" in row
