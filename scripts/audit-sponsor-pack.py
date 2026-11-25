#!/usr/bin/env python3
"""Create an isolated marker pack for gt2sponsorpackaudit (requires Pillow)."""
import argparse
import io
import json
from pathlib import Path, PurePosixPath
import zipfile
from PIL import Image


def prepare(pack, output):
    output.mkdir(parents=True, exist_ok=False)
    inventory = []
    with zipfile.ZipFile(pack) as archive:
        for name in archive.namelist():
            parts = PurePosixPath(name).parts
            if len(parts) < 4 or parts[0] != "textures" or not name.endswith(".png"):
                continue
            if parts[1] not in ("arcade", "simulation") or ".." in parts:
                raise ValueError("Unexpected texture path: " + name)
            relative = Path(*parts[2:])
            dest = output / relative
            data = archive.read(name)
            with Image.open(io.BytesIO(data)) as image:
                image.load()
                if not (1 <= image.width <= 1024 and 1 <= image.height <= 1024):
                    raise ValueError("Texture dimensions exceed budget: " + name)
                if parts[2] == "_sponsors":
                    marker = io.BytesIO()
                    Image.new("RGBA", image.size, (0, 255, 0, 255)).save(marker, format="PNG")
                    data = marker.getvalue()
                    inventory.append({"file": relative.as_posix(), "size": list(image.size)})
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(data)
    if not inventory:
        raise ValueError("Pack contains no sponsor PNGs")
    (output / "sponsor-inventory.json").write_text(json.dumps(inventory, indent=2))
    print(f"Prepared {len(inventory)} sponsor markers in {output}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pack", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    prepare(args.pack, args.output)
