"""Layout of the network tooltip: WPS on the security line, label column, vendor
elements, channel line, WPS note."""


def tooltip(app, overrides):
    """Tooltip of the Plain network with some fields changed, as text."""
    return app.js("""n => {
        const d = document.createElement('div');
        d.innerHTML = networkTooltip(Object.assign({},
            networks.find(x => x.ssid === 'Plain'), n));
        return d.innerText;
    }""", overrides)


def test_wps_on_the_security_line(opened):
    text = tooltip(opened, {"wps": 1, "wpsname": "FBox", "hs20": 1})

    assert "PMF off · WPS active, device “FBox”" in text
    assert "\nWPS" not in text
    assert "AlsoHotspot 2.0" in text


def test_no_wps_without_wps(opened):
    assert "WPS" not in tooltip(opened, {"wps": 0})


def test_label_column_as_narrow_as_its_labels(opened):
    # The security line spans both columns; it must not widen "Also"
    gap = opened.js("""() => {
        const n = Object.assign({}, networks.find(x => x.ssid === 'Plain'),
                                {wps: 1, wpsname: 'FBox', hs20: 1});
        showTip(networkTooltip(n), 10, 10);
        const label = [...document.querySelectorAll('#tip td.tipL')]
            .find(td => td.textContent === 'Also');
        const value = label.nextElementSibling;
        return value.getBoundingClientRect().left -
               label.getBoundingClientRect().right;
    }""")

    assert gap <= 12


def test_vendor_elements_grouped_per_oui(opened):
    text = opened.js("""vendorOuiText({voui: [
        ['00037F', 4, 'Atheros Communicatio'],
        ['00037F', 1, 'Atheros Communicatio'],
        ['8CFDF0', 1, 'Qualcomm'], ['8CFDF0', 4, 'Qualcomm'],
        ['0050F2', 2, 'Microsoft'], ['0050F2', 4, 'Microsoft'],
        ['00040E', 1, 'AVM']]})""").split("<br>")
    # Same maker and kind in one line, named types on their own
    assert text == [
        "00:03:7F type 4 Atheros Communicatio (chip)",
        "00:03:7F type 1 Atheros Communicatio (Atheros Advanced Capability)",
        "8C:FD:F0 type 1, 4 Qualcomm (chip)",
        "00:50:F2 type 2 Microsoft (WMM)",
        "00:50:F2 type 4 Microsoft (WPS)",
        "00:04:0E type 1 AVM"]




def test_no_centre_channel_at_20_mhz(opened):
    assert "centre channel" not in tooltip(opened, {})
    assert "(centre channel 42, 5210 MHz)" in \
        opened.js("""() => {
            const d = document.createElement('div');
            d.innerHTML = networkTooltip(
                networks.find(x => x.ssid === 'Office'));
            return d.innerText;
        }""")


def test_wps_note(opened):
    notes = "n => networkNotes(n).map(x => x[1]).join(' | ')"

    assert "WPS present, no configuration methods announced and so no " \
        "PIN method" in opened.js(notes, {"wps": 1, "wpsm": 0})
    assert "WPS present: Push Button. No PIN method among them" in \
        opened.js(notes, {"wps": 1, "wpsm": 0x0080})


def test_wps_maker_and_model(opened):
    text = tooltip(opened, {"wps": 1, "wpsdev": ["HP", "", "G5J38A"]})

    assert "WPS active, “HP G5J38A”" in text


def test_wps_device_text_variants(opened):
    f = "d => wpsDeviceText({wpsdev: d})"

    assert opened.js(f, ["HP", "", "G5J38A"]) == "HP G5J38A"
    # A model name that starts with the maker or holds the number
    assert opened.js(f, ["NETGEAR", "NETGEAR R7000", "R7000"]) == "NETGEAR R7000"
    assert opened.js(f, ["AVM", "FRITZ!Box", "7590"]) == "AVM FRITZ!Box 7590"
    assert opened.js(f, ["", "", "X1"]) == "X1"
    assert opened.js("wpsDeviceText({})") == ""


