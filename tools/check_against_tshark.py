#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Dr.-Ing. Simon Wunderlich Systementwicklung & Beratung
# SPDX-FileContributor: Jan Fuchs <jf [at] simonwunderlich . de>
"""
Checks the firmware's element parsers against Wireshark.

The parser functions are cut out of wlanscout.ino as they are,
compiled for the host with g++ and fed every beacon, probe
response, probe request and (re)association request of the given
captures. tshark decodes the same frames, and every field both
sides know is compared:

    AKM, pairwise and group cipher   (RSN element)
    spatial streams                  (HT, VHT, HE, EHT)
    radio measurement bits           (RM Enabled Capabilities)
    WNM / FTM bits                   (Extended Capabilities)
    operating classes                (Supported Operating Classes)
    MLD address, EML, MLD caps,
    links of the per-STA profiles    (Basic Multi-Link)

Besides mismatches it lists the AKM and cipher numbers the
firmware has no name for, and the elements it does not know at
all, with Wireshark's name for them. Those are the things to add.

Usage:
    python3 tools/check_against_tshark.py capture.pcap [more.pcap ...]

Needs tshark, editcap and g++. Exit status 1 if anything differs.
"""

import json
import os
import re
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SKETCH = os.path.join(HERE, "..", "source", "wlanscout", "wlanscout.ino")

# Functions and definitions taken from the sketch, in order
PIECES = [
    "const uint8_t channels24[]",
    "const uint8_t channels5[]",
    "struct MloInfo",
    "struct RsnInfo",
    "void appendName(",
    "uint32_t suiteAt(",
    "const char *vendorSuiteName(",
    "const char *cipherName(",
    "const char *akmName(",
    "void noteBadSuite(",
    "void parseSuites(",
    "const uint8_t *findIE(",
    "const uint8_t *findExtendedIE(",
    "uint64_t planBitOf(",
    "void addSuppChannels(",
    "uint8_t profileBandOf(",
    "uint16_t capsWidthOf(",
    "bool parseMultiLink(",
    "uint64_t opClassesOf(",
    "uint64_t suppChannelsOf(",
    "uint8_t maxStreamsOf(",
    "uint64_t rrmCapsOf(",
    "uint16_t wnmCapsOf(",
    "static const uint8_t decodedIes[]",
    "static const uint8_t decodedExt[]",
    "static const uint8_t passedIes[]",
    "static const uint8_t passedExt[]",
    "static bool inList(",
    "#define IN_LIST",
    "bool elementKnown(",
    "void collectUnknownElements(",
]

MAIN = r'''
#include <string>

static int hexval(char c)
{
    return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10;
}

int main()
{
    char line[20000];

    while (fgets(line, sizeof(line), stdin))
    {
        unsigned frame = 0, channel = 0;
        char hex[19000];

        if (sscanf(line, "%u %u %18999s", &frame, &channel, hex) != 3)
            continue;

        static uint8_t ies[9500];
        int n = 0;

        for (int i = 0; hex[i] && hex[i + 1] && n < (int)sizeof(ies); i += 2)
            ies[n++] = hexval(hex[i]) << 4 | hexval(hex[i + 1]);

        RsnInfo rsn;
        memset(&rsn, 0, sizeof(rsn));

        int len = 0;
        const uint8_t *r = findIE(ies, n, 48, len);
        parseSuites(r, len, false, rsn);

        MloInfo mlo;
        bool hasMlo = parseMultiLink(ies, n, mlo);

        uint16_t unk[16];
        uint8_t unkSeen[16];
        uint8_t unkCount = 0;
        collectUnknownElements(ies, n, unk, unkSeen, unkCount, 16, 0);

        uint64_t oc = opClassesOf(ies, n);

        printf("{\"frame\":%u,\"nss\":%u,\"rrm\":%llu,\"wnm\":%u,"
               "\"akm\":\"%s\",\"pairwise\":\"%s\",\"group\":\"%s\","
               "\"opcl\":[",
               frame, maxStreamsOf(ies, n, channel),
               (unsigned long long)rrmCapsOf(ies, n),
               wnmCapsOf(ies, n), rsn.akm, rsn.pairwise, rsn.group);

        bool first = true;
        for (int i = 0; i < 57; i++)
            if ((oc >> i) & 1)
            {
                printf("%s%d", first ? "" : ",", 81 + i);
                first = false;
            }

        printf("],\"unk\":[");
        for (int i = 0; i < unkCount; i++)
            printf("%s%u", i ? "," : "", unk[i]);
        printf("]");

        if (hasMlo)
            printf(",\"mld\":\"%02x:%02x:%02x:%02x:%02x:%02x\","
                   "\"eml\":%u,\"mldcap\":%u,\"fields\":%u,"
                   "\"links\":%u",
                   mlo.mldMac[0], mlo.mldMac[1], mlo.mldMac[2],
                   mlo.mldMac[3], mlo.mldMac[4], mlo.mldMac[5],
                   mlo.eml, mlo.mldCaps, mlo.fields, mlo.linkMask);

        printf("}\n");
    }
}
'''

