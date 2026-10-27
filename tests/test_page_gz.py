"""source/wlanscout/index_html_gz.h: the packed copy the sensor serves."""

import gzip
import importlib.util
import pathlib
import re

ROOT = pathlib.Path(__file__).resolve().parent.parent
SKETCH = ROOT / "source" / "wlanscout"


def page():
    source = (SKETCH / "index_html.h").read_text(encoding="utf-8")

    return re.search(r'R"HTML\((.*?)\)HTML"', source, re.S).group(1) \
        .encode("utf-8")


def packed():
    text = (SKETCH / "index_html_gz.h").read_text()
    body = text[text.index("PROGMEM = {"):text.index("};")]

    return bytes(int(h, 16) for h in re.findall(r"0x([0-9a-f]{2})", body)), \
        int(re.search(r"INDEX_HTML_SOURCE_SIZE (\d+)", text).group(1))


def test_packed_copy_is_this_page():
    # After a change to index_html.h: python3 tools/make_page_gz.py
    data, size = packed()

    assert gzip.decompress(data) == page()
    assert size == len(page()) + 1


def test_the_script_makes_the_same_file():
    # What the script would write, without writing it: a stale copy
    # stays stale and fails here again on the next run.
    # No time stamp inside, the same page gives the same bytes
    spec = importlib.util.spec_from_file_location(
        "make_page_gz", ROOT / "tools" / "make_page_gz.py")
    tool = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(tool)

    text, _ = tool.header(tool.page_bytes())

    assert (SKETCH / "index_html_gz.h").read_text(encoding="utf-8") == text
