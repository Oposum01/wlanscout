"""Builds without Ethernet or OTA (ENABLE_ETHERNET, ENABLE_OTA in
config.h, both off for the Seeed Studio XIAO ESP32-C5): the sketch keeps every use of the W5500, the
web server and the update code behind its switch, and the page copes
with a system answer that has no "net" or no "ota"."""

import pathlib
import re

import pytest

from test_settings_tabs import settings
from sketch import sketch_text

ROOT = pathlib.Path(__file__).resolve().parent.parent
SKETCH = ROOT / "source" / "wlanscout" / "wlanscout.ino"

# What only a build with the feature may use
ETHERNET_ONLY = [r"\bETH\.", r"\bserver\.", r"\bHTTPUpload\b",
                 r"\bINDEX_HTML_GZ\b", r"\bconfigTime\(", r"\bSPI\.begin",
                 r"\besp_netif_dhcpc_", r"\besp_sntp_"]
OTA_ONLY = [r"\bUpdate\.", r"\botaStart\(", r"\botaWrite\(",
            r"\botaFinish\(", r"\botaLoop\(", r"\boutOtaJSON\(",
            r"\besp_ota_mark_app"]


def evaluate(cond, flags):
    """True or False for a condition on the switches, None for any
    other (both branches stay)."""
    if "ENABLE_ETHERNET" not in cond and "ENABLE_OTA" not in cond:
        return None
    expr = cond
    for name, value in flags.items():
        expr = re.sub(r"\b%s\b" % name, str(value), expr)
    expr = re.sub(r'__has_include\([^)]*\)', "1", expr)
    expr = expr.replace("&&", " and ").replace("||", " or ")
    expr = re.sub(r"!(?!=)", " not ", expr)
    try:
        return bool(eval(expr, {}))
    except Exception:
        return None


def compiled(src, flags):
    """The lines a build with these switches compiles, as unifdef
    would leave them; the switch definitions themselves are skipped."""
    out = []
    stack = []      # (taking now, a branch was taken, known)

    for line in src.splitlines():
        d = re.match(r"\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)", line)
        live = all(s[0] for s in stack)

        if not d:
            if live:
                out.append(line)
            continue

        kind, cond = d.group(1), d.group(2).strip()

        if kind in ("ifdef", "ifndef"):
            stack.append((True, True, False))
        elif kind == "if":
            v = evaluate(cond, flags)
            stack.append((True, True, False) if v is None
                         else (v, v, True))
        elif kind == "elif":
            taking, taken, known = stack.pop()
            v = evaluate(cond, flags) if known else None
            if not known or v is None:
                stack.append((True, True, False))
            else:
                stack.append((not taken and v, taken or v, True))
        elif kind == "else":
            taking, taken, known = stack.pop()
            stack.append((not taken if known else True, True, known))
        else:
            stack.pop()

    assert not stack, "unbalanced #if"
    return "\n".join(out)


def strip_comments(code):
    code = re.sub(r"/\*.*?\*/", "", code, flags=re.S)
    return re.sub(r"//[^\n]*", "", code)


@pytest.mark.parametrize("eth,ota", [(0, 0), (1, 0), (0, 1), (1, 1)])
def test_nothing_left_outside_its_switch(eth, ota):
    src = sketch_text()
    code = strip_comments(compiled(src, {"ENABLE_ETHERNET": eth,
                                         "ENABLE_OTA": ota}))

    forbidden = ([] if eth else ETHERNET_ONLY) + ([] if ota else OTA_ONLY)
    left = [p for p in forbidden if re.search(p, code)]

    assert left == []

    # And what is switched on is still there
    wanted = (ETHERNET_ONLY if eth else []) + (OTA_ONLY if ota else [])
    assert [p for p in wanted if not re.search(p, code)] == []


def test_includes_follow_the_switches():
    src = sketch_text()
    code = compiled(src, {"ENABLE_ETHERNET": 0, "ENABLE_OTA": 0})

    for header in ["WebServer.h", "ETH.h", "SPI.h", "lwip/dhcp.h",
                   "Update.h", "esp_sntp.h"]:
        assert "#include <%s>" % header not in code
        assert '#include "%s"' % header not in code


CONFIG = SKETCH.parent / "config.h"


def config_default(name):
    m = re.search(r"#ifndef %s\n#define %s (.+)\n" % (name, name),
                  CONFIG.read_text(encoding="utf-8"))
    return m.group(1).strip()


def test_defaults_are_the_waveshare_kit():
    assert config_default("ENABLE_ETHERNET") == "1"
    assert config_default("ENABLE_OTA") == "1"
    assert config_default("STATUS_LED_TYPE") == "LED_RGB"
    assert "Waveshare" in config_default("BOARD_NAME")