TSHARK_FIELDS = [
    "frame.number", "frame.len", "frame.cap_len",
    "wlan.fc.type_subtype", "radiotap.channel.freq",
    "wlan.rsn.akms.count", "wlan.rsn.akms.type",
    "wlan.rsn.pcs.count", "wlan.rsn.pcs.type", "wlan.rsn.gcs.type",
    "wlan.ht.mcsset.rxbitmask.0to7", "wlan.ht.mcsset.rxbitmask.8to15",
    "wlan.ht.mcsset.rxbitmask.16to23", "wlan.ht.mcsset.rxbitmask.24to31",
    "wlan.vht.mcsset.rxmcsmap",
    "wlan.ext_tag.he_mcs_map.rx_he_mcs_map_lte_80",
    "wlan.eht.supported_eht_mcs_bss_set.le_80.rx_max_nss_supports_eht_mcs_0_9",
    "wlan.eht.supported_eht_mcs_bss_set.le_80.rx_max_nss_supports_eht_mcs_10_11",
    "wlan.eht.supported_eht_mcs_bss_set.le_80.rx_max_nss_supports_eht_mcs_12_13",
    "wlan.eht.supported_eht_mcs_bss_non_sta.rx_max_nss_supports_eht_mcs_0_7",
    "wlan.eht.supported_eht_mcs_bss_non_sta.rx_max_nss_supports_eht_mcs_8_9",
    "wlan.eht.supported_eht_mcs_bss_non_sta.rx_max_nss_supports_eht_mcs_10_11",
    "wlan.rmcap",
    "wlan.extcap.b11", "wlan.extcap.b12", "wlan.extcap.b16",
    "wlan.extcap.b17", "wlan.extcap.b18", "wlan.extcap.b19",
    "wlan.extcap.b26", "wlan.extcap.b46",
    "wlan.supopeclass.current", "wlan.supopeclass.alt",
    "wlan.eht.multi_link.common_info.mld_mac_address",
    "wlan.eht.multi_link.common_info.eml_capabilities",
    "wlan.eht.multi_link.common_info.mld_capabilities",
    "wlan.eht.multi_link.sta_profile.sta_control.link_id",
]

# Where the elements start in the frame body, by subtype
IE_OFFSET = {0: 24 + 4, 2: 24 + 10, 4: 24, 5: 24 + 12, 8: 24 + 12}


# Definitions that end with "};" rather than a function's "}"
TABLES = ("struct", "const uint8_t channels", "static const")


def cut(source, start):
    """The definition that starts with `start`, up to its closing brace."""
    for m in re.finditer(re.escape(start), source):
        line_start = source.rfind("\n", 0, m.start()) + 1
        if line_start != m.start():
            continue

        # A macro is its line
        if start.startswith("#define"):
            return source[m.start():source.index("\n", m.start()) + 1]

        end = source.find(";", m.start())
        brace = source.find("{", m.start())

        # A prototype ends before its first brace
        if end != -1 and end < brace and not start.startswith(TABLES):
            continue

        if start.startswith(TABLES):
            return source[m.start():source.index("};", m.start()) + 2]

        return source[m.start():source.index("\n}\n", m.start()) + 3]

    raise SystemExit("not found in the sketch: " + start)


