"""Compile audited physical input facts into GT2's offline wheel catalogue."""
import argparse
import json
import hashlib
from pathlib import Path

AXIS = {"steering": 0, "throttle": 1, "brake": 2, "clutch": 3}
BUTTON = {"shift_up": 0, "shift_down": 1, "reverse": 2, "handbrake": 3,
          "camera": 4, "look_back": 5, "menu": 6,
          **{f"gear_{i}": 6 + i for i in range(1, 8)}}
NAV = {"menu": 3, "up": 4, "right": 5, "down": 6, "left": 7,
       "shift_down": 10, "shift_up": 11, "back": 12, "confirm": 14}


def compile_catalog(catalog):
    rows = ["// Generated physical control facts. See docs/WHEEL-PROFILES.md.\n",
            "static const std::vector<Profile> builtInProfiles = {\n"]
    provenance = []
    for p in catalog["profiles"]:
        if p["status"] != "candidate":
            raise ValueError("Only audited candidates may be compiled")
        axes, buttons, nav = {}, {}, {}
        for b in p["bindings"]:
            action = b["action"]
            if b["kind"] == "axis" and action in AXIS:
                target = AXIS[action]
                value = (target, b["index"], b["rest"], b["end"], b.get("right", 32767))
                if target in axes and axes[target] != value:
                    raise ValueError(f"Conflicting axis in {p['file']}")
                axes[target] = value
            if b["kind"] == "button":
                # Settings have one binding per action. Keep the first documented
                # alternative, never infer missing buttons from a brand/layout.
                if action in BUTTON:
                    buttons.setdefault(BUTTON[action], b["index"])
                if action in NAV:
                    nav.setdefault(NAV[action], b["index"])
        if not axes and not buttons:
            continue
        def array(items):
            return "{" + ",".join("{" + ",".join(map(str, item)) + "}" for item in items) + "}"
        name = json.dumps(p["name"], ensure_ascii=True)
        rim = "true" if "rim_specific_bindings" in p["issues"] else "false"
        rows.append(f"    {{0x{p['vendor']},0x{p['product']},{name},{rim},"
                    f"{array(axes.values())},{array(buttons.items())},{array(nav.items())}}},\n")
        provenance.append({k: p[k] for k in ("file", "sha256", "vendor", "product", "name", "issues")})
    rows.append("};\n")
    text = "".join(rows)
    revision = hashlib.sha256(text.encode("utf-8")).hexdigest()[:16]
    return f'static constexpr const char* builtInProfileRevision = "{revision}";\n' + text, provenance


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("catalog", type=Path)
    parser.add_argument("--header", type=Path, required=True)
    parser.add_argument("--provenance", type=Path, required=True)
    args = parser.parse_args()
    text, provenance = compile_catalog(json.loads(args.catalog.read_text(encoding="utf-8")))
    args.header.write_text(text, encoding="utf-8", newline="\n")
    args.provenance.write_text(json.dumps({"schema": 1, "reference": "BeamNG factory inputmaps, Steam build 24617469",
                                         "profiles": provenance}, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(f"Compiled {len(provenance)} device records; header {len(text.encode('utf-8'))} bytes")


if __name__ == "__main__":
    main()
