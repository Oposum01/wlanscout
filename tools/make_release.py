#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Dr.-Ing. Simon Wunderlich Systementwicklung & Beratung
# SPDX-FileContributor: Jan Fuchs <jf [at] simonwunderlich . de>
"""
Puts a finished build into the repository: firmware/ with the images
for esptool and for an update over the network, and at the top the
static copy of the page and the phone flasher (tools/make_static.py),
so that GitHub Pages can publish the repository as it is.

Usage:
    python3 tools/make_release.py BUILDDIR [--xiao BUILDDIR]
                                  [--boot-app0 FILE] [--out DIR]

BUILDDIR is the Arduino build directory, e.g. the one given to
arduino-cli --build-path. Its firmware has to be the version in
source/wlanscout/wlanscout.ino. firmware/ is emptied first, so nothing
of an older version stays behind. --out puts it all somewhere else than
the repository.

    firmware/   wlanscout.ino.bin            update over the network
                wlanscout.ino.bootloader.bin, wlanscout.ino.partitions.bin,
                boot_app0.bin                the parts for esptool
                wlanscout.ino.merged.bin     everything at 0x0, wipes all
                FLASH.txt                    which file goes where
                version.json                 version, size and SHA-256 of
                                             wlanscout.ino.bin, which the
                                             page asks for new firmware
    firmware/xiao/                the same for the Seeed Studio XIAO ESP32-C5,
                                  with --xiao: its build (BOARD_ID
                                  "xiao", the 8 MB partition
                                  table); the page of a XIAO asks
                                  firmware/xiao/version.json
    index.html, manifest.webmanifest, sw.js, favicon.svg,
    icon-192.png, icon-512.png    the page, installable as an app
    flash.html                    the flasher, loading from firmware/
    .nojekyll                     GitHub Pages serves the files as they
                                  are
"""

import argparse
import hashlib
import json
import pathlib
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

sys.path.insert(0, str(ROOT / "tools"))

import make_static  # noqa: E402

PARTS = ["wlanscout.ino.bin", "wlanscout.ino.bootloader.bin",
         "wlanscout.ino.partitions.bin", "wlanscout.ino.merged.bin"]


def empty(folder):
    if folder.exists():
        shutil.rmtree(folder)

    folder.mkdir()


def put_board(folder, build, boot, version, board):
    folder.mkdir(parents=True, exist_ok=True)

    for name in PARTS:
        shutil.copyfile(build / name, folder / name)

    shutil.copyfile(boot, folder / "boot_app0.bin")

    template = "FLASH-xiao.txt" if board == "xiao" else "FLASH.txt"
    text = (ROOT / "tools" / "static" / template).read_text(
        encoding="utf-8")
    (folder / "FLASH.txt").write_text(text.replace("@VERSION@", version),
                                      encoding="utf-8")

    # What the page compares with its sensor's version, from GitHub Pages
    image = (folder / "wlanscout.ino.bin").read_bytes()
    (folder / "version.json").write_text(json.dumps({
        "version": version,
        "size": len(image),
        "sha256": hashlib.sha256(image).hexdigest(),
        "board": board,
    }) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("build", metavar="BUILDDIR")
    parser.add_argument("--xiao", metavar="BUILDDIR",
                        help="the build for the Seeed Studio XIAO ESP32-C5, "
                             "into firmware/xiao/")
    parser.add_argument("--boot-app0", metavar="FILE",
                        help="boot_app0.bin (default: from the installed "
                             "esp32 core)")
    parser.add_argument("--out", metavar="DIR", default=str(ROOT),
                        help="where it goes (default: the repository)")
    args = parser.parse_args()

    out = pathlib.Path(args.out)

    build = pathlib.Path(args.build)
    xiao = pathlib.Path(args.xiao) if args.xiao else None
    version = make_static.firmware_version()

    make_static.check_build(build, version, make_static.BOARDS[0][0], PARTS)

    if xiao:
        make_static.check_build(xiao, version, make_static.BOARDS[1][0],
                                PARTS, need_marker=True)

    boot = (pathlib.Path(args.boot_app0) if args.boot_app0
            else make_static.boot_app0())

    firmware = out / "firmware"
    empty(firmware)

    for (board, folder), board_build in zip(make_static.BOARDS,
                                            (build, xiao)):
        if board_build:
            put_board(firmware / folder, board_build, boot, version, board)

    command = [sys.executable, str(ROOT / "tools" / "make_static.py"),
               str(out), "--page", "index.html", "--firmware", str(build),
               "--boot-app0", str(boot), "--parts-dir", "firmware"]

    if xiao:
        command += ["--firmware-xiao", str(xiao)]

    subprocess.run(command, check=True)

    # No Jekyll run on GitHub Pages, the files go out as they are
    (out / ".nojekyll").write_text("")

    print("release v%s: firmware/ and the page at the top" % version)


if __name__ == "__main__":
    main()