def build(workdir):
    source = open(SKETCH).read()

    pieces = [cut(source, piece) for piece in PIECES]

    # The channel arrays first, then the sizes derived from them
    text = "\n".join(
        ["#include <cstdint>", "#include <cstring>", "#include <cstdio>"] +
        pieces[:2] +
        ["#define NUM_CHANNELS_24 (sizeof(channels24) / sizeof(channels24[0]))",
         "#define NUM_CHANNELS_5  (sizeof(channels5) / sizeof(channels5[0]))"] +
        pieces[2:]) + MAIN

    cpp = os.path.join(workdir, "parsers.cpp")
    exe = os.path.join(workdir, "parsers")

    open(cpp, "w").write(text)

    subprocess.run(["g++", "-O1", "-w", "-o", exe, cpp], check=True)

    return exe


def read_pcap(path, workdir):
    """(frame number, bytes) of every 802.11 frame, radiotap stripped."""
    plain = os.path.join(workdir, "plain.pcap")

    # editcap turns pcapng into classic pcap; harmless for pcap
    subprocess.run(["editcap", "-F", "pcap", path, plain], check=True)

    data = open(plain, "rb").read()

    magic = struct.unpack_from("<I", data, 0)[0]
    endian = "<" if magic in (0xA1B2C3D4, 0xA1B23C4D) else ">"
    linktype = struct.unpack_from(endian + "I", data, 20)[0]

    off, number = 24, 0

    while off + 16 <= len(data):
        incl = struct.unpack_from(endian + "I", data, off + 8)[0]
        rec = data[off + 16:off + 16 + incl]
        off += 16 + incl
        number += 1

        if linktype == 127 and len(rec) >= 4:
            yield number, rec[struct.unpack_from("<H", rec, 2)[0]:]
        elif linktype == 105:
            yield number, rec


def tshark_frames(path):
    out = subprocess.run(
        ["tshark", "-r", path, "-Y", "wlan.fc.type == 0", "-T", "fields",
         "-E", "occurrence=a", "-E", "aggregator=,"] +
        sum([["-e", f] for f in TSHARK_FIELDS], []),
        capture_output=True, text=True, check=True).stdout

    frames = {}

    for line in out.splitlines():
        values = line.split("\t")
        row = dict(zip(TSHARK_FIELDS, values))
        frames[int(row["frame.number"])] = {
            k: [x for x in v.split(",") if x != ""] for k, v in row.items()}

    return frames


def alternate_classes(path):
    """First 'Alternate Operating Classes' list per frame.

    tshark prints the field's value only as a byte string, the
    class numbers are in its display text.
    """
    out = subprocess.run(
        ["tshark", "-r", path, "-Y", "wlan.supopeclass.alt", "-T", "pdml"],
        capture_output=True, text=True).stdout

    result = {}

    for packet in out.split("<packet>")[1:]:
        number = re.search(r'name="num" pos="0" show="(\d+)"', packet)
        alt = re.search(r'showname="Alternate Operating Classes: ([0-9, ]*)"', packet)

        if number and alt:
            result[int(number.group(1))] = [x.strip() for x in alt.group(1).split(",") if x.strip()]

    return result


def as_int(x):
    return int(x, 0)


def freq_to_channel(freq):
    if 2412 <= freq <= 2472:
        return (freq - 2407) // 5
    if freq == 2484:
        return 14
    return (freq - 5000) // 5


def tshark_nss(t, channel):
    nss = 0

    # Only the first occurrence: the later ones sit in per-STA profiles
    for i, name in enumerate(["0to7", "8to15", "16to23", "24to31"]):
        v = t.get("wlan.ht.mcsset.rxbitmask." + name, [])
        if v and as_int(v[0]) != 0:
            nss = max(nss, i + 1)

    maps = []
    v = t.get("wlan.vht.mcsset.rxmcsmap", [])
    if v and channel > 14:
        maps.append(as_int(v[0]))
    v = t.get("wlan.ext_tag.he_mcs_map.rx_he_mcs_map_lte_80", [])
    if v:
        maps.append(as_int(v[0]))

    for m in maps:
        for i in range(8):
            if (m >> (2 * i)) & 3 != 3:
                nss = max(nss, i + 1)

    # EHT: up to 80 MHz, or the 20 MHz-only format of some stations
    for name in ["wlan.eht.supported_eht_mcs_bss_set.le_80."
                 "rx_max_nss_supports_eht_mcs_" + g for g in ("0_9", "10_11", "12_13")] + \
                ["wlan.eht.supported_eht_mcs_bss_non_sta."
                 "rx_max_nss_supports_eht_mcs_" + g for g in ("0_7", "8_9", "10_11")]:
        v = t.get(name, [])
        if v:
            nss = max(nss, as_int(v[0]))

    return nss


