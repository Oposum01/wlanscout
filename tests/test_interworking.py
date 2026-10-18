"""Interworking (107): decoding, venue names, "Device: Vehicle"."""

import pytest


@pytest.mark.parametrize("hex_body, text", [
    # Network type 2 + Internet, venue 10/1
    ("120A01", "chargeable public network, Internet, "
               "venue Vehicular / Automobile or Truck"),
    ("030A03", "free public network, venue Vehicular / Bus"),
    # Options only, no venue, no HESSID
    ("10", "private network, Internet"),
    # Options and HESSID (7 octets)
    ("03AABBCCDDEEFF", "free public network, HESSID AA:BB:CC:DD:EE:FF"),
    # All flags, venue and HESSID (9 octets)
    ("F30A07AABBCCDDEEFF",
     "free public network, Internet, additional step required, "
     "emergency services reachable, unauthenticated emergency service, "
     "venue Vehicular / Motor Bike, HESSID AA:BB:CC:DD:EE:FF"),
    ("020B05", "chargeable public network, venue Outdoor / Bus Stop"),
    # Unknown network type and venue type stay as numbers
    ("0A0A09", "network type 10, venue Vehicular / type 9"),
])
def test_iw_text(opened, hex_body, text):
    assert opened.js("h => iwText({iw: h})", hex_body) == text


def test_no_interworking(opened):
    assert opened.js("iwText({})") == ""
    assert opened.js("interworking({})") is None


@pytest.mark.parametrize("hex_body, vehicular", [
    ("120A01", True),
    ("030A06", True),
    ("030A00", True),
    ("020B05", False),
    ("10", False),
])
def test_device_vehicle(opened, hex_body, vehicular):
    header = opened.js("""h => {
        const d = document.createElement('div');
        d.innerHTML = apMakerHeader({iw: h});
        return d.textContent;
    }""", hex_body)

    assert ("Device: Vehicle" in header) == vehicular


def test_filter_finds_vehicular(opened):
    names = opened.js("""() => networks
        .filter(n => { filterText = 'vehicular'; return matchesFilter(n); })
        .map(n => n.ssid)""")

    assert names == ["My SEAT 0001"]
