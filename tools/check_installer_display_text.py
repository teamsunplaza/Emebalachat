#!/usr/bin/env python3
"""Installer DISPLAY-text gate (session 260911_0001, REQ-006 permanent gate).

Why this gate exists
--------------------
The static gate (tools/check_installer_encoding.py) audits compile *inputs*.
The 0.10.0 mojibake bug lived in the *display* layer: CreateOutputMsgMemoPage
hands its string to TRichEditViewer; when given PLAIN text the viewer's
internal plain->RTF conversion writes non-ASCII as ANSI hex escapes under the
language code page (CP949 for Korean), so AboutBody/GuideBody rendered as
CP949-misdecode-of-UTF-8 in every language containing non-ASCII characters.
See docs/260911_0001_session_installer-mojibake-regression/
115200_debug-rootcause-installer-mojibake.md for the byte-level proof.

What it does
------------
1. Extracts the REAL [Languages] + [CustomMessages] sections and the actual
   ToRtf()/MessageLines() functions from installer/setup.iss (verbatim code
   transplant) and compiles a lowest-privilege, payload-free probe wizard
   carrying the same two memo pages as the product installer (About, Guide).
2. For each /LANG in {korean, japanese, chinesesimplified, chinesetraditional,
   english}: launches the probe wizard and dumps every TRichEditViewer child
   via WM_GETTEXT. TRichEditViewer parses its RTF at assignment time inside
   InitializeWizard, so both memo viewers are fully populated (and the
   display conversion is exercised) without needing page navigation — this is
   exactly the read-back mechanism the debug session used to reproduce the
   bug and validate the fix (tools_tmp_mb17b_dump.py / tools_tmp_mb19_rtf.py).
   Assertions:
     - both memo pages materialise (2 viewers, distinct content);
     - About-body and Guide-body clean tokens are present, assigned to the
       right viewer (about tokens in the viewer WITHOUT guide tokens, etc.);
     - no mojibake signature: U+FFFD, nor the CP949-misdecode-of-UTF-8 form
       of each body's own non-ASCII seed (the exact bug output);
     - no raw RTF control words leaked into the rendered text.
   A modal #32770 error dialog (raised InitializeWizard) fails the gate.
3. Exit code 0 = PASS, non-zero = FAIL (fails closed; run before shipping
   alongside tools/check_installer_encoding.py).

Project rule enforced (installer/README.md):
  - About/Guide memo bodies MUST be produced by the RTF-safe MessageLines();
    plain non-ASCII text must never reach a memo page.

Usage:
  python tools/check_installer_display_text.py [--langs korean,english]
                                               [--inno "C:\\path\\to\\Inno Setup 6"]
                                               [--keep]  (keep the probe dir)
"""
from __future__ import annotations

import argparse
import ctypes
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import time
from ctypes import wintypes

WS = pathlib.Path(__file__).resolve().parent.parent
ISS = WS / "installer" / "setup.iss"

user32 = ctypes.windll.user32
kernel32 = ctypes.windll.kernel32
WNDENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

WM_GETTEXT = 0x000D
WM_GETTEXTLENGTH = 0x000E
WM_CLOSE = 0x0010
PROCESS_TERMINATE = 0x0001

PROBE_APP_NAME = "InstDispGate"  # appears in the wizard title 'Setup - <name>'

LANGS_DEFAULT = ["korean", "japanese", "chinesesimplified", "chinesetraditional",
                 "english"]

# Clean tokens expected in each memo body (verbatim prefixes from
# installer/setup.iss [CustomMessages]; %n = Inno line break, not matched here).
EXPECTED_ABOUT = {
    "korean": ["에메발라 챗 — 텍스트를 입력하는", "핵심 기능", "모든 텍스트 상자"],
    "japanese": ["エメバラチャット — テキストを入力する", "主な機能"],
    "chinesesimplified": ["埃梅巴拉 翻译——在您输入的任何地方", "主要功能"],
    "chinesetraditional": ["埃梅巴拉 翻譯——在您輸入的任何地方", "主要功能"],
    "english": ["Emebala Chat \u2014 real-time AI translation", "Key features:",
                "\u2022"],
}
EXPECTED_GUIDE = {
    "korean": ["단축키 (config.json에서 변경 가능)", "번역 켜기/끄기", "\u2014"],
    "japanese": ["ホットキー（config.jsonで変更可能）", "翻訳のオン/オフ", "\u2014"],
    "chinesesimplified": ["快捷键（可在 config.json 中配置）", "开启/关闭翻译", "\u2014"],
    "chinesetraditional": ["快捷鍵（可在 config.json 中設定）", "開啟/關閉翻譯", "\u2014"],
    "english": ["Hotkeys (configurable in config.json)", "F9 \u2014", "\u2014"],
}

