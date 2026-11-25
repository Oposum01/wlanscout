"""The reported airtime hatch: which channel its tooltip is about."""

import pytest


@pytest.mark.parametrize("f0, f1, text", [
    (2426, 2448, "channel 6"),
    (2402, 2422, "channel 1"),
    (5170, 5250, "channels 36–48"),
    (5170, 5190, "channel 36"),
    (5490, 5650, "channels 100–128"),
    (5945, 5965, "channel 1"),
    # Narrower than a channel: the one it lies in
    (2440, 2444, "channel 7"),
])
def test_stretch_channels(opened, f0, f1, text):
    assert opened.js("([a, b]) => stretchChannels(a, b)", [f0, f1]) == text


def test_tooltip_names_channel_and_reporters(opened):
    html = opened.js("""loadTooltip({util: 72, f0: 5170, f1: 5250, nets: [
        {ssid: 'Office', bssid: '02:00:00:00:00:01', util: 72,
         channel: 36, bandwidth: 80},
        {ssid: '', bssid: '02:00:00:00:00:02', util: 55,
         channel: 44, bandwidth: 20}]})""")

    text = opened.js("""h => { const d = document.createElement('div');
        d.innerHTML = h; return d.innerText; }""", html)

    assert text.startswith(
        "Reported airtime 72 % on channels 36–48 (5170–5250 MHz)")
    assert "Office 02:00:00:00:00:01: 72 % (channel 36, 80 MHz)" in text
    # Hidden SSIDs no longer escaped twice
    assert "<hidden> 02:00:00:00:00:02: 55 % (channel 44, 20 MHz)" in text


def test_hatch_keeps_its_frequencies(app):
    for n in app.api["networks"]["networks"]:
        if n["ssid"] == "Plain":
            n["util"] = 80

    app.open()
    app.js("drawNetworkGraph()")

    stretches = app.js("graphLoad.map(r => [r.f0, r.f1])")

    assert stretches and all(f0 < f1 for f0, f1 in stretches)
    assert "channel" in app.js("loadTooltip(graphLoad[0])")


def hatch_of(app, pax, other, xload=None):
    # A 160 MHz BSS on 100-128 and an 80 MHz one on its upper half
    return app.js("""([pax, other, xload]) => {
        liveData = null;
        setChannels(c => true);
        const base = networks.find(x => x.channel === 36);
        const saved = networks;
        networks = [
            Object.assign({}, base, {bssid: '02:00:00:00:01:00',
                ssid: 'Wide', channel: 100, bandwidth: 160, center: 114,
                util: pax, rssi: -40, xload: xload}),
            Object.assign({}, base, {bssid: '02:00:00:00:01:16',
                ssid: 'Upper', channel: 116, bandwidth: 80, center: 122,
                util: other, rssi: -50})];
        const P = CanvasRenderingContext2D.prototype, text = P.fillText;
        let badges = [];
        P.fillText = function (s, x, y) {
            if (this.canvas.id === 'networkGraph' && (s === '\\u2715' ||
                s === '!')) badges.push(x);
            return text.call(this, s, x, y); };
        try { drawNetworkGraph(); } finally {
            P.fillText = text; networks = saved; }
        return {badges, load: graphLoad.map(r => ({f0: r.f0, f1: r.f1,
            util: r.util, parts: r.parts && r.parts.map(p =>
                [p.f0, p.f1, p.util])})),
            tip: (() => { const d = document.createElement('div');
                d.innerHTML = loadTooltip(graphLoad[0]);
                return d.innerText; })()};
    }""", [pax, other, xload])


def test_one_badge_for_a_block_of_two_parts(opened):
    got = hatch_of(opened, 87, 100)

    assert len(got["badges"]) == 1
    assert got["load"] == [{"f0": 5490, "f1": 5650, "util": 100,
                            "parts": [[5490, 5570, 87], [5570, 5650, 100]]}]
    assert got["tip"].startswith("Reported airtime up to 100 % on channels "
                                 "100–128 (5490–5650 MHz)")
    assert "channels 100–112: 87 %, channels 116–128: 100 %" in got["tip"]


def test_one_part_when_the_upper_half_reports_less(opened):
    got = hatch_of(opened, 93, 72)

    assert len(got["badges"]) == 1
    assert got["load"] == [{"f0": 5490, "f1": 5650, "util": 93,
                            "parts": None}]
    assert got["tip"].startswith("Reported airtime 93 % on channels 100–128")
    assert "100–112:" not in got["tip"]


def test_unhatched_wide_half_leaves_the_upper_alone(opened):
    got = hatch_of(opened, 20, 85)

    assert len(got["badges"]) == 1
    assert got["load"] == [{"f0": 5570, "f1": 5650, "util": 85,
                            "parts": None}]


