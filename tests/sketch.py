"""The firmware's source as Arduino compiles it: config.h, the main
sketch, then the numbered tabs in name order. Tests cut functions out
of this text by markers, wherever a function lives."""

import pathlib

SOURCE = pathlib.Path(__file__).resolve().parent.parent / "source" / "wlanscout"


def sketch_files():
    tabs = sorted(p for p in SOURCE.glob("*.ino") if p.name != "wlanscout.ino")
    files = [SOURCE / "config.h", SOURCE / "wlanscout.ino"] + tabs
    return [p for p in files if p.exists()]


def sketch_text():
    return "\n".join(p.read_text(encoding="utf-8") for p in sketch_files())
