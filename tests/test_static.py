"""The static copy from tools/make_static.py: an installable page."""

import json
import pathlib
import subprocess
import sys

import pytest

from conftest import STATIC_BASE

ROOT = pathlib.Path(__file__).resolve().parent.parent


@pytest.fixture(scope="module")
def built(tmp_path_factory):
    out = tmp_path_factory.mktemp("static")

    subprocess.run([sys.executable, str(ROOT / "tools" / "make_static.py"),
                    str(out), "--page", "app.html"],
                   check=True, capture_output=True)

    return out


def test_files(built):
    names = sorted(p.name for p in built.iterdir())

    assert names == ["app.html", "favicon.svg", "icon-192.png",
                     "icon-512.png", "manifest.webmanifest", "sw.js"]


def test_manifest_makes_it_installable(built):
    m = json.loads((built / "manifest.webmanifest").read_text())

    # What Chrome wants before it offers "Install app"
    assert m["name"] and m["short_name"]
    # The name under the icon is the product's, with its plus
    assert m["name"] == m["short_name"] == "wlanscout+"
    assert m["start_url"] == "./app.html"
    assert m["display"] == "standalone"
    sizes = {i["sizes"] for i in m["icons"]}
    assert {"192x192", "512x512"} <= sizes


def test_icons_are_the_sizes_they_claim(built):
    for name, size in (("icon-192.png", 192), ("icon-512.png", 512)):
        data = (built / name).read_bytes()

        assert data[:8] == b"\x89PNG\r\n\x1a\n"
        # IHDR: width and height, big endian
        assert int.from_bytes(data[16:20], "big") == size
        assert int.from_bytes(data[20:24], "big") == size


def test_service_worker_keeps_every_file(built):
    sw = (built / "sw.js").read_text()
    version = (ROOT / "source" / "wlanscout" / "wlanscout.ino").read_text().split(
        '#define FW_VERSION "')[1].split('"')[0]

    assert 'const CACHE = "wlanscout-%s"' % version in sw
    for name in ("app.html", "manifest.webmanifest", "favicon.svg",
                 "icon-192.png", "icon-512.png"):
        assert '"./%s"' % name in sw


def test_page_links_manifest_and_registers_worker(built):
    html = (built / "app.html").read_text()

    assert '<link rel="manifest" href="manifest.webmanifest">' in html
    assert 'register("sw.js")' in html
    # Relative, so it works in any folder of a web space
    assert 'href="/favicon.svg"' not in html


def test_sensor_page_stays_without_them():
    source = (ROOT / "source" / "wlanscout" / "index_html.h").read_text()

    assert "manifest.webmanifest" not in source
    assert "serviceWorker" not in source


def test_static_page_runs(browser, built):
    context = browser.new_context()
    page = context.new_page()

    # Record the registration instead of running a worker
    context.add_init_script("""
        window.__registered = [];
        if (navigator.serviceWorker)
            navigator.serviceWorker.register = url => {
                window.__registered.push(url);
                return Promise.resolve({});
            };
    """)

    def serve(route):
        name = route.request.url[len(STATIC_BASE) + 1:].split("?")[0]
        path = built / name

        if path.is_file():
            route.fulfill(path=str(path))
        else:
            route.fulfill(status=404, body="")

    page.route(STATIC_BASE + "/**", serve)

    errors = []
    page.on("pageerror", lambda e: errors.append(str(e)))

    page.goto(STATIC_BASE + "/app.html")
    page.wait_for_function("() => typeof networks !== 'undefined'")
    page.wait_for_timeout(500)

    assert errors == []
    assert page.evaluate("isStaticPage()") is True
    if page.evaluate("'serviceWorker' in navigator"):
        assert page.evaluate("window.__registered") == ["sw.js"]

    context.close()


def version():
    return (ROOT / "source" / "wlanscout" / "wlanscout.ino").read_text().split(
        '#define FW_VERSION "')[1].split('"')[0]


def fake_build(path, ver):
    path.mkdir()
    (path / "wlanscout.ino.bootloader.bin").write_bytes(b"\xe9boot")
    (path / "wlanscout.ino.partitions.bin").write_bytes(b"\xaa\x50")
    (path / "wlanscout.ino.bin").write_bytes(b"\xe9app " + ver.encode())
    boot = path / "boot_app0.bin"
    boot.write_bytes(b"\xff" * 16)

    return boot


def run(out, build, boot):
    return subprocess.run([sys.executable,
                           str(ROOT / "tools" / "make_static.py"), str(out),
                           "--page", "app.html", "--firmware", str(build),
                           "--boot-app0", str(boot)],
                          capture_output=True, text=True)


def test_flasher_with_the_firmware(tmp_path):
    boot = fake_build(tmp_path / "build", version())
    out = tmp_path / "site"

    assert run(out, tmp_path / "build", boot).returncode == 0

    for name in ("flash.html", "wlanscout.ino.bootloader.bin",
                 "wlanscout.ino.partitions.bin", "boot_app0.bin",
                 "wlanscout.ino.bin"):
        assert (out / name).is_file()

    page = (out / "flash.html").read_text()

    assert "v" + version() in page
    assert "@VERSION@" not in page
    assert "<title>wlanscout+ flasher</title>" in page
    # Each part to its address, never the padded merged image
    for part, address in (("wlanscout.ino.bootloader.bin", "0x2000"),
                          ("wlanscout.ino.partitions.bin", "0x8000"),
                          ("boot_app0.bin", "0xe000"),
                          ("wlanscout.ino.bin", "0x10000")):
        assert '"%s",' % part in page and address in page
    assert "merged.bin" not in page.split("<script")[1]
    # Not kept for offline use: it needs esptool-js from the network
    assert "flash.html" not in (out / "sw.js").read_text()


def test_flasher_refuses_another_version(tmp_path):
    boot = fake_build(tmp_path / "build", "0.0.1")

    result = run(tmp_path / "site", tmp_path / "build", boot)

    assert result.returncode != 0
    assert "build it first" in result.stderr


def test_parts_in_a_folder_of_their_own(tmp_path):
    build = tmp_path / "build"
    boot = fake_build(build, version())
    out = tmp_path / "out"

    result = subprocess.run([sys.executable,
                             str(ROOT / "tools" / "make_static.py"),
                             str(out), "--firmware", str(build),
                             "--boot-app0", str(boot),
                             "--parts-dir", "firmware"],
                            capture_output=True, text=True)

    assert result.returncode == 0, result.stderr

    page = (out / "flash.html").read_text()

    assert 'const PARTS_DIR = "firmware/";' in page
    assert "fetch(PARTS_DIR + part.file" in page
    assert not (out / "wlanscout.ino.bin").exists()
    assert not (out / "boot_app0.bin").exists()


def test_parts_next_to_the_page_by_default(tmp_path):
    build = tmp_path / "build"
    boot = fake_build(build, version())
    out = tmp_path / "out"

    assert run(out, build, boot).returncode == 0
    assert 'const PARTS_DIR = "";' in (out / "flash.html").read_text()
