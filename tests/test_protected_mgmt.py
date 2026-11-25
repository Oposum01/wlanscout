"""protectedNonsense() of the firmware: a beacon, probe or (re)association
frame with the Protected bit is no such frame (a ZTE device sends
"beacons" of ciphertext), so it is neither parsed nor catalogued.
Compiled on the PC from the sketch."""

import pathlib
import shutil
import subprocess
import sys

import pytest

from sketch import sketch_text

ROOT = pathlib.Path(__file__).resolve().parent.parent

sys.path.insert(0, str(ROOT / "tools"))
import check_against_tshark as tshark  # noqa: E402

MAIN = r"""
#include <cstdio>

// Frame control octets in hex per argument, 1 or 0 for each
int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++)
    {
        uint8_t fc[2];

        sscanf(argv[i], "%2hhx%2hhx", &fc[0], &fc[1]);
        printf("%d", protectedNonsense(fc, 2) ? 1 : 0);
    }

    return 0;
}
"""


@pytest.fixture(scope="module")
def check(tmp_path_factory):
    if not shutil.which("g++"):
        pytest.skip("no g++")

    src = sketch_text()
    text = "\n".join(["#include <cstdint>",
                      tshark.cut(src, "bool protectedNonsense(")]) + MAIN

    d = tmp_path_factory.mktemp("protected")
    (d / "t.cpp").write_text(text)
    subprocess.run(["g++", "-O1", "-w", "-o", str(d / "t"), str(d / "t.cpp")],
                   check=True)

    def run(*fcs):
        return subprocess.run([str(d / "t"), *fcs], capture_output=True,
                              text=True, check=True).stdout

    return run


def test_zte_beacon_with_protected_bit(check):
    # .233's catalog: 80 58, Protected, Retry and PwrMgt
    assert check("8058") == "1"


def test_every_kind_that_is_never_encrypted(check):
    # assoc req/resp, reassoc req/resp, probe req/resp, beacon
    assert check("0040", "1040", "2040", "3040", "4040", "5040",
                 "8040") == "1111111"


def test_the_plain_ones_pass(check):
    assert check("8000", "5000", "4000", "0000", "8008") == "00000"


def test_protected_frames_that_exist_pass(check):
    # disassoc, auth (shared key), deauth, action; a protected data frame
    assert check("a040", "b040", "c040", "d040", "8842") == "00000"
