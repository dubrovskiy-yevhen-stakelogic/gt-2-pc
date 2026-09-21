"""Offline libretro BIOS capture. Neither firmware nor captured media is bundled."""
import ctypes as C
import importlib
import io
import json
from pathlib import Path
import platform
import shutil
import struct
import sys
import tempfile
import zipfile

from gt2_disc import Disc, safe_relative

CORE_URL = "https://buildbot.libretro.com/nightly/linux/x86_64/latest/mednafen_psx_libretro.so.zip"
CORE_HASH = "66e920f8218938070e848d6d2784651b5bab4232edc5f9ad281308fe4cb532c2"


def load_pillow(cache, fetch):
    try:
        return importlib.import_module("PIL.Image")
    except ImportError:
        pass
    abi = f"cp{sys.version_info.major}{sys.version_info.minor}"
    dependencies = json.loads(Path(__file__).with_name("linux-downloads.json").read_text())
    if sys.platform != "linux" or platform.machine() != "x86_64" or abi not in dependencies["pillow"]:
        raise ValueError("Optional startup capture needs Pillow. Install it for this Python or continue without --bios.")
    package = dependencies["pillow"][abi]
    print("Downloading Pillow for optional startup capture into the user cache...", flush=True)
    wheel = fetch(package["url"], package["sha256"], cache / package["name"])
    folder = cache / ("pillow-" + dependencies["pillowVersion"] + "-" + abi)
    folder.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(wheel) as archive:
        for entry in archive.infolist():
            if entry.is_dir():
                continue
            name = safe_relative(entry.filename)
            path = folder / name
            if not path.resolve().is_relative_to(folder.resolve()):
                raise ValueError("Pillow cache contains an escaping symlink.")
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(archive.read(entry))
    sys.path.insert(0, str(folder))
    return importlib.import_module("PIL.Image")


class Variable(C.Structure):
    _fields_ = [("key", C.c_char_p), ("value", C.c_char_p)]


class Game(C.Structure):
    _fields_ = [("path", C.c_char_p), ("data", C.c_void_p), ("size", C.c_size_t), ("meta", C.c_char_p)]


class Geometry(C.Structure):
    _fields_ = [("width", C.c_uint), ("height", C.c_uint), ("max_width", C.c_uint), ("max_height", C.c_uint), ("aspect", C.c_float)]


class Timing(C.Structure):
    _fields_ = [("fps", C.c_double), ("sample_rate", C.c_double)]


class AVInfo(C.Structure):
    _fields_ = [("geometry", Geometry), ("timing", Timing)]


