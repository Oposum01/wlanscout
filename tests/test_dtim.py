"""The TIM element: DTIM period in LOAD, buffered traffic, the amber
note for a long DTIM period, the filter term."""


def tooltip(app, extra):
    return app.js("""extra => { const d = document.createElement('div');
        d.innerHTML = networkTooltip(Object.assign({}, networks[0], extra));
        return d.innerText; }""", extra)


def notes_of(app, extra):
    return app.js("""extra => networkNotes(Object.assign({}, networks[0], extra))
        .map(n => n[1])""", extra)


def test_dtim_row(opened):
    assert "beacon 100 TU (102 ms), DTIM 3 (307 ms) · mcast buffered · " \
        "2 STA waiting" in \
        tooltip(opened, {"bi": 100, "dtim": [3, 1, 2]})
    assert "1 (102 ms)" in tooltip(opened, {"bi": 100, "dtim": [1, 0, 0]})


def test_note_only_above_three(opened):
    assert any("DTIM every 10 beacons" in n and "1024 ms" in n
               for n in notes_of(opened, {"bi": 100, "dtim": [10, 0, 0]}))
    assert not any("DTIM every" in n
                   for n in notes_of(opened, {"bi": 100, "dtim": [3, 0, 0]}))


def test_filter(opened):
    opened.js("networks[0].dtim = [10, 0, 0]")

    assert opened.js("""networks.filter(n => {
        filterText = 'dtim:10'; return matchesFilter(n); }).length""") == 1
