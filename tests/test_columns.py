"""Columns chosen per browser above each table; the BLE table's optional
columns (Vendor from the OUI, service, known for); company names
fetched by the browser from the Bluetooth SIG only when asked for."""
import json

BLE = [
    {"addr": "A4:C1:38:00:11:22", "type": "public", "name": "Watch",
     "svc": "0000180d-0000-1000-8000-00805f9b34fb", "vendor": "Telink",
     "company": 76, "companyName": "Apple", "rssi": -60, "peak": -55,
     "tx": 127, "adv": 10, "rate": "0.5", "age": 1, "seen": 20},
    {"addr": "5A:11:22:33:44:55", "type": "resolvable private", "name": "",
     "svc": "", "vendor": "", "company": 0x1142, "companyName": "",
     "rssi": -70, "peak": -65, "tx": 127, "adv": 3, "rate": "0.1",
     "age": 2, "seen": 30},
    {"addr": "00:11:22:33:44:55", "type": "public", "name": "", "svc": "",
     "vendor": "", "company": 65535, "companyName": "", "rssi": -80,
     "peak": -75, "tx": 127, "adv": 1, "rate": "0.0", "age": 3, "seen": 3}]

YAML = """company_identifiers:

  - value: 0x1142
    name: 'Eight Sleep Inc.'

  - value: 0x004C
    name: 'Apple, Inc.'
""" + "".join("""
  - value: 0x%04X
    name: 'Company ''%d'''
""" % (0x2000 + i, i) for i in range(120))

JSON_LIST = json.dumps([{"code": 0x1142, "name": "Eight Sleep (copy)"}] +
                       [{"code": 0x3000 + i, "name": "C%d" % i}
                        for i in range(120)])


def heads(app, id_):
    return app.js("(id) => [...document.querySelectorAll('#' + id + ' th')]"
                  ".map(th => th.innerText.replace(/[\\u25B2\\u25BC]/g, '')"
                  ".trim())", id_)


def ble_rows(app):
    app.js("(d) => { bleDevices = d; updateBleTable(); }", BLE)
    return app.js("""[...document.querySelectorAll('#bleTable tr')]
        .map(r => [...r.cells].map(c => c.innerText))""")


def test_hiding_a_column_kept_across_a_reload(opened):
    assert "Vendor" in heads(opened, "networkHead")

    box = opened.page.locator(".colChooser[data-table=networks]")
    box.locator(".colBtn").click()
    box.locator("label", has_text="Vendor").locator("input").uncheck()

    assert "Vendor" not in heads(opened, "networkHead")
    # The rows follow the head: one cell less, notes and pcap after
    cells = opened.js("document.querySelector('#networkTable tr').cells.length")
    assert cells == len(heads(opened, "networkHead"))

    opened.page.reload()
    opened.page.wait_for_function("() => typeof networks !== 'undefined'")
    assert "Vendor" not in heads(opened, "networkHead")

    box = opened.page.locator(".colChooser[data-table=networks]")
    box.locator(".colBtn").click()
    box.locator(".colDefault").click()
    assert "Vendor" in heads(opened, "networkHead")


def test_the_last_column_cannot_go(opened):
    opened.js("setColumnPick('zb', []); buildZbHead()")

    assert heads(opened, "zbHead")[0] == "Address"


def test_ble_optional_columns(opened):
    assert "Vendor (OUI)" not in heads(opened, "bleHead")

    opened.js("""setColumnPick('ble', ['addr', 'type', 'vendor', 'svc',
                                       'companyName', 'seen']);
                 buildBleHead()""")
    assert heads(opened, "bleHead") == ["Address", "Type", "Company",
                                        "Vendor (OUI)", "Service",
                                        "Known for"]

    rows = {r[0]: r for r in ble_rows(opened)}
    assert rows["A4:C1:38:00:11:22"][3] == "Telink"
    # A random address has no OUI; a public one unknown shows its OUI
    assert rows["5A:11:22:33:44:55"][3] == "-"
    assert rows["00:11:22:33:44:55"][3] == "00:11:22"
    assert rows["A4:C1:38:00:11:22"][4].startswith("0000180d")
    assert rows["5A:11:22:33:44:55"][5] == "30 s"