def test_xiao_recipe_in_the_config():
    text = CONFIG.read_text(encoding="utf-8")
    recipe = text[text.index("Seeed Studio XIAO ESP32-C5 (8 MB"):]

    for line in ["#define ENABLE_ETHERNET 0",
                 "#define STATUS_LED_TYPE LED_PLAIN_LOW",
                 "partitions_xiao.csv"]:
        assert line in recipe

    # Updates from the page by default, one slot only as the option
    option = recipe[recipe.index("Option, one firmware slot"):]

    assert "#define ENABLE_OTA" not in recipe[:len(recipe) - len(option)]
    assert "#define ENABLE_OTA      0" in option
    assert "partitions_xiao_1slot.csv" in option


def test_xiao_build_two_slots_by_default():
    script = (ROOT / "tools" / "build_xiao.sh").read_text(encoding="utf-8")
    source = ROOT / "source" / "wlanscout"

    # ENABLE_OTA 0 only with XIAO_ONE_SLOT=1, and the matching table
    default = script[:script.index('if [ "${XIAO_ONE_SLOT')]
    assert "ENABLE_OTA" not in default.split("cat <<'EOF'")[1]
    assert "PARTITIONS=partitions_xiao_1slot.csv" in script

    two = (source / "partitions_xiao.csv").read_text(encoding="utf-8")
    one = (source / "partitions_xiao_1slot.csv").read_text(encoding="utf-8")

    assert "ota_1" in two and "ota_1" not in one


def test_no_board_detection():
    # The configuration is the switches, not the board in the IDE
    assert "ARDUINO_XIAO_ESP32C5" not in sketch_text()
    assert "ARDUINO_XIAO_ESP32C5" not in CONFIG.read_text(encoding="utf-8")


def test_without_ota_no_upload(opened):
    del opened.api["system"]["ota"]
    settings(opened, "sensor")

    assert opened.page.locator("#otaControls").is_hidden()
    assert opened.page.locator("#otaInfo").inner_text().startswith(
        "not from the page in this build")


def test_with_ota_the_upload(opened):
    settings(opened, "sensor")

    assert opened.page.locator("#otaControls").is_visible()


def test_without_ethernet_usb_only(opened):
    del opened.api["system"]["net"]
    settings(opened, "network")

    rows = opened.js("""[...document.querySelectorAll('#netTable tr')]
        .map(r => [r.cells[0].textContent, r.cells[1].textContent])""")

    assert rows == [["Ethernet",
                     "not in this build, the sensor answers on USB only"]]
    assert opened.js(
        "document.querySelectorAll('#dhcpTable tr').length") == 0
    assert opened.page.locator("#hintNetwork").inner_text() == "USB only"


def test_with_ethernet_the_address(opened):
    settings(opened, "network")

    first = opened.js(
        "document.querySelector('#netTable tr').cells[0].textContent")

    assert first == "Address"


def test_every_file_closes_its_guards():
    # Arduino joins the tabs, so a guard opened in one file and closed
    # in the next still compiles with the switch on, and breaks with it
    # off
    from sketch import sketch_files

    for path in sketch_files():
        depth = 0
        for line in path.read_text(encoding="utf-8").splitlines():
            d = re.match(r"\s*#\s*(if|ifdef|ifndef|endif)\b", line)
            if d:
                depth += -1 if d.group(1) == "endif" else 1
                assert depth >= 0, path.name
        assert depth == 0, path.name


def test_one_slot_table_points_to_the_flasher(opened):
    # A sensor from before 1.53.0: OTA built in, no second slot
    del opened.api["system"]["ota"]["other"]
    settings(opened, "sensor")

    text = opened.page.locator("#otaInfo").inner_text()

    assert "full install with the flasher or esptool" in text
    assert "Arduino IDE" not in text


def test_refusals_point_to_the_flasher(tmp_path):
    """No update while it runs: the answers name the flasher and
    esptool, not the Arduino IDE, and the one sent as JSON is JSON."""
    import json
    import shutil
    import subprocess

    src = sketch_text()

    assert "with the Arduino IDE" not in src

    start = src.index("#define OTA_NOT_BUILT")
    macro = src[start:src.index("\n\n", start)]

    assert "Update in download mode" in macro and "esptool" in macro

    if not shutil.which("g++"):
        pytest.skip("no g++")

    (tmp_path / "t.cpp").write_text(
        "#include <cstdio>\n" + macro +
        '\nint main() { puts("{\\"ok\\":0,\\"error\\":\\"" OTA_NOT_BUILT '
        '"\\"}"); }\n')
    subprocess.run(["g++", "-o", str(tmp_path / "t"), str(tmp_path / "t.cpp")],
                   check=True)
    out = subprocess.run([str(tmp_path / "t")], capture_output=True,
                         text=True, check=True).stdout

    answer = json.loads(out)
    assert answer["ok"] == 0
    assert answer["error"].startswith("this build has one firmware slot")
