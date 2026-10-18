"""Maker and chip guess top right in the network tooltip."""

import pytest


def header_lines(app, network):
    """The BEST GUESS lines as text, without the heading."""
    lines = app.js("""n => {
        const d = document.createElement('div');
        d.innerHTML = apMakerHeader(n);
        return [...d.querySelectorAll('.tipMaker > div')]
            .map(x => x.textContent);
    }""", network)

    if lines:
        assert lines[0] == "BEST GUESS"

    return lines[1:]


def test_maker_and_chip_under_best_guess(opened):
    lines = header_lines(opened, {"voui": [["00040E", 1, "AVM"],
                                           ["8CFDF0", 1, "Qualcomm"]]})

    assert lines == ["Manufacturer: AVM", "Chip: Qualcomm"]


def test_broadcom_compat_elements_say_nothing(opened):
    # Epigram type 4 (256-QAM in 2.4 GHz) is copied by other chips
    lines = header_lines(opened, {"voui": [["00904C", 4, "Epigram"]]})

    assert lines == []


def test_broadcom_chip_from_other_type(opened):
    lines = header_lines(opened, {"voui": [["00904C", 1, "Epigram"]]})

    assert lines == ["Chip: Broadcom"]


def test_apple_device_ie_is_not_apple(opened):
    # 00:A0:40 type 0 comes from accessories, a car with CarPlay
    lines = header_lines(opened, {"voui": [["00A040", 0, "Apple"]]})

    assert lines == []


def test_apple_accessory_names_its_maker(opened):
    lines = header_lines(opened, {
        "voui": [["00A040", 0, "Apple"], ["001018", 2, "Broadcom"]],
        "acc": ["My SEAT 0001", "SEAT", "SE0000"]})

    assert lines == ["Manufacturer: SEAT", "Chip: Broadcom"]


def test_apple_accessory_text(opened):
    assert opened.js("appleAccText({acc: ['My SEAT 0001', 'SEAT', 'SE0000']})") \
        == "SEAT · SE0000 · My SEAT 0001"
    assert opened.js("appleAccText({acc: ['', '', '']})") == "element present"
    assert opened.js("appleAccText({})") == ""


def test_tooltip_of_fixture_car(opened):
    car = opened.js("networks.find(n => n.ssid === 'My SEAT 0001')")

    assert header_lines(opened, car) == [
        "Manufacturer: SEAT", "Chip: Broadcom", "Device: Vehicle"]

    text = opened.js("""() => {
        const d = document.createElement('div');
        d.innerHTML = networkTooltip(networks.find(n => n.ssid === 'My SEAT 0001'));
        return d.innerText;
    }""")

    assert "SEAT · SE0000 · My SEAT 0001" in text
    assert "venue Vehicular / Automobile or Truck" in text


def test_filter_finds_carplay(opened):
    names = opened.js("""() => networks
        .filter(n => { filterText = 'carplay'; return matchesFilter(n); })
        .map(n => n.ssid)""")

    assert names == ["My SEAT 0001"]


def test_no_header_without_a_guess(opened):
    assert opened.js("apMakerHeader({})") == ""


