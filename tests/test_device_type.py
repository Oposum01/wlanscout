"""What a device is, from the WSC Primary Device Type of its WPS
element or P2P Device Info, an Xbox's controller link and Nintendo's
suites; a Wi-Fi Direct device in its Listen state and the Xbox no
longer taken for WEP. The firmware's primaryDeviceType() and
xboxWirelessCost() are compiled on the PC from the sketch."""

import copy
import pathlib
import shutil
import subprocess

import pytest

from test_tooltip import tooltip

ROOT = pathlib.Path(__file__).resolve().parent.parent
SKETCH = ROOT / "source" / "wlanscout" / "wlanscout.ino"

HARNESS = r"""
#include <cstdio>
#include <cstdint>

struct WiFiNetwork { uint16_t pdtCat; uint16_t pdtSub; };

%s

// Lines of eight hex octets
int main()
{
    unsigned b[8];

    while (scanf("%%x %%x %%x %%x %%x %%x %%x %%x", &b[0], &b[1], &b[2],
                 &b[3], &b[4], &b[5], &b[6], &b[7]) == 8)
    {
        uint8_t t[8];
        WiFiNetwork n = { 0, 0 };

        for (int i = 0; i < 8; i++)
            t[i] = (uint8_t)b[i];

        primaryDeviceType(t, n);
        printf("%%u %%u\n", n.pdtCat, n.pdtSub);
    }
}
"""

# The Samsung TV of a walk: P2P Device Info as the sensor saw it
SAMSUNG_TV = {"ssid": "DIRECT-", "bssid": "42:16:3B:37:61:B2",
              "sec": "WEP", "p2p": [33, 0, "[TV] Samsung 6 Series (43)"],
              "wfd": 275, "pdt": [7, 1]}


XBOX_HARNESS = r"""
#include <cstdio>
#include <cstdint>

%s

%s

// Lines of hex octets: the elements of a beacon
int main()
{
    char line[1024];

    while (fgets(line, sizeof(line), stdin))
    {
        uint8_t ies[256];
        int n = 0;
        unsigned b;

        for (char *t = line; n < 256; )
        {
            int k = 0;

            if (sscanf(t, "%%x%%n", &b, &k) != 1)
                break;

            ies[n++] = (uint8_t)b;
            t += k;
        }

        int len = 0;
        const uint8_t *cost = findVendorIE(ies, n, 0x00, 0x50, 0xF2, 0x11, len);

        printf("%%d\n", xboxWirelessCost(cost, len) ? 1 : 0);
    }
}
"""

# The elements of the switched-off Xbox One's beacon (ch 48): no SSID,
# Microsoft's Network Cost with 12 octets
XBOX_IES = "00 00 dd 10 00 50 f2 11 01 10 06 2c 01 28 00 00 00 00 00 00"

# A Windows hotspot's Network Cost, the documented 4 octets
HOTSPOT_IES = "00 00 dd 08 00 50 f2 11 02 00 00 00"


@pytest.fixture(scope="module")
def xbox(tmp_path_factory):
    if not shutil.which("g++"):
        pytest.skip("no g++")

    src = SKETCH.read_text(encoding="utf-8")
    a = src.index("const uint8_t *findVendorIE(")
    find = src[a:src.index("\n}\n", a) + 3]
    b = src.index("bool xboxWirelessCost(")
    cost = src[b:src.index("\n}\n", b) + 3]

    d = tmp_path_factory.mktemp("xbox")
    (d / "t.cpp").write_text(XBOX_HARNESS % (find, cost))
    subprocess.run(["g++", "-o", str(d / "t"), str(d / "t.cpp")], check=True)

    return d / "t"


def is_xbox(binary, ies):
    return subprocess.run([str(binary)], input=ies + "\n", capture_output=True,
                          text=True, check=True).stdout.strip() == "1"