def test_wps_model_in_the_filter(opened):
    names = opened.js("""() => {
        networks[0].wpsdev = ['HP', '', 'G5J38A'];
        filterText = 'g5j38a';
        return networks.filter(matchesFilter).map(n => n.bssid);
    }""")

    assert len(names) == 1


def test_display_pin_without_lock_is_red(opened):
    # The HP printer: Display + Physical Display, no AP Setup Locked
    notes = opened.js("n => networkNotes(n).map(x => [x[0], x[1]])",
                      {"wps": 1, "wpsm": 0x4008})

    assert any(c == "#d08080" and t.startswith("WPS with a PIN method and "
               "AP Setup Locked not set") for c, t in notes)


def note_texts(app, overrides):
    return app.js("""n => networkNotes(Object.assign({},
        networks.find(x => x.ssid === 'Plain'), n)).map(x => x[1])""",
                  overrides)


def test_hidden_ssid_note(opened):
    notes = note_texts(opened, {"ssid": "<hidden>"})

    assert any(t.startswith("Hidden SSID: no protection.") for t in notes)


def test_hidden_ssid_learned_says_how(opened):
    notes = note_texts(opened, {"ssid": "Office", "resolved": 1})

    assert any("that is how the name above was learned" in t for t in notes)


def test_hidden_note_not_for_mesh_nan_or_owe(opened):
    for extra in ({"ssid": "<hidden>", "mesh": 1},
                  {"ssid": "<hidden>", "bssid": "50:6F:9A:01:00:01"},
                  {"ssid": "Open", "resolved": 1, "owename": 1}):
        assert not any(t.startswith("Hidden SSID")
                       for t in note_texts(opened, extra)), extra


def test_broadcom_256qam_in_2_4_ghz(opened):
    voui = [["00904C", 4, "Epigram"]]

    wifi6 = note_texts(opened, {"channel": 5, "std": 6, "voui": voui})
    wifi4 = note_texts(opened, {"channel": 5, "std": 4, "voui": voui})
    five = note_texts(opened, {"channel": 36, "std": 6, "voui": voui})

    assert any("Wi-Fi 6 brings 256-QAM in 2.4 GHz anyway" in t for t in wifi6)
    assert any("802.11n itself stops at 64-QAM" in t for t in wifi4)
    assert not any(t.startswith("Broadcom's element") for t in five)


def test_vht_elements_note_wins_over_the_vendor_one(opened):
    notes = note_texts(opened, {"channel": 5, "vht24": 1,
                                "voui": [["00904C", 4, "Epigram"]]})

    assert any(t.startswith("VHT elements in a 2.4 GHz beacon") for t in notes)
    assert not any(t.startswith("Broadcom's element") for t in notes)


def country_notes(app, codes, which):
    """Notes of network `which` among networks with these countries."""
    return app.js("""([codes, which]) => {
        const base = networks.find(x => x.ssid === 'Plain');
        networks = codes.map((cc, i) => Object.assign({}, base,
            {bssid: '02:00:00:00:77:' + String(i).padStart(2, '0'), cc,
             ssid: 'Net' + i}));
        return networkNotes(networks[which]).map(x => x[0] + ' ' + x[1]);
    }""", [codes, which])


def test_country_other_than_most(opened):
    notes = country_notes(opened, ["DE", "DE", "DE", "DE", "AT", "CN"], 5)

    blue = [t for t in notes if t.startswith("#7aaed0 Country CN")]

    assert blue and "4 of 6 networks here with a Country element say DE " \
        "(67 %)" in blue[0]


def test_country_note_needs_five_and_a_majority(opened):
    # Only four with a Country element
    assert not any("Country CN" in t for t in
                   country_notes(opened, ["DE", "DE", "DE", "CN", ""], 3))
    # Half is not more than half
    assert not any("Country CN" in t for t in
                   country_notes(opened, ["DE", "DE", "DE", "CN", "CN", "AT"],
                                 3))
    # The majority itself says nothing
    assert not any(" while " in t for t in
                   country_notes(opened, ["DE"] * 5 + ["CN"], 0))


def test_preauth_without_8021x(opened):
    psk = note_texts(opened, {"preauth": 1, "akm": "PSK"})
    eap = note_texts(opened, {"preauth": 1, "akm": "802.1X, FT-802.1X"})

    assert any(t.startswith("Pre-authentication announced without 802.1X")
               and "which PSK does not use" in t for t in psk)
    assert not any(t.startswith("Pre-authentication") for t in eap)


