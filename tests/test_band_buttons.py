"""The band headings of the BSSID graph as buttons: one band across the
whole width, the sweep left as it is."""


def bands(app):
    return app.js("""() => { drawNetworkGraph();
        return {buttons: graphBandButtons.map(b => b.band),
                hits: [...new Set(graphHits.map(h => isBand24(h.n.channel) ? '24' : '5'))].sort(),
                plan: activeChannels().length}; }""")


def click_button(app, band):
    box = app.js("b => graphBandButtons.find(x => x.band === b)", band)
    canvas = app.page.locator("#networkGraph").bounding_box()
    app.page.mouse.click(canvas["x"] + (box["x0"] + box["x1"]) / 2,
                         canvas["y"] + (box["y0"] + box["y1"]) / 2)


def test_one_band_then_both_again(opened):
    opened.js("liveData = null; setChannels(c => true); localStorage.removeItem('wlanscout.graphBand'); graphBand = ''")
    start = bands(opened)

    assert start["buttons"] == ["24", "5"]
    assert start["hits"] == ["24", "5"]

    click_button(opened, "5")
    five = bands(opened)

    # Only 5 GHz drawn, a button to switch to 2.4 GHz beside it; the
    # sweep untouched
    assert five["hits"] == ["5"]
    assert five["buttons"] == ["5", "24"]
    assert five["plan"] == start["plan"]
    assert opened.js("localStorage.getItem('wlanscout.graphBand')") == "5"

    click_button(opened, "24")
    assert bands(opened)["hits"] == ["24"]

    # The lit one shows both again
    click_button(opened, "24")
    assert bands(opened)["hits"] == ["24", "5"]


def test_no_buttons_for_a_sweep_over_one_band(opened):
    opened.js("liveData = null; setChannels(c => c <= 14); graphBand = '5'")

    got = bands(opened)

    assert got["buttons"] == []
    assert opened.js("graphBand") == ""


def test_table_follows_the_band(opened):
    opened.js("liveData = null; setChannels(c => true); setGraphBand('')")
    rows = lambda: opened.js("""[...document.querySelectorAll('#networkTable tr')]
        .map(r => r.dataset.bssid)""")
    info = lambda: opened.js("document.getElementById('filterInfo').innerText")
    all_rows = rows()

    opened.js("setGraphBand('24')")
    two = rows()
    want = opened.js("""networks.filter(n => isBand24(n.channel)).map(n => n.bssid)""")

    assert sorted(two) == sorted(want) and len(two) < len(all_rows)
    assert info().startswith("%d of %d networks shown, 2.4 GHz only"
                             % (len(two), len(all_rows)))

    opened.js("setGraphBand('')")
    assert len(rows()) == len(all_rows)
    assert info().startswith("%d networks" % len(all_rows))


def test_hover_says_what_a_click_does(opened):
    opened.js("liveData = null; setChannels(c => true); setGraphBand('')")

    def hover(band):
        box = opened.js("b => graphBandButtons.find(x => x.band === b)", band)
        canvas = opened.page.locator("#networkGraph").bounding_box()
        opened.page.mouse.move(canvas["x"] + (box["x0"] + box["x1"]) / 2,
                               canvas["y"] + (box["y0"] + box["y1"]) / 2)
        return opened.js("""() => [document.getElementById('tip').innerText,
            document.getElementById('networkGraph').style.cursor]""")

    text, cursor = hover("24")
    assert text.startswith("Show only 2.4 GHz") and "sweep stays" in text
    assert cursor == "pointer"

    opened.js("setGraphBand('24')")
    assert hover("24")[0].startswith("Show both bands again")
    assert hover("5")[0].startswith("Show only 5 GHz")
