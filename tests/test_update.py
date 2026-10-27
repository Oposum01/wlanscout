"""New firmware from GitHub Pages: asked once a session and by button,
fetched on a click and written on another."""

import hashlib


def settings(app, running="2.7.0", latest=None, image=None):
    app.api["system"]["version"] = running

    if latest:
        app.updates["version.json"] = {
            "version": latest, "size": len(image or b""),
            "sha256": hashlib.sha256(image or b"").hexdigest()}

    if image is not None:
        app.updates["wlanscout.ino.bin"] = image

    app.open()
    app.js("showSettings()")


def text(app):
    return app.js("document.getElementById('otaUpdate').innerText")


def shown(app, button):
    return app.js("b => document.getElementById(b).style.display !== 'none'",
                  button)


def test_newer_release_offered_once_a_session(app):
    settings(app, latest="2.7.1", image=b"\xe9" * 2048)

    app.page.wait_for_function(
        "() => document.getElementById('otaUpdate').innerText !== ''")

    assert text(app) == "v2.7.1 available, this sensor runs v2.7.0"
    assert shown(app, "otaFetch") and not shown(app, "otaInstall")
    assert app.js("document.getElementById('otaFetch').innerText") == \
        "Download v2.7.1"

    # Settings again, and a reload: not asked again in this session
    app.js("showSettings()")
    app.page.reload()
    app.page.wait_for_function("() => typeof networks !== 'undefined'")
    app.js("showSettings()")
    app.page.wait_for_timeout(500)

    assert app.update_requests.count("version.json") == 1


def test_up_to_date_only_says_so_when_asked(app):
    settings(app, latest="2.7.0", image=b"\xe9" * 16)
    app.page.wait_for_function("() => updateLatest !== null")

    assert text(app) == ""
    assert not shown(app, "otaFetch")

    app.js("checkFirmwareUpdate(true)")
    app.page.wait_for_function(
        "() => document.getElementById('otaUpdate').innerText.includes('newest')")

    assert text(app) == "v2.7.0 is the newest release"


def test_no_internet_quiet_unless_asked(app):
    settings(app)
    app.page.wait_for_function("() => typeof updateLatest !== 'undefined'")
    app.page.wait_for_timeout(500)

    assert text(app) == ""

    app.js("checkFirmwareUpdate(true)")
    app.page.wait_for_function(
        "() => document.getElementById('otaUpdate').innerText.includes('GitHub')")

    assert text(app).startswith("GitHub not reachable from this browser")
    assert not shown(app, "otaFetch")


def test_download_then_install(app):
    image = b"\xe9" + b"\x00" * 4095
    settings(app, latest="2.7.1", image=image)
    app.page.wait_for_function("() => updateLatest !== null")

    app.js("fetchFirmwareUpdate()")
    app.page.wait_for_function("() => updateFile !== null")

    assert text(app).startswith("v2.7.1 downloaded, 4 kB")
    assert shown(app, "otaInstall") and not shown(app, "otaFetch")
    assert app.js("document.getElementById('otaInstall').innerText") == \
        "Install v2.7.1"

    # Written the way a chosen file is, after the same question
    app.page.on("dialog", lambda d: d.accept())
    app.js("installFirmwareUpdate()")
    app.page.wait_for_function(
        "() => document.getElementById('otaStatus').innerText !== ''")

    assert "/api/ota" in app.http_api


def test_size_other_than_announced_refused(app):
    settings(app, latest="2.7.1", image=b"\xe9" * 100)
    app.updates["version.json"]["size"] = 200
    app.js("checkFirmwareUpdate(true)")
    app.page.wait_for_function("() => updateLatest && updateLatest.size === 200")

    app.js("fetchFirmwareUpdate()")
    app.page.wait_for_function(
        "() => document.getElementById('otaUpdate').innerText.includes('failed')")

    assert text(app).startswith("download failed: 100 bytes instead of 200")
    assert not shown(app, "otaInstall")


def test_versions_compared_by_number(opened):
    assert opened.js("versionNewer('2.10.0', '2.9.3')") is True
    assert opened.js("versionNewer('2.7.1', '2.7.0')") is True
    assert opened.js("versionNewer('2.7.0', '2.7.0')") is False
    assert opened.js("versionNewer('2.6.27', '2.7.0')") is False
