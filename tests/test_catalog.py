"""The IE catalog view: columns, parsed marks, sorting, file names."""

import pytest


@pytest.fixture
def catalog(opened):
    opened.tab("IE catalog")
    opened.page.wait_for_function(
        "() => document.querySelectorAll('#catalogTable tr').length >= 5")

    return opened


def column(app, name):
    """Text of one column, top to bottom."""
    return app.js("""name => {
        const heads = [...document.querySelectorAll('#catalogHead th')]
            .map(t => t.dataset.label || t.textContent.trim());
        const i = heads.indexOf(name);
        return [...document.querySelectorAll('#catalogTable tr')]
            .map(r => r.cells[i].textContent);
    }""", name)


def test_tab_and_title(catalog):
    assert catalog.page.locator(".tabs .tab").nth(3).inner_text() == "IE catalog"
    assert catalog.page.locator("#catalogView .title").inner_text() \
        .startswith("Elements to look at in Wireshark")


def test_columns(catalog):
    heads = catalog.js("""[...document.querySelectorAll('#catalogHead th')]
        .map(t => t.dataset.label || t.textContent.trim())""")

    assert heads[:5] == ["Type", "Number", "Vendor", "Subtype", "Name"]
    assert "First bytes" not in heads


def test_default_order_and_cells(catalog):
    assert column(catalog, "Number") == [
        "73", "75", "00:0C:42", "50:6F:9A", "00:0F:AC"]
    assert column(catalog, "Vendor") == [
        "", "", "Routerboard.com", "Wi-Fi Alliance", "IEEE"]
    assert column(catalog, "Subtype") == ["", "", "0", "41", "30"]


def test_parsed_marks(catalog):
    names = column(catalog, "Name")

    assert names[0] == "20/40 BSS Intolerant Channel Report"
    assert names[1] == "RIC Descriptor"
    # Elements the firmware lists as decoded in part get the mark
    # (MikroTik's up to 1.66)
    assert names[2] == "MikroTik radio name (parsed)"
    assert names[3] == "RSNE Override"


def test_sort_by_first_seen_newest_on_top(catalog):
    catalog.page.locator("#catalogHead th[data-sort=first]").click()

    assert column(catalog, "Number") == [
        "00:0F:AC", "75", "00:0C:42", "50:6F:9A", "73"]

    # A second click turns it round
    catalog.page.locator("#catalogHead th[data-sort=first]").click()

    assert column(catalog, "Number")[0] == "73"


def test_sort_by_vendor(catalog):
    catalog.page.locator("#catalogHead th[data-sort=vendor]").click()

    assert column(catalog, "Vendor")[-3:] == ["IEEE", "Routerboard.com", "Wi-Fi Alliance"]


def test_sort_is_kept_over_reload(catalog):
    catalog.page.locator("#catalogHead th[data-sort=count]").click()
    catalog.open()
    catalog.tab("IE catalog")
    catalog.page.wait_for_function(
        "() => document.querySelectorAll('#catalogTable tr').length >= 5")

    # Counts start descending: 75 was seen 50 times
    assert column(catalog, "Number")[0] == "75"


def test_pcap_file_names(catalog):
    names = catalog.js("""[...document.querySelectorAll('#catalogTable a.dl')]
        .map(a => a.download)""")

    assert names[0] == "element-ie73-20-40-bss-intolerant-channel-report.pcap"
    assert names[1] == "element-ie75-ric-descriptor.pcap"
    assert names[2].startswith("element-vendor-000C42-0-mikrotik-radio-name")
    assert all(not n.startswith("unknown-") for n in names)


def reads_groups(app):
    """{group title: [items]} of the reads list below the catalog."""
    return app.js("""() => {
        const out = {};
        let current = null;
        for (const el of document.querySelectorAll('#catalogReadsBody > *')) {
            if (el.classList.contains('readsGroup'))
                out[current = el.textContent.replace(/ \\(\\d+\\)$/, '')] = [];
            else
                out[current].push(el.textContent);
        }
        return out;
    }""")


def test_reads_list_closed_by_default(catalog):
    assert catalog.js("document.getElementById('catalogReads').open") is False
    assert catalog.page.locator("#catalogReads summary").inner_text() \
        == "What this sensor reads"


