"""Notes of a station: tooltip, the notes column, sorting, filter."""

import copy


def with_station_changes(app, changes):
    """Open the page with the first fixture station changed."""
    clients = copy.deepcopy(app.api["clients"])
    clients["clients"][0].update(changes)
    app.api["clients"] = clients
    app.open()
    app.tab("Stations")
    app.page.evaluate("updateClients()")
    app.page.wait_for_function("() => stations.length >= 2")
    app.page.locator(".subTab", has_text="Probing only").click()

    return app


def notes_cell(app, mac):
    return app.js("""mac => {
        const row = [...document.querySelectorAll('#clientTableProbe tr')]
            .find(r => r.cells[0].textContent === mac);
        return row.cells[row.cells.length - 2].textContent;
    }""", mac)


def test_unknown_elements_in_column_and_tooltip(app):
    # Three unknown elements, as some speakers send in probe requests
    with_station_changes(app, {"unk": [71, 72, 73], "unkin": [1, 1, 1]})

    assert notes_cell(app, "02:00:00:00:01:01") == "???"
    assert notes_cell(app, "02:00:00:00:01:02") == "-"

    text = app.js("""() => {
        const d = document.createElement('div');
        d.innerHTML = clientTooltip(stations.find(
            c => c.mac === '02:00:00:00:01:01'));
        return d.innerText;
    }""")

    assert "Unknown element 71" in text


def test_rejected_association_is_amber(app):
    with_station_changes(app, {"status": 17,
                               "statusbss": "02:00:00:00:00:02"})

    notes = app.js("""stationNotes(stations.find(
        c => c.mac === '02:00:00:00:01:01'), false)""")

    assert notes[0][0] == "#c8a050"
    assert notes[0][1].startswith("Last association rejected by ")
    assert notes_cell(app, "02:00:00:00:01:01") == "!"


def test_status_temporarily_rejected_with_comeback(app):
    # 30 with PMF: the AP checks the old association first; 200 TU
    with_station_changes(app, {"status": 30, "comeback": 200,
                               "statusbss": "02:00:00:00:00:02"})

    notes = app.js("""stationNotes(stations.find(
        c => c.mac === '02:00:00:00:01:01'), false)""")

    assert "rejected temporarily, try again later (30)" in notes[0][1]
    assert "SA Query" in notes[0][1]
    assert "try again in 205 ms" in notes[0][1]

    last = app.js("""() => {
        const d = document.createElement('div');
        d.innerHTML = clientTooltip(stations.find(
            c => c.mac === '02:00:00:00:01:01'));
        return [...d.querySelectorAll('span')]
            .find(s => s.textContent.includes('(30)')).style.color;
    }""")

    # Amber, not red: part of normal PMF operation
    assert last == "rgb(200, 160, 80)"


def test_status_names(opened):
    assert opened.js("[0, 17, 30, 53, 82, 130, 250].map(assocStatusText)") == [
        "success",
        "denied, AP is at its station limit",
        "rejected temporarily, try again later",
        "invalid PMKID",
        "rejected, suggested BSS transition",
        "denied, station affiliated with an MLD already associated",
        "status 250"]
    # Others than 30 carry no hint
    assert opened.js("assocStatusHint({status: 17})") == ""


def test_sorting_by_notes(app):
    with_station_changes(app, {"unk": [71], "unkin": [1]})

    app.js("setClientSort('notes')")

    first = app.js("""document.querySelector('#clientTableProbe tr')
        .cells[0].textContent""")

    assert first == "02:00:00:00:01:01"


def test_filter_finds_note_text(app):
    with_station_changes(app, {"unk": [71], "unkin": [1]})

    app.page.fill("#stationFilter", "unknown element 71")
    app.page.dispatch_event("#stationFilter", "input")

    assert app.js("filteredStations().map(c => c.mac)") == \
        ["02:00:00:00:01:01"]


def test_history_has_the_column(app):
    hist = {"dropped": 0, "slots": 120, "clients": [
        dict(copy.deepcopy(app.api["clients"]["clients"][0]),
             unk=[71], unkin=[2], assocs=1, lastepoch=1791794541)]}
    app.api["assochistory"] = hist

    with_station_changes(app, {})
    app.page.evaluate("updateAssocHistory()")
    app.page.locator(".subTab", has_text="History").click()
    app.page.wait_for_function(
        "() => document.querySelectorAll('#assocHistTable tr').length > 0")

    cells = app.js("""[...document.querySelector('#assocHistTable tr').cells]
        .map(c => c.textContent)""")

    assert cells[-2] == "?"
