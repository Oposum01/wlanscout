"""The event log in the settings view: entries of earlier starts."""

LOG = {
    "total": 2,
    "entries": [
        {"at": "2026-10-15 16:20:23", "ago": 15, "boot": 128,
         "who": "192.168.1.23", "text": "snapshot 3 named Küche"},
        {"at": "2026-10-15 16:17:13", "ago": 205, "boot": 128,
         "who": "system",
         "text": "device started, firmware v1.77.18, power on"},
        {"at": "2026-10-15 14:33:40", "ago": 6328, "boot": 127,
         "who": "192.168.1.23", "text": "firmware update to 1.77.17"},
        {"at": "41 s after a start", "ago": -1, "boot": 126,
         "who": "system",
         "text": "device started, firmware v1.77.15, crash"},
    ],
}


def rows(app):
    app.api["log"] = LOG
    app.js("updateLog()")
    app.page.wait_for_function(
        "() => document.querySelectorAll('#logTable tr').length === 4")

    return app.js("""[...document.querySelectorAll('#logTable tr')]
        .map(r => [...r.cells].map(c => c.textContent))""")


def test_entries_of_earlier_starts(opened):
    r = rows(opened)

    assert r[0][3] == "snapshot 3 named Küche"
    assert r[1][3] == "device started, firmware v1.77.18, power on"
    assert r[2][1].endswith(" ago")

    # An earlier start without a clock: its stamp, no age
    assert r[3] == ["41 s after a start", "", "system",
                    "device started, firmware v1.77.15, crash"]

    assert opened.js("document.getElementById('logInfo').innerText") == \
        "4 shown, kept across restarts; 2 since the last start"
    assert opened.errors == []


def long_log(n):
    return {"total": n, "entries": [
        {"at": "2026-10-16 12:%02d:00" % (59 - i % 60), "ago": i, "boot": 129,
         "who": "system", "text": "entry %d" % i} for i in range(n)]}


def log_page(app):
    return app.js("""({
        rows: [...document.querySelectorAll('#logTable tr')]
            .map(r => r.cells[3].textContent),
        pages: [...document.querySelectorAll('#logPages button')]
            .map(b => [b.innerText, b.classList.contains('active'),
                       b.disabled])})""")


def test_twenty_to_a_page(opened):
    opened.api["log"] = long_log(45)
    opened.js("showSettings()")
    opened.js("updateLog()")
    opened.page.wait_for_function(
        "() => document.querySelectorAll('#logTable tr').length === 20")

    p = log_page(opened)

    assert p["rows"][0] == "entry 0" and p["rows"][-1] == "entry 19"
    assert p["pages"] == [["‹ newer", False, True], ["1", True, False],
                          ["2", False, False], ["3", False, False],
                          ["older ›", False, False]]

    opened.page.locator("#logPages button", has_text="3").click()
    p = log_page(opened)

    assert p["rows"] == ["entry %d" % i for i in range(40, 45)]
    assert p["pages"][-1] == ["older ›", False, True]

    opened.page.locator("#logPages button", has_text="newer").click()

    assert log_page(opened)["rows"][0] == "entry 20"

    # A refresh keeps the page
    opened.js("updateLog()")
    opened.page.wait_for_timeout(200)

    assert log_page(opened)["rows"][0] == "entry 20"


def test_one_page_needs_no_choice(opened):
    rows(opened)

    assert log_page(opened)["pages"] == []
