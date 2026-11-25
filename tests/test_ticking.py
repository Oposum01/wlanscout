"""Times that count on by themselves between two answers of the
sensor: one second a second, not standing still or jumping by two."""
from test_settings_tabs import settings


def at(app, t):
    """Stops the page's clock at second t and lets the times tick"""
    app.js("t => { performance.now = () => t * 1000; tickAll(); }", t)


def row(app, table, label):
    return app.js("""([t, l]) => [...document.querySelectorAll(
        '#' + t + ' tr')].find(r => r.cells[0].innerText === l)
        .cells[1].innerText""", [table, label])


def sensor_at(app, t, **sys):
    """Settings at second t, after an answer sent then"""
    app.js("t => { performance.now = () => t * 1000; }", t)
    app.api["system"] = dict(app.api["system"], **sys)
    app.js("""() => { lastStatus = Object.assign({}, lastStatus,
        {tz: 60, dst: 0}); }""")
    settings(app)


def test_sensor_times_tick(opened):
    sensor_at(opened, 1000, uptime=235, now=1792247552, ntpago=230,
              tzlabel="UTC+01:00")

    assert row(opened, "timeTable", "Device time") == \
        "2026-10-17 15:32:32 UTC+01:00"
    assert row(opened, "sysTable", "Uptime") == "3 min 55 s"
    assert ", 3 min 50 s ago" in row(opened, "timeTable", "Last clock sync")

    at(opened, 1001)

    assert row(opened, "timeTable", "Device time") == \
        "2026-10-17 15:32:33 UTC+01:00"
    assert row(opened, "sysTable", "Uptime") == "3 min 56 s"
    assert ", 3 min 51 s ago" in row(opened, "timeTable", "Last clock sync")


def test_sensor_times_kept_across_answers(opened):
    sensor_at(opened, 1000, uptime=235, now=1792247552)

    # 1.8 s later, cut to whole seconds by the sensor: still one a second
    sensor_at(opened, 1001.8, uptime=237, now=1792247554)

    assert row(opened, "sysTable", "Uptime") == "3 min 57 s"
    assert row(opened, "timeTable", "Device time").startswith(
        "2026-10-17 15:32:34")

    at(opened, 1002.6)

    assert row(opened, "sysTable", "Uptime") == "3 min 58 s"
    assert opened.errors == []


def test_clock_set_anew(opened):
    sensor_at(opened, 1000, now=1792247552)
    sensor_at(opened, 1002, now=1792247000)

    assert row(opened, "timeTable", "Device time").startswith(
        "2026-10-17 15:23:20")


def test_device_time_from_older_firmware(opened):
    sensor_at(opened, 1000, tzlabel="UTC+01:00")
    at(opened, 1005)

    assert row(opened, "timeTable", "Device time") == \
        "2026-10-11 14:22:32 UTC+01:00"


def network_age(app):
    return app.js("""(() => {
        const i = columns.findIndex(c => c.key === 'age');
        return document.querySelector('#networkTable tr').cells[i]
            .textContent; })()""")


def networks_at(app, t, age):
    app.js("t => { performance.now = () => t * 1000; }", t)
    app.api["networks"] = dict(app.api["networks"], networks=[
        dict(n, age=age) for n in app.api["networks"]["networks"][:1]])
    app.js("updateNetworks()")
    app.page.wait_for_function("""a => networks.length === 1 &&
        networks[0].age === a""", arg=age)


def test_last_seen_ticks_between_answers(opened):
    networks_at(opened, 1000, 3)

    assert network_age(opened) == "3 s"

    at(opened, 1001)
    assert network_age(opened) == "4 s"

    at(opened, 1002)
    assert network_age(opened) == "5 s"

    # Cut to 6 at 6.9 s: kept, no step back
    networks_at(opened, 1003.4, 6)
    assert network_age(opened) == "6 s"

    # A beacon came: back to 0
    networks_at(opened, 1005, 0)
    assert network_age(opened) == "0 s"


def test_last_seen_stands_while_paused(opened):
    networks_at(opened, 1000, 3)
    opened.js("paused = true")
    at(opened, 1001)
    at(opened, 1010)

    assert network_age(opened) == "4 s"

    opened.js("paused = false")


def test_every_age_column(opened):
    opened.js("t => { performance.now = () => t * 1000; }", 1000)

    texts = opened.js("""() => {
        tickAnchor('sta|S', 7); tickAnchor('ble|B', 7);
        tickAnchor('zb|Z', 7);
        performance.now = () => 1002 * 1000;
        return [clientCellText({mac: 'S', age: 7}, 'age'),
                bleCellText({addr: 'B', age: 7}, 'age'),
                zbCellText({addr: 'Z', age: 7}, 'age')]; }""")

    assert texts == ["9 s", "9 s", "9 s"]


def test_snapshot_ages_as_taken(opened):
    opened.js("t => { performance.now = () => t * 1000; }", 1000)

    assert opened.js("""() => {
        tickAnchor('net|X', 7);
        performance.now = () => 1050 * 1000;
        snapshotView = true;
        const t = cellText({bssid: 'X', age: 7}, 'age');
        snapshotView = false;
        return t; }""") == "7 s"


def test_turns_soon_after_its_second(opened):
    # Uptime 235 sent at 1000.3 turns to 236 at 1000.8; the page's own
    # timer has to show it within its round, not up to a second later
    sensor_at(opened, 1000.3, uptime=235)
    at(opened, 1000.7)

    assert row(opened, "sysTable", "Uptime") == "3 min 55 s"

    opened.js("() => { performance.now = () => 1000.85 * 1000; }")
    opened.page.wait_for_timeout(300)

    assert row(opened, "sysTable", "Uptime") == "3 min 56 s"
