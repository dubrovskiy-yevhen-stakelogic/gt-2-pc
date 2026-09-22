"""Inspect locally installed factory input maps without modifying the installation.

The output is development evidence, not a tested-device/support list. Only
physical control identifiers and relevant actions are normalized. Game code,
force curves, gains, scripts and unrelated game bindings are not exported.
"""
import argparse
import collections
import hashlib
import json
from pathlib import Path
import re


AXES = {name: index for index, name in enumerate(
    ("xaxis", "yaxis", "zaxis", "rxaxis", "ryaxis", "rzaxis", "slider", "slider2"))}
POV = {"upov": 128, "rpov": 129, "dpov": 130, "lpov": 131}
AXIS_ACTIONS = {"steering": "steering", "accelerate": "throttle",
                "brake": "brake", "clutch": "clutch"}
BUTTON_ACTIONS = {
    "shiftUp": "shift_up", "shiftDown": "shift_down", "gearR": "reverse",
    **{f"gear{i}": f"gear_{i}" for i in range(1, 8)},
    "parkingbrake": "handbrake", "parkingbrake_temporary": "handbrake",
    "switch_camera_next": "camera", "look_back": "look_back",
    "toggleMenues": "menu", "menu_item_select": "confirm", "accept": "confirm",
    "menu_item_back": "back", "decline": "back",
    **{f"menu_item_{d}": d for d in ("up", "right", "down", "left")},
}
RELEVANT = set(AXIS_ACTIONS) | set(BUTTON_ACTIONS) | {"accelerate_brake"}
MENU = {"menu", "confirm", "back", "up", "right", "down", "left"}


def physical(control):
    if control in AXES:
        return {"kind": "axis", "index": AXES[control]}
    if control in POV:
        return {"kind": "button", "index": POV[control]}
    if isinstance(control, str) and re.fullmatch(r"button\d+", control):
        index = int(control[6:])
        if index < 128:
            return {"kind": "button", "index": index}
    return None


def normalize(filename, data):
    """Never repair identifiers, swapped fields or undocumented controls by guessing."""
    name = data.get("displayName") or data.get("name") or filename
    record = {"file": filename, "name": name, "bindings": [], "issues": []}
    issues = record["issues"]
    stem = Path(filename).stem.lower()
    if not re.fullmatch(r"[0-9a-f]{8}", stem):
        record["status"] = "non_usb_filename"
        return record
    pidvid = str(data.get("vidpid", "")).lower()
    if pidvid != stem:
        issues.append("filename_vidpid_mismatch")
    guid = data.get("guid", "")
    if guid and guid.lstrip("{").split("-")[0].lower() != stem:
        issues.append("guid_vidpid_mismatch")
    record["vendor"] = stem[4:]
    record["product"] = stem[:4]
    record["device_type"] = data.get("devicetype", "unspecified")
    label = name.lower()
    # Device type alone is insufficient: the SHH shifter reports as a gamepad.
    non_racing = any(term in label for term in (
        "gamepad", "dualshock", "dualsense", "access controller", "flightstick",
        "r/c", "spacemouse", "dual psx", "dual trigger", "sabertooth"))
    if non_racing:
        record["status"] = "non_racing_device"
        return record
    if not data.get("bindings"):
        record["status"] = "metadata_only"
        return record
    if data.get("notes"):
        record["has_source_notes"] = True
        if "rim" in data["notes"].lower():
            issues.append("rim_specific_bindings")
    bindings = record["bindings"]
    for b in data.get("bindings", []):
        action, control = b.get("action"), b.get("control")
        if control in RELEVANT and physical(action):
            issues.append("swapped_action_control")
            continue
        if action not in RELEVANT:
            continue
        p = physical(control)
        if p is None:
            issues.append(f"unsupported_control:{control}")
            continue
        inverted = b.get("isInverted", False)
        if not isinstance(inverted, bool):
            issues.append(f"invalid_inversion:{control}")
            continue
        result = {**p, "source_action": action, "source_control": control}
        if action in AXIS_ACTIONS or action == "accelerate_brake":
            if p["kind"] != "axis":
                issues.append(f"digital_driving_axis:{action}")
                continue
            if action == "accelerate_brake":
                for target, sign in (("throttle", 1), ("brake", -1)):
                    end = 32767 if sign * (-1 if inverted else 1) > 0 else -32768
                    bindings.append({**result, "action": target, "rest": 0, "end": end})
                continue
            result["action"] = AXIS_ACTIONS[action]
            if action == "steering":
                result.update(rest=0, end=32767 if inverted else -32768,
                              right=-32768 if inverted else 32767)
            else:
                result.update(rest=32767 if inverted else -32768,
                              end=-32768 if inverted else 32767)
        else:
            result["action"] = BUTTON_ACTIONS[action]
            if p["kind"] == "axis":
                if result["action"] != "handbrake":
                    issues.append(f"analog_button_action:{action}")
                    continue
                result.update(rest=32767 if inverted else -32768,
                              end=-32768 if inverted else 32767)
        # Preserve multiple physical bindings for the same logical action.
        semantic = {k: v for k, v in result.items() if not k.startswith("source_")}
        if not any({k: v for k, v in x.items() if not k.startswith("source_")} == semantic
                   for x in bindings):
            bindings.append(result)
    issues[:] = sorted(set(issues))
    actions = {b["action"] for b in bindings}
    roles = []
    if "steering" in actions:
        roles.append("wheel")
    elif {"throttle", "brake"} <= actions:
        roles.append("pedals")
    if "steering" not in actions and ("gear_1" in actions or {"shift_up", "shift_down"} <= actions):
        roles.append("shifter")
    if "steering" not in actions and "handbrake" in actions:
        roles.append("handbrake")
    record["roles"] = roles
    record["missing_menu_actions"] = sorted(MENU - actions) if "wheel" in roles else []
    record["driving_axes_present"] = {"steering", "throttle", "brake"} <= actions
    fatal = {"filename_vidpid_mismatch", "guid_vidpid_mismatch", "swapped_action_control"}
    record["status"] = "quarantined" if fatal.intersection(issues) else "candidate" if roles else "no_racing_controls"
    return record


