"""arpCheck() of the firmware: once a minute the gateway is asked by
ARP, and after three checks without an answer the Ethernet interface
is reinitialised. Compiled on the PC from the sketch, with lwIP's ARP
cache, the clock and the interface state played by the test."""

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
#include <cstdarg>

struct ip4_addr_t { uint32_t addr; };
struct eth_addr   { uint8_t addr[6]; };
struct netif      { ip4_addr_t gw; };
struct esp_netif_t {};

#define ip4_addr_copy(d, s)    ((d).addr = (s).addr)
#define ip4_addr_isany_val(a)  ((a).addr == 0)
#define netif_ip4_gw(n)        (&(n)->gw)
#define LOCK_TCPIP_CORE()
#define UNLOCK_TCPIP_CORE()

netif       lwNetif;
esp_netif_t espNetif;
bool        hasNetif = true;

struct { esp_netif_t *netif() { return hasNetif ? &espNetif : nullptr; } } ETH;
void *esp_netif_get_netif_impl(esp_netif_t *) { return &lwNetif; }

// The ARP cache: the gateway is in it when it answered
bool gatewayAnswers = true;
bool inCache        = false;
int  requests       = 0;
int  cleanups       = 0;

int etharp_find_addr(netif *, const ip4_addr_t *, eth_addr **,
                     const ip4_addr_t **)
{
    return inCache ? 0 : -1;
}
void etharp_cleanup_netif(netif *) { inCache = false; cleanups++; }
int etharp_request(netif *, const ip4_addr_t *)
{
    requests++;
    if (gatewayAnswers)
        inCache = true;
    return 0;
}
const char *ip4addr_ntoa(const ip4_addr_t *) { return "192.0.2.1"; }
void w5500Dump(const char *) { }

uint32_t now = 0;
uint32_t millis() { return now; }

struct { void printf(const char *, ...) {} } Serial;

int events = 0;
void logEventFrom(const char *, const char *f, ...) { events++; }

bool ethLinkUp       = true;
bool ethGotIP        = true;
bool ethStaticActive = false;
"""

MAIN = r"""
// Each argument: a step of a minute, "a" the gateway answers, "s"
// silent, "d" link down, "f" static fallback, "g" no gateway;
// prints 1 for a step that asks for a reinit, else 0
int main(int argc, char **argv)
{
    lwNetif.gw.addr = 0x010200c0;

    for (int i = 1; i < argc; i++)
    {
        char c = argv[i][0];

        gatewayAnswers  = c != 's';
        ethLinkUp       = c != 'd';
        ethStaticActive = c == 'f';
        lwNetif.gw.addr = c == 'g' ? 0 : 0x010200c0;

        now += 60000;
        printf("%d", arpCheck() ? 1 : 0);
    }

    printf(" requests=%d events=%d\n", requests, events);
    return 0;
}
"""


@pytest.fixture(scope="module")
def check(tmp_path_factory):
    if not shutil.which("g++"):
        pytest.skip("no g++")

    src = sketch_text()
    text = "\n".join([HEAD,
                      tshark.cut(src, "#define ARP_CHECK_MS"),
                      tshark.cut(src, "#define ARP_CHECK_FAILURES"),
                      "uint8_t  arpFailures  = 0;",
                      "uint32_t arpLastCheck = 0;",
                      "bool     arpAsked     = false;",
                      tshark.cut(src, "bool arpCheck(")]) + MAIN

    d = tmp_path_factory.mktemp("arp")
    (d / "t.cpp").write_text(text)
    subprocess.run(["g++", "-O1", "-w", "-o", str(d / "t"), str(d / "t.cpp")],
                   check=True)

    def run(*steps):
        return subprocess.run([str(d / "t"), *steps], capture_output=True,
                              text=True, check=True).stdout.strip()

    return run


def test_gateway_answering_never_reinits(check):
    assert check(*"aaaaaaaaaa") == "0000000000 requests=10 events=0"


def test_three_silent_checks_reinit(check):
    # A request is looked at a minute later: the first silent one
    # (step 2) counts at step 3, the third at step 5
    assert check("a", "s", "s", "s", "s") == "00001 requests=5 events=1"


def test_one_answer_between_starts_over(check):
    assert check("a", "s", "s", "a", "s", "s") == "000000 requests=6 events=0"


def test_a_silent_gateway_from_the_start(check):
    # A request is checked a minute later, so the first silent one
    # counts at the second step
    assert check("s", "s", "s", "s") == "0001 requests=4 events=1"


def test_after_a_reinit_it_counts_again(check):
    out = check("s", "s", "s", "s", "s", "s", "s")

    assert out.startswith("0001000")
    assert out.endswith("events=1")


def test_link_down_resets_the_count(check):
    # Link down: no request; the next one asks without counting
    assert check("a", "s", "s", "d", "s", "s") == "000000 requests=5 events=0"


def test_not_on_the_static_fallback(check):
    assert check(*"ffffff") == "000000 requests=0 events=0"


def test_no_gateway_no_request(check):
    assert check(*"gggg") == "0000 requests=0 events=0"


def test_once_a_minute():
    src = sketch_text()

    assert "#define ARP_CHECK_MS       60000" in src
    assert "#define ARP_CHECK_FAILURES 3" in src


def test_eth_loop_checks_first_and_reinits():
    loop = tshark.cut(sketch_text(), "bool ethLoop(")
    head = loop[:loop.index("Link diagnostics")]

    assert "if (arpCheck())" in head
    assert head.count("ethReinit();") == 3


def test_lost_ip_handled_in_eth_loop():
    """The event task only raises a flag: clearing ethGotIP there let
    the DHCP watchdog in loop() count an attempt before the count was
    reset, so the first attempt was counted twice."""
    src = sketch_text()
    event = src[src.index("case ARDUINO_EVENT_ETH_LOST_IP:"):]
    event = event[:event.index("break;")]

    assert "ethIpLost = true;" in event
    assert "ethGotIP" not in event.split("*/")[-1]
    assert "dhcpAttempts" not in event.split("*/")[-1]

    loop = tshark.cut(src, "bool ethLoop(")
    lost = loop.index("if (ethIpLost)")

    assert lost < loop.index("DHCP watchdog")
    assert "dhcpAttempts     = 0;" in loop[lost:loop.index("DHCP watchdog")]
