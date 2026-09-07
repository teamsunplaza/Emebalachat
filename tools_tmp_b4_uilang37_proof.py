#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""B-4 offline proof for the uilang_37 scenario logic (req027_e2e.py).

Creative validation per the code-excellence rule: instead of an interactive
UIA run (needs the user's desktop + a live tray menu), the three verdict
paths of scenario_uilang_37 are exercised with a monkeypatched enumerator:

  A) refusal  -> INCONCLUSIVE (note_env, never FAIL)  [UIA-starve bound]
  B) correct 38-item dump (with root-pane noise + an RLM-bearing name and
     padded names to also prove _norm) -> PASS
  C) WRONG dump (one endonym missing, one out of order) -> FAIL

Exits 0 only if all three match expectations.
"""
import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "tools", "e2e"))
import req027_e2e as H


class FakeProc:
    pid = 4242
    def poll(self): return None


class FakeApp:
    def __init__(self):
        self.proc = FakeProc()
        self.exe_path = os.path.join(H.REPO_ROOT, "build", "Emebala_chat.exe")
        self.log_path = None
    def log_text(self): return ""


class FakeState(dict):
    pass


def good_dump():
    # 38 endonyms in the §2-Q3 order + the two root-pane noise items a live
    # dump also contains (status toggle + the submenu title itself), and
    # deliberately dirty names (padding + one RLM) to prove _norm.
    names = [H.UILANG_EXPECTED_ORDER[0], "상태: 활성", "  " + H.UILANG_EXPECTED_ORDER[1],
             H.UILANG_EXPECTED_ORDER[2], "\u200f" + H.UILANG_EXPECTED_ORDER[3],
             H.UILANG_EXPECTED_ORDER[4], H.UILANG_EXPECTED_ORDER[5],
             "인터페이스 언어"]
    names += list(H.UILANG_EXPECTED_ORDER[6:])
    names.insert(7, "자동 (시스템 언어)")  # auto entry right after the title
    return names


def bad_dump():
    names = good_dump()
    names = [n for n in names if n != "العربية"]           # missing endonym
    names[3] = "Suomi"                                     # order break
    return names


def run_case(label, patch_names, expect_verdict):
    orig_enum = H.tray_enumerate_uilang
    orig_cfg = H._config_path
    if patch_names is None:
        H.tray_enumerate_uilang = lambda: (None, "NOTFOUND:menu-items-uia-empty(test)")
    else:
        H.tray_enumerate_uilang = lambda: (list(patch_names), "ENUMDONE:%d(test)" % len(patch_names))
    H._config_path = lambda app: os.devnull  # force readback-skip path
    try:
        app = FakeApp()
        state = {"app": app}
        rep, _ret = H.scenario_uilang_37(state, 0)
        verdict = rep.verdict()[0]
        ok = verdict == expect_verdict
        print(f"[{'OK ' if ok else 'BAD'}] {label}: verdict={verdict} (expected {expect_verdict})")
        if not ok:
            text, _ = rep.render()
            print(text)
        return ok
    finally:
        H.tray_enumerate_uilang = orig_enum
        H._config_path = orig_cfg


def main():
    results = [
        run_case("A refusal -> INCONCLUSIVE", None, "INCONCLUSIVE"),
        run_case("B correct dump -> PASS", good_dump(), "PASS"),
        run_case("C wrong dump -> FAIL", bad_dump(), "FAIL"),
    ]
    print("PROOF", "PASS" if all(results) else "FAIL")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.exit(main())