def test_reads_groups(catalog):
    catalog.page.locator("#catalogReads summary").click()
    groups = reads_groups(catalog)

    assert list(groups) == [
        "Decoded and shown", "Decoded in part, still catalogued",
        "Recognised, not shown", "Vendor elements decoded",
        "Vendor elements reviewed, content not shown",
        "AKM suites with a name", "Cipher suites with a name",
        "Everything else"]

    decoded = groups["Decoded and shown"]

    assert "7 Country Information" in decoded
    assert "ext 108 EHT Capabilities" in decoded

    # Interworking is decoded in full since 1.60.0; only MikroTik's
    # element is still collected for more samples
    assert "107 Interworking" in decoded
    assert groups["Decoded in part, still catalogued"] == [
        "00:0C:42 type 0 MikroTik radio name"]

    # Decided while going through the catalog in October 2026
    for item in ["40 Quiet", "76 Management MIC", "133 Cisco CCX1 CKIP + Device Name",
                 "199 Operating Mode Notification"]:
        assert any(x.startswith(item) for x in decoded), item

    passed = groups["Recognised, not shown"]

    for item in ["10 Request", "128 ", "200 ", "231 ", "240 FILS Indication",
                 "ext 37 ", "ext 41 "]:
        assert any(x.startswith(item) for x in passed), item

    assert "5 Traffic Indication Map (TIM)" in groups["Recognised, not shown"]
    assert "00:50:F2 type 1 WPA" in groups["Vendor elements decoded"]
    assert "00:0F:AC 8 SAE" in groups["AKM suites with a name"]
    assert "00:0F:AC 4 CCMP-128" in groups["Cipher suites with a name"]


def test_reads_and_catalog_agree(catalog):
    # Nothing the sensor decodes fully may show up as a catalog row
    rows = catalog.js("catalogData.entries.filter(e => e.k === 0).map(e => e.id)")
    reads = catalog.js("catalogData.reads")
    full = set(reads["dec"]) - set(reads["part"])

    assert not full & set(rows)


def test_reads_vendor_groups(catalog):
    catalog.page.locator("#catalogReads summary").click()
    groups = reads_groups(catalog)

    decoded = groups["Vendor elements decoded"]

    for item in ["00:A0:40 type 0 Apple Device IE",
                 "50:6F:9A type 9 P2P", "50:6F:9A type 22 MBO-OCE",
                 "50:6F:9A type 41 RSNE Override", "08:00:09 type 0 HP printer info"]:
        assert any(x.startswith(item) for x in decoded), item

    reviewed = groups["Vendor elements reviewed, content not shown"]

    assert len(reviewed) >= 100
    assert any(x.startswith("00:0C:43 type 3") for x in reviewed)

    # Wi-Fi Alliance type 35 is not known yet and stays catalogued
    assert not any(x.startswith("50:6F:9A type 35") for x in reviewed + decoded)

    assert "BEST GUESS" in groups["Everything else"][0]


def test_example_counts_senders(catalog):
    examples = column(catalog, "Example")

    # 73: ten senders kept, more seen; RSNE Override: four; others: one
    assert examples[0].endswith("(+9 or more senders)")
    assert examples[3].endswith("(+3 more senders)")
    assert "more sender" not in examples[1]


def test_qca_vendor_names(opened):
    names = opened.js("""[1, 2].map(t => catalogName({k: 2,
        id: (0x00137400 | t) >>> 0, vn: 'Atheros Communicatio'})[1])""")

    assert names == [
        "00:13:74 type 1 QCA HE Capabilities (pre-802.11ax) (Atheros Communicatio)",
        "00:13:74 type 2 QCA HE Operation (pre-802.11ax) (Atheros Communicatio)"]