def pct(p):
    return round(p * 255 / 100)


def test_extended_load_splits_the_width(opened):
    # Primary 100 at 93 %, secondary 20 (104) 45 %, 40 (108-112) 50 %,
    # 80 (116-128) 80 %; the 80 MHz BSS above stays below that
    got = hatch_of(opened, 93, 60, [2, 0, pct(45), pct(50), pct(80)])

    assert len(got["badges"]) == 1
    assert got["load"] == [{"f0": 5490, "f1": 5650, "util": 93, "parts": [
        [5490, 5510, 93], [5510, 5530, 45], [5530, 5570, 50],
        [5570, 5650, 80]]}]
    tip = got["tip"]
    assert ("channel 100: 93 %, channel 104: 45 %, channels 108–112: 50 %, "
            "channels 116–128: 80 %") in tip
    assert "secondary busy 20 MHz 45 % / 40 MHz 50 % / 80 MHz 80 %" in tip


def test_extended_load_gap_below_the_floor(opened):
    # 104 below 40 %: two stretches, a badge each
    got = hatch_of(opened, 93, 0, [2, 0, pct(10), pct(50), pct(80)])

    assert [(r["f0"], r["f1"]) for r in got["load"]] == \
        [(5490, 5510), (5530, 5650)]
    assert len(got["badges"]) == 2


@pytest.mark.parametrize("n, spans", [
    # No element, or only zeros: the primary value over the whole width
    ({"channel": 100, "bandwidth": 160, "center": 114, "util": 70},
     [[5490, 5650, 70]]),
    ({"channel": 100, "bandwidth": 160, "center": 114, "util": 70,
      "xload": [0, 0, 0, 0, 0]}, [[5490, 5650, 70]]),
    # Primary at the top of an 80 MHz BSS
    ({"channel": 48, "bandwidth": 80, "center": 42, "util": 60,
      "xload": [0, 0, 51, 102, 0]},
     [[5170, 5210, 40], [5210, 5230, 20], [5230, 5250, 60]]),
    # A FRITZ!Box: 96 % on the secondary 20 MHz, 0 on the secondary
    # 40; that does not fit, the primary value over the width
    ({"channel": 116, "bandwidth": 80, "center": 122, "util": 0,
      "xload": [1, 0, 246, 0, 0]}, [[5570, 5650, 0]]),
    # 160 MHz with the secondary 80 left at 0
    ({"channel": 100, "bandwidth": 160, "center": 114, "util": 70,
      "xload": [0, 0, 51, 102, 0]}, [[5490, 5650, 70]]),
    # 40 MHz on 5 GHz: one secondary, nothing to check it against
    ({"channel": 36, "bandwidth": 40, "center": 38, "util": 10,
      "xload": [0, 0, 128, 0, 0]}, [[5170, 5190, 10], [5190, 5210, 50]]),
    # 2.4 GHz 40 MHz with the primary in the upper half
    ({"channel": 6, "bandwidth": 40, "center": 4, "util": 30,
      "xload": [0, 0, 255, 0, 0]}, [[2407, 2427, 100], [2427, 2447, 30]]),
])
def test_load_spans(opened, n, spans):
    assert opened.js("n => loadSpans(n)", n) == spans


def test_implausible_extended_load_said_in_the_tooltip(opened):
    n = {"channel": 52, "bandwidth": 80, "center": 58, "util": 0,
         "xload": [0, 0, 244, 0, 0]}

    assert opened.js("n => xloadText(n)", n) == (
        "0 MU-MIMO stations · streams idle 0 % · secondary busy 20 MHz "
        "96 % / 40 MHz 0 % · Note: implausible load")

    # Taken: no such note
    n["xload"] = [0, 0, 244, 30, 0]
    assert "Note" not in opened.js("n => xloadText(n)", n)


def test_implausible_extended_load_not_in_the_hatch(opened):
    # The FRITZ!Box from the capture: 0 % primary, the secondary 20 at
    # 96 % and the secondary 40 at 0: nothing hatched
    got = opened.js("""() => {
        liveData = null;
        setChannels(c => true);
        const base = networks.find(x => x.channel === 36);
        const saved = networks;
        networks = [Object.assign({}, base, {bssid: '02:00:00:00:01:52',
            ssid: 'Fritz', channel: 52, bandwidth: 80, center: 58, util: 0,
            rssi: -60, xload: [0, 0, 244, 0, 0]})];
        try { drawNetworkGraph(); } finally { networks = saved; }
        return graphLoad.length; }""")

    assert got == 0
