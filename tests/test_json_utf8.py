"""outJSON() of the firmware: what goes out is valid UTF-8 JSON, whatever
bytes an SSID or name brought in. Compiled on the PC from the sketch."""

import json
import pathlib
import shutil
import subprocess

import pytest

ROOT = pathlib.Path(__file__).resolve().parent.parent
SKETCH = ROOT / "source" / "wlanscout" / "wlanscout.ino"

HARNESS = r"""
#include <string>
#include <cstdio>
#include <cstdint>
#include <cstring>

struct Out : std::string
{
    Out &operator+=(char c) { push_back(c); return *this; }
    Out &operator+=(const char *t) { append(t); return *this; }
};

Out outBuf;

%s
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++)
    {
        outBuf.clear();
        outJSON(argv[i]);
        fwrite(outBuf.data(), 1, outBuf.size(), stdout);
        putchar('\n');
    }
}
"""

CASES = [
    (b"o\xa9\xf7", "o©÷"),
    (b"Caf\xc3\xa9", "Café"),
    (b'a"b\\c', 'a"b\\c'),
    (b"\xe2\x82\xac", "€"),
    (b"\xf0\x9f\x93\xb6x", "\U0001F4F6x"),
    (b"\xc0\xaf", "À¯"),              # overlong
    (b"\xed\xa0\x80", "í \u0080"),    # surrogate
    (b"\xe2\x82", "â\u0082"),              # cut short
    (b"\xf4\x90\x80\x80", "ô\u0090\u0080\u0080"),  # above U+10FFFF
    (b"tab\there", "tabhere"),
]


@pytest.fixture(scope="module")
def outjson(tmp_path_factory):
    if not shutil.which("g++"):
        pytest.skip("no g++")

    src = SKETCH.read_text(encoding="utf-8")
    a = src.index("/*\n * Length of the UTF-8 sequence at p")
    b = src.index("    if (outBuf.length() >= 1024)", a)

    d = tmp_path_factory.mktemp("outjson")
    (d / "t.cpp").write_text(HARNESS % src[a:b])
    subprocess.run(["g++", "-o", str(d / "t"), str(d / "t.cpp")],
                   check=True)

    return d / "t"


def test_strings(outjson):
    out = subprocess.run([str(outjson)] + [c for c, _ in CASES],
                         capture_output=True, check=True).stdout

    lines = out.split(b"\n")[:-1]

    assert [json.loads(l.decode("utf-8")) for l in lines] == \
        [want for _, want in CASES]
