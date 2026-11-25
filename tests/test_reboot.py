"""Reboot under Settings -> Sensor: asked first, then /api/reboot and
waiting for the sensor to answer again."""
from test_settings_tabs import settings


def press(app, accept=True):
    dialogs = []

    def answer(d):
        dialogs.append(d.message)
        d.accept() if accept else d.dismiss()

    app.page.on("dialog", answer)
    settings(app)
    app.page.locator("#rebootBtn").click()
    app.page.wait_for_timeout(300)

    return dialogs


def test_asks_then_restarts(opened):
    assert press(opened) == ["Reboot the sensor?"]
    assert opened.js("document.getElementById('rebootBtn').innerText") == \
        "Reboot"
    assert "/api/reboot" in opened.http_api
    assert opened.js("document.getElementById('otaStatus').innerText") == \
        "restarting"


def test_cancelled_sends_nothing(opened):
    press(opened, accept=False)

    assert "/api/reboot" not in opened.http_api
    assert not opened.js("otaBusy")


def test_refused(app):
    app.api["reboot"] = {"ok": 0, "error": "busy"}
    app.open()
    press(app)

    assert app.js("document.getElementById('otaStatus').innerText") == \
        "reboot failed: busy"
    assert not app.js("otaBusy")


def test_back_once_it_answers(opened):
    opened.js("otaBusy = true; waitForFirmware('', Date.now(), 'x')")
    opened.page.wait_for_function(
        "() => document.getElementById('otaStatus').innerText"
        ".endsWith('running, reloading')")


def test_no_answer_says_so(opened):
    opened.page.route("**/api/system", lambda r: r.abort())
    opened.js("""waitForFirmware('', Date.now() - 121000,
        'no answer after 2 min')""")
    opened.page.wait_for_function(
        "() => document.getElementById('otaStatus').innerText === "
        "'no answer after 2 min'")
