#!/usr/bin/env python3
"""Prepare original GT2 discs and install the standalone Quest release on Linux."""
import argparse
import json
import os
from pathlib import Path
import platform
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import urllib.request
import uuid
import zipfile
import zlib

from gt2_disc import Disc, digest, safe_relative

VERSION = "0.5.0"
PACKAGE = "io.github.gt2pc.quest"
EXTERNAL = f"/sdcard/Android/data/{PACKAGE}/files"
ADB_URL = "https://dl.google.com/android/repository/platform-tools_r36.0.2-linux.zip"
ADB_HASH = "3afdea91441815ab41254193df0343d92c1b1c0d0237165c3a345c8af8891c31"
DATA = Path(os.environ.get("XDG_DATA_HOME", str(Path.home() / ".local/share"))) / "gt2-vr"
CACHE = Path(os.environ.get("XDG_CACHE_HOME", str(Path.home() / ".cache"))) / "gt2-vr"


def verify_release(root):
    manifest = json.loads((root / "release-manifest.json").read_text(encoding="utf-8-sig"))
    if manifest.get("version") != VERSION or manifest.get("package") != PACKAGE:
        raise ValueError("Unexpected release manifest.")
    seen = set()
    for entry in manifest["files"]:
        name = safe_relative(entry["path"])
        path = root / name
        if name in seen or not path.resolve().is_relative_to(root.resolve()):
            raise ValueError("Duplicate or escaping manifest path.")
        seen.add(name)
        if not path.is_file() or digest(path) != entry["sha256"].lower():
            raise ValueError("Release file failed verification: " + name)
    required = {f"GT2-VR-{VERSION}.apk", "scripts/install-linux.py", "scripts/gt2_disc.py", "scripts/gt2_boot.py", "scripts/linux-downloads.json"}
    if not required <= seen:
        raise ValueError("Incomplete Linux release package.")
    print("Release files verified.", flush=True)


def fetch(url, checksum, destination):
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.is_file() and digest(destination) == checksum:
        return destination
    with tempfile.NamedTemporaryFile(dir=destination.parent, delete=False) as stream:
        temporary = Path(stream.name)
        try:
            with urllib.request.urlopen(url, timeout=90) as response:
                shutil.copyfileobj(response, stream)
            stream.close()
            if digest(temporary) != checksum:
                raise ValueError("Download checksum mismatch: " + url)
            temporary.replace(destination)
        finally:
            temporary.unlink(missing_ok=True)
    return destination


def find_adb(explicit=None):
    found = shutil.which(explicit or "adb")
    if found:
        return found
    if explicit:
        raise ValueError("ADB executable not found: " + explicit)
    if sys.platform != "linux" or platform.machine().lower() not in ("x86_64", "amd64"):
        raise ValueError("Automatic ADB download requires x86_64 Linux; provide --adb on other hosts.")
    print("Downloading Google Platform Tools 36.0.2 into your home folder.\n"
          "SDK terms: https://developer.android.com/studio/terms", flush=True)
    archive = fetch(ADB_URL, ADB_HASH, CACHE / "platform-tools-36.0.2-linux.zip")
    target = CACHE / "platform-tools-36.0.2-linux"
    target.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(archive) as bundle:
        for name in ("adb", "NOTICE.txt", "source.properties"):
            (target / name).write_bytes(bundle.read("platform-tools/" + name))
    (target / "adb").chmod(0o755)
    return str(target / "adb")