# Populated by load_bodies() from the real [CustomMessages] at run time.
BODIES: dict[str, dict[str, str]] = {}


def find_inno_dir(override: str | None) -> pathlib.Path | None:
    if override:
        p = pathlib.Path(override)
        return p if p.exists() else None
    candidates = [
        pathlib.Path(r"C:\Users\k1yt\AppData\Local\Programs\Inno Setup 6"),
        pathlib.Path(r"C:\Program Files (x86)\Inno Setup 6"),
        pathlib.Path(r"C:\Program Files\Inno Setup 6"),
    ]
    try:
        import winreg

        for hive in (winreg.HKEY_LOCAL_MACHINE, winreg.HKEY_CURRENT_USER):
            for view in (getattr(winreg, "KEY_WOW64_64KEY", 0), 0):
                try:
                    key = winreg.OpenKey(
                        hive,
                        r"Software\Microsoft\Windows\CurrentVersion\Uninstall\Inno Setup 6_is1",
                        0, winreg.KEY_READ | view,
                    )
                    loc = winreg.QueryValueEx(key, "InstallLocation")[0]
                    if loc and pathlib.Path(loc).exists():
                        return pathlib.Path(loc)
                except OSError:
                    pass
    except ImportError:
        pass
    for c in candidates:
        if c.exists():
            return c
    return None


def extract(raw: str, name: str) -> str:
    m = re.search(rf"(?ms)^\[{re.escape(name)}\]\s*\n(.*?)(?=^\[|\Z)", raw)
    if not m:
        raise SystemExit(f"SETUP-ISS-PARSE-ERROR: section [{name}] not found")
    return m.group(1)


def extract_func(raw: str, signature: str) -> str:
    """Grab a top-level Pascal function verbatim (to its own 'end;')."""
    m = re.search(rf"(?ms)^{re.escape(signature)}.*?^end;$", raw)
    if not m:
        raise SystemExit(
            f"SETUP-ISS-PARSE-ERROR: function '{signature}' not found in "
            f"{ISS.name}. The About/Guide memo pages require the RTF-safe "
            "MessageLines() (see installer/README.md); gate refuses to run "
            "against an installer without it."
        )
    return m.group(0)


def load_bodies(raw: str) -> None:
    """Store AboutBody/GuideBody per probed language for mojibake seeding."""
    cm = extract(raw, "CustomMessages")
    entries: dict[str, str] = {}
    cur: str | None = None
    for line in cm.splitlines():
        m = re.match(r"(?i)^([a-z]+)\.([A-Za-z][A-Za-z0-9]*)=(.*)$", line)
        if m:
            cur = f"{m.group(1).lower()}.{m.group(2)}"
            entries[cur] = m.group(3)
        elif line.lstrip().startswith((">", "\"")) and cur:  # rare continuations
            entries[cur] += line.strip()
    for lang in LANGS_DEFAULT:
        about = entries.get(f"{lang}.AboutBody")
        guide = entries.get(f"{lang}.GuideBody")
        if not about or not guide:
            raise SystemExit(
                f"SETUP-ISS-PARSE-ERROR: AboutBody/GuideBody missing for {lang}"
            )
        BODIES[lang] = {"about": about, "guide": guide}


def mojibake_markers(lang: str) -> list[str]:
    """The bug's own output: body non-ASCII seeds re-encoded UTF-8 -> CP949.
    Only non-ASCII fragments are used (an ASCII seed cannot garble this way)."""
    markers = ["\ufffd"]
    for page in ("about", "guide"):
        body = BODIES[lang][page].replace("%n", "")
        # all maximal non-ASCII runs of >= 3 chars, first 2 runs per body
        runs = re.findall(r"[^\x00-\x7f]{3,}", body)
        for run in runs[:2]:
            markers.append(run[:8].encode("utf-8").decode("cp949", "replace"))
    return [m for m in markers if m and m != "\ufffd" or m == "\ufffd"]


