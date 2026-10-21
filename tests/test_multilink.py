"""A station's multi-link request in its tooltip: links by band, NSTR."""

# Association request on 5 GHz naming a 6 GHz link 2 that cannot
# send and receive at the same time as link 1, as an EMLSR phone
# sends it (synthetic addresses)
STATION = {
    "mac": "02:00:00:00:02:01",
    "bssid": "02:00:00:00:0a:01",
    "mld": "02:00:00:00:02:00",
    "mlolink": -1, "mlof": 3, "eml": 0x25, "mldcap": 0x0fe1,
    "mlolinks": 4,
    "mlomacs": [{"link": 2, "mac": "02:00:00:00:02:02"}],
    "mloband": 5,
    "mloprof": [{"link": 2, "band": 6, "width": 160, "nstr": 2}],
}


def test_links_by_band(opened):
    text = opened.js("""c => mloLinkList(c, {mlolink: 1})
        .map(mloLinkText)""", STATION)

    assert text == [
        "5 GHz · link 1 · association link",
        "6 GHz · link 2 · up to 160 MHz · 02:00:00:00:02:02",
    ]


def test_association_link_unknown_without_ap(opened):
    text = opened.js("c => mloLinkList(c, undefined).map(mloLinkText)",
                     STATION)

    assert text[0] == "5 GHz · association link"


def test_nstr_by_band(opened):
    assert opened.js("c => nstrText(mloLinkList(c, {mlolink: 1}))",
                     STATION) == \
        "5 + 6 GHz, not sending on one while receiving on the other"


def test_nstr_between_two_profiles(opened):
    # 2.4 and 6 GHz named in profiles, each pointing at the other:
    # one pair, not two
    station = dict(STATION, mloprof=[
        {"link": 0, "band": 2, "width": 40, "nstr": 4},
        {"link": 2, "band": 6, "width": 320, "nstr": 1}])

    assert opened.js("c => nstrText(mloLinkList(c, {mlolink: 1}))",
                     station) == \
        "2.4 + 6 GHz, not sending on one while receiving on the other"


def test_no_nstr(opened):
    station = dict(STATION, mloprof=[
        {"link": 2, "band": 6, "width": 160, "nstr": 0}])

    assert opened.js("c => nstrText(mloLinkList(c, {mlolink: 1}))",
                     station) == ""


def test_band_not_given(opened):
    station = dict(STATION, mloprof=[
        {"link": 3, "band": 0, "width": 0, "nstr": 0}])

    assert opened.js("c => mloLinkText(mloLinkList(c, null)[1])",
                     station) == "band not given · link 3"


def test_tooltip_rows(opened):
    text = opened.js("""c => {
        const d = document.createElement('div');
        d.innerHTML = clientTooltip(c);
        document.body.appendChild(d);
        const t = d.innerText;
        d.remove();
        return t;
    }""", STATION)

    assert "6 GHz · link 2 · up to 160 MHz" in text
    assert "5 + 6 GHz, not sending on one" in text
    assert "Requested" not in text


def test_tooltip_older_firmware(opened):
    # Without mloprof (firmware before 1.68.0) the old rows stay
    station = {k: v for k, v in STATION.items()
               if k not in ("mloprof", "mloband")}

    text = opened.js("""c => {
        const d = document.createElement('div');
        d.innerHTML = clientTooltip(c);
        document.body.appendChild(d);
        const t = d.innerText;
        d.remove();
        return t;
    }""", station)

    assert "links 2" in text
    assert "link 2 02:00:00:00:02:02" in text


# ---- multi-link split up over several single-link MLDs -----------------

def ap(bssid, ssid, channel, sec, mld=None, link=0, links=0):
    return {"bssid": bssid, "ssid": ssid, "channel": channel, "sec": sec,
            "mld": mld or bssid, "mlolink": link, "mlolinks": links,
            "mlof": 3, "mldcap": 32, "eml": 1, "vendor": ""}


def split_notes(app, nets):
    return app.js("""nets => { networks = nets;
        return nets.map(n => mloSplitText(n)); }""", nets)


def test_split_names_and_security(opened):
    # One device, the 5 GHz BSSID with the locally administered bit,
    # each band an AP MLD of its own with one link
    notes = split_notes(opened, [
        ap("08:00:00:00:09:C0", "Hall", 6, "WPA2"),
        ap("0A:00:00:00:09:C0", "Hall_5GHz", 112, "WPA2/WPA3")])

    assert notes[0].startswith(
        "Multi-link split: this device also runs “Hall_5GHz” "
        "on 5 GHz as a separate AP MLD with one link")
    assert "the names differ and the security differs " \
           "(WPA2 vs WPA2/WPA3)" in notes[0]
    assert "“Hall” on 2.4 GHz" in notes[1]