def compare(f, t, channel, akm_names, cipher_names):
    """Mismatches between firmware output f and tshark fields t."""
    diffs = []

    # RSN: the first element's counts slice its lists
    akms = t.get("wlan.rsn.akms.type", [])
    count = as_int(t["wlan.rsn.akms.count"][0]) if t.get("wlan.rsn.akms.count") else 0
    want = ", ".join(akm_names.get(as_int(a), "AKM %d" % as_int(a))
                    for a in akms[:count])
    if want != f["akm"]:
        diffs.append("AKM: firmware %r, tshark %r" % (f["akm"], want))

    pcs = t.get("wlan.rsn.pcs.type", [])
    count = as_int(t["wlan.rsn.pcs.count"][0]) if t.get("wlan.rsn.pcs.count") else 0
    want = ", ".join(cipher_names.get(as_int(c), "cipher %d" % as_int(c))
                    for c in pcs[:count])
    if want != f["pairwise"]:
        diffs.append("pairwise: firmware %r, tshark %r" % (f["pairwise"], want))

    gcs = t.get("wlan.rsn.gcs.type", [])
    want = cipher_names.get(as_int(gcs[0]), "cipher %d" % as_int(gcs[0])) if gcs else ""
    if want != f["group"]:
        diffs.append("group: firmware %r, tshark %r" % (f["group"], want))

    want = tshark_nss(t, channel)
    if want != f["nss"]:
        diffs.append("NSS: firmware %d, tshark %d" % (f["nss"], want))

    # Printed as its octets, "0x73,0xda,...", so the aggregator
    # split it up; the first five are the element's
    v = t.get("wlan.rmcap", [])
    if len(v) >= 5:
        want = int.from_bytes(bytes(as_int(x) for x in v[:5]), "little")
        if want != f["rrm"]:
            diffs.append("RRM: firmware 0x%x, tshark 0x%x" % (f["rrm"], want))

    want = 0
    for i, bit in enumerate([19, 17, 18, 12, 11, 16, 26, 46]):
        v = t.get("wlan.extcap.b%d" % bit, [])
        if v and v[0] in ("1", "True"):
            want |= 1 << i
    if want != (f["wnm"] & 0xFF):
        diffs.append("WNM: firmware 0x%x, tshark 0x%x" % (f["wnm"] & 0xFF, want))

    cur = t.get("wlan.supopeclass.current", [])
    alt = t.get("alternate", [])
    if cur:
        want = set()
        for c in [cur[0]] + alt:
            c = as_int(c)
            if c in (0, 130) and want:
                break
            if 81 <= c <= 137:
                want.add(c)
        # The alt list of later elements belongs to per-STA profiles;
        # firmware and tshark agree when the firmware set is a subset
        if not set(f["opcl"]) <= want or (cur and as_int(cur[0]) in range(81, 138)
                                           and as_int(cur[0]) not in f["opcl"]):
            diffs.append("operating classes: firmware %s, tshark %s"
                         % (f["opcl"], sorted(want)))

    mld = t.get("wlan.eht.multi_link.common_info.mld_mac_address", [])
    if mld:
        if f.get("mld") != mld[0].lower():
            diffs.append("MLD address: firmware %s, tshark %s" % (f.get("mld"), mld[0]))
        eml = t.get("wlan.eht.multi_link.common_info.eml_capabilities", [])
        if eml and (f.get("fields", 0) & 1) and as_int(eml[0]) != f.get("eml"):
            diffs.append("EML: firmware 0x%x, tshark %s" % (f.get("eml", 0), eml[0]))
        cap = t.get("wlan.eht.multi_link.common_info.mld_capabilities", [])
        if cap and (f.get("fields", 0) & 2) and as_int(cap[0]) != f.get("mldcap"):
            diffs.append("MLD caps: firmware 0x%x, tshark %s" % (f.get("mldcap", 0), cap[0]))
        links = 0
        for l in t.get("wlan.eht.multi_link.sta_profile.sta_control.link_id", []):
            links |= 1 << as_int(l)
        if links != f.get("links", 0):
            diffs.append("profile links: firmware 0x%x, tshark 0x%x" % (f.get("links", 0), links))
    elif "mld" in f:
        diffs.append("Multi-Link: firmware found one, tshark none")

    return diffs


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2

    names = {}

    with tempfile.TemporaryDirectory() as work:
        exe = build(work)

        # The firmware's own names, to compare like with like
        source = open(SKETCH).read()
        akm_body = cut(source, "const char *akmName(")
        akm_body = akm_body[akm_body.index("else"):]
        akm_names = {int(n): s for n, s in re.findall(r'case (\d+): +return "([^"]+)"', akm_body)}
        cipher_body = cut(source, "const char *cipherName(")
        cipher_names = {int(n): s for n, s in re.findall(r'case (\d+): +return "([^"]+)"', cipher_body)}

        total, bad, truncated = 0, 0, 0
        unknown = {}

        for path in sys.argv[1:]:
            frames = tshark_frames(path)

            for number, alt in alternate_classes(path).items():
                if number in frames:
                    frames[number]["alternate"] = alt
            lines, channels = [], {}

            for number, frame in read_pcap(path, work):
                t = frames.get(number)
                if t is None or len(frame) < 24:
                    continue

                subtype = (frame[0] >> 4) & 0x0F
                if subtype not in IE_OFFSET:
                    continue

                # Frames cut off by the capture are not comparable.
                # Firmware before 1.45.1 cut at 512 octets without
                # saying so: 527 with its 15 octets of radiotap.
                length = int(t["frame.len"][0])
                caplen = int(t["frame.cap_len"][0])
                if caplen < length or length == 512 + 15:
                    truncated += 1
                    continue

                freq = t.get("radiotap.channel.freq", [])
                channel = freq_to_channel(int(freq[0])) if freq else 0
                channels[number] = channel

                lines.append("%d %d %s\n" % (number, channel,
                                             frame[IE_OFFSET[subtype]:].hex()))

            out = subprocess.run([exe], input="".join(lines),
                                 capture_output=True, text=True, check=True).stdout

            for line in out.splitlines():
                f = json.loads(line)
                total += 1

                for code in f["unk"]:
                    unknown.setdefault(code, set()).add(os.path.basename(path))

                for name in f["akm"].split(", ") + f["pairwise"].split(", ") + [f["group"]]:
                    if name.startswith(("AKM ", "cipher ")):
                        names.setdefault(name, set()).add(os.path.basename(path))

                diffs = compare(f, frames[f["frame"]], channels[f["frame"]],
                                akm_names, cipher_names)
                if diffs:
                    bad += 1
                    print("%s frame %d:" % (os.path.basename(path), f["frame"]))
                    for d in diffs:
                        print("    " + d)

        print("\n%d frames compared, %d with differences, "
              "%d skipped as cut off by the capture" % (total, bad, truncated))

        if names:
            print("\nNumbers the firmware has no name for:")
            for n, files in sorted(names.items()):
                print("    %-12s in %s" % (n, ", ".join(sorted(files))))

        if unknown:
            # Wireshark's names, as the web interface carries them
            page = open(os.path.join(HERE, "..", "source", "wlanscout", "index_html.h")).read()
            tags = dict(re.findall(r'(\d+):"([^"]*)"',
                                   page[page.index("const IE_NAMES"):page.index("const IE_EXT_NAMES")]))
            exts = dict(re.findall(r'(\d+):"([^"]*)"',
                                   page[page.index("const IE_EXT_NAMES"):page.index("function elementName")]))
            print("\nElements the firmware does not know:")
            for code, files in sorted(unknown.items()):
                label = ("ext %d %s" % (code - 256, exts.get(str(code - 256), "unnamed"))
                         if code >= 256 else "%d %s" % (code, tags.get(str(code), "unnamed")))
                print("    %-45s in %s" % (label, ", ".join(sorted(files))))

        return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
