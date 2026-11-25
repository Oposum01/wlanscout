"""The growing capture rings in the page: Settings, Capture shows the
frames, the memory taken and the most a ring may take, and the line
under the capture panel the frames and their size. Firmware before the
frame logs reported fixed slots, and the page still talks to it over
USB, so that form is shown as before."""

from test_settings_tabs import settings

OLD_PCAP = {"count": 241, "cap": 6000, "total": 241, "beacons": 55,
            "beaconslots": 160, "beacontotal": 10246, "assoc": 3,
            "assoccap": 300, "assoctotal": 3, "bytes": 40985,
            "slot": 528, "enabled": 1, "mask": 15}


def stores(app):
    return app.js("""Object.fromEntries([...document.querySelectorAll(
        '#storeTable tr')].map(r => [r.cells[0].textContent,
        [r.cells[1].textContent,
         (r.querySelector('.fillBar i') || {style: {}}).style.width]]))""")


def kv(app, label):
    return app.js("""l => [...document.querySelectorAll('#capTable tr')]
        .find(r => r.cells[0].textContent === l).cells[1].textContent""",
                  label)


def test_rings_show_memory_taken_and_the_most_they_may_take(opened):
    opened.api["system"]["pcap"].update(
        mem=4 * 65536, dropped=0, assoc=2, assocbytes=1200,
        assocmem=65536, assocdropped=0, assoctotal=2)
    settings(opened, "capture")
    got = stores(opened)

    ring = got["Frames in the ring"]
    assert ring[0] == "241 frames (40.0 kB), 256.0 kB taken of up to 5120.0 kB"
    assert ring[1] == "5%"

    assoc = got["Association requests in their own ring"]
    assert assoc[0] == ("2 requests (1.2 kB), 64.0 kB taken of up to "
                        "1536.0 kB, 2 captured in total")

    assert got["Beacons held, newest per AP"][0] == \
        "55 of 160 slots (10246 seen in total)"

    assert kv(opened, "Buffer reserved") == \
        "320.0 kB now, in chunks of 64.0 kB, growing while PSRAM allows"


def test_dropped_frames_are_said(opened):
    opened.api["system"]["pcap"].update(mem=5242880, dropped=91234)
    settings(opened, "capture")
    ring = stores(opened)["Frames in the ring"]

    assert ring[0].endswith(", 91234 older ones dropped")
    assert ring[1] == "100%"


def test_firmware_with_fixed_slots(opened):
    opened.api["system"]["pcap"] = dict(OLD_PCAP)
    settings(opened, "capture")
    got = stores(opened)

    assert got["Frames in the ring"] == \
        ["241 of 6000 (40.0 kB of frame data)", "4%"]
    assert got["Association requests in their own ring"][0] == \
        "3 of 300 (3 captured in total)"
    assert kv(opened, "Buffer reserved") == \
        "3093.8 kB, 528 bytes per slot"


def capture_line(app):
    app.page.wait_for_function(
        "() => document.getElementById('captureInfo').innerText !== ''")
    return app.page.locator("#captureInfo").inner_text()


def test_capture_line(opened):
    line = capture_line(opened)

    assert "| 241 frames (40.0 kB) + 0 assoc + 55 beacons, 241 captured" \
        in line


def test_capture_line_fixed_slots(app):
    live = dict(app.api["live"], pcapcap=6000, assoccap=300)
    del live["pcapbytes"], live["assocbytes"]
    app.api["live"] = live
    app.open()

    assert "| 241 of 6000 frames + 0/300 assoc + 55 beacons" \
        in capture_line(app)