@pytest.mark.parametrize("voui, vendor, lines", [
    # Heritage OUIs get today's name
    ([["000B86", 1, "Hewlett Packard Ente"]], "", ["Manufacturer: Aruba (HPE)"]),
    ([["001977", 1, "Extreme Networks"]], "", ["Manufacturer: Aerohive (Extreme)"]),
    ([["00A0F8", 5, "Zebra"]], "", ["Manufacturer: Extreme WiNG"]),
    ([["1422DB", 2, "eero"]], "", ["Manufacturer: eero (Amazon)"]),
    # New chip OUIs
    ([["001C51", 170, "Celeno"]], "", ["Chip: Celeno"]),
    ([["4045DA", 1, "Spreadtrum"]], "", ["Chip: Unisoc (Spreadtrum)"]),
    ([["00025B", 3, "Cambridge Silicon"]], "", ["Chip: CSR (Qualcomm)"]),
    # Only the vendor elements count, not the BSSID's vendor
    ([["94BBAE", 250, "Husqvarna"]], "Espressif",
     ["Manufacturer: Husqvarna"]),
    # No registry name, NeXT and LVL7: no maker
    ([["00FDB8", 0, ""], ["7D3F80", 179, ""]], "", []),
    ([["00000F", 254, "NEXT"], ["0002BC", 1, "LVL 7 Systems"]], "", []),
    # MikroTik element from DD-WRT on a Belkin: not MikroTik
    ([["000C42", 0, "Routerboard.com"]], "Belkin", []),
    ([["000C42", 0, "Routerboard.com"]], "Routerboard.com",
     ["Manufacturer: MikroTik"]),
    # Short names instead of the registry's
    ([["00090F", 6, "Fortinet"]], "", ["Manufacturer: Fortinet"]),
    ([["00E0FC", 17, "HUAWEI TECHNOLOGIES"]], "",
     ["Manufacturer: HUAWEI"]),
    ([["8CBEBE", 135, "Xiaomi Communications"]], "",
     ["Manufacturer: Xiaomi"]),
    ([["6CB227", 0, "Sony Video & Sound Products"]], "",
     ["Manufacturer: Sony"]),
    ([["009D85", 101, "Sigmastar"]], "", ["Manufacturer: SigmaStar"]),
    # TI's ACX chips in early 11g routers
    ([["080028", 1, "Texas Instruments"]], "", ["Chip: Texas Instruments"]),
    ([["001D0F", 1, "TP-LINK TECHNOLOGIES"]], "", ["Manufacturer: TP-Link"]),
    # Intel's driver element: the chip, also for stations
    ([["001735", 8, "Intel Wireless Netwo"]], "", ["Chip: Intel"]),
    # hostapd's QCA elements say nothing about chip or maker
    ([["001374", 1, "Atheros Communicatio"],
      ["001374", 2, "Atheros Communicatio"]], "", []),
    # Sophos's OUI reversed, no registry entry
    ([["1C5A7C", 4, ""]], "", ["Manufacturer: Sophos"]),
    # Not an OUI at all
    ([["112233", 1, ""]], "", []),
])
def test_maker_rules(opened, voui, vendor, lines):
    assert header_lines(opened, {"voui": voui, "vendor": vendor}) == lines


def test_station_best_guess(opened):
    # A station behind a random MAC: Apple's and Broadcom's elements
    # in its probe requests, Epigram's compatibility element left out
    opened.page.evaluate("updateClients()")
    opened.page.wait_for_function("() => stations.length > 0")

    text = opened.js("""() => {
        const d = document.createElement('div');
        d.innerHTML = clientTooltip(stations[0]);
        return [...d.querySelectorAll('.tipMaker > div')].map(x => x.textContent)
            .concat([d.innerText]);
    }""")

    assert text[:3] == ["BEST GUESS", "Manufacturer: Apple", "Chip: Broadcom"]
    assert "00:17:F2 type 10 Apple" in text[-1]


def test_station_without_vendor_elements(opened):
    assert opened.js("apMakerHeader({mac: '02:00:00:00:01:02', voui: []})") == ""


def test_printer_under_best_guess(opened):
    lines = header_lines(opened, {"voui": [], "hp": ["HP OfficeJet 8010"]})

    assert lines == ["Device: Printer"]


def test_station_with_intel_element(opened):
    lines = opened.js("""() => {
        const d = document.createElement('div');
        d.innerHTML = apMakerHeader({mac: '02:00:00:00:01:09',
            voui: [['001735', 8, 'Intel Wireless Netwo']]});
        return [...d.querySelectorAll('div')].map(x => x.textContent);
    }""")

    assert "Chip: Intel" in lines
