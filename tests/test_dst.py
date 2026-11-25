"""Daylight saving: the standard offset plus a rule, the summer hour
added by the date of each time, as the firmware does."""
from calendar import timegm

import pytest

from test_settings_tabs import settings


def utc(*t):
    return timegm(t + (0,) * (6 - len(t)))


def offset(app, tz, dst, epoch):
    app.js("([tz, dst]) => { lastStatus = Object.assign({}, lastStatus, "
           "{tz: tz, dst: dst}); }", [tz, dst])

    return app.js("e => tzOffsetSeconds(e) / 60", epoch)


# rule, standard offset, the last second before a switch, the offset
# then and after it
SWITCHES = [
    (1, 60, utc(2026, 3, 29, 0, 59, 59), 60, 120),
    (1, 60, utc(2026, 10, 25, 0, 59, 59), 120, 60),
    (1, 120, utc(2027, 3, 28, 0, 59, 59), 120, 180),
    (2, -300, utc(2026, 3, 8, 6, 59, 59), -300, -240),
    (2, -300, utc(2026, 11, 1, 5, 59, 59), -240, -300),
    (3, 600, utc(2026, 4, 4, 15, 59, 59), 660, 600),
    (3, 600, utc(2026, 10, 3, 15, 59, 59), 600, 660),
    (4, 720, utc(2026, 4, 4, 13, 59, 59), 780, 720),
    (4, 720, utc(2026, 9, 26, 13, 59, 59), 720, 780),
]


@pytest.mark.parametrize("rule,tz,before,was,now", SWITCHES)
def test_the_switch_to_the_second(opened, rule, tz, before, was, now):
    assert offset(opened, tz, rule, before) == was
    assert offset(opened, tz, rule, before + 1) == now


def test_none_keeps_the_offset(opened):
    assert offset(opened, 120, 0, utc(2026, 1, 15)) == 120
    assert offset(opened, 120, 0, utc(2026, 7, 15)) == 120
    assert offset(opened, 60, 1, utc(2026, 1, 15)) == 60
    assert offset(opened, 60, 1, utc(2026, 7, 15)) == 120


def test_before_the_clock_was_set(opened):
    assert offset(opened, 60, 1, 1000) == 60


def test_each_time_by_its_own_date(opened):
    offset(opened, 60, 1, 0)

    assert opened.js("stampText(%d, 0)" % utc(2026, 7, 1, 10)) == \
        "2026-07-01 12:00"
    assert opened.js("stampText(%d, 0)" % utc(2026, 12, 1, 10)) == \
        "2026-12-01 11:00"


def test_graph_times_in_the_sensors_zone(opened):
    offset(opened, 60, 1, 0)

    # 2026-10-25 00:59 UTC, then a step of an hour across the switch
    w = "{epoch: %d, step: 3600, next: 2}" % utc(2026, 10, 25, 0, 59)

    assert opened.js("graphTimeText(%s, 0)" % w) == "02:59"
    assert opened.js("graphTimeText(%s, 1)" % w) == "02:59"


def test_rule_picked_and_shown(app):
    app.api["system"] = dict(app.api["system"], dst=1)
    app.open()
    sent = []
    app.page.on("request", lambda r: sent.append(r.url))
    settings(app)

    assert app.js("document.getElementById('dstSelect').value") == "1"
    assert app.js("""document.getElementById('dstSelect')
        .previousElementSibling.innerText""") == "Auto daylight saving"
    assert app.js("document.querySelector('#dstSelect option').innerText") \
        == "off"

    app.page.select_option("#dstSelect", "2")
    app.page.wait_for_timeout(500)

    assert any(u.endswith("/api/mode?dst=2") for u in sent)


def time_rows(app, **sys):
    app.api["system"] = dict(app.api["system"], **sys)
    app.open()
    settings(app)

    return app.js("""Object.fromEntries([...document.querySelectorAll(
        '#timeTable tr')].map(r => [r.cells[0].innerText,
        r.cells[1].innerText]))""")


def test_auto_from_the_majority(app):
    rows = time_rows(app, tz=60, dst=1, tzlabel="UTC+01:00", tzauto=1,
                     tzcc="DE", tzccn=12, tzccof=14, tzccok=1)

    assert app.js("document.getElementById('tzSelect').value") == "auto"
    assert rows["Timezone"] == ("UTC+01:00, daylight saving of Europe, "
                                "from DE, the country code of 12 of 14 APs")


def test_auto_country_with_several_zones(app):
    rows = time_rows(app, tz=0, dst=0, tzauto=1, tzcc="US", tzccn=6,
                     tzccof=8, tzccok=0)

    assert rows["Timezone"] == ("US, the country code of 6 of 8 APs, has "
                                "several timezones: pick one above")


def test_auto_without_a_majority_yet(app):
    rows = time_rows(app, tz=60, dst=1, tzlabel="UTC+01:00", tzauto=1)

    assert rows["Timezone"].startswith("UTC+01:00 as last set, until more")


def test_picked_by_hand_no_auto_row(app):
    rows = time_rows(app, tz=120, dst=0, tzauto=0)

    assert app.js("document.getElementById('tzSelect').value") == "120"
    assert "Timezone" not in rows


def test_auto_sent(app):
    app.api["system"] = dict(app.api["system"], tzauto=0)
    app.open()
    sent = []
    app.page.on("request", lambda r: sent.append(r.url))
    settings(app)
    app.page.select_option("#tzSelect", "auto")
    app.page.wait_for_timeout(500)

    assert any(u.endswith("/api/mode?tz=auto") for u in sent)
