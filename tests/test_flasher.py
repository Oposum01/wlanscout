"""The flasher (tools/static/flash.html): the checks it makes before
writing through the download mode, what "Ignore failed checks" and
"Force" let through, the board and mode a sensor's page opens it with,
and the release with a folder per board (tools/make_release.py)."""

import json
import pathlib
import subprocess
import sys

import pytest

from conftest import STATIC_BASE
from test_static import version

ROOT = pathlib.Path(__file__).resolve().parent.parent

WAVESHARE = "kit"
XIAO = "xiao"


def image(ver=None, board=WAVESHARE, chip=23, flash_mb=16, size=4096):
    """An image as the Arduino build makes it: header 0xE9, the flash
    size in byte 3, the chip id at 12, the two markers inside."""
    nibble = {1: 0, 2: 1, 4: 2, 8: 3, 16: 4, 32: 5}[flash_mb]
    head = bytes([0xE9, 5, 2, (nibble << 4) | 0x0F]) + bytes(8) + \
        bytes([chip & 0xFF, chip >> 8]) + bytes(10)
    body = b""
    if ver:
        # The firmware's own scanner keeps the prefix alone, then a 0
        body += b"\x01wlanscout-fw:\x00" + b"\x01wlanscout-fw:" + \
            ver.encode() + b"\x02"
    if board:
        body += b"\x01wlanscout-board:" + board.encode() + b"\x02"
    data = head + body
    return data + bytes(size - len(data))


def fake_board_build(path, board, flash_mb):
    path.mkdir(parents=True)
    (path / "wlanscout.ino.bootloader.bin").write_bytes(b"\xe9boot")
    (path / "wlanscout.ino.partitions.bin").write_bytes(b"\xaa\x50")
    (path / "wlanscout.ino.bin").write_bytes(
        image(version(), board, flash_mb=flash_mb))
    (path / "wlanscout.ino.merged.bin").write_bytes(b"\xe9merged")
    boot = path / "boot_app0.bin"
    boot.write_bytes(b"\xff" * 16)
    return boot


@pytest.fixture(scope="module")
def site(tmp_path_factory):
    tmp = tmp_path_factory.mktemp("flasher")
    boot = fake_board_build(tmp / "ws", WAVESHARE, 16)
    fake_board_build(tmp / "xiao", XIAO, 8)
    out = tmp / "site"

    subprocess.run([sys.executable, str(ROOT / "tools" / "make_static.py"),
                    str(out), "--firmware", str(tmp / "ws"),
                    "--firmware-xiao", str(tmp / "xiao"),
                    "--boot-app0", str(boot)],
                   check=True, capture_output=True)
    return out


@pytest.fixture
def flasher(browser, site):
    context = browser.new_context()
    page = context.new_page()

    def serve(route):
        name = route.request.url[len(STATIC_BASE) + 1:].split("?")[0]
        path = site / name
        if path.is_file():
            route.fulfill(path=str(path))
        else:
            route.fulfill(status=404, body="")

    page.route(STATIC_BASE + "/**", serve)
    errors = []
    page.on("pageerror", lambda e: errors.append(str(e)))

    def open_(query=""):
        page.goto(STATIC_BASE + "/flash.html" + query)
        page.wait_for_function("() => window.wlanscoutFlash")
        page.wait_for_function(
            "() => document.getElementById('state').textContent !== 'idle'")
        return page

    yield open_
    assert errors == []
    context.close()


def checks(page, data, board=WAVESHARE, running=None):
    return page.evaluate(
        "([d, b, r]) => window.wlanscoutFlash.checkImage("
        "new Uint8Array(d), b, r)",
        [list(data), board, running])


def levels(result, kind="check"):
    return [c["level"] for c in result["checks"] if c["kind"] == kind]


def test_a_good_image_passes(flasher):
    page = flasher()
    r = checks(page, image("2.9.0"), running="2.8.1")

    assert r["info"] == {"version": "2.9.0", "board": WAVESHARE}
    assert set(levels(r)) == {"ok"}
    assert levels(r, "version") == ["ok"]


@pytest.mark.parametrize("data,says", [
    (b"\x00" * 64, "not an ESP32 firmware image"),
    (image("2.9.0", chip=13), "an image for another chip"),
    (image(None), "not a wlanscout+ firmware"),
    (image("2.9.0", board=XIAO, flash_mb=8), "built for the Seeed Studio XIAO"),
])
def test_what_fails(flasher, data, says):
    page = flasher()
    r = checks(page, data)

    bad = [c["text"] for c in r["checks"] if c["level"] == "bad"]
    assert any(t.startswith(says) for t in bad), bad


