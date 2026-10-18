"""BSS colour, beacon timestamp, max idle and fast transition roams."""

from test_tooltip import tooltip
from test_wfa import station_tooltip


def notes(app, fields):
    return app.js("""f => networkNotes(Object.assign({},
        networks.find(x => x.ssid === 'Plain'), f))""", fields)


def test_bss_colour(opened):
    assert "BSS colour23" in tooltip(opened, {"color": [23, 0]})
    assert "23 (disabled)" in tooltip(opened, {"color": [23, 1]})

    text = tooltip(opened, {"color": [23, 0], "bcc": [56, 180, 3]})

    assert "switch to 56 announced 3 min 0 s ago, 3 switches seen" in text
    assert "BSS colour" not in tooltip(opened, {})


def test_colour_note_from_the_second_switch(opened):
    blue = [n for n in notes(opened, {"bcc": [56, 10, 2]})
            if "BSS colour switched" in n[1]]

    assert blue and blue[0][0] == "#7aaed0"
    assert not [n for n in notes(opened, {"bcc": [56, 10, 1]})
                if "BSS colour switched" in n[1]]


def test_uptime_from_the_timestamp(opened):
    assert "≈ 2 d" in tooltip(opened, {"tsfup": 2 * 86400 + 60})
    assert "timer not counting from the start" in \
        tooltip(opened, {"tsfup": 5 * 365 * 86400})

    text = tooltip(opened, {"tsfup": 120, "tsfreset": [1, 120]})

    assert "reset 2 min 0 s ago" in text
    assert [n for n in notes(opened, {"tsfup": 120, "tsfreset": [1, 120]})
            if "the AP restarted" in n[1]]

    # An old reset is no note
    assert not [n for n in notes(opened, {"tsfreset": [1, 7200]})
                if "the AP restarted" in n[1]]


def test_ap_max_idle(opened):
    assert "Max idle307 s, protected keep-alive" in \
        tooltip(opened, {"maxidle": [300, 1]})
    assert "Max idle" not in tooltip(opened, {})


def test_station_max_idle(opened):
    text = station_tooltip(opened, {"maxidle": [300, 1, 600, 0]})

    assert "asks 307 s, protected keep-alive · AP grants 614 s" in text


def test_fast_transition_roams(opened):
    text = station_tooltip(opened, {
        "roams": 2, "roamsok": 2, "roamsft": 1, "seen": 100,
        "roamlog": [{"ago": 50, "bssid": "02:00:00:00:00:01", "ok": 1},
                    {"ago": 10, "bssid": "02:00:00:00:00:02", "ok": 1,
                     "ft": 1}]})

    assert "2 times (2 via assoc request, 1 of them by fast transition)" \
        in text
    assert "[fast transition]" in text
    assert "[assoc request]" in text
