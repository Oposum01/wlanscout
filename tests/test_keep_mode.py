"""Remember capture mode in Settings → Sensor: shown only by firmware
that has it, ticked as the sensor says, and sent to /api/mode."""
from test_settings_tabs import settings


def test_hidden_with_older_firmware(opened):
    settings(opened, "sensor")

    assert opened.page.locator("#keepModeBox").is_hidden()


def test_shown_as_the_sensor_says(app):
    app.api["system"]["counters"]["keepmode"] = 1
    app.open()
    settings(app, "sensor")

    assert app.page.locator("#keepModeBox").is_visible()
    assert app.js("document.getElementById('keepMode').checked")


def test_sent_to_the_sensor(app):
    app.api["system"]["counters"]["keepmode"] = 0
    app.open()
    settings(app, "sensor")

    assert not app.js("document.getElementById('keepMode').checked")
    app.page.locator("#keepMode").check()
    app.page.wait_for_function("""() => performance.getEntriesByType(
        'resource').some(e => e.name.includes('/api/mode?keepmode=1'))""")
