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


def shape(app, extra):
    """The points of the one network's outline in the BSSID graph."""
    return app.js("""extra => {
        const n = Object.assign({}, networks.find(x => x.channel === 6),
                                {rssi: -40}, extra);
        const saved = networks;
        networks = [n];
        const P = CanvasRenderingContext2D.prototype;
        const line = P.lineTo, move = P.moveTo;
        let points = [], drawing = false;
        P.moveTo = function (x, y) {
            if (this.canvas.id === 'networkGraph') { points = [[x, y]]; drawing = true; }
            return move.call(this, x, y); };
        P.lineTo = function (x, y) {
            if (drawing && this.canvas.id === 'networkGraph') points.push([x, y]);
            return line.call(this, x, y); };
        try { drawNetworkGraph(); } finally {
            P.lineTo = line; P.moveTo = move; networks = saved; }
        return points;
    }""", extra)


def test_dsss_is_a_bell(opened):
    bell = shape(opened, {"std": 1})
    ofdm = shape(opened, {"std": 4})

    # A trapeze: bottom, two corners on top, bottom
    assert len(ofdm) == 4
    assert ofdm[1][1] == ofdm[2][1]

    # A bell: many points, the top in the middle, falling both ways,
    # 22 MHz wide against 20
    assert len(bell) > 30
    ys = [p[1] for p in bell]
    top = ys.index(min(ys))
    assert abs(top - len(ys) / 2) <= 1
    assert all(ys[i] <= ys[i - 1] for i in range(1, top))
    assert all(ys[i] >= ys[i - 1] for i in range(top + 1, len(ys)))
    assert bell[-1][0] - bell[0][0] > ofdm[-1][0] - ofdm[0][0]


def test_80_plus_80_draws_both_segments(opened):
    hits = opened.js("""() => {
        liveData = null;
        setChannels(c => true);
        const n = Object.assign({}, networks.find(x => x.channel === 36),
            {bssid: '02:80:80:00:00:01', ssid: 'TwoSeg', channel: 36,
             bandwidth: 8080, center: 42, center2: 155, rssi: -40});
        networks.push(n);
        drawNetworkGraph();
        const mine = graphHits.filter(h => h.n === n);
        const labels = graphLabels.filter(l => l.n === n).length;
        networks.pop();
        return [mine.length, mine.length === 2 && mine[1].xl > mine[0].xr,
                labels];
    }""")

    assert hits[0] == 2
    assert hits[1]
    assert hits[2] >= 1

    text = opened.js("""() => { const d = document.createElement('div');
        d.innerHTML = networkTooltip(Object.assign({}, networks[0],
            {channel: 36, bandwidth: 8080, center: 42, center2: 155}));
        return d.innerText; }""")

    assert "80+80 \u00b7 5170\u20135250 MHz + 5735\u20135815 MHz" in text


def test_80_plus_80_filtered_keeps_the_second_segment(opened):
    # Filtered to it alone, primary 100 and the second 80 on 42, the
    # sweep on 36 and 100 only: the panel of 36 is stretched over 36
    # ... 48, both drawn alike wide
    got = opened.js("""() => {
        liveData = null;
        setChannels(c => c === 36 || c === 100);
        const n = Object.assign({}, networks.find(x => x.channel === 36),
            {bssid: '02:80:80:00:00:02', ssid: 'OnlyTwoSeg', channel: 100,
             bandwidth: 8080, center: 106, center2: 42, rssi: -40});
        networks.push(n);
        filterText = 'OnlyTwoSeg';
        drawNetworkGraph();
        const mine = graphHits.filter(h => h.n === n)
            .map(h => [Math.round(h.xl), Math.round(h.xr)]);
        filterText = '';
        networks.pop();
        return mine;
    }""")

    assert len(got) == 2
    widths = [xr - xl for xl, xr in got]
    assert min(widths) > 0.5 * max(widths)