def test_firmware_knows_the_xbox_by_its_network_cost(xbox):
    assert is_xbox(xbox, XBOX_IES)
    assert not is_xbox(xbox, HOTSPOT_IES)
    assert not is_xbox(xbox, "00 00")


@pytest.fixture(scope="module")
def pdt(tmp_path_factory):
    if not shutil.which("g++"):
        pytest.skip("no g++")

    src = SKETCH.read_text(encoding="utf-8")
    a = src.index("static void primaryDeviceType(")
    helper = src[a:src.index("\n}\n", a) + 3]

    d = tmp_path_factory.mktemp("pdt")
    (d / "t.cpp").write_text(HARNESS % helper)
    subprocess.run(["g++", "-o", str(d / "t"), str(d / "t.cpp")], check=True)

    return d / "t"


def device_type(binary, octets):
    return subprocess.run([str(binary)], input=octets + "\n",
                          capture_output=True, text=True,
                          check=True).stdout.split()


def test_firmware_reads_category_and_subcategory(pdt):
    # Displays / Television, the Wi-Fi Alliance OUI
    assert device_type(pdt, "00 07 00 50 f2 04 00 01") == ["7", "1"]
    # Printers / All-in-one
    assert device_type(pdt, "00 03 00 50 f2 04 00 05") == ["3", "5"]


def test_firmware_keeps_only_the_category_for_another_oui(pdt):
    assert device_type(pdt, "00 08 00 00 f0 00 00 04") == ["8", "0"]


def test_firmware_ignores_category_0(pdt):
    assert device_type(pdt, "00 00 00 50 f2 04 00 01") == ["0", "0"]


def test_tv_in_the_tooltip(opened):
    text = tooltip(opened, SAMSUNG_TV)

    # Up in BEST GUESS, beside the maker; not again under RADIO
    assert "BEST GUESS" in text
    assert "Device: TV" in text
    assert "DeviceTV" not in text


def test_category_without_a_known_subcategory(opened):
    assert "Device: Printer or scanner" in tooltip(opened, {"pdt": [3, 0]})
    assert "Device: Device category 42" in tooltip(opened, {"pdt": [42, 3]})


def test_miracast_sink_without_a_type_is_a_display(opened):
    text = tooltip(opened, {"wfd": 17})

    assert "Device: Display (Miracast sink)" in text

    # A source says nothing about being a display
    assert "Miracast sink)" not in tooltip(opened, {"wfd": 16})


def test_no_device_line_for_routers(opened):
    text = tooltip(opened, {"pdt": [6, 1]})

    assert "access point" not in text
    assert "Device:" not in text


def test_search_finds_the_tv(opened):
    found = opened.js("""n => { filterText = 'device tv';
        const hit = matchesFilter(Object.assign({},
            networks.find(x => x.ssid === 'Plain'), n));
        const miss = matchesFilter(networks.find(x => x.ssid === 'Plain'));
        filterText = '';
        return [hit, miss]; }""", SAMSUNG_TV)

    assert found == [True, False]


def test_consoles_without_a_type(opened):
    xbox = tooltip(opened, {"xbox": 1, "sec": "Xbox Wireless"})

    assert "Device: Game console (Xbox" in xbox
    assert "Manufacturer: Microsoft" in xbox
    assert "Device: Game console (Nintendo" in tooltip(opened, {"akm": "Nintendo"})
    assert "Device: Game console (Nintendo" in tooltip(opened, {"pair": "Nintendo"})


def test_xbox_is_not_wep(opened):
    text = tooltip(opened, {"xbox": 1, "sec": "Xbox Wireless", "ssid": ""})

    assert "Xbox Wireless" in text
    assert "WEP" not in text


def with_network(app, extra):
    data = copy.deepcopy(app.api["networks"])
    data["networks"].append(dict(data["networks"][0], **extra))
    app.api["networks"] = data
    app.js("updateNetworks()")
    app.page.wait_for_function("b => networks.some(n => n.bssid === b)",
                               arg=extra["bssid"])

    return app.js("b => networks.find(n => n.bssid === b)", extra["bssid"])