def test_no_company_list_fetched_unless_asked(opened):
    fetched = []
    opened.page.route("https://bitbucket.org/**",
                      lambda r: (fetched.append(r.request.url),
                                 r.fulfill(body=YAML)))
    opened.page.reload()
    opened.page.wait_for_function("() => typeof networks !== 'undefined'")
    opened.page.wait_for_timeout(300)

    assert fetched == []
    assert not opened.js("document.getElementById('btCompanies').checked")
    rows = {r[0]: r for r in ble_rows(opened)}
    assert rows["5A:11:22:33:44:55"][3] == "0x1142"


def test_company_names_from_the_sig(opened):
    opened.page.route("https://bitbucket.org/**",
                      lambda r: r.fulfill(body=YAML))
    opened.js("setBtCompanies(true)")
    opened.page.wait_for_function("() => btCompanyName(0x1142) !== ''")

    rows = {r[0]: r for r in ble_rows(opened)}
    assert rows["5A:11:22:33:44:55"][3] == "Eight Sleep Inc."
    assert rows["A4:C1:38:00:11:22"][3] == "Apple, Inc."
    assert rows["00:11:22:33:44:55"][3] == "-"
    assert opened.js("btCompanyName(0x2003)") == "Company '3'"
    assert "names" in opened.js(
        "document.getElementById('btCompanyInfo').innerText")

    # Kept: after a reload no fetch is needed
    opened.page.unroute("https://bitbucket.org/**")
    opened.page.route("https://bitbucket.org/**", lambda r: r.abort())
    opened.page.reload()
    opened.page.wait_for_function("() => btCompanyName(0x1142) !== ''")
    assert opened.js("document.getElementById('btCompanies').checked")

    # Off again: the list goes
    opened.js("setBtCompanies(false)")
    opened.page.wait_for_function("() => btCompanyName(0x1142) === ''")


def test_the_copy_when_the_sig_does_not_answer(opened):
    opened.page.route("https://bitbucket.org/**",
                      lambda r: r.fulfill(status=503, body=""))
    opened.page.route("https://raw.githubusercontent.com/**",
                      lambda r: r.fulfill(body=JSON_LIST))
    opened.js("setBtCompanies(true)")
    opened.page.wait_for_function("() => btCompanyName(0x1142) !== ''")

    assert opened.js("btCompanyName(0x1142)") == "Eight Sleep (copy)"


def test_nothing_fetched_says_why(opened):
    opened.page.route("https://bitbucket.org/**", lambda r: r.abort())
    opened.page.route("https://raw.githubusercontent.com/**",
                      lambda r: r.fulfill(status=404, body=""))
    opened.js("setBtCompanies(true)")
    opened.page.wait_for_function(
        "() => document.getElementById('btCompanyInfo').innerText"
        ".startsWith('not fetched')")

    rows = {r[0]: r for r in ble_rows(opened)}
    assert rows["A4:C1:38:00:11:22"][3] == "Apple"


def visible_heads(app, tbody):
    return app.js("""(id) => [...document.getElementById(id).closest('table')
        .querySelectorAll('thead th')].filter(th =>
        getComputedStyle(th).display !== 'none').map(th => th.innerText.trim())""",
                  tbody)


def test_every_table_has_a_columns_button(opened):
    tables = set(opened.js("""[...document.querySelectorAll('.colChooser')]
        .map(b => b.dataset.table)"""))

    assert tables >= {"networks", "stations", "history", "ble", "zb",
                      "sixghz", "catalog", "blehits", "stores", "log",
                      "tasks", "alloc"}


