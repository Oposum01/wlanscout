"""The CPU section in Settings: load, temperature, the longest loop()
round and request, the management queue, the tasks."""

CPU = {
    "cpu": {"win": 10, "load": 734, "tasks": [
        ["loopTask", 612, 3120], ["wifi", 85, 2208],
        ["IDLE", 266, 1460]]},
    "temp": 47.25,
    "loop": {"win": [820000, 812000, "/api/networks", 95],
             "boot": [1900000, 1850000, "/api/snapshot", 95],
             "queue": 95},
}


def cpu_rows(app, extra):
    app.api["system"] = dict(app.api["system"], **extra)
    app.js("showSettings()")
    app.js("updateSystem()")
    app.page.wait_for_function(
        "() => document.querySelectorAll('#cpuTable tr').length > 0")

    return app.js("""[...document.querySelectorAll('#cpuTable tr')]
        .map(r => [...r.cells].map(c => c.textContent))""")


def test_cpu_section(opened):
    r = dict(cpu_rows(opened, CPU))

    assert r["Load"] == "73.4 %"
    assert r["Chip temperature"] == "47.3 °C"
    assert r["Longest loop() round"] == \
        "820 ms (last 10 s), 1.9 s since start"
    assert r["Longest request"] == \
        "812 ms (/api/networks) (last 10 s), 1.9 s (/api/snapshot) since start"
    assert r["Management queue, highest fill"] == \
        "95 of 95 (last 10 s), 95 since start: it ran full, frames were dropped"

    tasks = opened.js("""[...document.querySelectorAll('#cpuTasks tr')]
        .map(r => [...r.cells].map(c => c.textContent))""")

    assert tasks[0] == ["loopTask", "61.2 %", "3120 bytes"]
    assert len(tasks) == 3


def test_first_window_not_yet_done(opened):
    r = dict(cpu_rows(opened, {"loop": {
        "win": [0, 0, "", 0], "boot": [3000, 0, "", 4], "queue": 95}}))

    assert r["Load"] == "first value after 10 s"
    assert r["Longest request"] == "- (last 10 s), - since start"
    assert r["Management queue, highest fill"] == \
        "0 of 95 (last 10 s), 4 since start"
    assert r["Chip temperature"] == "-"


def test_older_firmware(opened):
    assert cpu_rows(opened, {}) == \
        [["CPU", "not reported by this firmware"]]


def test_answers_given_up(opened):
    loop = dict(CPU["loop"], givenup=[3, 95, "192.0.2.50"])
    r = dict(cpu_rows(opened, dict(CPU, loop=loop)))

    assert r["Answers given up"] == \
        "3 since start, the last 1 min 35 s ago to 192.0.2.50"


def test_no_answer_given_up(opened):
    loop = dict(CPU["loop"], givenup=[0, 0, ""])

    assert dict(cpu_rows(opened, dict(CPU, loop=loop)))["Answers given up"] \
        == "none since start"


def test_given_up_missing_on_older_firmware(opened):
    assert "Answers given up" not in dict(cpu_rows(opened, CPU))


def test_author_address_not_harvestable(opened):
    opened.js("showSettings()")
    opened.js("updateSystem()")
    opened.page.wait_for_function(
        "() => document.querySelectorAll('#sysTable tr').length > 0")
    rows = opened.js("""[...document.querySelectorAll('#sysTable tr')]
        .map(r => [...r.cells].map(c => c.textContent))""")

    assert ["Author", "jf [at] simonwunderlich . de"] in rows
    # Nowhere in the page as a plain address
    assert "@simonwunderlich" not in opened.page.content()
