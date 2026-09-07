# B-6 one-off proof for the E2E-RESET-1 harness additions (headless:
# registry wiring + config-seed/restore helpers + the Python-side §2.6
# oracle). Does NOT launch the app - the interactive click leg is covered by
# TestReq040SystemDefaults37 (unit) + user QA. Mirrors the structure of
# tools_tmp_b4_uilang37_proof.py.
import json
import os
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "tools", "e2e"))
import req027_e2e as h  # noqa: E402

ok_all = True

def check(name, cond, detail=""):
    global ok_all
    mark = "OK " if cond else "BAD"
    if not cond:
        ok_all = False
    print(f"[{mark}] {name}" + (f": {detail}" if detail else ""))

# 1) Scenario registered in both the argv choices and the driver map.
check("reset_defaults in ALL_SCENARIOS", "reset_defaults" in h.ALL_SCENARIOS)
check("reset_defaults in DRIVERS", "reset_defaults" in h.DRIVERS)

# 2) The five seeded keys are exactly the reset's coverage (4 languages + G-1).
check("RESET_KEYS == 5 reset-covered keys",
      set(h.RESET_KEYS) == {
          "ui_language", "drag_source_language", "drag_target_language",
          "type_source_language", "type_target_language"},
      str(h.RESET_KEYS))

# 3) patch/restore roundtrip on a temp file, incl. the absent-key (None)
#    deletion path, json-module (multiline-safe) as documented.
tmp = os.path.join(tempfile.gettempdir(), "emebala_b6_reset_proof.json")
try:
    base = {"ui_language": "auto", "engine_type": "google",
            "drag_target_language": "Korean"}
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(base, f)
    prev = h.patch_config_fields(tmp, h.RESET_NONDEFAULTS)
    seeded = h._read_json(tmp)
    check("patch writes all five seeded values",
          all(seeded.get(k) == v for k, v in h.RESET_NONDEFAULTS.items()),
          str(seeded))
    check("patch returns previous values (None for absent keys)",
          prev["ui_language"] == "auto" and
          prev.get("type_source_language") is None and
          prev.get("drag_source_language") is None, str(prev))
    restored = {k: prev.get(k) for k in h.RESET_KEYS}
    h.patch_config_fields(tmp, restored)
    final = h._read_json(tmp)
    check("restore rewrites values and deletes the previously-absent key",
          final["ui_language"] == "auto" and
          final["drag_target_language"] == "Korean" and
          "type_source_language" not in final, str(final))
finally:
    try:
        os.remove(tmp)
    except OSError:
        pass

# 4) Oracle returns a registry name_en for the live host (no crash, sane value)
#    and the G-2/English special-cases are encoded.
val = h.expected_system_drag_default()
check("oracle returns a concrete language name",
      isinstance(val, str) and val and val not in ("Auto Detect",), val)
src = __import__("inspect").getsource(h.expected_system_drag_default)
check("oracle encodes G-2 EN->Korean pivot", "return \"Korean\"" in src)
check("oracle encodes unsupported->English", "return \"English\"" in src)

# 5) Runtime config path mirrors GetDefaultConfigOrder (LOCALAPPDATA first
#    when present).
lad = os.environ.get("LOCALAPPDATA", "")
p = h._runtime_config_path(type("A", (), {"exe_path": os.path.join(h.REPO_ROOT, "build", "Emebala_chat.exe")})())
if lad and os.path.exists(os.path.join(lad, "Emebalachat", "config.json")):
    check("_runtime_config_path prefers LOCALAPPDATA",
          p == os.path.join(lad, "Emebalachat", "config.json"), p)
else:
    check("_runtime_config_path falls back to exe-dir",
          p == os.path.join(h.REPO_ROOT, "build", "config.json"), p)

# 6) The About-item regex matches every real menu_about string pattern
#    (brand + trailing ellipsis) and NOT the exit items.
import re
rx = re.compile(h.ABOUT_ITEM_RX)
check("About regex matches EN/TH sample", rx.search("About Emebala Chat…") and
      rx.search("เกี่ยวกับ Emebala Chat…"))
check("About regex rejects exit items", not rx.search("Exit Emebala Chat") and
      not rx.search("ออกจาก Emebala Chat"))

print("PROOF", "PASS" if ok_all else "FAIL")
sys.exit(0 if ok_all else 1)
