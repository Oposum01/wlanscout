"""Beacons faster than their interval: the amber note, the tooltip
row and the filter term."""


def notes_of(app, extra):
    return app.js("""extra => networkNotes(Object.assign({}, networks[0], extra))
        .map(n => n[1])""", extra)


def test_note_for_a_second_sender(opened):
    notes = notes_of(opened, {"bi": 100, "bgap": [40, 18, 2300]})

    assert any("Beacons come faster than their interval allows: 45 %"
               in n and "(51 ms)" in n for n in notes)


def test_no_note_for_a_few_late_beacons(opened):
    assert not any("faster than their interval" in n for n in
                   notes_of(opened, {"bi": 100, "bgap": [40, 4, 30000]}))
    assert not any("faster than their interval" in n for n in
                   notes_of(opened, {"bi": 100, "bgap": [6, 5, 2000]}))


def test_tooltip_row_and_filter(opened):
    text = opened.js("""() => { const d = document.createElement('div');
        d.innerHTML = networkTooltip(Object.assign({}, networks[0],
            {bi: 100, bgap: [40, 18, 2300]}));
        return d.innerText; }""")

    assert "18 of 40 gaps under half the interval, shortest 2.3 ms" in text

    opened.js("networks[0].bi = 100; networks[0].bgap = [40, 18, 2300]")
    found = opened.js("""networks.filter(n => {
        filterText = 'fastbeacons'; return matchesFilter(n); }).length""")

    assert found == 1
