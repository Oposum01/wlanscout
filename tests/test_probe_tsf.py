"""Probe responses with timestamp 0: the amber note and the filter
term, only with a timer in the beacons."""


def notes_of(app, extra):
    return app.js("""extra => networkNotes(Object.assign({}, networks[0], extra))
        .map(n => n[1])""", extra)


def has_note(notes):
    return any("Timestamp 0 in every probe response" in n for n in notes)


def test_note_when_every_probe_response_has_0(opened):
    notes = notes_of(opened, {"ptsf": [12, 0], "tsfup": 86400})

    assert any("Timestamp 0 in every probe response (12 seen)" in n
               for n in notes)
    # Red: not as 802.11 has it
    assert opened.js("""networkNotes(Object.assign({}, networks[0],
        {ptsf: [12, 0], tsfup: 86400})).some(n => n[0] === '#d08080' &&
        n[1].startsWith('Timestamp 0'))""")


def test_no_note_otherwise(opened):
    # A few, some with a time, or no timer in the beacons either
    assert not has_note(notes_of(opened, {"ptsf": [2, 0], "tsfup": 86400}))
    assert not has_note(notes_of(opened, {"ptsf": [12, 3], "tsfup": 86400}))
    assert not has_note(notes_of(opened, {"ptsf": [12, 0],
                                          "tsfup": None}))


def test_filter_term(opened):
    opened.js("networks[0].ptsf = [12, 0]; networks[0].tsfup = 86400")
    found = opened.js("""networks.filter(n => {
        filterText = 'probetsf0'; return matchesFilter(n); }).length""")

    assert found == 1