def capture(core, system, cue, output, Image):
    library = C.CDLL(str(Path(core).resolve()))
    options, audio, index, errors = {}, bytearray(), [], []
    folder = str(Path(system).resolve()).encode()
    state = {"format": 0, "white": 0, "logo": 0, "end": 0}
    previous = Image.new("RGB", (640, 480))

    def environment(command, data):
        try:
            if command in (9, 31):  # System/save directory.
                C.cast(data, C.POINTER(C.c_char_p))[0] = folder
            elif command == 10:
                state["format"] = C.cast(data, C.POINTER(C.c_int))[0]
                return state["format"] in (0, 1, 2)
            elif command in (3, 17):  # Can duplicate frames / variable updates.
                C.cast(data, C.POINTER(C.c_bool))[0] = command == 3
            elif command in (39, 52):  # English / legacy core options.
                C.cast(data, C.POINTER(C.c_uint))[0] = 0
            elif command == 15:
                variable = C.cast(data, C.POINTER(Variable))[0]
                variable.value = options.get(variable.key)
                return variable.value is not None
            elif command == 16:
                variables = C.cast(data, C.POINTER(Variable))
                i = 0
                while variables[i].key:
                    key, value = variables[i].key, variables[i].value or b""
                    if b"; " in value:
                        options.setdefault(key, value.split(b"; ", 1)[1].split(b"|")[0])
                    if b"skip_bios" in key:
                        options[key] = b"disabled"
                    i += 1
            elif command not in (11, 18, 32, 35, 37):
                return False
            return True
        except BaseException as error:
            errors.append(error)
            return False

    with Path(output).open("w+b") as stream:
        stream.write(bytes(64))

        def video(pixels, width, height, pitch):
            nonlocal previous
            try:
                if pixels == C.c_void_p(-1).value or width > 4096 or height > 4096 or pitch > 65536:
                    raise ValueError("Capture requires a software-rendering core.")
                if pixels and width and height:
                    rawmode = {0: "BGR;15", 1: "BGRX", 2: "BGR;16"}[state["format"]]
                    previous = Image.frombytes("RGB", (width, height), C.string_at(pixels, pitch * height), "raw", rawmode, pitch)
                    previous = previous.resize((640, 480), Image.Resampling.NEAREST)
                jpeg = io.BytesIO()
                previous.save(jpeg, "JPEG", quality=95, subsampling=0)
                data = jpeg.getvalue()
                index.append((stream.tell(), len(data)))
                stream.write(data)
                sample = previous.tobytes()
                white = red = green = blue = 0
                for i in range(0, len(sample), 12):
                    r, g, b = sample[i:i + 3]
                    white += r > 140 and g > 140 and b > 140 and abs(r - g) < 8 and abs(g - b) < 8
                    red += r > 100 and r > g * 2 and r > b * 2
                    green += g > 65 and g > r * 3 // 2 and g > b * 7 // 10
                    blue += b > 65 and b > r * 3 // 2 and b > g * 4 // 5
                frame = len(index)
                if not state["white"] and white > 40000:
                    state["white"] = frame
                    print("Captured white Sony screen.", flush=True)
                if state["white"] and not state["logo"] and white < 18000 and red > 80 and green > 30 and blue > 30:
                    state["logo"] = frame
                    print("Captured PlayStation logo.", flush=True)
                if state["logo"] and frame > state["logo"] + 60 and red < 8 and green < 8 and blue < 8:
                    state["end"] = frame
            except BaseException as error:
                errors.append(error)

        def audio_sample(left, right):
            audio.extend(struct.pack("<hh", left, right))

        def audio_batch(data, frames):
            audio.extend(C.string_at(data, frames * 4))
            return frames

        callbacks = [
            ("environment", C.CFUNCTYPE(C.c_bool, C.c_uint, C.c_void_p)(environment)),
            ("video_refresh", C.CFUNCTYPE(None, C.c_void_p, C.c_uint, C.c_uint, C.c_size_t)(video)),
            ("audio_sample", C.CFUNCTYPE(None, C.c_int16, C.c_int16)(audio_sample)),
            ("audio_sample_batch", C.CFUNCTYPE(C.c_size_t, C.c_void_p, C.c_size_t)(audio_batch)),
            ("input_poll", C.CFUNCTYPE(None)(lambda: None)),
            ("input_state", C.CFUNCTYPE(C.c_int16, C.c_uint, C.c_uint, C.c_uint, C.c_uint)(lambda *_: 0)),
        ]
        for name, callback in callbacks:
            setter = getattr(library, "retro_set_" + name)
            setter.argtypes, setter.restype = [type(callback)], None
            setter(callback)
        library.retro_load_game.argtypes, library.retro_load_game.restype = [C.POINTER(Game)], C.c_bool
        library.retro_get_system_av_info.argtypes = [C.POINTER(AVInfo)]
        for name in ("init", "deinit", "run", "unload_game"):
            getattr(library, "retro_" + name).restype = None
        library.retro_init()
        loaded = False
        try:
            game = Game(str(Path(cue).resolve()).encode(), None, 0, None)
            loaded = library.retro_load_game(C.byref(game))
            if not loaded:
                raise ValueError("Could not boot with this BIOS/disc combination.")
            av = AVInfo()
            library.retro_get_system_av_info(C.byref(av))
            if av.timing.sample_rate != 44100 or not 45 <= av.timing.fps <= 65:
                raise ValueError("Unsupported BIOS capture timing.")
            for _ in range(1200):
                library.retro_run()
                if errors:
                    raise RuntimeError("Capture callback failed: " + str(errors[0]))
                if state["end"]:
                    break
            if not all(state[key] for key in ("white", "logo", "end")):
                raise ValueError("Both complete BIOS screens were not detected; use firmware matching your disc region.")
            audio_at = stream.tell()
            stream.write(audio)
            index_at = stream.tell()
            for offset, size in index:
                stream.write(struct.pack("<QII", offset, size, 0))
            stream.seek(0)
            stream.write(struct.pack("<8s6I3Q2I", b"G2MEDIA1", 640, 480, len(index),
                                     int(av.timing.fps * 1000 + .5), 1000, 44100,
                                     index_at, audio_at, len(audio) // 2, 640, 480))
        finally:
            if loaded:
                library.retro_unload_game()
            library.retro_deinit()
    print(f"Prepared complete PS1 startup: {len(index)} frames.", flush=True)


def prepare_startup(runtime, modes, bios, core, cache, fetch):
    if bios.stat().st_size != 524288:
        raise ValueError("A local 512 KiB PS1 BIOS dump is required only for the optional startup.")
    Image = load_pillow(cache, fetch)
    if not core:
        if sys.platform != "linux" or platform.machine() != "x86_64":
            raise ValueError("Supply a matching software libretro core with --capture-core.")
        archive = fetch(CORE_URL, CORE_HASH, cache / "beetle-linux-20260921.zip")
        core = cache / "mednafen_psx_libretro.so"
        with zipfile.ZipFile(archive) as bundle:
            core.write_bytes(bundle.read("mednafen_psx_libretro.so"))
    with tempfile.TemporaryDirectory(prefix=".boot-", dir=runtime) as temp:
        folder = Path(temp)
        for name in ("scph5500.bin", "scph5501.bin", "scph5502.bin"):
            shutil.copyfile(bios, folder / name)
        disc_path = runtime / modes[0] / "disc.raw2352"
        if '"' in str(disc_path) or "\n" in str(disc_path):
            raise ValueError("The runtime path cannot contain quotes or newlines for CUE capture.")
        cue = folder / "disc.cue"
        cue.write_text(f'FILE "{disc_path}" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n', encoding="utf-8")
        output = folder / "startup.gtm"
        capture(core, folder, cue, output, Image)
        for mode in modes:
            hd = runtime / mode / "hd"
            hd.mkdir(exist_ok=True)
            with Disc(runtime / mode / "disc.raw2352") as disc:
                (hd / "profile.txt").write_text(disc.profile, encoding="ascii")
            shutil.copyfile(output, hd / "startup.gtm.new")
            (hd / "startup.gtm.new").replace(hd / "startup.gtm")
        output.replace(runtime / "startup.gtm")