def test_split_with_the_same_name(opened):
    # Consecutive BSSIDs, same SSID and security, still two MLDs
    notes = split_notes(opened, [
        ap("02:00:00:00:09:C0", "Hall", 1, "WPA3"),
        ap("02:00:00:00:09:C1", "Hall", 36, "WPA3")])

    assert notes[0].endswith("share SSID and security, so clients get "
                             "no multi-link across the bands")


def test_one_mld_with_two_links_is_fine(opened):
    notes = split_notes(opened, [
        ap("02:00:00:00:09:C0", "Hall", 1, "WPA3",
           mld="02:00:00:00:09:00", link=0),
        ap("02:00:00:00:09:C1", "Hall", 36, "WPA3",
           mld="02:00:00:00:09:00", link=1)])

    assert notes == ["", ""]


def test_other_devices_or_a_lone_mld_say_nothing(opened):
    notes = split_notes(opened, [
        ap("02:00:00:00:09:C0", "Hall", 1, "WPA3"),
        ap("02:00:00:00:0A:C0", "Hall", 36, "WPA3"),
        ap("02:00:00:00:0B:C0", "Lone", 44, "WPA3")])

    assert notes == ["", "", ""]


def test_split_shows_in_the_tooltip_and_the_filter(opened):
    nets = [ap("08:00:00:00:09:C0", "Hall", 6, "WPA2"),
            ap("0A:00:00:00:09:C0", "Hall_5GHz", 112, "WPA2/WPA3")]

    assert opened.js("""nets => { networks = nets;
        return networkNotes(nets[0]).some(n => n[0] === '#c8a050' &&
            n[1].startsWith('Multi-link split')); }""", nets)


def test_guest_beside_main_on_one_radio_is_fine(opened):
    # Two SSIDs on the same 5 GHz radio, each its own MLD: no split
    notes = split_notes(opened, [
        ap("0C:00:00:00:5E:C2", "Main", 36, "WPA2"),
        ap("0E:00:00:00:5E:C2", "Guest", 36, "WPA2", mld="EE:00:00:00:5E:C2")])

    assert notes == ["", ""]


def test_link_one_has_a_link_zero_somewhere(opened):
    # The guest names itself link 1: its link 0 is just not heard
    notes = split_notes(opened, [
        ap("08:00:00:00:09:C0", "Hall", 6, "WPA2"),
        ap("0A:00:00:00:09:C0", "Hall guest", 36, "WPA2",
           mld="EA:00:00:00:09:C0", link=1)])

    assert notes == ["", ""]


def ap_notes(app, extra):
    return app.js("""o => networkNotes(Object.assign({}, networks[0], o))
        .map(n => n[0] + ' ' + n[1])""", extra)


def test_wifi7_security_gaps(opened):
    base = {"std": 7, "akm": "PSK, SAE", "pair": "CCMP-128", "bprot": 0,
            "ovr": None}

    notes = ap_notes(opened, base)
    assert any(n.startswith("#c8a050 Wi-Fi 7 without SAE-EXT-KEY, GCMP-256, "
                            "beacon protection") for n in notes)

    # All of it in the RSNE Override, with beacon protection: none
    full = dict(base, bprot=1, ovr={"akm": "SAE-EXT-KEY", "pair":
                                    "GCMP-256, CCMP-128", "pmf": 2, "h2e": 1})
    assert not any("Wi-Fi 7 without" in n for n in ap_notes(opened, full))

    # Not for Wi-Fi 6, nor for an open network
    assert not any("Wi-Fi 7 without" in n
                   for n in ap_notes(opened, dict(base, std=6)))
    assert not any("Wi-Fi 7 without" in n
                   for n in ap_notes(opened, dict(base, akm="")))


def test_link_removal_note(opened):
    notes = ap_notes(opened, {"mlrm": [0b100, 30], "bi": 100})

    assert any(n.startswith("#7aaed0 Multi-link AP removing link 2 in 30 "
                            "beacons (≈ 3 s") for n in notes)


def test_eht_capabilities_named(opened):
    assert opened.js("ehtCapText({ehtcap: [0x31, 0x02, 1]})") == \
        "4096-QAM, 320 MHz (6 GHz), restricted TWT, EPCS priority " \
        "access, SCS traffic description"
    assert opened.js("ehtCapText({ehtcap: [0, 0, 0]})") == ""


def test_twt_text(opened):
    assert opened.js("twtText({twt: 6, btwt: [[3, 4, 102400, 8192]]})") == \
        "responder, broadcast, schedule 3: every 102.4 ms for 8.2 ms " \
        "(restricted TWT, for low latency)"
    assert opened.js("twtText({twt: 1})") == "requester"
    assert opened.js("twtText({})") == ""


