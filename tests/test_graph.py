"""Channel axis of the BSSID graph."""


def axis(app, focus):
    """Channels on the axis per segment with only `focus` scanned."""
    return app.js("""ch => {
        liveData = null;
        setChannels(c => c === ch);
        const bands = computeLayout(activeChannels(), 58, 1385,
                                    filteredNetworks());
        return bands.map(b => b.segments.map(s => axisChannels(b, s)));
    }""", focus)


def test_focused_channel_keeps_the_band_axis(opened):
    # Networks on 36 (80 MHz, centre 42) keep the segment wide, so
    # the axis shows 36 ... 48 and not only the scanned channel
    assert axis(opened, 36) == [[[36, 40, 44, 48]]]


def test_focused_24_channel_keeps_the_band_axis(opened):
    # Networks on 6 and 11: 2427 ... 2472 MHz, channels 4 ... 13
    assert axis(opened, 6) == [[[4, 5, 6, 7, 8, 9, 10, 11, 12, 13]]]


def test_all_channels_unchanged(opened):
    same = opened.js("""() => {
        liveData = null;
        setChannels(c => true);
        const bands = computeLayout(activeChannels(), 58, 1385,
                                    filteredNetworks());
        return bands.every(b => b.segments.every(s =>
            JSON.stringify(axisChannels(b, s)) ===
            JSON.stringify(s.channels.slice().sort((x, y) => x - y))));
    }""")

    assert same
