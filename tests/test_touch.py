"""Rows on touch screens: no channel change on a tap, a tooltip
that can be closed and stays on screen."""


def selected(app):
    return app.js("selectedChannels()")


def test_table_row_click_keeps_the_sweep(opened):
    before = selected(opened)

    opened.page.locator("#networkTable tr").first.click()

    assert selected(opened) == before


def test_station_row_click_keeps_the_sweep(opened):
    opened.tab("Stations")
    opened.page.evaluate("updateClients()")
    opened.page.wait_for_function("() => stations.length > 0")
    opened.page.locator(".subTab", has_text="Probing only").click()

    before = selected(opened)

    opened.page.locator("#clientTableProbe tr").first.click()

    assert selected(opened) == before


def show(app):
    app.js("showTip(networkTooltip(networks[0]), 100, 100)")

    assert app.js("tip.style.display") == "block"


def test_close_button(opened):
    show(opened)

    assert opened.js("!!tip.querySelector('.tipClose')")

    opened.js("tip.querySelector('.tipClose').click()")

    assert opened.js("tip.style.display") == "none"


def tap(app, selector, pointer):
    app.js("""([sel, type]) => document.querySelector(sel).dispatchEvent(
        new PointerEvent('pointerdown', {pointerType: type, bubbles: true}))""",
           [selector, pointer])


def test_tap_elsewhere_closes(opened):
    show(opened)
    tap(opened, "header", "touch")

    assert opened.js("tip.style.display") == "none"


def test_tap_inside_keeps_it(opened):
    show(opened)
    tap(opened, "#tip", "touch")

    assert opened.js("tip.style.display") == "block"


def test_mouse_click_elsewhere_keeps_it(opened):
    # On a desktop the pointer leaving the row closes it, not a click
    show(opened)
    tap(opened, "header", "mouse")

    assert opened.js("tip.style.display") == "block"


def test_tooltip_stays_on_a_phone_screen(phone):
    width = phone.js("window.innerWidth")

    phone.js("x => showTip(networkTooltip(networks[0]), x, 100)", width - 20)

    box = phone.js("(() => { const r = tip.getBoundingClientRect(); "
                   "return [r.left, r.right]; })()")

    assert box[0] >= 0
    assert box[1] <= width


def test_tall_tooltip_scrolls_within_the_screen(phone):
    # A long one, like a Wi-Fi 7 AP with all its notes, on a small
    # phone screen: it fits the screen and scrolls inside
    phone.page.set_viewport_size({"width": 360, "height": 500})

    phone.js("""() => showTip(networkTooltip(networks[0]) +
        '<div>line</div>'.repeat(60), 100, 300)""")

    box = phone.js("""(() => { const r = tip.getBoundingClientRect();
        return [r.top, r.bottom, tip.scrollHeight > tip.clientHeight,
                getComputedStyle(tip).overflowY]; })()""")

    assert box[0] >= 0
    assert box[1] <= 500
    assert box[2]
    assert box[3] == "auto"


def test_tooltip_opens_scrolled_to_the_top(phone):
    phone.page.set_viewport_size({"width": 360, "height": 500})

    long = "networkTooltip(networks[0]) + '<div>line</div>'.repeat(60)"

    phone.js("() => showTip(" + long + ", 100, 300)")
    phone.js("tip.scrollTop = 200")
    phone.js("() => showTip(" + long + ", 100, 300)")

    assert phone.js("tip.scrollTop") == 0


def test_tall_tooltip_opened_by_a_tap_stays(phone):
    # A tooltip too tall for the space above and below the finger lands
    # under it; the mouse events the browser makes up for the tap must
    # still reach the row, or the row closes it right away
    phone.page.set_viewport_size({"width": 360, "height": 420})

    # Every row's tooltip made tall
    phone.js("""() => {
        const original = networkTooltip;
        window.networkTooltip = n => original(n) + '<div>line</div>'.repeat(40);
    }""")

    row = phone.page.locator("#networkTable tr").first
    row.scroll_into_view_if_needed()
    box = row.bounding_box()

    phone.page.touchscreen.tap(box["x"] + 40, box["y"] + box["height"] / 2)
    phone.page.wait_for_timeout(300)

    assert phone.js("tip.style.display") == "block"

    # Afterwards it takes taps again, for scrolling and its close button
    phone.page.wait_for_timeout(900)

    assert phone.js("getComputedStyle(tip).pointerEvents") != "none"


def test_layout_mouse_events_after_a_tap_leave_the_tooltip(phone):
    """After a tap the browser sends mouse events again where the finger
    was each time the page changes under it; they must neither move,
    swap nor reopen the tooltip."""
    row = phone.page.locator("#networkTable tr").first
    row.scroll_into_view_if_needed()
    box = row.bounding_box()
    phone.page.touchscreen.tap(box["x"] + 40, box["y"] + box["height"] / 2)
    phone.page.wait_for_timeout(300)

    tip = phone.page.locator("#tip")
    assert tip.is_visible()
    first = tip.inner_text()

    # A second later: a table drawn anew, another row under the finger
    phone.page.wait_for_timeout(1000)
    phone.js("""() => { const r = document.querySelectorAll('#networkTable tr');
        r[0].dispatchEvent(new MouseEvent('mouseleave'));
        r[1].dispatchEvent(new MouseEvent('mouseenter',
                                          {clientX: 40, clientY: 300}));
        r[1].dispatchEvent(new MouseEvent('mousemove',
                                          {clientX: 40, clientY: 300})); }""")

    assert tip.is_visible()
    assert tip.inner_text() == first

    # Closed by a tap elsewhere, it stays closed
    phone.page.touchscreen.tap(5, 5)
    phone.page.wait_for_timeout(1000)
    assert not tip.is_visible()
    phone.js("""() => document.querySelectorAll('#networkTable tr')[1]
        .dispatchEvent(new MouseEvent('mouseenter', {clientX: 40, clientY: 300}))""")

    assert not tip.is_visible()


def test_mouse_hover_still_opens_the_tooltip(opened):
    row = opened.page.locator("#networkTable tr").nth(0)
    row.hover()
    opened.page.wait_for_timeout(200)

    assert opened.page.locator("#tip").is_visible()