def build_probe(inno_dir: pathlib.Path, workdir: pathlib.Path) -> pathlib.Path:
    raw = ISS.read_bytes().decode("utf-8-sig")
    load_bodies(raw)
    languages = extract(raw, "Languages").replace(
        '"languages\\', f'"{WS}\\installer\\languages\\'
    )
    custom = extract(raw, "CustomMessages")
    to_rtf = extract_func(raw, "function ToRtf(const S: String): String;")
    msg_lines = extract_func(raw, "function MessageLines(const MsgName: String): String;")

    code = (
        "[Code]\n"
        + to_rtf
        + "\n\n"
        + msg_lines
        + "\n\nvar\n  AboutPage: TWizardPage;\n\n"
        "procedure InitializeWizard();\n"
        "begin\n"
        "  AboutPage := CreateOutputMsgMemoPage(wpWelcome,\n"
        "    CustomMessage('AboutTitle'), '', '', MessageLines('AboutBody'));\n"
        "  CreateOutputMsgMemoPage(AboutPage.ID,\n"
        "    CustomMessage('GuideTitle'), '', '', MessageLines('GuideBody'));\n"
        "end;\n"
    )
    script = (
        "[Setup]\n"
        f"AppName={PROBE_APP_NAME}\n"
        "AppVersion=1.0\n"
        "DefaultDirName={localappdata}\\Programs\\installer_display_gate\n"
        "PrivilegesRequired=lowest\n"
        "DisableProgramGroupPage=yes\n"
        "OutputDir=" + str(workdir / "out").replace("\\", "\\\\") + "\n"
        "OutputBaseFilename=dgate\n"
        "Compression=none\n"
        "Uninstallable=no\n"
        "CreateAppDir=no\n"
        "\n[Languages]\n" + languages + "\n[CustomMessages]\n" + custom + "\n" + code
    )
    iss = workdir / "dgate.iss"
    iss.write_bytes(b"\xef\xbb\xbf" + script.encode("utf-8"))
    r = subprocess.run(
        [str(inno_dir / "ISCC.exe"), str(iss)], capture_output=True, text=True,
        encoding="utf-8", errors="replace", timeout=900,
    )
    exe = workdir / "out" / "dgate.exe"
    if r.returncode != 0 or not exe.exists():
        print(r.stdout[-2000:])
        raise SystemExit("PROBE-COMPILE-FAIL: see ISCC output above")
    return exe


# --------------------------------------------------------------------- win32
def get_class(hwnd) -> str:
    b = ctypes.create_unicode_buffer(256)
    user32.GetClassNameW(hwnd, b, 256)
    return b.value


def window_pid(hwnd) -> int:
    pid = wintypes.DWORD()
    user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
    return pid.value


def window_title(hwnd) -> str:
    b = ctypes.create_unicode_buffer(512)
    user32.GetWindowTextW(hwnd, b, 512)
    return b.value


def get_text(hwnd) -> str:
    n = user32.SendMessageW(hwnd, WM_GETTEXTLENGTH, 0, 0)
    if n > 0:
        buf = ctypes.create_unicode_buffer(n + 2)
        user32.SendMessageW(hwnd, WM_GETTEXT, n + 2, buf)
        return buf.value
    return window_title(hwnd)


def probe_wizards() -> list[tuple[int, int]]:
    """(hwnd, pid) of TWizardForm windows belonging to OUR probe (title match)."""
    found: list[tuple[int, int]] = []

    def cb(h, lp):
        if get_class(h) == "TWizardForm" and PROBE_APP_NAME in window_title(h):
            found.append((h, window_pid(h)))
        return True

    user32.EnumWindows(WNDENUMPROC(cb), 0)
    return found


def error_dialogs(pids: set[int]) -> list[int]:
    found: list[int] = []

    def cb(h, lp):
        if get_class(h) == "#32770" and window_pid(h) in pids \
                and user32.IsWindowVisible(h):
            found.append(h)
        return True

    user32.EnumWindows(WNDENUMPROC(cb), 0)
    return found


def memo_viewer_texts(wnd) -> list[str]:
    out: list[str] = []

    def cb(h, lp):
        if get_class(h) == "TRichEditViewer":
            out.append(get_text(h))
        return True

    user32.EnumChildWindows(wnd, WNDENUMPROC(cb), 0)
    return out


def kill_pid(pid: int) -> None:
    h = kernel32.OpenProcess(PROCESS_TERMINATE, False, pid)
    if h:
        kernel32.TerminateProcess(h, 1)
        kernel32.CloseHandle(h)