def test_low_basic_rate_suggests_switching_off_11b(opened):
    notes = note_texts(opened, {"basic": 2})

    assert any(t.startswith("Lowest basic rate 1 Mbit/s") and
               "switch 802.11b off (lowest basic rate 6 Mbit/s or more)" in t
               for t in notes)
    assert not any(t.startswith("Lowest basic rate")
                   for t in note_texts(opened, {"basic": 12}))


def test_no_basic_rate_marked(opened):
    notes = note_texts(opened, {"basic": 0, "basicrates": []})

    assert any(t.startswith("No basic rate marked") for t in notes)
    assert not any(t.startswith("No basic rate marked") for t in
                   note_texts(opened, {"basic": 12, "basicrates": [12, 24]}))


def test_filter_basic_rates(opened):
    match = """([n, t]) => { networks = [Object.assign({},
        networks.find(x => x.ssid === 'Plain'), n)];
        filterText = t; const r = matchesFilter(networks[0]);
        filterText = ''; return r; }"""

    assert opened.js(match, [{"basicrates": []}, "none"])
    assert opened.js(match, [{"basicrates": [2, 4]}, "basic rates 1"])
    assert not opened.js(match, [{"basicrates": [12]}, "none"])


def test_intolerant_but_40_mhz_itself_is_red(opened):
    red = opened.js("""n => networkNotes(Object.assign({},
        networks.find(x => x.ssid === 'Plain'), n)).filter(x =>
        x[1].startsWith('40 MHz Intolerant')).map(x => x[0])""",
        {"intol": 1, "bandwidth": 40, "channel": 3})
    amber = opened.js("""n => networkNotes(Object.assign({},
        networks.find(x => x.ssid === 'Plain'), n)).filter(x =>
        x[1].startsWith('40 MHz Intolerant')).map(x => x[0])""",
        {"intol": 1, "bandwidth": 20, "channel": 3})

    assert red == ["#d08080"]
    assert amber == ["#c8a050"]


def test_notes_sorted_red_amber_blue_pink(opened):
    colours = opened.js("""() => {
        const d = document.createElement('div');
        d.innerHTML = tipNotes([['#7aaed0', 'b'], ['#e070c0', 'p'],
                                ['#c8a050', 'a1'], ['#d08080', 'r'],
                                ['#c8a050', 'a2']]);
        return [...d.querySelectorAll('span')].map(s => s.textContent);
    }""")

    # Within a colour the order stays as it was
    assert colours == ["• r", "• a1", "• a2", "• b", "• p"]


# ---- overlap, legacy, open, security and country across BSSIDs -------

def test_overlapping_24_ghz_channel(opened):
    for ch in [2, 3, 4, 7, 8, 10, 12]:
        notes = note_texts(opened, {"channel": ch, "bandwidth": 20})
        assert any(t.startswith("Channel %d overlaps the neighbours" % ch)
                   for t in notes), ch

    for ch in [1, 5, 6, 9, 11, 13, 36]:
        notes = note_texts(opened, {"channel": ch})
        assert not any("overlaps the neighbours" in t for t in notes), ch


def test_legacy_only_bss_says_replace_it(opened):
    notes = note_texts(opened, {"std": 3})

    assert any(t.startswith("No 802.11n: a 802.11g BSS") and
               t.endswith("Better replace the old device with a new one")
               for t in notes)
    assert not any("No 802.11n" in t for t in note_texts(opened, {"std": 4}))
    assert not any("No 802.11n" in t for t in note_texts(opened, {"std": 0}))


def test_open_network_suggests_owe(opened):
    notes = note_texts(opened, {"sec": "Open", "owetrans": 0, "owename": ""})

    assert any(t.startswith("Open network: everything is sent in clear") and
               "OWE (Enhanced Open)" in t for t in notes)

    # The open half of an OWE transition pair, and a mesh, are fine
    for extra in [{"owetrans": 1}, {"owename": "x"}, {"mesh": 1}]:
        notes = note_texts(opened, dict({"sec": "Open"}, **extra))
        assert not any(t.startswith("Open network") for t in notes), extra