class Adb:
    def __init__(self, executable, serial=None):
        self.executable = executable
        self.serial = None
        output = self.run("devices")
        devices = re.findall(r"^(\S+)\s+device\s*$", output, re.M)
        if "no permissions" in output.lower():
            raise ValueError("Linux denied USB access. See docs/LINUX-INSTALL.md#usb-access; no system changes were made.")
        if serial is None and len(devices) == 1:
            serial = devices[0]
        if serial not in devices or not re.fullmatch(r"[A-Za-z0-9._:-]+", serial or ""):
            raise ValueError("Connect one Quest with developer mode enabled and accept USB debugging inside it. "
                             "Use --serial if more than one device is connected.\n" + output)
        self.serial = serial

    def run(self, *args):
        command = [self.executable]
        if self.serial:
            command += ["-s", self.serial]
        command += list(map(str, args))
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if result.returncode or re.search(r"\bFailure \[", result.stdout):
            raise RuntimeError("ADB failed: " + result.stdout.strip() + "\nExisting app data was not uninstalled.")
        return result.stdout

    def shell(self, *args):
        return self.run("shell", shlex.join(map(str, args)))

    def broadcast(self, action, paths=None):
        args = ["am", "broadcast", "-n", PACKAGE + "/.ImportAccessReceiver", "-a", PACKAGE + "." + action]
        if paths:
            args += ["--es", "paths", ",".join(paths)]
        reply = self.shell(*args)
        if not re.search(r"\bresult=0\b", reply):
            raise RuntimeError("Application data access check failed: " + reply)

    def verify_file(self, local, remote):
        expected = digest(local)
        if not self.shell("sha256sum", remote).startswith(expected + " "):
            raise RuntimeError("Headset SHA-256 mismatch: " + remote)


def publish_disc(staged, runtime, mode, profile):
    target = runtime / mode
    if target.is_symlink():
        raise ValueError("Refusing a symlinked disc directory.")
    target.mkdir(exist_ok=True)
    names = ["assets", "disc.raw2352"]
    hd = target / "hd"
    if hd.exists():
        stamp = hd / "profile.txt"
        if not stamp.is_file() or stamp.read_text().strip() != profile:
            names.append("hd")
    for name in names:
        if (target / name).is_symlink():
            raise ValueError("Refusing a symlinked managed path: " + str(target / name))
    backup = runtime / ("backup-" + mode + "-" + uuid.uuid4().hex[:12])
    backup.mkdir()
    moved, installed = [], []
    try:
        for name in names:
            if (target / name).exists():
                (target / name).rename(backup / name)
                moved.append(name)
            if (staged / name).exists():
                (staged / name).rename(target / name)
                installed.append(name)
    except BaseException:
        for name in reversed(installed):
            (target / name).rename(staged / name)
        for name in reversed(moved):
            (backup / name).rename(target / name)
        raise
    print(f"Prepared {mode}; previous managed files retained in {backup}.", flush=True)


def install_quest(adb, root, runtime, modes):
    print("Installing Quest APK (preserving saves/settings)...", flush=True)
    print(adb.run("install", "-r", root / f"GT2-VR-{VERSION}.apk"), flush=True)
    adb.broadcast("PREPARE_DATA")
    for mode in modes:
        source = runtime / mode
        directories = [mode, mode + "/assets"]
        for folder in (source / "assets", source / "hd"):
            if folder.is_dir():
                directories.append(folder.relative_to(runtime).as_posix())
                directories += [p.relative_to(runtime).as_posix() for p in folder.rglob("*") if p.is_dir()]
        # The receiver caps each path list at 8192 bytes.
        batch = []
        for directory in sorted(set(directories)):
            if not re.fullmatch(r"(arcade|simulation)(/[A-Za-z0-9_.-]+)*", directory):
                raise ValueError("Unsupported asset directory: " + directory)
            if sum(len(p) + 1 for p in batch) + len(directory) > 7000:
                adb.broadcast("PREPARE_DATA", batch)
                batch = []
            batch.append(directory)
        if batch:
            adb.broadcast("PREPARE_DATA", batch)
        for name in ("assets", "hd", "disc.raw2352"):
            local = source / name
            if local.exists():
                print(f"Copying {mode}/{name}...", flush=True)
                print(adb.run("push", "--sync", local, f"{EXTERNAL}/{mode}/"), flush=True)
        adb.verify_file(source / "disc.raw2352", f"{EXTERNAL}/{mode}/disc.raw2352")
        adb.broadcast("VERIFY_DATA", [mode + "/disc.raw2352", mode + "/assets/.carcolor"])
    for name in ("startup.gtm", "startup-hd.gtm"):
        if (runtime / name).is_file():
            adb.run("push", "--sync", runtime / name, EXTERNAL + "/" + name)
            adb.verify_file(runtime / name, EXTERNAL + "/" + name)
    with tempfile.TemporaryDirectory() as temp:
        path = Path(temp) / "launch-mode.txt"
        path.write_text(modes[0], encoding="ascii")
        adb.run("push", path, EXTERNAL + "/launch-mode.txt")
    print("Installation verified. Launch GT2 VR from Unknown Sources on Quest. No PC is needed to play.")


