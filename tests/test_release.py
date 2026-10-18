"""tools/make_release.py: firmware/ and the page at the top, from a build."""

import pathlib
import subprocess
import sys

from test_static import fake_build, version

ROOT = pathlib.Path(__file__).resolve().parent.parent


def release(build, boot, out):
    return subprocess.run([sys.executable,
                           str(ROOT / "tools" / "make_release.py"),
                           str(build), "--boot-app0", str(boot),
                           "--out", str(out)],
                          capture_output=True, text=True)


def fake_release_build(path, ver):
    boot = fake_build(path, ver)
    (path / "wlanscout.ino.merged.bin").write_bytes(b"\xe9merged")

    return boot


def test_folders(tmp_path):
    build = tmp_path / "build"
    boot = fake_release_build(build, version())
    out = tmp_path / "repo"
    out.mkdir()

    result = release(build, boot, out)

    assert result.returncode == 0, result.stderr

    names = sorted(p.name for p in (out / "firmware").iterdir())

    assert names == ["FLASH.txt", "boot_app0.bin", "wlanscout.ino.bin",
                     "wlanscout.ino.bootloader.bin",
                     "wlanscout.ino.merged.bin",
                     "wlanscout.ino.partitions.bin"]

    flash = (out / "firmware" / "FLASH.txt").read_text()

    assert flash.startswith("wlanscout firmware %s " % version())
    assert "@VERSION@" not in flash

    # The page at the top, for GitHub Pages, without Jekyll
    for name in ("index.html", "flash.html", "sw.js",
                 "manifest.webmanifest", "favicon.svg", "icon-192.png",
                 "icon-512.png", ".nojekyll"):
        assert (out / name).is_file(), name

    # The flasher loads from firmware/, no copies next to it
    assert 'const PARTS_DIR = "firmware/";' in (out / "flash.html").read_text()
    assert not (out / "wlanscout.ino.bin").exists()
    assert not (out / "site").exists()


def test_old_files_go(tmp_path):
    build = tmp_path / "build"
    boot = fake_release_build(build, version())
    out = tmp_path / "repo"
    (out / "firmware").mkdir(parents=True)
    (out / "firmware" / "old.bin").write_bytes(b"x")
    (out / "index.html").write_text("old")

    assert release(build, boot, out).returncode == 0

    assert not (out / "firmware" / "old.bin").exists()
    assert (out / "index.html").read_text() != "old"


def test_other_version_refused(tmp_path):
    build = tmp_path / "build"
    boot = fake_release_build(build, "0.0.1")
    out = tmp_path / "repo"
    out.mkdir()

    result = release(build, boot, out)

    assert result.returncode != 0
    assert "build it first" in result.stderr
    assert not (out / "firmware").exists()


def test_missing_part(tmp_path):
    build = tmp_path / "build"
    boot = fake_build(build, version())
    out = tmp_path / "repo"
    out.mkdir()

    result = release(build, boot, out)

    assert result.returncode != 0
    assert "wlanscout.ino.merged.bin missing" in result.stderr
