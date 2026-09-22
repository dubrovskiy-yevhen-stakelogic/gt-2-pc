"""Regression cases for factory-profile extraction; no game installation required."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


spec = importlib.util.spec_from_file_location(
    "wheel_audit", Path(__file__).resolve().parents[1] / "scripts/audit-wheel-inputmaps.py")
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)


def profile(bindings, name="Test racing wheel", pidvid="12345678", **extra):
    return audit.normalize(pidvid + ".json", {
        "name": name, "vidpid": pidvid, "bindings": bindings, **extra})


def axis(action, control, inverted=False):
    return {"action": action, "control": control, "isInverted": inverted}


class InputMapAuditTests(unittest.TestCase):
    def test_base_and_usb_pedals_have_distinct_layouts(self):
        base = profile([axis("steering", "xaxis"), axis("accelerate", "yaxis", True),
                        axis("brake", "rzaxis", True), axis("clutch", "slider", True)])
        usb = profile([axis("accelerate", "xaxis"), axis("brake", "yaxis"),
                       axis("clutch", "zaxis")], name="USB pedals")
        self.assertEqual(base["roles"], ["wheel"])
        self.assertEqual(usb["roles"], ["pedals"])
        self.assertEqual([(b["index"], b["rest"], b["end"]) for b in base["bindings"]],
                         [(0, 0, -32768), (1, 32767, -32768),
                          (5, 32767, -32768), (6, 32767, -32768)])
        self.assertEqual([(b["index"], b["rest"], b["end"]) for b in usb["bindings"]],
                         [(0, -32768, 32767), (1, -32768, 32767), (2, -32768, 32767)])

    def test_combined_pedals_have_opposite_halves(self):
        for inverted in (False, True):
            r = profile([axis("steering", "xaxis"), axis("accelerate_brake", "yaxis", inverted)])
            throttle, brake = r["bindings"][1:]
            self.assertEqual(throttle["rest"], 0)
            self.assertEqual(brake["rest"], 0)
            self.assertLess(throttle["end"] * brake["end"], 0)
            self.assertEqual(throttle["end"], -32768 if inverted else 32767)
            self.assertTrue(r["driving_axes_present"])

    def test_inverted_steering_and_second_slider(self):
        r = profile([axis("steering", "xaxis", True), axis("clutch", "slider2", True)])
        self.assertEqual(r["bindings"][0]["end"], 32767)
        self.assertEqual(r["bindings"][0]["right"], -32768)
        self.assertEqual(r["bindings"][1]["index"], 7)

    def test_no_repair_of_swapped_shifter_fields(self):
        r = profile([{"control": "gear1", "action": "button1"}], name="USB shifter")
        self.assertEqual(r["status"], "quarantined")
        self.assertIn("swapped_action_control", r["issues"])
        self.assertFalse(r["bindings"])

    def test_conflicting_identifiers_are_quarantined(self):
        r = profile([axis("steering", "xaxis")], guid="{99995678-0000-0000-0000-504944564944}")
        self.assertEqual(r["status"], "quarantined")
        r = audit.normalize("12345678.json", {"vidpid": "99995678", "bindings": [axis("steering", "xaxis")]})
        self.assertEqual(r["status"], "quarantined")

    def test_undefined_slider_is_not_guessed(self):
        r = profile([axis("steering", "xaxis"), axis("clutch", "slider0", True)])
        self.assertIn("unsupported_control:slider0", r["issues"])
        self.assertNotIn("clutch", [b["action"] for b in r["bindings"]])

    def test_menu_aliases_deduplicate_without_losing_alternatives(self):
        r = profile([axis("steering", "xaxis"),
                     {"control": "button1", "action": "accept"},
                     {"control": "button1", "action": "menu_item_select"},
                     {"control": "button7", "action": "accept"},
                     {"control": "upov", "action": "menu_item_up"}])
        confirm = [b["index"] for b in r["bindings"] if b["action"] == "confirm"]
        self.assertEqual(confirm, [1, 7])
        self.assertEqual(r["bindings"][-1]["index"], 128)
        self.assertIn("back", r["missing_menu_actions"])
        self.assertNotIn("confirm", r["missing_menu_actions"])

    def test_gamepad_shaped_shifter_is_not_excluded(self):
        r = profile([{"action": "gear1", "control": "button0"}], name="SHH Shifter Newt", devicetype="gamepad")
        self.assertEqual(r["roles"], ["shifter"])
        self.assertEqual(r["status"], "candidate")
        r = profile([axis("steering", "xaxis")], name="DualSense wireless controller")
        self.assertEqual(r["status"], "non_racing_device")

    def test_generic_windows_name_does_not_hide_a_wheel(self):
        r = profile([axis("steering", "xaxis"), axis("accelerate", "button2")],
                    name="USB Gamepad", displayName="Hama Thunder VS Racing Wheel")
        self.assertEqual(r["status"], "candidate")
        self.assertEqual(r["roles"], ["wheel"])
        self.assertIn("digital_driving_axis:accelerate", r["issues"])
        self.assertFalse(r["driving_axes_present"])

    def test_rim_specific_and_metadata_only_are_visible(self):
        r = profile([axis("steering", "xaxis")], notes="Requires the CS V2 rim.")
        self.assertIn("rim_specific_bindings", r["issues"])
        r = profile([], ffbSupported=True)
        self.assertEqual(r["status"], "metadata_only")

    def test_ffb_curves_and_game_code_are_not_exported(self):
        binding = {**axis("steering", "xaxis"), "ffb": {"forceCoef": 9999},
                   "isForceInverted": True, "onChange": "foreign code"}
        r = profile([binding, {"control": "button1", "action": "reset_physics"}])
        text = json.dumps(r)
        for unexpected in ("forceCoef", "onChange", "foreign code", "reset_physics", "isForceInverted"):
            self.assertNotIn(unexpected, text)

    def test_deterministic_read_only_scan_and_json_errors(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "12345678.json").write_text('{"vidpid":"12345678","name":"USB pedals","bindings":[]}', encoding="utf-8")
            (root / "12345679.json").write_text('{broken', encoding="utf-8")
            (root / "keyboard.json").write_text('{"bindings":[],}', encoding="utf-8")
            before = {p.name: p.read_bytes() for p in root.iterdir()}
            first = audit.audit(root)
            self.assertEqual(first, audit.audit(root))
            self.assertEqual(first["summary"]["statuses"],
                             {"metadata_only": 1, "parse_error": 1, "non_usb_filename": 1})
            self.assertEqual(before, {p.name: p.read_bytes() for p in root.iterdir()})


if __name__ == "__main__":
    unittest.main()
