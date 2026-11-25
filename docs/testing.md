# Tests

`tests/` holds browser tests for the web page in
`source/wlanscout/index_html.h`. They
run on a PC, no sensor needed: the page is cut out of `index_html.h`,
loaded into a Playwright browser, and every `/api/...` request is
answered from the JSON files in `tests/fixtures/`.

```
python3 -m venv .venv
.venv/bin/pip install -r tests/requirements.txt
.venv/bin/python -m playwright install firefox
.venv/bin/python -m pytest -n auto tests
```

The tests marked *compiled on the PC* cut functions out of the firmware
with g++ by their first line. They read it through `tests/sketch.py`,
which joins `config.h`, `wlanscout.ino` and the numbered tabs in the
order Arduino does, so a function can move between files.

Firefox is the default, the browser the page is used with on phones.
`--engine chromium` switches; `--browser-path /usr/bin/chromium` uses
an installed browser instead of the one Playwright downloads.

`-n auto` (pytest-xdist) runs the tests on all CPU cores, each worker
with a browser of its own; the tests need no server or port, so they
do not get in each other's way. With 24 cores a run takes about
25 seconds per browser, one after the other (without `-n`) over two
minutes in Firefox.

A few tests rely on timing that does not hold up beside a dozen other
browsers. They are listed in `SERIAL` in `tests/conftest.py`
(`test_eventlog.py`, `test_graph_pin.py`, `test_graphs.py`): with `-n` the workers leave
them out, and the run starts them on their own right after the
parallel part, with the same browser; a failure there fails the run.
Without `-n` they run with the rest.

Each test file says in its first lines what it covers; by area:

| Area | Files (`tests/test_*.py`) |
|------|-------|
| Firmware compiled on the PC with g++ (see above) | `arp_check`, `beacon_versions`, `ble_capture`, `broken_elements`, `cfg_extras`, `console`, `csa_wide`, `device_type`, `double_reset`, `eht_caps`, `framelog`, `ftm`, `heapsnap`, `history`, `json_utf8`, `oui_blocks`, `protected_mgmt`, `radiotap`, `six_name`, `tsf`, `vht_width`, `watch` |
| Network table, tooltip, notes, IE catalog | `airtime`, `awdl`, `beacon_gaps`, `beacon_tab`, `bss_extras`, `catalog`, `drone`, `dtim`, `elements`, `filter_help`, `ibss`, `interworking`, `makers`, `mdid`, `multilink`, `order`, `pin`, `probe_tsf`, `six_announced`, `sixghz`, `suitebad`, `tooltip`, `twins`, `wfa` |
| Stations | `stations`, `station_notes` |
| Configuration changes and the bell | `bell`, `config_changes` |
| Graphs, spectrum, layout, phone, columns | `band_buttons`, `columns`, `graph`, `graph_pin`, `graphs`, `layout`, `ticking`, `touch` |
| Settings, time, snapshots, event log | `capture_logs`, `cpu`, `crash`, `dst`, `eventlog`, `flush`, `lease`, `reboot`, `settings_tabs`, `snapshot` |
| USB, updates, flasher, release, build switches | `build_features`, `flasher`, `page_gz`, `release`, `static`, `update`, `usb` |

The fixtures are made up: locally administered MACs (`02:00:00:…`),
documentation addresses (`192.0.2.x`), invented SSIDs. Real captures
do not belong there. A test can swap one before the page loads:

```python
def test_something(app):
    app.api["networks"]["networks"][0]["iw"] = "030A03"
    app.open()
    ...
```

The parsers as a whole are checked against Wireshark with
`tools/check_against_tshark.py` ([Tuning](tuning.md#checking-the-parsers-against-wireshark)).