def test_punctured_subchannels_are_hatched(opened):
    # 36/160, centre 50: 56 and 60 left out
    got = opened.js("""() => {
        liveData = null;
        setChannels(c => true);
        const n = Object.assign({}, networks.find(x => x.channel === 36),
            {bssid: '02:00:00:00:01:60', ssid: 'Punct', channel: 36,
             bandwidth: 160, center: 50, rssi: -40,
             eht: [160, 50, 0b1100000]});
        const saved = networks;
        networks = [n];
        const P = CanvasRenderingContext2D.prototype;
        const line = P.lineTo, move = P.moveTo, rect = P.fillRect;
        const text = P.fillText;
        let path = [], fills = [], hatch = [], badges = 0;
        P.fillText = function (s, x, y) {
            if (this.canvas.id === 'networkGraph' && s === '!') badges++;
            return text.call(this, s, x, y); };
        P.moveTo = function (x, y) {
            if (this.canvas.id === 'networkGraph') { path = [[x, y]]; hatch.push(x); }
            return move.call(this, x, y); };
        P.lineTo = function (x, y) {
            if (this.canvas.id === 'networkGraph') path.push([x, y]);
            return line.call(this, x, y); };
        P.fillRect = function (x, y, w, h) {
            if (this.canvas.id === 'networkGraph') fills.push([x, w]);
            return rect.call(this, x, y, w, h); };
        try { drawNetworkGraph(); } finally {
            P.lineTo = line; P.moveTo = move; P.fillRect = rect;
            P.fillText = text; networks = saved; }
        return {path, fills, hatch, badges, sub: (path[path.length - 1][0] - path[0][0]) / 8,
                xl: path[0][0]};
    }""")

    # The outline stays the whole trapeze
    assert len(got["path"]) == 4

    # Filled over the subchannels it sends on, 36 ... 52 as one run and
    # 64, not over 56 and 60
    sub, xl = got["sub"], got["xl"]
    filled = sorted((round((x - xl) / sub), round(w / sub)) for x, w in got["fills"])
    assert filled == [(0, 5), (7, 1)]

    # Hatch lines start inside the two left out
    inside = [x for x in got["hatch"] if xl + 5 * sub <= x < xl + 7 * sub]
    assert len(inside) >= 4

    # With the "!" of the BSS load hatch at their top right
    assert got["badges"] == 1

    text = opened.js("""() => { const d = document.createElement('div');
        d.innerHTML = networkTooltip(Object.assign({}, networks[0],
            {eht: [160, 50, 0b1100000]}));
        return d.innerText; }""")

    assert "56, 60 left out (5270–5310 MHz)" in text
    assert "Part of the channel left out (preamble puncturing): 56, 60" in text


def test_punctured_ranges_joined(opened):
    # Neighbours as one range, as the hatch draws them; apart, one each
    got = opened.js("""() => [0b1100, 0b10000001, 0b11000010].map(b =>
        puncturedText({eht: [160, 50, b]}))""")

    assert got == ["44, 48 left out (5210–5250 MHz)",
                   "36, 64 left out (5170–5190, 5310–5330 MHz)",
                   "40, 60, 64 left out (5190–5210, 5290–5330 MHz)"]


def test_tooltip_over_the_hatch(opened):
    got = opened.js("""() => {
        liveData = null;
        setChannels(c => true);
        const n = Object.assign({}, networks.find(x => x.channel === 36),
            {bssid: '02:00:00:00:01:61', ssid: 'Punct', channel: 36,
             bandwidth: 160, center: 50, rssi: -40,
             eht: [160, 50, 0b1100000]});
        const saved = networks;
        networks = [n];
        drawNetworkGraph();
        const h = graphHits.find(h => h.n === n);
        const [x0, x1] = h.punct[0];
        const ym = (h.y + h.bottom) / 2;
        const over = puncturedAt((x0 + x1) / 2, ym);
        const beside = puncturedAt(h.xl + 10, ym);
        const tip = puncturedTooltip(n);
        networks = saved;
        return [h.punct.length, over === n, beside, tip];
    }""")

    assert got[0] == 1          # 56 and 60 as one run
    assert got[1] is True
    assert got[2] is None
    assert got[3].startswith("<div class='tipSec'>PUNCTURED</div>56, 60 left out")
