"""Default order of the network table."""


def test_networks_sorted_by_ssid_then_bssid(opened):
    rows = opened.js("""[...document.querySelectorAll('#networkTable tr')]
        .map(r => r.dataset.bssid)""")

    expected = opened.js("""networks.slice().sort((a, b) =>
        String(a.ssid).localeCompare(String(b.ssid), undefined,
            {numeric: true, sensitivity: 'base'}) ||
        a.bssid.localeCompare(b.bssid)).map(n => n.bssid)""")

    assert opened.js("[sortKey, sortDir]") == ["ssid", 1]
    assert rows == expected


def test_same_ssid_keeps_bssid_order(opened):
    # Two BSSIDs of one SSID, delivered in reverse order
    order = opened.js("""() => {
        const a = Object.assign({}, networks[0], {ssid: 'Same', bssid: '02:00:00:00:00:A2'});
        const b = Object.assign({}, networks[0], {ssid: 'Same', bssid: '02:00:00:00:00:A1'});
        networks = [a, b];
        updateNetworkTable();
        return [...document.querySelectorAll('#networkTable tr')]
            .map(r => r.dataset.bssid);
    }""")

    assert order == ["02:00:00:00:00:A1", "02:00:00:00:00:A2"]