def across(app, nets):
    """networkNotes of each network, with these as the network table."""
    return app.js("""nets => { networks = nets;
        return nets.map(n => networkNotes(n).map(x => x[0] + ' ' + x[1])); }""",
                  nets)


def bss(bssid, ssid, channel, sec="WPA2", cc="DE", **more):
    return dict({"bssid": bssid, "ssid": ssid, "channel": channel,
                 "sec": sec, "cc": cc, "akm": "PSK", "pair": "CCMP-128",
                 "group": "CCMP-128", "bi": 100, "std": 6, "bandwidth": 20,
                 "vendor": "", "voui": [], "basic": 12,
                 "basicrates": [12]}, **more)


def test_same_ssid_other_security_on_this_device(opened):
    # FRITZ!Box: 2.4 GHz WPA2, 5 GHz WPA2/WPA3, consecutive addresses
    notes = across(opened, [
        bss("02:00:00:00:44:FF", "Home", 6, "WPA2"),
        bss("02:00:00:00:45:00", "Home", 36, "WPA2/WPA3"),
        bss("02:00:00:00:90:00", "Home", 44, "WPA2/WPA3")])

    text = "The same SSID on channel 36 of this device offers WPA2/WPA3, " \
        "here WPA2"
    assert any(t.startswith("#7aaed0 " + text) for t in notes[0])
    assert any("offers WPA2, here WPA2/WPA3" in t for t in notes[1])
    # Another device far off in the address space is not this device
    assert not any("The same SSID" in t for t in notes[2])


def test_same_security_or_other_ssid_no_note(opened):
    notes = across(opened, [
        bss("02:00:00:00:44:F0", "Home", 6, "WPA2/WPA3"),
        bss("02:00:00:00:44:F1", "Home", 36, "WPA2/WPA3"),
        bss("02:00:00:00:44:F2", "Guest", 36, "Open")])

    assert not any("The same SSID" in t for n in notes for t in n)


def test_several_countries_for_one_network(opened):
    # Router DE, its repeater CH, same SSID
    notes = across(opened, [
        bss("02:00:00:00:01:00", "Vodafone-123456", 36, cc="DE"),
        bss("02:00:00:11:22:00", "Vodafone-123456", 3, cc="CH"),
        bss("02:00:00:33:44:00", "Other", 1, cc="CH")])

    assert any(t == "#7aaed0 Country DE here, CH on other access points "
               "with this SSID: several countries for one network are "
               "implausible, one of them has its region set wrong"
               for t in notes[0])
    assert any(t.startswith("#7aaed0 Country CH here, DE") for t in notes[1])
    assert not any("several countries" in t for t in notes[2])


# ---- SAE without PMF: an access point, or an 802.11s mesh point -------

def test_sae_without_pmf_red_for_an_ap(opened):
    notes = note_texts(opened, {"sec": "WPA3", "akm": "SAE", "mfpc": 0,
                                "mfpr": 0, "mesh": 0})

    assert any(t.startswith("SAE offered while protected management "
                            "frames are not even supported") for t in notes)
    assert any(t.endswith("so this BSS is not conforming") for t in notes)


def test_sae_without_pmf_on_a_mesh_point_is_blue(opened):
    notes = opened.js("""n => networkNotes(Object.assign({},
        networks.find(x => x.ssid === 'Plain'), n))""",
        {"sec": "WPA3", "akm": "SAE", "mfpc": 0, "mfpr": 0, "mesh": 1})
    texts = [t for _, t in notes]

    assert not any("not conforming" in t or "not even supported" in t
                   for t in texts)
    assert ["#7aaed0", "Mesh with SAE, management frames between the mesh "
            "points not protected: PMF is off, the default of "
            "wpa_supplicant for a mesh. The peering itself is secured by "
            "SAE and AMPE"] in notes

    # With PMF on, nothing to say
    notes = note_texts(opened, {"sec": "WPA3", "akm": "SAE", "mfpc": 1,
                                "mfpr": 0, "mesh": 1})
    assert not any(t.startswith("Mesh with SAE") for t in notes)
