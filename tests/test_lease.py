"""The DHCP lease row in the network table of Settings."""


def net_rows(app, net):
    app.api["system"] = dict(app.api["system"],
                             net=dict(app.api["system"]["net"], **net))
    app.js("showSettings()")
    app.js("updateSystem()")
    app.page.wait_for_function(
        "() => document.querySelectorAll('#netTable tr').length > 0")

    return dict(app.js("""[...document.querySelectorAll('#netTable tr')]
        .map(r => [...r.cells].map(c => c.textContent))"""))


def test_lease_bound(opened):
    r = net_rows(opened, {"lease": {
        "state": "bound", "time": 5400, "left": 4680, "ack": 720,
        "server": "192.0.2.1", "renewals": 3, "renew": 1980}})

    assert r["DHCP lease"] == \
        "bound, 90 min from 192.0.2.1, 78 min left, " \
        "next renewal in 33 min, renewed 3 times since start"


def test_lease_before_first_ack(opened):
    r = net_rows(opened, {"lease": {"state": "selecting", "renewals": 1}})

    assert r["DHCP lease"] == "selecting, renewed once since start"


def test_no_lease_row_on_older_firmware(opened):
    assert "DHCP lease" not in net_rows(opened, {})