def test_an_image_without_board_is_the_kits(flasher):
    # Before 2.8.19 no image named its board, and all were the kit's
    page = flasher()
    kit = checks(page, image("2.9.0", board=None))
    assert set(levels(kit)) == {"ok"}
    assert any("(no board in the image" in c["text"] for c in kit["checks"])

    xiao = checks(page, image("2.9.0", board=None, flash_mb=8), board=XIAO)
    bad = [c["text"] for c in xiao["checks"] if c["level"] == "bad"]
    assert any(t.startswith("built for the Waveshare") for t in bad), bad


def test_a_16_mb_image_on_the_xiao_fails(flasher):
    page = flasher()
    r = checks(page, image("2.9.0", board=XIAO, flash_mb=16), board=XIAO)

    assert any("built for 16 MB flash" in c["text"] for c in r["checks"])


def test_ignore_and_force(flasher):
    page = flasher()
    other_board = checks(page, image("2.9.0", board=XIAO, flash_mb=8))
    older = checks(page, image("2.8.0"), running="2.8.1")

    def blocked(r, ignore, force):
        return page.evaluate(
            "([c, i, f]) => window.wlanscoutFlash.blockedBy(c, i, f)",
            [r["checks"], ignore, force])

    assert "Ignore failed checks" in blocked(other_board, False, False)
    # Force alone does not pass a failed check
    assert blocked(other_board, False, True) is not None
    assert blocked(other_board, True, False) is None

    assert "Force" in blocked(older, False, False)
    # Ignore alone does not pass an older version
    assert blocked(older, True, False) is not None
    assert blocked(older, False, True) is None


def test_the_same_version_needs_force(flasher):
    page = flasher()
    r = checks(page, image("2.8.1"), running="2.8.1")

    assert levels(r, "version") == ["bad"]


def partition_table(*entries):
    data = b""
    for name, ptype, sub, offset, size in entries:
        data += b"\xaa\x50" + bytes([ptype, sub]) + \
            offset.to_bytes(4, "little") + size.to_bytes(4, "little") + \
            name.encode().ljust(16, b"\0") + bytes(4)
    return data + b"\xff" * 64


XIAO_TABLE = partition_table(("nvs", 1, 2, 0x9000, 0x5000),
                             ("app0", 0, 16, 0x10000, 0x340000),
                             ("spiffs", 1, 130, 0x350000, 0x4A0000))
WAVESHARE_TABLE = partition_table(("app0", 0, 16, 0x10000, 0x640000),
                                  ("app1", 0, 17, 0x650000, 0x640000),
                                  ("spiffs", 1, 130, 0xC90000, 0x360000))


def device(page, chip, flash, table, size, board, mode="update"):
    return page.evaluate(
        """([c, f, t, s, b, m]) => {
            const w = window.wlanscoutFlash;
            return w.checkDevice(c, f, w.parsePartitions(new Uint8Array(t)),
                                 s, b, m);
        }""", [chip, flash, list(table), size, board, mode])


def test_the_board_checked_in_download_mode(flasher):
    page = flasher()

    good = device(page, "ESP32-C5 (revision v1.0)", "8MB", XIAO_TABLE,
                  2733472, XIAO)
    assert [c["level"] for c in good] == ["ok", "ok", "ok"]
    assert "fits its slot app0 of 3.25 MB" in good[2]["text"]

    # The Waveshare release on a XIAO: its flash is too small
    small = device(page, "ESP32-C5", "8MB", XIAO_TABLE, 3069552, WAVESHARE)
    assert "the board has 8 MB flash" in small[1]["text"]

    # A 16 MB table on an 8 MB chip, as after this morning's build
    wrong = device(page, "ESP32-C5", "8MB", WAVESHARE_TABLE, 2733472, XIAO)
    assert any("needs 16 MB" in c["text"] and c["level"] == "bad"
               for c in wrong)

    big = device(page, "ESP32-C5", "8MB", XIAO_TABLE, 0x400000, XIAO)
    assert any(c["text"].startswith("larger than its slot") for c in big)

    empty = device(page, "ESP32-C5", "8MB", b"\xff" * 64, 1000, XIAO)
    assert "choose Full install" in empty[-1]["text"]

    other = device(page, "ESP32-C6", "8MB", XIAO_TABLE, 1000, XIAO)
    assert other[0]["level"] == "bad"

    # A full install brings its own table
    full = device(page, "ESP32-C5", "8MB", WAVESHARE_TABLE, 2733472, XIAO,
                  "full")
    assert [c["level"] for c in full] == ["ok", "ok"]