# ------------------------------------------------------------------ one lang
def run_language(exe: pathlib.Path, lang: str, failures: list[str]) -> None:
    logf = exe.parent / f"dgate_{lang}.log"
    if logf.exists():
        logf.unlink()
    proc = subprocess.Popen([str(exe), f"/LANG={lang}", f"/LOG={logf}"])
    tracked_pids = {proc.pid}
    tmp_dirs: list[pathlib.Path] = []
    try:
        # wait for the wizard window (the extracted temp exe owns it; pid differs
        # from proc.pid) or an early process exit
        wnd = None
        deadline = time.time() + 25
        while time.time() < deadline and wnd is None:
            time.sleep(0.5)
            wins = probe_wizards()
            if wins:
                wnd, wpid = wins[0]
                tracked_pids.add(wpid)
            elif proc.poll() is not None:
                break
        if wnd is None:
            failures.append(f"{lang}: probe wizard never appeared")
            return
        # wait until both memo viewers exist (InitializeWizard finished)
        viewers: list[str] = []
        deadline = time.time() + 15
        while time.time() < deadline and len(viewers) < 2:
            errs = error_dialogs(tracked_pids)
            if errs:
                msg = get_text(errs[0]) or "modal error dialog"
                failures.append(f"{lang}: setup engine raised an error: {msg[:120]}")
                return
            viewers = [v for v in memo_viewer_texts(wnd) if v.strip()]
            if len(viewers) < 2:
                time.sleep(0.5)

        print(f"--- {lang} --- ({len(viewers)} memo viewers)")
        if len(viewers) < 2:
            failures.append(
                f"{lang}: only {len(viewers)} populated memo viewer(s); both "
                "About and Guide pages must materialise")

        def assign(tokens: list[str]) -> str | None:
            for v in viewers:
                if all(t in v for t in tokens):
                    return v
            return None

        about_v = assign(EXPECTED_ABOUT[lang])
        guide_v = assign(EXPECTED_GUIDE[lang])
        for page, toks, got in (("about", EXPECTED_ABOUT[lang], about_v),
                                ("guide", EXPECTED_GUIDE[lang], guide_v)):
            for tok in toks:
                # token must be in the viewer assigned to this page
                hit = got is not None and tok in got
                print(f"  [{'PASS' if hit else 'FAIL'}] {page} memo contains {tok!r}")
                if not hit:
                    failures.append(f"{lang}/{page}: missing token {tok!r}")
                    for i, v in enumerate(viewers):
                        print(f"        viewer[{i}][:120]={v[:120]!r}")
        blob = "\n".join(viewers)
        for mk in mojibake_markers(lang):
            if mk in blob:
                print(f"  [FAIL] mojibake marker {mk[:16]!r} present")
                failures.append(f"{lang}: mojibake marker {mk[:16]!r} present")
        for v in viewers:
            if "\\rtf1" in v or "\\par" in v or "\\u" in v.replace("\u005c", "X"):
                print("  [FAIL] raw RTF control words leaked into display text")
                failures.append(f"{lang}: raw RTF leaked into display")
                break
    finally:
        for h, pid in probe_wizards():
            user32.PostMessageW(h, WM_CLOSE, 0, 0)
        time.sleep(1.5)
        for pid in tracked_pids:
            kill_pid(pid)
        if proc.poll() is None:
            proc.kill()
        proc.wait(timeout=10)
        # remove Inno extraction temp dirs this run logged
        if logf.exists():
            try:
                text = logf.read_text(encoding="utf-8", errors="replace")
                for m in re.finditer(
                        r"Created temporary directory: (.+\.tmp)", text):
                    tmp_dirs.append(pathlib.Path(m.group(1).strip()))
            except OSError:
                pass
        for d in tmp_dirs:
            shutil.rmtree(d, ignore_errors=True)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--langs", default=",".join(LANGS_DEFAULT))
    ap.add_argument("--inno", default=None, help="Inno Setup 6 install dir override")
    ap.add_argument("--keep", action="store_true", help="keep the probe build dir")
    args = ap.parse_args()

    langs = [l.strip() for l in args.langs.split(",") if l.strip()]
    for lang in langs:
        if lang not in EXPECTED_ABOUT:
            print(f"ERROR: unknown lang {lang!r}; known: {', '.join(EXPECTED_ABOUT)}")
            return 2

    inno_dir = find_inno_dir(args.inno)
    if inno_dir is None:
        print("ERROR: Inno Setup 6 not found (use --inno). Gate cannot run.")
        return 2

    workdir = pathlib.Path(tempfile.mkdtemp(prefix="installer_display_gate_"))
    try:
        exe = build_probe(inno_dir, workdir)
        failures: list[str] = []
        for lang in langs:
            run_language(exe, lang, failures)
        print()
        if failures:
            print("DISPLAY GATE: FAIL")
            for f in failures:
                print("  -", f)
            return 1
        print("DISPLAY GATE: PASS - About+Guide memo pages render clean text "
              "for all probed languages.")
        return 0
    finally:
        for h, pid in probe_wizards():
            user32.PostMessageW(h, WM_CLOSE, 0, 0)
            kill_pid(pid)
        if not args.keep:
            shutil.rmtree(workdir, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
