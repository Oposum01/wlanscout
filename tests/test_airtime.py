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
