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
