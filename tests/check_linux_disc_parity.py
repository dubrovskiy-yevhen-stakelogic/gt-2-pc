"""Compare portable extraction against a fresh gt2install extract output.

Usage: python3 tests/check_linux_disc_parity.py /path/to/reference-disc-directory [...]
The reference directories must contain disc.raw2352 and untouched assets/.
"""
import argparse
import hashlib
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from gt2_disc import Disc, digest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference", type=Path, nargs="+")
    args = parser.parse_args()
    total = 0
    for root in args.reference:
        with Disc(root / "disc.raw2352") as disc:
            expected_paths = {entry[0] for entry in disc.files}
            actual_paths = {p.relative_to(root / "assets").as_posix() for p in (root / "assets").rglob("*") if p.is_file()}
            if actual_paths != expected_paths:
                raise ValueError("Asset path set differs: " + str(root))
            for entry in disc.files:
                actual = hashlib.sha256(disc.asset(entry)).hexdigest()
                if actual != digest(root / "assets" / entry[0]):
                    raise ValueError("Asset bytes differ: " + str(root / entry[0]))
            total += len(disc.files)
            print(f"PASS {disc.mode} {disc.profile}: {len(disc.files)} identical assets", flush=True)
    print(f"PASS: {total} assets")


if __name__ == "__main__":
    main()
