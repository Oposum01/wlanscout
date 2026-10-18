"""With a mouse, a click on a table row pins its tooltip, so the links
in it can be clicked."""


def tip_state(app):
    return app.js("""() => { const t = document.getElementById('tip');
        return {shown: t.style.display === 'block',
                pinned: t.classList.contains('pinned'),
                events: getComputedStyle(t).pointerEvents,
                close: getComputedStyle(t.querySelector('.tipClose')).display,
                text: t.innerText}; }""")


def row(app, i):
    return app.page.locator("#networkTable tr").nth(i)


def test_hover_says_how_to_keep_it(opened):
    row(opened, 0).hover()
    state = tip_state(opened)

    assert state["shown"] and not state["pinned"]
    assert state["events"] == "none"
    assert "click the row to keep it open" in state["text"]


def test_click_pins_and_it_stays(opened):
    row(opened, 0).click()
    first = tip_state(opened)

    assert first["pinned"]
    assert first["events"] == "auto"
    assert first["close"] == "block"
    assert "click the row to keep it open" not in first["text"]

    # The pointer goes on over other rows: the tooltip stays as it is
    row(opened, 2).hover()
    opened.page.mouse.move(5, 5)
    opened.page.wait_for_timeout(200)

    assert tip_state(opened)["text"] == first["text"]
    assert tip_state(opened)["shown"]


def test_pinned_tooltip_takes_the_mouse(opened):
    row(opened, 0).click()
    box = opened.page.locator("#tip").bounding_box()

    inside = opened.js("""([x, y]) => document.getElementById('tip')
        .contains(document.elementFromPoint(x, y))""",
        [box["x"] + box["width"] / 2, box["y"] + box["height"] / 2])

    assert inside


def test_closed_by_escape_the_cross_or_a_click_outside(opened):
    row(opened, 0).click()
    opened.page.keyboard.press("Escape")
    assert not tip_state(opened)["shown"]

    row(opened, 0).click()
    opened.page.locator("#tip .tipClose").click()
    assert not tip_state(opened)["shown"]

    row(opened, 0).click()
    opened.page.mouse.click(5, 5)
    assert not tip_state(opened)["shown"]
    assert not tip_state(opened)["pinned"]


def test_click_on_another_row_pins_that_one(opened):
    row(opened, 0).click()
    first = tip_state(opened)["text"]

    row(opened, 3).click()
    second = tip_state(opened)

    assert second["pinned"] and second["text"] != first


def test_station_rows_pin_too(opened):
    # The fixture's stations only probe: their table is on its own tab
    opened.tab("Stations")
    opened.js("updateClients()")
    opened.page.wait_for_function("() => stations.length >= 2")
    opened.js("window.showStationPane('probePane')")
    opened.page.wait_for_function(
        "() => document.querySelectorAll('#clientTableProbe tr').length > 0")

    opened.page.locator("#clientTableProbe tr").first.click()

    assert tip_state(opened)["pinned"]


def test_no_pinning_on_a_touch_screen(phone):
    r = phone.page.locator("#networkTable tr").first
    r.scroll_into_view_if_needed()
    box = r.bounding_box()
    phone.page.touchscreen.tap(box["x"] + 40, box["y"] + box["height"] / 2)
    phone.page.wait_for_timeout(300)

    state = tip_state(phone)

    assert state["shown"] and not state["pinned"]
    assert "click the row to keep it open" not in state["text"]