def test_reads_vendor_names_or_maker(opened):
    # Every vendor element below the catalog gets a name: its own,
    # or "<maker> proprietary" from the OUI
    names = opened.js("""[0x00037F01, 0x00409603, 0x0050F211, 0x0017F206,
        0x00E0FC11, 0xAC853D20, 0x00173508, 0x00904C5C, 0x00137401,
        0x0002BC01, 0x7D3F80B3].map(vendorIeName)""")

    assert names == [
        "Atheros Advanced Capability", "Cisco CCX version",
        "Microsoft Network Cost", "Apple proprietary",
        "HUAWEI proprietary", "HUAWEI proprietary", "Intel proprietary",
        "Broadcom proprietary", "QCA HE Capabilities (pre-802.11ax)",
        "proprietary, the OUI names no maker",
        "proprietary, OUI without a known maker"]


def test_reads_list_has_no_bare_vendor_line(opened):
    html = opened.js("""catalogReadsHTML({dec: [], decx: [], part: [],
        pass: [], passx: [], vend: [0x0050F201], vpart: [],
        vrev: [0x000C4303, 0x00173508, 0x00137402], akm: [], cipher: []})""")

    assert "00:0C:43 type 3 MediaTek proprietary" in html
    assert "00:17:35 type 8 Intel proprietary" in html
    assert "00:13:74 type 2 QCA HE Operation (pre-802.11ax)" in html
    assert "00:50:F2 type 1 WPA" in html


def test_reads_vendor_names_from_the_176_catalog(opened):
    names = opened.js("""[0x5C5B3501, 0x000B8209, 0x00117400, 0x00504303,
        0x00000000, 0xAABBCC00].map(vendorIeName)""")

    assert names == [
        "Mist AP name", "Grandstream proprietary",
        "Mojo Networks (Arista) proprietary", "Marvell proprietary",
        "proprietary, the OUI names no maker",
        "proprietary, OUI without a known maker"]


def test_mist_ap_name_and_zero_suite(opened):
    html = opened.js("""catalogReadsHTML({dec: [], decx: [], part: [],
        pass: [], passx: [], vend: [0x5C5B3501], vpart: [], vrev: [],
        akm: [], cipher: [[0, "zero suite (broken WPA element)"]]})""")

    assert "5C:5B:35 type 1 Mist AP name" in html
    assert "00:00:00 0 zero suite (broken WPA element)" in html


def test_reads_vendor_names_from_the_233_catalog(opened):
    names = opened.js("""[0x00301A01, 0x44324400, 0x000F7DF9,
        0x000C4301].map(vendorIeName)""")

    assert names == [
        "Firmware version", "DJI proprietary",
        "Xirrus (Cambium) proprietary", "MediaTek proprietary"]


def test_xirrus_and_dji_as_maker(opened):
    # 00:30:1A keeps its registry name
    makers = opened.js("""apMakers({voui: [["00301A", 1, "SMARTBRIDGES"],
        ["000F7D", 249, "Xirrus"], ["443244", 0, ""]]}).makers""")

    assert makers == ["SMARTBRIDGES", "Xirrus (Cambium)", "DJI"]


def test_fits_the_width_of_the_network_table(catalog):
    # At 1400 px no sideways scrolling to reach the pcap link; long
    # names and examples wrap instead
    catalog.page.set_viewport_size({"width": 1400, "height": 900})

    wrap = catalog.js("""(() => {
        const w = document.getElementById('catalogTable')
            .closest('.tableWrap');
        return [w.scrollWidth, w.clientWidth];
    })()""")

    assert wrap[0] <= wrap[1]
    assert catalog.js("""getComputedStyle(document
        .querySelector('#catalogTable td')).whiteSpace""") == "normal"


def test_magsafe_catalog_dpp_and_nintendo_suites(opened):
    assert opened.js("vendorIeName(0x506F9A1E)") == \
        "DPP Configurator Connectivity"

    html = opened.js("""catalogReadsHTML({dec: [], decx: [], part: [],
        pass: [], passx: [], vend: [], vpart: [], vrev: [0x506F9A1E],
        akm: [[0xA4C0E102, "Nintendo"]], cipher: [[0xA4C0E104, "Nintendo"]]})""")

    assert "50:6F:9A type 30 DPP Configurator Connectivity" in html
    assert "A4:C0:E1 2 Nintendo" in html
    assert "A4:C0:E1 4 Nintendo" in html

    assert opened.js("[0x00146C08, 0x00268602].map(vendorIeName)") == [
        "NETGEAR proprietary", "Quantenna proprietary"]