def test_listen_state_is_not_wep(opened):
    n = with_network(opened, SAMSUNG_TV)

    assert n["sec"] == "Wi-Fi Direct"
    assert "WEP" not in tooltip(opened, n)


def test_real_wep_stays_wep(opened):
    # A group owner, or another SSID, with Privacy and no RSN is WEP
    owner = with_network(opened, dict(SAMSUNG_TV, bssid="42:16:3B:37:61:B3",
                                      p2p=[33, 1, "TV"]))
    named = with_network(opened, dict(SAMSUNG_TV, bssid="42:16:3B:37:61:B4",
                                      ssid="DIRECT-xy-TV"))

    assert owner["sec"] == "WEP"
    assert named["sec"] == "WEP"


def notes(app, extra):
    return app.js("""n => networkNotes(Object.assign({},
        networks.find(x => x.ssid === 'Plain'), n)).map(x => x[1])""", extra)


AP_NOTES = ("Hidden SSID", "No basic rate marked", "No Country element",
            "No 802.11n")

OLD_HIDDEN = {"ssid": "<hidden>", "basicrates": [], "basic": 0, "cc": "",
              "std": 2}


def test_no_ap_notes_for_an_xbox(opened):
    got = notes(opened, dict(OLD_HIDDEN, xbox=1, sec="Xbox Wireless"))

    assert "Not an access point: an Xbox's link to its controllers" in got
    assert not [t for t in got if t.startswith(AP_NOTES)]


def test_no_ap_notes_for_wifi_direct_listening(opened):
    got = notes(opened, dict(OLD_HIDDEN, sec="Wi-Fi Direct", ssid="DIRECT-"))

    assert any(t.startswith("Not an access point: a Wi-Fi Direct") for t in got)
    assert not [t for t in got if t.startswith(AP_NOTES)]


def test_an_old_ap_keeps_its_notes(opened):
    got = notes(opened, dict(OLD_HIDDEN, sec="WEP"))

    assert all(any(t.startswith(p) for t in got) for p in AP_NOTES)
    assert not any(t.startswith("Not an access point") for t in got)

    # Hidden: WEP, or the link of a device that is no AP
    wep = [t for t in got if t.startswith("WEP")][0]
    assert wep.endswith("or, with no SSID, the proprietary link of a "
                        "device that is no AP")
    assert "Xbox" not in wep

    # With an SSID it is WEP alone
    named = [t for t in notes(opened, dict(OLD_HIDDEN, sec="WEP", ssid="Old"))
             if t.startswith("WEP")][0]
    assert "no SSID" not in named


# A BMW's head unit (ch 11): Wi-Fi Direct group owner, WPS "Computer,
# unknown", Apple's Device IE with no name, a Miracast sink
BMW = {"ssid": "DIRECT-BMW 90527", "sec": "WPA2-PSK", "pdt": [1, 0],
       "wfd": 17, "acc": ["", "", ""], "p2p": [37, 1, "BMW 90527"]}


def test_carplay_head_unit_is_a_car(opened):
    text = tooltip(opened, BMW)

    assert "Device: Car (head unit, BMW, wireless CarPlay)" in text
    assert "Device: Computer" not in text


def test_apple_accessory_without_wifi_direct_is_no_car(opened):
    plain = dict(BMW, ssid="Plain")
    del plain["p2p"]

    assert "Device: Car" not in tooltip(opened, plain)


def test_computer_unknown_miracast_sink_is_a_display(opened):
    text = tooltip(opened, {"pdt": [1, 0], "wfd": 17})

    assert "Device: Display (Miracast sink)" in text

    # Without the sink, or with a real subcategory, the type stands
    assert "Device: Computer" in tooltip(opened, {"pdt": [1, 0]})
    assert "Device: Display" not in tooltip(opened, {"pdt": [1, 1], "wfd": 17})


