"""The 6 GHz BSSs an AP announces in its Reduced Neighbor Report, in
its own tooltip and not only in the 6 GHz table."""

from test_tooltip import tooltip

# As .233 heard it: a 5 GHz AP without MLD announcing a 320 MHz BSS
# of another SSID, its short SSID matching nothing known
SIX = {"bssid": "02:00:00:00:06:42", "ssid": "", "ch": 49, "freq": 6195,
       "width": 320, "opclass": 137, "same": 0, "colocated": 1,
       "unsolicited": 0, "psd": 127, "shortssid": 146382653,
       "reports": 66, "age": 0}


def announced(app, entries):
    """The tooltip's text and its 6 GHz lines, for the Plain network."""
    return app.js("""l => { const n = networks.find(x => x.ssid === 'Plain');
        sixGhzList = l.map(e => Object.assign({reporter: n.bssid}, e));
        const d = document.createElement('div');
        d.innerHTML = sixAnnouncedText(n);
        return [d.innerHTML ? d.innerHTML.split('<br>').map(x => {
            const s = document.createElement('span');
            s.innerHTML = x; return s.textContent; }) : [],
            (() => { d.innerHTML = networkTooltip(n);
                     return d.textContent; })()]; }""", entries)


def test_announced_by_this_ap(opened):
    lines, text = announced(opened, [SIX])

    assert lines == ["ch 49 · 320 MHz · 02:00:00:00:06:42 · co-located · "
                     "other SSID (short SSID 0x08B99F3D, unknown) · "
                     "announced only"]
    assert "6 GHzch 49 · 320 MHz" in text


def test_one_line_each_named_when_known(opened):
    lines, _ = announced(opened, [
        dict(SIX, ch=101, bssid="02:00:00:00:06:43", same=1, colocated=0,
             width=0),
        dict(SIX, ssid="<Lab>")])

    # By channel, the name escaped
    assert lines == [
        "ch 49 · 320 MHz · 02:00:00:00:06:42 · co-located · SSID “<Lab>” · "
        "announced only",
        "ch 101 · 02:00:00:00:06:43 · same SSID · announced only"]


def test_nothing_announced_no_row(opened):
    assert announced(opened, [])[0] == []
    assert "6 GHzch" not in announced(opened, [])[1]
    # Announced by another AP
    assert announced(opened, [dict(SIX, reporter="02:00:00:00:00:99")])[0] \
        == []
