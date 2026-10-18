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
    python3 tools/make_release.py BUILDDIR [--boot-app0 FILE] [--out DIR]

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
    index.html, manifest.webmanifest, sw.js, favicon.svg,
    icon-192.png, icon-512.png    the page, installable as an app
    flash.html                    the flasher, loading from firmware/
    .nojekyll                     GitHub Pages serves the files as they
                                  are
"""

import argparse
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


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("build", metavar="BUILDDIR")
    parser.add_argument("--boot-app0", metavar="FILE",
                        help="boot_app0.bin (default: from the installed "
                             "esp32 core)")
    parser.add_argument("--out", metavar="DIR", default=str(ROOT),
                        help="where it goes (default: the repository)")
    args = parser.parse_args()

    out = pathlib.Path(args.out)

    build = pathlib.Path(args.build)
    version = make_static.firmware_version()

    for name in PARTS:
        if not (build / name).is_file():
            sys.exit("%s missing in %s" % (name, build))

    if version.encode() not in (build / "wlanscout.ino.bin").read_bytes():
        sys.exit("wlanscout.ino.bin in %s is not v%s: build it first"
                 % (build, version))

    boot = (pathlib.Path(args.boot_app0) if args.boot_app0
            else make_static.boot_app0())

    firmware = out / "firmware"
    empty(firmware)

    for name in PARTS:
        shutil.copyfile(build / name, firmware / name)

    shutil.copyfile(boot, firmware / "boot_app0.bin")

    text = (ROOT / "tools" / "static" / "FLASH.txt").read_text(
        encoding="utf-8")
    (firmware / "FLASH.txt").write_text(text.replace("@VERSION@", version),
                                        encoding="utf-8")

    subprocess.run([sys.executable, str(ROOT / "tools" / "make_static.py"),
                    str(out), "--page", "index.html", "--firmware",
                    str(build), "--boot-app0", str(boot),
                    "--parts-dir", "firmware"], check=True)

    # No Jekyll run on GitHub Pages, the files go out as they are
    (out / ".nojekyll").write_text("")

    print("release v%s: firmware/ and the page at the top" % version)


if __name__ == "__main__":
    main()
