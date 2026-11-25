"""RESET pressed twice: doubleResetCheck() and doubleResetClear() of the
firmware, the way back to Wi-Fi from a remembered capture mode.
Compiled on the PC from the sketch, with NVS kept across the starts."""

import pathlib
import shutil
import subprocess

import pytest
from sketch import sketch_text

HARNESS = r"""
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>

#define LED_RGB        1
#define LED_PLAIN_HIGH 2
#define LED_PLAIN_LOW  3
#define STATUS_LED_TYPE 0

enum esp_reset_reason_t { ESP_RST_POWERON, ESP_RST_SW, ESP_RST_PANIC };

static esp_reset_reason_t reason;
static uint32_t now;
static std::map<std::string, bool> nvs;     // survives the starts
static int writes;

esp_reset_reason_t esp_reset_reason() { return reason; }
uint32_t millis() { return now; }

struct Preferences
{
    std::string ns;
    bool begin(const char *name, bool) { ns = name; return true; }
    void end() {}
    bool getBool(const char *key, bool dflt)
    {
        auto it = nvs.find(ns + "/" + key);
        return it == nvs.end() ? dflt : it->second;
    }
    void putBool(const char *key, bool v) { nvs[ns + "/" + key] = v; writes++; }
};

%s

// One start per line: "<p|s|c> <ms it runs before the next>"
// (p power on or RESET, s software restart, c crash)
int main()
{
    char kind;
    unsigned ms;

    while (scanf(" %%c %%u", &kind, &ms) == 2)
    {
        reason = kind == 'p' ? ESP_RST_POWERON
               : kind == 's' ? ESP_RST_SW : ESP_RST_PANIC;
        doubleReset = doubleResetMark = false;
        now = 0;
        doubleResetCheck();
        printf("%%d", doubleReset ? 1 : 0);

        for (now = 0; now <= ms; now += 100)
            doubleResetClear();
    }

    printf(" writes %%d\n", writes);
}
"""


@pytest.fixture(scope="module")
def starts(tmp_path_factory):
    if not shutil.which("g++"):
        pytest.skip("no g++")

    src = sketch_text()
    a = src.index("#define DOUBLE_RESET_MS")
    code = src[a:src.index("void sensorSetup()", a)]

    d = tmp_path_factory.mktemp("doublereset")
    (d / "t.cpp").write_text(HARNESS % code)
    subprocess.run(["g++", "-std=c++17", "-o", str(d / "t"), str(d / "t.cpp")],
                   check=True)

    def run(seq):
        out = subprocess.run([str(d / "t")], input=seq, text=True,
                             capture_output=True, check=True).stdout
        return out.split()[0], int(out.split()[2])

    return run


def test_one_press_is_no_double(starts):
    assert starts("p 60000")[0] == "0"


def test_second_press_within_the_window(starts):
    # The first start ran 3 s, the LED lit, then RESET again
    assert starts("p 3000\np 60000")[0] == "01"


def test_second_press_after_the_window(starts):
    assert starts("p 12000\np 60000")[0] == "00"


def test_a_third_quick_press_starts_afresh(starts):
    # p p: double; a third within the window is a first press again,
    # a fourth makes the next double
    assert starts("p 3000\np 3000\np 3000\np 60000")[0] == "0101"


def test_power_cuts_far_apart_never_count(starts):
    assert starts("p 60000\np 60000\np 60000")[0] == "000"


def test_a_restart_in_between_ends_the_window(starts):
    # RESET, the firmware restarts (an update, a crash) at once, then
    # a power cut much later: not a double press
    assert starts("p 2000\ns 60000\np 60000")[0] == "000"
    assert starts("p 2000\nc 60000\np 60000")[0] == "000"


def test_the_mark_is_cleared_after_the_window(starts):
    # Set at the start, cleared after 10 s: two writes per start,
    # none on a restart that finds nothing to clear
    assert starts("p 60000")[1] == 2
    assert starts("p 60000\ns 60000")[1] == 2