def test_opened_from_a_xiao_updates_its_release(flasher):
    page = flasher("?board=xiao&running=2.0.0")

    assert page.evaluate("document.getElementById('board').value") == XIAO
    assert page.evaluate(
        "document.querySelector('input[name=mode]:checked').value") \
        == "update"
    page.wait_for_function(
        "() => document.querySelectorAll('#checks li').length >= 4")
    texts = page.evaluate(
        "[...document.querySelectorAll('#checks li')].map(l => l.className"
        " + ' ' + l.textContent)")

    assert "ok built for the Seeed Studio XIAO ESP32-C5" in texts
    assert "ok newer than the running v2.0.0" in texts
    assert page.evaluate("document.getElementById('state').textContent") \
        == "ready"


def test_on_its_own_a_full_install(flasher):
    page = flasher()

    assert page.evaluate("document.getElementById('board').value") \
        == WAVESHARE
    assert page.evaluate(
        "document.querySelector('input[name=mode]:checked').value") == "full"


def test_a_file_for_another_board_needs_ignore(flasher):
    page = flasher("?board=xiao&running=2.0.0")
    page.wait_for_function("() => document.getElementById('state')"
                           ".textContent === 'ready'")

    # The XIAO's sensor, a Waveshare image chosen by mistake
    page.set_input_files("#file", files=[{
        "name": "wlanscout.ino.bin", "mimeType": "application/octet-stream",
        "buffer": image("2.9.0", WAVESHARE)}])
    page.wait_for_function("() => document.getElementById('state')"
                           ".textContent.startsWith('a check failed')")
    assert page.evaluate("document.querySelector("
                         "'input[name=source]:checked').value") == "file"

    page.check("#ignore")
    page.wait_for_function("() => document.getElementById('state')"
                           ".textContent === 'ready'")


def test_the_site_has_a_folder_per_board(site):
    page = (site / "flash.html").read_text()

    assert (site / "wlanscout.ino.bin").is_file()
    assert (site / "xiao" / "wlanscout.ino.bin").is_file()
    assert (site / "xiao" / "boot_app0.bin").is_file()
    assert 'release: true }' in page and '"@XIAO@"' not in page
    assert "@VERSION@" not in page and "@PARTS_DIR@" not in page


def release(tmp_path, xiao_board=XIAO):
    ws = tmp_path / "ws"
    boot = fake_board_build(ws, WAVESHARE, 16)
    fake_board_build(tmp_path / "xiao", xiao_board, 8)
    out = tmp_path / "repo"
    out.mkdir()
    return out, subprocess.run(
        [sys.executable, str(ROOT / "tools" / "make_release.py"), str(ws),
         "--xiao", str(tmp_path / "xiao"), "--boot-app0", str(boot),
         "--out", str(out)], capture_output=True, text=True)


def test_release_with_the_xiao(tmp_path):
    out, result = release(tmp_path)

    assert result.returncode == 0, result.stderr
    xiao = out / "firmware" / "xiao"
    assert sorted(p.name for p in xiao.iterdir()) == [
        "FLASH.txt", "boot_app0.bin", "version.json", "wlanscout.ino.bin",
        "wlanscout.ino.bootloader.bin", "wlanscout.ino.merged.bin",
        "wlanscout.ino.partitions.bin"]
    assert json.loads((xiao / "version.json").read_text())["board"] == XIAO
    assert json.loads((out / "firmware" / "version.json").read_text())[
        "board"] == WAVESHARE
    assert "Seeed Studio XIAO ESP32-C5" in (xiao / "FLASH.txt").read_text()
    assert 'const PARTS_DIR = "firmware/";' in (out / "flash.html").read_text()


def test_release_refuses_a_xiao_build_for_another_board(tmp_path):
    _, result = release(tmp_path, xiao_board=WAVESHARE)

    assert result.returncode != 0
    assert "built for kit, not xiao" in result.stderr


# ---- The sensor's page ----

from test_settings_tabs import settings  # noqa: E402


def test_no_ota_offers_the_flasher(opened):
    sys_ = opened.api["system"]
    del sys_["ota"]
    sys_["boardid"] = XIAO
    sys_["version"] = "2.8.1"
    settings(opened, "sensor")

    button = opened.page.locator("#otaFlasher")
    assert button.is_visible()
    assert button.inner_text() == "Update in download mode"
    assert opened.page.locator("#otaFetch").is_hidden()
    assert opened.js("flasherUrl()").endswith(
        "flash.html?board=xiao&running=2.8.1")
    # The XIAO's releases are in their own folder
    assert opened.js("updateBase()").endswith("/firmware/xiao/")


def test_with_ota_no_flasher(opened):
    settings(opened, "sensor")

    assert opened.page.locator("#otaFlasher").is_hidden()
    assert opened.js("updateBase()").endswith("/firmware/")