def test_he_details_and_colour_collision(opened):
    assert opened.js("heCapText({hecap: 1 | 2 | 4 | 16})") == \
        "1024-QAM, UL MU-MIMO, MU beamformer, DCM"
    assert opened.js("twtText({twt: 2, heop: 1})") == \
        "responder, required by the AP"

    opened.js("""networks.push(
        Object.assign({}, networks[0], {bssid: '0A:11:00:00:00:01',
            ssid: 'One', channel: 6, color: [12, 0]}),
        Object.assign({}, networks[0], {bssid: '0A:22:00:00:00:01',
            ssid: 'Two', channel: 6, color: [12, 0]}),
        Object.assign({}, networks[0], {bssid: '0A:11:00:00:00:02',
            ssid: 'OneToo', channel: 6, color: [12, 0]}),
        Object.assign({}, networks[0], {bssid: '0A:33:00:00:00:01',
            ssid: 'Far', channel: 11, color: [12, 0]}))""")

    def notes(bssid):
        return opened.js("""b => networkNotes(networks.find(n => n.bssid === b))
            .map(n => n[0] + ' ' + n[1])""", bssid)

    one = notes("0A:11:00:00:00:01")
    assert any(n.startswith("#c8a050 BSS colour 12 also on Two 0A:22:00:00:00:01 "
                            "on this channel") for n in one)
    # Its own device's second BSS is no collision, another channel neither
    assert not any("colour collision" in n for n in notes("0A:33:00:00:00:01"))


def test_vht_details(opened):
    # An AP: Wave 2 with MU-MIMO, Wave 1 with the SU beamformer
    assert opened.js("vhtCapText({vhtcap: 32 | 16 | 1 | 4}, false)") == \
        "256-QAM, MU-MIMO (Wave 2)"
    assert opened.js("vhtCapText({vhtcap: 32 | 4}, false)") == \
        "no 256-QAM (MCS 0\u20137), no MU-MIMO (Wave 1), SU beamformer"
    # A station: what it takes
    assert opened.js("vhtCapText({vhtcap: 32 | 16 | 2 | 8}, true)") == \
        "256-QAM, MU-MIMO (MU beamformee)"
    assert opened.js("vhtCapText({}, true)") == ""

    def tip(fn, base, extra):
        return opened.js("""o => { const d = document.createElement('div');
            d.innerHTML = %s(Object.assign({}, %s[0], o));
            return d.innerText; }""" % (fn, base), extra)

    # In the Standard row of a Wi-Fi 5 AP, not of a Wi-Fi 6 one
    assert "256-QAM, MU-MIMO (Wave 2)" in tip("networkTooltip", "networks",
                                             {"std": 5, "vhtcap": 49})
    assert "Wave 2" not in tip("networkTooltip", "networks",
                               {"std": 6, "vhtcap": 49})
    # A station's Wi-Fi 5 row
    assert "MU-MIMO (MU beamformee)" in tip("clientTooltip", "stations",
                                           {"std": 5, "vhtcap": 50})


def test_station_80_plus_80(opened):
    opened.page.evaluate("updateClients()")
    opened.page.wait_for_function("() => stations.length > 0")

    text = opened.js("""() => {
        const ap = Object.assign({}, networks[0], {bssid: '02:00:00:00:80:80',
            channel: 100, bandwidth: 8080, center: 106, center2: 42});
        networks.push(ap);
        const c = Object.assign({}, stations[0], {std: 6, width: 160,
            w8080: 1, assoc: 1, bssid: ap.bssid});
        const d = document.createElement('div');
        d.innerHTML = clientTooltip(c);
        return d.innerText; }""")

    assert "up to 160 MHz, also 80+80" in text
    assert "80+80 with its AP" in text

    # Without the AP running 80+80 only what the station offers
    text = opened.js("""() => { const d = document.createElement('div');
        d.innerHTML = clientTooltip(Object.assign({}, stations[0],
            {std: 6, width: 160, w8080: 1, assoc: 0}));
        return d.innerText; }""")

    assert "also 80+80" in text and "with its AP" not in text


def test_bands_name_80_plus_80(opened):
    def bands(classes):
        return opened.js("c => bandsText(c)", {"opcl": classes})

    # 128 80 MHz, 129 160, 130 80+80; 6 GHz 134 160, 135 80+80
    assert "up to 160 MHz, also 80+80" in bands([115, 128, 129, 130])
    assert "up to 80+80)" in bands([115, 128, 130])
    assert "up to 160 MHz)" in bands([115, 128, 129])
    assert "6 GHz (up to 160 MHz, also 80+80)" in bands([131, 134, 135])


def test_width_column_80_plus_80(opened):
    assert opened.js("clientCellText({width: 160, w8080: 1}, 'width')") == "160/80+80"
    assert opened.js("clientCellText({width: 160}, 'width')") == "160 MHz"
