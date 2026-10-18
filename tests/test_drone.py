"""Drone Remote ID: BEST GUESS, the REMOTE ID section, notes, filter."""

from test_tooltip import tooltip

# Synthetic: a multirotor at 25.5 m above takeoff, climbing; positions
# made up, the maker code is DJI's
RID = {"ua": 2, "mk": "1581", "ago": 3, "st": 2, "lat": 480000000,
       "lon": 110000000, "ab": 2587, "ag": 2850, "h": 2051, "hagl": 0,
       "spd": 700, "vs": 600, "trk": 357, "olat": 480010000,
       "olon": 110020000, "oalt": 2806, "eu": 0x12, "sid": "Test flight"}


def guess(app, n):
    return app.js("""n => {
        const d = document.createElement('div');
        d.innerHTML = apMakerHeader(n);
        return [...d.querySelectorAll('.tipMaker > div')]
            .map(x => x.textContent);
    }""", n)


def test_best_guess(opened):
    lines = guess(opened, {"voui": [], "rid": RID})

    assert lines[0] == "BEST GUESS"
    assert "Manufacturer: DJI" in lines
    assert "Device: Drone" in lines
    assert "Height: 25.5 m above takeoff" in lines
    assert "Pilot: map" in lines

    href = opened.js("""n => {
        const d = document.createElement('div');
        d.innerHTML = apMakerHeader(n);
        return d.querySelector('a').href;
    }""", {"voui": [], "rid": RID})

    assert href.startswith("https://www.openstreetmap.org/?mlat=48.001000"
                           "&mlon=11.002000")


def test_other_aircraft_type(opened):
    assert "Device: Drone (aeroplane)" in \
        guess(opened, {"voui": [], "rid": dict(RID, ua=1)})


def test_remote_id_section(opened):
    text = tooltip(opened, {"rid": RID})

    assert "REMOTE ID" in text
    assert "airborne" in text
    assert "7.0 m/s towards 357°, climbing 6.0 m/s" in text
    assert "425 m GNSS, 293.5 m barometric" in text
    assert "map 48.000000, 11.000000" in text
    assert "map 48.001000, 11.002000, 403 m altitude" in text
    assert "open category, class C1" in text
    assert "Test flight" in text
    assert "3 s ago" in text

    assert "REMOTE ID" not in tooltip(opened, {})


def test_without_location_or_pilot(opened):
    rid = {"ua": 2, "mk": "", "ago": 1, "sid": ""}
    text = tooltip(opened, {"rid": rid})

    assert "REMOTE ID" in text
    assert "map" not in text
    assert guess(opened, {"voui": [], "rid": rid})[1:] == ["Device: Drone"]


def test_emergency_is_red(opened):
    notes = opened.js("""r => networkNotes(Object.assign({},
        networks.find(x => x.ssid === 'Plain'), {rid: r}))""",
                      dict(RID, st=3))

    assert ["#d08080", "The drone reports an emergency in its Remote ID"] \
        in notes


def test_filter(app):
    for n in app.api["networks"]["networks"]:
        if n["ssid"] == "Plain":
            n["rid"] = RID

    app.open()

    for word in ["drone", "remote id", "dji"]:
        app.page.fill("#filter", word)
        app.page.wait_for_timeout(100)

        assert app.js("networks.filter(matchesFilter).map(n => n.ssid)") \
            == ["Plain"], word


def test_catalog_name(opened):
    assert opened.js("catalogName({k: 2, id: 0xFA0BBC0D, vn: ''})[1]") == \
        "FA:0B:BC type 13 Drone Remote ID (ASTM F3411)"