def test_a_fixed_head_table_hides_by_position(opened):
    opened.js("showView('catalogView', document.querySelector("
              "'[onclick*=catalogView]'))")
    opened.js("updateCatalog && updateCatalog()")
    opened.page.wait_for_function(
        "() => document.querySelectorAll('#catalogTable tr').length > 1")
    assert "Vendor" in visible_heads(opened, "catalogTable")

    box = opened.page.locator(".colChooser[data-table=catalog]")
    box.locator(".colBtn").click()
    box.locator("label", has_text="Vendor").locator("input").uncheck()

    assert "Vendor" not in visible_heads(opened, "catalogTable")
    # A data row loses the same cell, a group heading spanning the
    # table stays
    assert opened.js("""(() => { const r = [...document.querySelectorAll(
        '#catalogTable tr')].find(r => r.cells.length > 3);
        return getComputedStyle(r.cells[2]).display; })()""") == "none"
    assert opened.js("""[...document.querySelectorAll('#catalogTable td[colspan]')]
        .every(td => getComputedStyle(td).display !== 'none')""")

    opened.page.reload()
    opened.page.wait_for_function("() => typeof networks !== 'undefined'")
    assert "Vendor" not in visible_heads(opened, "catalogTable")

    opened.js("showView('catalogView', document.querySelector("
              "'[onclick*=catalogView]'))")
    box = opened.page.locator(".colChooser[data-table=catalog]")
    box.locator(".colBtn").click()
    box.locator(".colDefault").click()
    assert "Vendor" in visible_heads(opened, "catalogTable")


def test_a_fixed_head_table_keeps_one_column(opened):
    opened.js("""localStorage.setItem('wlanscout.columns.sixghz',
                 JSON.stringify([0,1,2,3,4,5,6,7,8]))""")
    opened.page.reload()
    opened.page.wait_for_function("() => typeof networks !== 'undefined'")
    # A stored pick hiding all is shown as it is; the panel refuses it
    opened.js("document.querySelector('.colChooser[data-table=sixghz] .colBtn')"
              ".click()")
    opened.js("""(() => { const p = document.querySelector(
        '.colChooser[data-table=sixghz] .colPanel');
        p.querySelectorAll('input').forEach(i => i.checked = false);
        p.dispatchEvent(new Event('change')); })()""")
    assert opened.js("localStorage.getItem('wlanscout.columns.sixghz')") is None


def head_width(app, label):
    return app.js("""(l) => [...document.querySelectorAll('#networkHead th')]
        .find(th => th.innerText.trim() === l).offsetWidth""", label)


def drag(app, label, dx):
    grip = app.page.locator("#networkHead th", has_text=label) \
        .locator(".colResize")
    box = grip.bounding_box()
    x, y = box["x"] + box["width"] / 2, box["y"] + box["height"] / 2
    app.page.mouse.move(x, y)
    app.page.mouse.down()
    app.page.mouse.move(x + dx, y, steps=4)
    app.page.mouse.up()


def test_a_column_dragged_wider_kept_across_a_reload(opened):
    before = head_width(opened, "Vendor")
    sort = opened.js("sortKey")

    drag(opened, "Vendor", 80)

    assert head_width(opened, "Vendor") >= before + 70
    # Dragging the edge is no click on the head: the sort stays
    assert opened.js("sortKey") == sort
    # The rows follow the head
    assert opened.js("""(() => { const i = [...document.querySelectorAll(
        '#networkHead th')].findIndex(th => th.innerText.trim() === 'Vendor');
        return document.querySelector('#networkTable tr').cells[i]
            .offsetWidth; })()""") >= before + 70

    opened.page.reload()
    opened.page.wait_for_function("() => networks.length > 0")
    assert head_width(opened, "Vendor") >= before + 70

    # The head is built anew on sorting: the width stays
    opened.page.locator("#networkHead th", has_text="Channel").click()
    assert head_width(opened, "Vendor") >= before + 70

    # A double click on the edge gives it back
    opened.page.locator("#networkHead th", has_text="Vendor") \
        .locator(".colResize").dblclick()
    assert abs(head_width(opened, "Vendor") - before) <= 2


def test_default_widths_in_the_columns_list(opened):
    before = head_width(opened, "Vendor")
    drag(opened, "Vendor", 60)
    assert head_width(opened, "Vendor") > before + 50

    box = opened.page.locator(".colChooser[data-table=networks]")
    box.locator(".colBtn").click()
    box.locator(".colWidths").click()
    assert abs(head_width(opened, "Vendor") - before) <= 2


def test_every_table_with_a_head_has_handles(opened):
    assert opened.js("""[...document.querySelectorAll('table')]
        .filter(t => t.querySelector('thead tr th'))
        .every(t => t.querySelector('thead th .colResize'))""")
