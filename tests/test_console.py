"""consoleLoop() and consoleCommand() of the firmware: commands on the
serial console, one per line, "ethreinit" asking ethLoop() to take
the Ethernet interface down and up again. Compiled on the PC from the
sketch with a Serial that reads stdin."""

import pathlib
import shutil
import subprocess
import sys

import pytest

from sketch import sketch_text

ROOT = pathlib.Path(__file__).resolve().parent.parent

sys.path.insert(0, str(ROOT / "tools"))
import check_against_tshark as tshark  # noqa: E402

HEAD = r"""
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstddef>
#include <cstdarg>

#define ENABLE_ETHERNET 1

struct FakeSerial
{
    int available() { int c = getchar(); if (c != EOF) ungetc(c, stdin); return c != EOF; }
    int read() { return getchar(); }
    void println(const char *s) { printf("%s\n", s); }
    void printf(const char *f, ...)
    {
        va_list a;
        va_start(a, f);
        vprintf(f, a);
        va_end(a);
    }
} Serial;

bool ethReinitRequested = false;

void w5500Dump(const char *why) { printf("dump %s\n", why); }
"""

MAIN = r"""
int main()
{
    consoleLoop();
    printf("reinit=%d\n", ethReinitRequested ? 1 : 0);
    return 0;
}
"""


@pytest.fixture(scope="module")
def console(tmp_path_factory):
    if not shutil.which("g++"):
        pytest.skip("no g++")

    src = sketch_text()
    text = "\n".join([HEAD,
                      tshark.cut(src, "#define CONSOLE_LINE_MAX"),
                      tshark.cut(src, "void consoleCommand("),
                      tshark.cut(src, "void consoleLoop(")]) + MAIN

    d = tmp_path_factory.mktemp("console")
    (d / "t.cpp").write_text(text)
    subprocess.run(["g++", "-O1", "-w", "-o", str(d / "t"), str(d / "t.cpp")],
                   check=True)

    def run(stdin):
        return subprocess.run([str(d / "t")], input=stdin, capture_output=True,
                              text=True, check=True).stdout

    return run


def test_eth_reinit_asks_for_it(console):
    assert console("ethreinit\n").endswith("reinit=1\n")


def test_w5500_dumps_the_registers(console):
    out = console("w5500\n")

    assert "dump console" in out
    assert "Unknown" not in out
    assert out.endswith("reinit=0\n")


def test_missed_arp_answer_dumps_the_registers():
    src = sketch_text()
    check = tshark.cut(src, "bool arpCheck(")
    miss = check[check.index("arpFailures++;"):]
    assert 'w5500Dump("no ARP answer");' in miss[:miss.index("arpAsked = true;")]

    dump = tshark.cut(src, "void w5500Dump(")
    assert "w5500Read(0x0000, 0, c, sizeof(c));" in dump
    assert "w5500Read(0x0000, 1, s, sizeof(s));" in dump


def test_lost_lease_and_link_dump_the_registers():
    src = sketch_text()
    loop = tshark.cut(src, "bool ethLoop(")
    lease = loop[loop.index('"ETH: no DHCP lease, attempt'):]
    assert 'w5500Dump("no DHCP lease");' in lease[:lease.index("if (dhcpAttempts >=")]

    link = loop[loop.index('"ETH: no link (phy reports'):]
    link = link[:link.index("A link change always returns to DHCP")]
    assert "if (noLinkDumps < 3)" in link
    assert 'w5500Dump("no link");' in link
    assert "if (ethLinkUp)\n        noLinkDumps = 0;" in link


def test_w5500_reset_by_itself_reinitialises_first():
    src = sketch_text()
    check = tshark.cut(src, "bool w5500WasReset(")
    assert "w5500Read(0x0009, 0, mac, sizeof(mac));" in check
    assert "if (version != 0x04)\n        return false;" in check
    assert "millis() - ethStartedAt < 3000" in check

    loop = tshark.cut(src, "bool ethLoop(")
    head = loop[:loop.index("if (arpCheck())")]
    assert "if (w5500WasReset())" in head
    assert 'w5500Dump("reset by itself");' in head
    assert "ethReinit();" in head

    setup = tshark.cut(src, "void setupEthernet(")
    assert setup.index("ethStartedAt = millis();") < setup.index("ETH.begin(")


def test_carriage_return_ends_a_line_too(console):
    # A terminal sends CR, or CR LF
    assert console("ethreinit\r\n").endswith("reinit=1\n")
    assert console("ethreinit\r").endswith("reinit=1\n")


def test_a_line_without_its_end_waits(console):
    assert console("ethreinit").endswith("reinit=0\n")


def test_unknown_command_lists_the_known_ones(console):
    out = console("reboot now\n")

    assert 'Unknown command "reboot now"' in out
    assert "Commands: ethreinit, w5500, help" in out
    assert out.endswith("reinit=0\n")


def test_help(console):
    out = console("help\n")

    assert "Unknown" not in out
    assert "Commands: ethreinit, w5500, help" in out


def test_overlong_line_is_cut_not_overrun(console):
    # A burst of noise: cut to the buffer, no command, no crash
    out = console("x" * 500 + "\n")

    assert "Unknown command" in out
    assert out.endswith("reinit=0\n")


def test_eth_reinit_in_eth_loop():
    src = sketch_text()
    loop = tshark.cut(src, "bool ethLoop(")

    # Done first in ethLoop(), ending the loop() round like the
    # watchdog's reinit
    head = loop[:loop.index("Link diagnostics")]
    assert "if (ethReinitRequested)" in head
    assert "ethReinit();" in head
    assert "return false;" in head