def prompt_path(message):
    text = input(message).strip()
    # Dolphin's terminal drag-and-drop may quote paths with spaces.
    try:
        parts = shlex.split(text)
    except ValueError:
        return text
    return parts[0] if len(parts) == 1 else text


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--disc", action="append", help="BIN/CUE or raw 2352-byte ISO; repeat for both discs")
    parser.add_argument("--runtime", type=Path, default=DATA / "runtime")
    parser.add_argument("--adb", help="Use an existing ADB executable")
    parser.add_argument("--serial", help="Choose an authorized device")
    startup = parser.add_mutually_exclusive_group()
    startup.add_argument("--bios", type=Path, help="Optional local 512 KiB PS1 BIOS for startup capture")
    startup.add_argument("--no-bios", action="store_true", help="Do not ask for or capture a BIOS intro")
    parser.add_argument("--capture-core", type=Path, help="Trusted software Beetle libretro .so for optional capture")
    parser.add_argument("--prepare-only", action="store_true", help="Prepare files locally without accessing Quest")
    parser.add_argument("--verify-only", action="store_true", help="Verify release checksums without installing")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    verify_release(root)
    if args.verify_only:
        return
    if not args.disc:
        args.disc = [prompt_path("Path to your first GT2 BIN or CUE (extract ZIP/7z first): ")]
        second = prompt_path("Second disc path, or Enter for just one disc: ")
        if second:
            args.disc.append(second)
    if not args.bios and not args.no_bios:
        print("PlayStation startup is optional. Press Enter to play without a BIOS.\n"
              "Existing prepared intros are retained; you can disable them in the VR menu.")
        bios = prompt_path("Local PS1 BIOS path, or Enter to skip: ")
        if bios:
            args.bios = Path(bios)
    if args.bios and (not args.bios.expanduser().is_file() or args.bios.expanduser().stat().st_size != 524288):
        raise ValueError("Select a 512 KiB PS1 BIOS dump, or omit --bios to play without the intro.")
    adb = None if args.prepare_only else Adb(find_adb(args.adb), args.serial)
    runtime = args.runtime.expanduser().resolve()
    runtime.mkdir(parents=True, exist_ok=True)
    modes = []
    # Inspect all inputs before replacing any prepared disc.
    for image in args.disc:
        with Disc(image) as disc:
            if disc.mode in modes:
                raise ValueError("Select one Arcade and/or one Simulation disc, not two copies of the same mode.")
            modes.append(disc.mode)
            print(f"Recognized {disc.mode}: {disc.profile}", flush=True)
    with tempfile.TemporaryDirectory(prefix=".install-", dir=runtime) as temp:
        for image in args.disc:
            with Disc(image) as disc:
                staged = Path(temp) / disc.mode
                disc.extract(staged)
                publish_disc(staged, runtime, disc.mode, disc.profile)
    if args.bios:
        from gt2_boot import prepare_startup
        prepare_startup(runtime, modes, args.bios.expanduser(), args.capture_core, CACHE, fetch)
    if adb:
        install_quest(adb, root, runtime, modes)
    else:
        print("Prepared data: " + str(runtime))


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, ImportError, zlib.error, EOFError, KeyboardInterrupt) as error:
        print("Installation stopped: " + str(error), file=sys.stderr)
        sys.exit(1)