# A Mercedes' MBUX (ch 6, snapshot of a walk): Wi-Fi Direct group owner
# calling itself a smartphone, no Apple element
MBUX = {"ssid": "DIRECT-MBUX 93321", "sec": "WPA2", "pdt": [10, 5],
        "p2p": [33, 139, ""], "wpsname": "MBUX 93321"}


def test_mbux_is_a_car_not_a_phone(opened):
    text = tooltip(opened, MBUX)

    assert "Device: Car (head unit, Mercedes MBUX)" in text
    assert "Smartphone" not in text
    assert "CarPlay" not in text


def test_car_by_its_ssid_alone(opened):
    assert "Device: Car (head unit, BMW)" in tooltip(
        opened, {"ssid": "DIRECT-BMW 12345"})


def test_a_phone_stays_a_phone(opened):
    phone = dict(MBUX, ssid="DIRECT-xy-Pixel 10", wpsname="Pixel 10")

    assert "Device: Smartphone" in tooltip(opened, phone)


# Ford SYNC (ch 1): nothing but its name and Ford's OUI
SYNC = {"ssid": "SYNC_XU614B6V", "bssid": "00:26:B4:BA:95:E9"}

# VW (ch 6): an AP in a vehicle by Interworking, Apple's Device IE
MY_VW = {"ssid": "My VW 9305", "iw": "020A01", "pdt": [6, 1],
         "acc": ["My VW 9305", "VW", "VW4820"]}

# TF790 (ch 36), unknown: Apple's Device IE with placeholders
TF790 = {"ssid": "TF790-10691e4e", "acc": ["ZJ", "Apple", "Device1,1"],
         "voui": [["00A040", 0, "Apple"]]}


def test_ford_sync_by_name_and_oui(opened):
    assert "Device: Car (head unit, Ford SYNC)" in tooltip(opened, SYNC)

    # The name alone from another OUI is no Ford
    assert "Ford" not in tooltip(opened, dict(SYNC, bssid="02:11:22:33:44:55"))


def test_vehicle_with_carplay(opened):
    text = tooltip(opened, MY_VW)

    assert "Device: Vehicle (wireless CarPlay)" in text
    assert "Manufacturer: VW" in text

    plain = dict(MY_VW)
    del plain["acc"]
    assert "CarPlay" not in tooltip(opened, plain)


def test_apple_as_accessory_maker_is_left_out(opened):
    text = tooltip(opened, TF790)

    assert "Manufacturer: Apple" not in text
    assert "Device: Car" not in text


# Audi (ch 6): a vehicle by Interworking, Apple's element left empty
AUDI = {"ssid": "Audi_MMI_2527", "iw": "020A01", "acc": ["", "", ""]}


def test_car_maker_from_the_head_unit(opened):
    assert "Manufacturer: Mercedes-Benz" in tooltip(opened, MBUX)
    assert "Manufacturer: Ford" in tooltip(opened, SYNC)

    audi = tooltip(opened, AUDI)
    assert "Manufacturer: Audi" in audi
    assert "Device: Vehicle (wireless CarPlay)" in audi

    # The maker a vendor element names stands alone
    assert tooltip(opened, MY_VW).count("Manufacturer: VW") == 1


def test_bmw_with_digits_right_after(opened):
    text = tooltip(opened, {"ssid": "BMW51589 CarPlay"})

    assert "Device: Car (head unit, BMW)" in text
    assert "Manufacturer: BMW" in text

    # A word that only starts with BMW is no BMW
    assert "Device: Car" not in tooltip(opened, {"ssid": "BMWorld"})


# Škoda (ch 6): a vehicle by Interworking, Apple's element left empty
SKODA = {"ssid": "My Skoda 9610", "iw": "120A01", "acc": ["", "", ""]}


