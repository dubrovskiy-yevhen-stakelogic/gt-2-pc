#!/usr/bin/env python3
"""Build a relocatable macOS player app, retain licences, sign and optionally notarize it."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import plistlib
import re
import shutil
import subprocess
import sys
import tempfile
import uuid


def run(*args):
    result = subprocess.run(list(map(str, args)), stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if result.returncode:
        raise RuntimeError(f"{args[0]} failed:\n{result.stdout}\n{result.stderr}")
    return result.stdout


def digest(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def dependencies(text):
    return [line.strip().split(" (compatibility version", 1)[0]
            for line in text.splitlines()[1:] if " (compatibility version" in line]


def rpaths(text):
    return re.findall(r"cmd LC_RPATH\s+cmdsize \d+\s+path (.*?) \(offset \d+\)", text)


def minimum_os(text):
    versions = re.findall(r"\bminos (\d+(?:\.\d+){0,2})", text)
    versions += re.findall(r"cmd LC_VERSION_MIN_MACOSX\s+cmdsize \d+\s+version (\d+(?:\.\d+){0,2})", text)
    return max((tuple(map(int, v.split("."))) for v in versions), default=(14, 0))


def system_library(name):
    return name.startswith(("/System/Library/", "/usr/lib/"))


def resolve_dependency(name, source, executable_dir, search_dirs, inherited=()):
    def expand(value):
        return value.replace("@loader_path", str(source.parent)).replace("@executable_path", str(executable_dir))
    if name.startswith("@rpath/"):
        relative = name[len("@rpath/"):]
        candidates = [Path(expand(path)) / relative for path in inherited]
        candidates += [Path(path) / relative for path in search_dirs]
    else:
        candidates = [Path(expand(name))]
    for path in candidates:
        if path.is_file():
            return path.resolve()
    raise ValueError(f"Unresolved dependency {name} required by {source}; use --library-dir.")


class Bundler:
    def __init__(self, app, architecture, search_dirs):
        self.app, self.arch = app, architecture
        self.frameworks = app / "Contents/Frameworks"
        self.frameworks.mkdir(parents=True)
        self.search_dirs = search_dirs
        self.copied = {}
        self.names = {}
        self.binaries = []
        self.minimum = (14, 0)

    def copy(self, source, destination=None, executable_dir=None, inherited=()):
        source = source.resolve(strict=True)
        if source in self.copied:
            return self.copied[source]
        if self.arch not in run("lipo", "-archs", source).split():
            raise ValueError(f"Missing {self.arch} slice: {source}")
        if destination is None:
            destination = self.frameworks / source.name
        if destination in self.names and self.names[destination] != source:
            raise ValueError(f"Conflicting dependency names: {destination.name}")
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)
        destination.chmod(0o755)
        self.copied[source] = destination
        self.names[destination] = source
        self.binaries.append(destination)
        load_commands = run("otool", "-l", source)
        self.minimum = max(self.minimum, minimum_os(load_commands))
        own_rpaths = rpaths(load_commands)
        # Expand inherited rpaths in the context of the file which declared them.
        expanded = [p.replace("@loader_path", str(source.parent)).replace("@executable_path", str(executable_dir or source.parent))
                    for p in own_rpaths] + list(inherited)
        own_id = run("otool", "-D", source).splitlines()[1:]
        for name in dict.fromkeys(dependencies(run("otool", "-L", source))):
            if name in own_id or system_library(name):
                continue
            child = resolve_dependency(name, source, executable_dir or source.parent, self.search_dirs, expanded)
            copied = self.copy(child, executable_dir=executable_dir or source.parent, inherited=expanded)
            relative = os.path.relpath(copied, destination.parent).replace(os.sep, "/")
            run("install_name_tool", "-change", name, "@loader_path/" + relative, destination)
        if own_id:
            run("install_name_tool", "-id", "@rpath/" + destination.name, destination)
        for path in dict.fromkeys(own_rpaths):
            run("install_name_tool", "-delete_rpath", path, destination)
        return destination

    def verify(self):
        for binary in self.binaries:
            own_id = run("otool", "-D", binary).splitlines()[1:]
            for name in dependencies(run("otool", "-L", binary)):
                if name in own_id or system_library(name):
                    continue
                if not name.startswith("@loader_path/"):
                    raise ValueError(f"Non-portable dependency remains: {binary}: {name}")
                resolved = (binary.parent / name[len("@loader_path/"):]).resolve()
                if not resolved.is_relative_to(self.app.resolve()) or not resolved.is_file():
                    raise ValueError(f"Dependency escapes/is absent from bundle: {name}")
            if rpaths(run("otool", "-l", binary)):
                raise ValueError(f"Unexpected build-time rpath: {binary}")


def copy_licences(source, destination):
    parts = source.parts
    if "Cellar" in parts:
        root = Path(*parts[:parts.index("Cellar") + 3])
    else:
        root = source.parent.parent  # SDL install prefix or Vulkan SDK macOS directory
    candidates = list(root.glob("LICENSE*")) + list(root.glob("COPYING*")) + list(root.glob("NOTICE*"))
    for folder in (root / "share/licenses", root / "share/doc", root / "licenses"):
        if folder.is_dir():
            candidates += [p for p in folder.rglob("*") if p.is_file() and any(word in p.name.upper() for word in ("LICENSE", "COPYING", "NOTICE", "COPYRIGHT"))]
    candidates = sorted(set(p for p in candidates if p.is_file()))
    if not candidates:
        raise ValueError(f"No installed dependency licence found under {root}; retain its upstream licences there before packaging.")
    for path in candidates:
        target = destination / path.relative_to(root)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, target)


def source_snapshot(root):
    manifest = root / "SOURCE-MANIFEST.json"
    if manifest.is_file():
        data = json.loads(manifest.read_text(encoding="utf-8-sig"))
        seen = set()
        for entry in data["files"]:
            name = entry["path"]
            relative = PurePosixPath(name)
            if not name or relative.is_absolute() or ".." in relative.parts or "\\" in name or ":" in name or name in seen:
                raise ValueError("Unsafe/duplicate source manifest path")
            seen.add(name)
            path = root / name
            matches = path.is_file() and digest(path) == entry["sha256"]
            if not matches and path.is_file() and entry.get("textSha256"):
                # Git checkouts may apply different line endings to source text.
                matches = hashlib.sha256(path.read_bytes().replace(b"\r\n", b"\n")).hexdigest() == entry["textSha256"]
            if not path.resolve().is_relative_to(root.resolve()) or not matches:
                raise ValueError("Source snapshot changed: " + name)
        return data
    # A development checkout has no source kit manifest; record its relevant files directly.
    paths = [root / "CMakeLists.txt"]
    for folder in ("src", "tools", "cmake", "third_party", "scripts"):
        paths += [p for p in (root / folder).rglob("*") if p.is_file() and not any(
            part in ("bin", "obj", "__pycache__") or part.startswith("build") for part in p.relative_to(root / folder).parts[:-1])]
    return {"sourceProvenance": "filesystem-sha256", "files": [
        {"path": p.relative_to(root).as_posix(), "sha256": digest(p)} for p in sorted(set(paths))]}


def install_app(app, target):
    """Same-volume staged update; old app is retained and restored on publication failure."""
    if target.is_symlink():
        raise ValueError("Refusing to replace a symlinked app")
    target.parent.mkdir(parents=True, exist_ok=True)
    staged = target.parent / (".GT2-new-" + uuid.uuid4().hex + ".app")
    backup = target.parent / ("GT2-backup-" + uuid.uuid4().hex + ".app")
    shutil.copytree(app, staged, symlinks=True)
    moved = False
    try:
        if target.exists():
            info = target / "Contents/Info.plist"
            if not info.is_file() or plistlib.loads(info.read_bytes()).get("CFBundleIdentifier") != "io.github.gt2pc.macos":
                raise ValueError("Existing GT2.app has a different identity; refusing to replace it")
            target.rename(backup)
            moved = True
        staged.rename(target)
    except BaseException:
        if moved:
            backup.rename(target)
        raise
    return backup if moved else None


def sign(app, binaries, identity):
    options = ["--options", "runtime", "--timestamp"] if identity != "-" else []
    for binary in binaries:
        run("codesign", "--force", "--sign", identity, *options, binary)
    run("codesign", "--force", "--sign", identity, *options, app)
    run("codesign", "--verify", "--deep", "--strict", app)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=Path("build_macos"))
    parser.add_argument("--moltenvk", type=Path, required=True)
    parser.add_argument("--library-dir", type=Path, action="append", default=[])
    parser.add_argument("--license-dir", type=Path, action="append", default=[])
    parser.add_argument("--output", type=Path)
    parser.add_argument("--install", action="store_true")
    parser.add_argument("--identity", default="-", help="Developer ID Application identity; default is local ad-hoc signing")
    parser.add_argument("--notary-profile", help="Existing notarytool keychain profile; explicitly authorizes Apple's notarization upload")
    args = parser.parse_args()
    if sys.platform != "darwin":
        parser.error("Packaging requires macOS and its Apple tools")
    if args.identity != "-" and not args.identity.startswith("Developer ID Application:"):
        parser.error("Use a Developer ID Application identity or '-' for local ad-hoc signing")
    if args.notary_profile and args.identity == "-":
        parser.error("Notarization requires a Developer ID Application identity")
    root = Path(__file__).resolve().parents[1]
    build = args.build.resolve()
    cache = (build / "CMakeCache.txt").read_text()
    match = re.search(r"^CMAKE_HOME_DIRECTORY:INTERNAL=(.+)$", cache, re.M)
    if not match or Path(match[1].strip()).resolve() != root:
        raise ValueError("Build directory belongs to a different source tree")
    version = re.search(r"project\(gt2pc VERSION ([0-9.]+)", (root / "CMakeLists.txt").read_text())[1]
    arch = platform.machine()
    if arch not in ("arm64", "x86_64"):
        raise ValueError("Unsupported native architecture")
    provenance = source_snapshot(root)
    print(run("ctest", "--test-dir", build, "--output-on-failure"), flush=True)
    out = (args.output or root / "dist" / f"GT2-{version}-macos-{arch}-{datetime.now().strftime('%Y%m%d-%H%M%S')}").resolve()
    out.mkdir(parents=True, exist_ok=False)
    app = out / "GT2.app"
    bundler = Bundler(app, arch, args.library_dir)
    for executable, name in (("gt2game", "gt2game"), ("gt2install", "gt2install"), ("gt2media", "gt2media"), ("gt2maclauncher", "GT2")):
        bundler.copy(build / executable, app / "Contents/MacOS" / name)
    driver = bundler.copy(args.moltenvk)
    resources = app / "Contents/Resources"
    icd_dir = resources / "vulkan/icd.d"
    icd_dir.mkdir(parents=True)
    (icd_dir / "MoltenVK_icd.json").write_text(json.dumps({"file_format_version": "1.0.0", "ICD": {
        "library_path": "../../../Frameworks/" + driver.name, "api_version": "1.3.0", "is_portability_driver": True}}, indent=2) + "\n")
    licences = resources / "LICENSES"
    licences.mkdir()
    for relative in ("LICENSE", "THIRD_PARTY.md",
                     "third_party/stb/LICENSE.txt", "third_party/xbr/LICENSE.txt", "third_party/vrhands/ULTIMATEXR_LICENSE.txt",
                     "third_party/vrhands/MIAMIVR_LICENSE.txt"):
        target = licences / relative.replace("/", "-")
        shutil.copy2(root / relative, target)
    for original, copied in bundler.copied.items():
        if copied.parent == bundler.frameworks:
            copy_licences(original, licences / copied.name)
    for i, folder in enumerate(args.license_dir):
        shutil.copytree(folder, licences / f"additional-{i}")
    shutil.copy2(root / "docs/MACOS.md", resources / "MACOS.md")
    (resources / "source-manifest.json").write_text(json.dumps(provenance, indent=2) + "\n")
    if (build / "macos-dependencies.json").is_file():
        shutil.copy2(build / "macos-dependencies.json", resources / "build-dependencies.json")
    minimum = ".".join(map(str, bundler.minimum))
    info = {"CFBundleExecutable": "GT2", "CFBundleIdentifier": "io.github.gt2pc.macos", "CFBundleName": "GT2",
            "CFBundleDisplayName": "GT2", "CFBundlePackageType": "APPL", "CFBundleShortVersionString": version,
            "CFBundleVersion": version, "LSMinimumSystemVersion": minimum, "NSHighResolutionCapable": True,
            "LSApplicationCategoryType": "public.app-category.racing-games"}
    (app / "Contents/Info.plist").write_bytes(plistlib.dumps(info))
    bundler.verify()
    sign(app, bundler.binaries, args.identity)
    # Launch the relocated binary with build-machine library paths removed.
    clean = {k: v for k, v in os.environ.items() if not k.startswith(("DYLD_", "VK_", "VULKAN_", "SDL_"))}
    subprocess.run([str(app / "Contents/MacOS/gt2game"), "--help"], env=clean, check=True)
    notary = None
    if args.notary_profile:
        upload = out / "notary-upload.zip"
        run("ditto", "-c", "-k", "--keepParent", app, upload)
        notary = json.loads(run("xcrun", "notarytool", "submit", upload, "--keychain-profile", args.notary_profile, "--wait", "--output-format", "json"))
        (out / "notarization.json").write_text(json.dumps(notary, indent=2) + "\n")
        if notary.get("status") != "Accepted":
            raise ValueError("Apple did not accept notarization; no release archive was produced")
        run("xcrun", "stapler", "staple", app)
        run("xcrun", "stapler", "validate", app)
        run("spctl", "--assess", "--type", "execute", app)
        upload.unlink()
    archive = out / f"GT2-{version}-macos-{arch}.zip"
    run("ditto", "-c", "-k", "--keepParent", app, archive)
    with tempfile.TemporaryDirectory(prefix="gt2-dmg-", dir=out) as stage:
        shutil.copytree(app, Path(stage) / "GT2.app", symlinks=True)
        (Path(stage) / "Applications").symlink_to("/Applications")
        (Path(stage) / "READ-ME.txt").write_text("Drag GT2 to Applications. Open it and select your own GT2 BIN/CUE images.\n"
            "No build tools or Vulkan SDK are needed. Saves: ~/Library/Application Support/GT2/saves\n"
            + ("Apple notarization: accepted.\n" if notary else "Apple notarization: not notarized.\n"))
        dmg = out / f"GT2-{version}-macos-{arch}.dmg"
        run("hdiutil", "create", "-volname", f"GT2 {version}", "-srcfolder", stage, "-format", "UDZO", dmg)
    if notary:
        run("codesign", "--sign", args.identity, "--timestamp", dmg)
        disk_notary = json.loads(run("xcrun", "notarytool", "submit", dmg, "--keychain-profile", args.notary_profile, "--wait", "--output-format", "json"))
        (out / "dmg-notarization.json").write_text(json.dumps(disk_notary, indent=2) + "\n")
        if disk_notary.get("status") != "Accepted":
            raise ValueError("Apple did not accept DMG notarization")
        run("xcrun", "stapler", "staple", dmg)
        run("xcrun", "stapler", "validate", dmg)
    metadata = {"version": version, "architecture": arch, "minimumMacOS": minimum,
                "createdUtc": datetime.now(timezone.utc).isoformat(), "notarized": bool(notary),
                "signing": "ad-hoc-local" if args.identity == "-" else "developer-id",
                "sourceManifestSha256": digest(resources / "source-manifest.json"),
                "files": [{"path": p.relative_to(out).as_posix(), "sha256": digest(p)} for p in sorted(out.rglob("*")) if p.is_file()]}
    (out / "release-manifest.json").write_text(json.dumps(metadata, indent=2) + "\n")
    if args.install:
        # Avoid replacing the running app while its first-run importer might be active.
        import fcntl
        lock_path = Path.home() / "Library/Application Support/GT2/.macos-app.lock"
        lock_path.parent.mkdir(parents=True, exist_ok=True)
        with lock_path.open("a") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            backup = install_app(app, Path.home() / "Applications/GT2.app")
            if backup:
                print("Previous app retained:", backup)
            print("Installed source revision:", provenance.get("sourcePackageRevision", "development"))
            print("Installed game SHA256:", digest(Path.home() / "Applications/GT2.app/Contents/MacOS/gt2game"))
    print("Packaged:", out)
    print("Minimum macOS:", minimum, "Architecture:", arch, "Notarized:", bool(notary))


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        sys.exit(str(error))
