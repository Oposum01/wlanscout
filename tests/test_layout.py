"""Layout on a phone: wide tables scroll inside, the page does not."""

import pytest


@pytest.mark.parametrize("tab", [
    "BSSIDs", "Stations", "RSSI spectrum",
    "IE catalog"])
def test_page_not_wider_than_phone(phone, tab):
    phone.tab(tab)
    phone.page.wait_for_timeout(300)

    assert phone.js("document.documentElement.scrollWidth") == 390


def test_every_table_is_wrapped(phone):
    loose = phone.js("""[...document.querySelectorAll('table')]
        .filter(t => !t.closest('#tip'))
        .filter(t => !t.parentElement.classList.contains('tableWrap'))
        .length""")

    assert loose == 0


def test_no_script_errors(phone):
    for tab in ["Stations", "RSSI spectrum", "IE catalog"]:
        phone.tab(tab)
        phone.page.wait_for_timeout(300)

    assert phone.errors == []


def test_status_message_under_the_header(browser):
    # A long message on a tablet squeezed the header into a column
    context = browser.new_context(viewport={"width": 1280, "height": 800})
    page = context.new_page()
    from conftest import App, load_fixtures
    app = App(page, load_fixtures()).open()

    before = app.js("document.querySelector('header').offsetHeight")

    app.js("""document.getElementById('status').innerText =
        "Not connected: plug in the sensor's USB port and press USB" """)

    assert app.js("document.querySelector('header').offsetHeight") == before
    assert app.js("document.getElementById('status').closest('header')") is None
    # One line, the full width
    assert app.js("document.getElementById('status').offsetHeight") < 30

    context.close()


def test_status_line_hidden_when_empty(opened):
    opened.js("document.getElementById('status').innerText = ''")

    assert opened.js("getComputedStyle(document.getElementById('status')).display") == "none"


def test_pause_is_the_last_button(opened):
    buttons = opened.js("""[...document.querySelectorAll('header button')]
        .map(b => b.id)""")

    assert buttons[-1] == "pauseBtn"
    assert opened.js("pauseBtn.innerText").startswith("❚❚")

    opened.js("togglePause()")
    assert opened.js("pauseBtn.innerText").startswith("▶")
    opened.js("togglePause()")


def header_rows(browser, width):
    from conftest import App, load_fixtures

    context = browser.new_context(viewport={"width": width, "height": 800})
    app = App(context.new_page(), load_fixtures()).open()

    rows = app.js("""[...document.querySelectorAll('.headerButtons button')]
        .filter(b => b.offsetParent)
        .map(b => [Math.round(b.getBoundingClientRect().top),
                   Math.round(b.getBoundingClientRect().right)])""")
    page_right = app.js("document.querySelector('header').getBoundingClientRect().right")
    overflow = app.js("document.documentElement.scrollWidth")

    context.close()

    return rows, page_right, overflow


def test_header_buttons_stay_together_on_a_tablet(browser):
    # 800 px: a tablet held upright; the buttons used to split, Wi-Fi
    # beside the title and the rest on the next line
    for width in (1280, 800):
        rows, right, _ = header_rows(browser, width)

        assert len({top for top, _ in rows}) == 1
        # Right-aligned: the last one ends near the header's edge
        assert right - rows[-1][1] < 30


def test_header_fits_a_small_phone(browser):
    rows, right, overflow = header_rows(browser, 360)

    assert overflow <= 360
    assert right - rows[-1][1] < 30


def test_view_tab_names(opened):
    names = opened.js("[...document.querySelectorAll('.tabs .tab')].map(t => t.innerText.trim())")

    assert names == ["BSSIDs", "Stations", "RSSI spectrum", "IE catalog"]


def tab_rows(browser, width):
    from conftest import App, load_fixtures

    context = browser.new_context(viewport={"width": width, "height": 800})
    app = App(context.new_page(), load_fixtures()).open()

    tops = app.js("""[...document.querySelectorAll('.tabs .tab')]
        .map(t => Math.round(t.getBoundingClientRect().top))""")
    names = app.js("[...document.querySelectorAll('.tabs .tab')].map(t => t.innerText.trim())")

    context.close()

    return set(tops), names


def test_view_tabs_on_one_line_on_a_phone(browser):
    tops, names = tab_rows(browser, 390)

    assert len(tops) == 1
    assert names == ["BSSIDs", "Stations", "RSSI spec.", "IE cat."]


def test_view_tabs_full_names_on_a_desktop(browser):
    tops, names = tab_rows(browser, 1280)

    assert names == ["BSSIDs", "Stations", "RSSI spectrum", "IE catalog"]


def test_first_round_fetches_the_tables(app):
    # The tables come in the very first round, not only in round four
    app.page.goto(app.base + "/")
    app.page.wait_for_function(
        "() => typeof networks !== 'undefined' && networks.length > 0",
        timeout=3000)

    calls = app.http_api
    first_status = calls.index("/api/status")

    assert "/api/networks" in calls
    assert calls.index("/api/networks") < first_status + 6


def test_clicks_leave_the_console_quiet(opened):
    logged = []
    opened.page.on("console", lambda m: logged.append(m.text))
    opened.js("uiAction(x => x, 1)")
    opened.page.wait_for_timeout(200)
    assert not [t for t in logged if "ui action" in t]
    # A handler that throws still shows up
    opened.js("uiAction(() => { throw new Error('boom'); })")
    assert opened.js("lastCommandError") == "click failed: boom"