def test_vw_group_by_name(opened):
    assert "Manufacturer: \u0160koda" in tooltip(opened, SKODA)

    # Without Interworking and Apple's element the name still tells
    assert "Device: Car (head unit, \u0160koda)" in tooltip(
        opened, {"ssid": "My Skoda 9610"})
    assert "Device: Car (head unit, VW)" in tooltip(opened, {"ssid": "My VW 5975"})
    assert "Manufacturer: Volkswagen" in tooltip(opened, {"ssid": "My VW 5975"})
    assert "Device: Car (head unit, SEAT)" in tooltip(opened, {"ssid": "My SEAT 2945"})

    # The maker Apple's element names stays as sent
    assert "Manufacturer: VW" in tooltip(opened, MY_VW)


# Porsche (ch 6): Harman's head unit, the brand only as its model
PORSCHE = {"ssid": "911_1892", "iw": "020A01",
           "acc": ["911_1892", "HARMAN", "PORSCHE"]}

# A car's hotspot (ch 6) from AUMOVIO's OUI, a vehicle by Interworking
HOTSPOT = {"ssid": "Hotspot67B0", "bssid": "00:54:AF:EE:67:B0",
           "iw": "000A01"}


def test_car_brand_from_the_model_field(opened):
    assert "Manufacturer: Porsche \u00b7 HARMAN" in tooltip(opened, PORSCHE)
    assert "Manufacturer: Audi \u00b7 HARMAN" in tooltip(
        opened, dict(PORSCHE, acc=["Slugi", "HARMAN", "AUDI"]))

    # A model number is no brand; a brand said twice is said once
    assert "Manufacturer: SEAT" in tooltip(opened, {"acc": ["", "SEAT", "SE3820"]})
    assert "Manufacturer: ZJ" in tooltip(opened, {"acc": ["", "ZJ", "Linux"]})
    assert "Manufacturer: BMWPlain" in tooltip(opened, {"acc": ["", "BMW", "BMW"]})


def test_car_module_on_a_vehicle_only(opened):
    assert "Module: AUMOVIO (telematics)" in tooltip(opened, HOTSPOT)

    plain = dict(HOTSPOT)
    del plain["iw"]
    assert "Module:" not in tooltip(opened, plain)


# A CarPlay dongle (ch 36): its SoC maker in Apple's Device IE
CARLINK = {"ssid": "CARLINK-8AE40F", "iw": "000A01",
           "acc": ["", "arkmicro", "Linux"]}

# A retrofitted Android head unit (ch 44): a bus by Interworking
ANDROID_AP = {"ssid": "AndroidAP_8126", "iw": "030A03", "acc": ["", "", ""]}


def test_soc_maker_is_a_chip(opened):
    text = tooltip(opened, CARLINK)

    assert "Chip: Arkmicro" in text
    assert "Manufacturer: arkmicro" not in text


def test_aftermarket_head_unit(opened):
    assert "Device: Vehicle (aftermarket head unit, wireless CarPlay)" in \
        tooltip(opened, ANDROID_AP)

    # A real bus, or an AndroidAP elsewhere, is not one
    assert "aftermarket" not in tooltip(opened, dict(ANDROID_AP, ssid="Bus 42"))
    assert "aftermarket" not in tooltip(opened, dict(ANDROID_AP, iw="030A01"))


# Fiat (ch 149): Marelli's head unit, the brand as the model's first word
FIAT = {"ssid": "My Fiat SJ806696", "iw": "040A01",
        "acc": ["My Fiat SJ806696", "Marelli", "Fiat CARLINE_11"]}


def test_fiat_from_the_model_and_its_name(opened):
    text = tooltip(opened, FIAT)

    assert "Manufacturer: Fiat \u00b7 Marelli" in text
    assert "Device: Vehicle (wireless CarPlay)" in text

    # By its name alone
    assert "Device: Car (head unit, Fiat)" in tooltip(
        opened, {"ssid": "My Fiat SJ806696"})
    assert "Manufacturer: Fiat" in tooltip(opened, {"ssid": "My Fiat SJ806696"})