def audit(folder):
    records = []
    manifest = []
    for path in sorted(folder.glob("*.json")):
        raw = path.read_bytes()
        sha = hashlib.sha256(raw).hexdigest()
        manifest.append({"file": path.name, "bytes": len(raw), "sha256": sha})
        # Generic keyboard/pad input maps may be JSONC. They are not USB profiles.
        if not re.fullmatch(r"[0-9a-fA-F]{8}", path.stem):
            records.append({"file": path.name, "status": "non_usb_filename"})
            continue
        try:
            data = json.loads(raw.decode("utf-8-sig"))
            record = normalize(path.name, data)
        except (UnicodeError, ValueError, TypeError, AttributeError) as error:
            record = {"file": path.name, "status": "parse_error", "error": str(error)}
        record["sha256"] = sha
        records.append(record)
    statuses = dict(collections.Counter(r["status"] for r in records))
    candidates = [r for r in records if r["status"] == "candidate"]
    summary = {"files": len(manifest), "bytes": sum(m["bytes"] for m in manifest),
               "statuses": statuses,
               "candidate_roles": dict(collections.Counter(role for r in candidates for role in r["roles"])),
               "candidate_full_driving_axes": sum(r["driving_axes_present"] for r in candidates),
               "candidate_complete_menu": sum("wheel" in r["roles"] and not r["missing_menu_actions"] for r in candidates)}
    return {"schema": 1, "summary": summary, "records": records, "source_manifest": manifest}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--inputmaps", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    source = args.inputmaps.resolve(strict=True)
    target = args.output.resolve()
    if source == target or source in target.parents:
        parser.error("Output must be outside the source installation's inputmaps directory")
    result = audit(source)
    if not result["summary"]["files"]:
        parser.error("No input maps found")
    target.mkdir(parents=True, exist_ok=True)
    (target / "beamng-audit.json").write_text(json.dumps(result, indent=2, ensure_ascii=False) + "\n", encoding="utf-8", newline="\n")
    candidates = [r for r in result["records"] if r["status"] == "candidate"]
    # A compact, private intermediate catalogue. Runtime activation is a separate step.
    pack = {"schema": 1, "hardware_tested": False, "profiles": candidates}
    packed = json.dumps(pack, ensure_ascii=False, separators=(",", ":")) + "\n"
    (target / "beamng-candidates.json").write_text(packed, encoding="utf-8", newline="\n")
    print(json.dumps(result["summary"], indent=2))
    print(f"Candidate catalogue: {len(packed.encode('utf-8'))} bytes")


if __name__ == "__main__":
    main()
