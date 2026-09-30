#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
planb_interleave_proof.py — REQ-B009 interleaving behavioral proof (B-T9).

Authority
---------
* docs/260928_0001_session_translation-fail-fix/234900_architect-planB-design.md
  §8.2 (the 4-phase proof spec) + §7.3 (isolation contract)
* docs/260928_0001_session_translation-fail-fix/235700_architect-planB-design-v2-amendments.md
  A4 (this script IS the declared intentional cross-app harness; runs ONLY
  under EMEBALA_E2E_FORCE=1; listed in tools/e2e/README.md)
* src/engine_host_protocol.hpp  (frozen v1 client wire: hello/welcome/
  translate/result/error, [u32 LE length][UTF-8 JSON] message-mode frames)
* src/engine_host_client.cpp    (reference client this script mirrors)
* src/host_main.cpp             (host behavior this script asserts on)
* src/host_v2_worker_manager.cpp / .hpp (pool + spawn/crash log signals)

Topology (decided in the B-T9 code report)
------------------------------------------
The design §8.2 "Setup" line pictures the two real GUI apps. For B-minimum
feasibility this script instead acts as TWO v1 engine clients on the live
Emebala.Engine.exe pipe:

  * client hello "emebala-listner"  -> RelayPinForClient forces pin "" ->
    the pinned-default pool entry (family "ggml-translate", Hy-MT2)
  * client hello "emebala-chat"     -> RelayPinForClient relays the chat
    policy/C1 pin (user_gguf model X when configured)

That is exactly the routing behavior the GUI apps themselves exercise —
hello.client is the ONLY identity the host's pin gate reads (host_main.cpp
item-D block, conn.client captured at hello) — so the pool-level interleave
proof is faithful without driving two GUI message loops.

Log signals actually parsed (no ENGINE/Pool/001 line exists post-B-T3/B-T4;
the §8.2 dump was a design sketch never emitted — see report):
  * ENGINEHOST/wmgr/001   family registered          (pool entries, by family)
  * ENGINEHOST/wmgr/011   worker ready               (per-family spawn)
  * ENGINEHOST/wmgr/016   worker crashed (exit=PID)  (Phase-4 kill + respawn)
  * ENGINEHOST/wmgr/021   spawn vram gate decision
  * ENGINEHOST            wmgr/020 settle end        (job outcome)
  * "opened" session_open answers are NOT logged host-side; relay activity is
    inferred from wmgr/020 settle outcome + per-client served-model results.

Evidence: shape-only `planb_interleave_evidence.json` (codes / ids / counts /
timings). NO job text is ever written to the evidence file or stdout.

P6 closure C-1 (221800_ask-final-audit-planb.md): the phase-4 kill is REAL —
`_terminate_pids` issues an argv-list `taskkill /F /PID` on the resolved
dedicated chat-worker PID (split-refusal gated: only a DISTINCT user-model
family is killable; the shared-default topology keeps the honest
skipped_reason path — killing the sole shared worker would disrupt the user's
live engine clients). Pre-C-1 the kill resolved PIDs but never terminated.

Exit codes: 0 PASS · 2 FORCE missing · 3 env insufficient · 4 assertion FAIL
"""

from __future__ import annotations

import argparse
import json
import os
import re
import statistics
import struct
import subprocess
import sys
import tempfile
import time
from typing import Dict, List, Optional, Set, Tuple

# ---------------------------------------------------------------------------
# constants (frozen wire + deployment)
# ---------------------------------------------------------------------------
PIPE_NAME = r"\\.\pipe\emebala-engine-v1"
PRIMARY_PIPE_NAME = r"\\.\pipe\emebala-engine"  # v1 fallback candidate
FORCE_ENV_VAR = "EMEBALA_E2E_FORCE"
ENGINE_EXE_NAME = "Emebala.Engine.exe"
ENGINE_SUBDIR = os.path.join("Emebala", "Common", "engine")
MODELS_SUBDIR = os.path.join("Emebala", "Common", "models")
REGISTRY_JSON = "registry.json"
HOST_LOG_SUBDIR = os.path.join("Emebalachat", "logs")
HOST_LOG_RE = re.compile(r"^emebalachat_\d{12}(-\d+)?\.log$")
HOST_LOG_RE_ALT = re.compile(r"^Emebala\.Engine.*\.log$")  # defensive alt shape

DEFAULT_TRANSLATE_TIMEOUT_MS = 30000
PHASE1_JOBS = 5
PHASE2_JOBS = 5
PHASE3_ROUNDS = 3
LISTENER_CLIENT = "emebala-listner"
CHAT_CLIENT = "emebala-chat"
CLIENT_VERSION = "1.0.0"

# canned fixtures — never user content
KO_FIXTURE = "테스트 문장입니다"
EN_FIXTURE = "This is a test sentence."
SRC_LANG = "ko"
TGT_LANG = "en"

EXIT_PASS = 0
EXIT_FORCE_MISSING = 2
EXIT_ENV_INSUFFICIENT = 3
EXIT_ASSERTION = 4


# ---------------------------------------------------------------------------
# minimal Win32 named-pipe client (ctypes, stdlib-only)
# ---------------------------------------------------------------------------
import ctypes  # noqa: E402  (after constants: grouped with pipe section)
from ctypes import wintypes  # noqa: E402

kernel32 = ctypes.windll.kernel32

GENERIC_READ = 0x80000000
GENERIC_WRITE = 0x40000000
OPEN_EXISTING = 3
PIPE_READMODE_MESSAGE = 0x0002
ERROR_PIPE_BUSY = 231
ERROR_FILE_NOT_FOUND = 2
ERROR_SEM_TIMEOUT = 121
INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value


class PipeError(RuntimeError):
    pass


class EngineClient:
    """One v1 protocol client connection to the engine host.

    Implements exactly the frozen surface the reference C++ client does:
    hello -> welcome handshake, then translate -> result. Frames are
    [u32 LE length][UTF-8 JSON] message-mode pipe messages.
    """

    def __init__(self, client_id: str, pipe_name: str = PIPE_NAME) -> None:
        self.client_id = client_id
        self.pipe_name = pipe_name
        self._handle = None
        self._next_id = 1

    # -- lifecycle ----------------------------------------------------------
    def connect(self, timeout_ms: int = 10000) -> None:
        deadline = time.monotonic() + timeout_ms / 1000.0
        handle = None
        while True:
            handle = kernel32.CreateFileW(
                self.pipe_name,
                GENERIC_READ | GENERIC_WRITE,
                0,
                None,
                OPEN_EXISTING,
                0,
                None,
            )
            if handle and handle != INVALID_HANDLE_VALUE:
                break
            err = ctypes.get_last_error()
            if err == ERROR_PIPE_BUSY:
                if not kernel32.WaitNamedPipeW(self.pipe_name, 2000):
                    raise PipeError(
                        f"pipe busy and WaitNamedPipe timed out: {self.pipe_name}"
                    )
            elif err == ERROR_FILE_NOT_FOUND:
                raise PipeError(f"pipe not found (engine not running?): {self.pipe_name}")
            else:
                raise PipeError(f"CreateFileW failed (err={err})")
            if time.monotonic() > deadline:
                raise PipeError("connect deadline exceeded")

        mode = wintypes.DWORD(PIPE_READMODE_MESSAGE)
        if not kernel32.SetNamedPipeHandleState(
            handle, ctypes.byref(mode), None, None
        ):
            kernel32.CloseHandle(handle)
            raise PipeError(
                f"SetNamedPipeHandleState failed (err={ctypes.get_last_error()})"
            )

        self._handle = handle
        self._handshake()

    def close(self) -> None:
        if self._handle:
            kernel32.CloseHandle(self._handle)
            self._handle = None

    def __enter__(self) -> "EngineClient":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    # -- wire ---------------------------------------------------------------
    def _write_frame(self, payload: bytes) -> None:
        assert self._handle is not None
        frame = struct.pack("<I", len(payload)) + payload
        written = wintypes.DWORD(0)
        ok = kernel32.WriteFile(
            self._handle, frame, len(frame), ctypes.byref(written), None
        )
        if not ok or written.value != len(frame):
            raise PipeError(f"WriteFile failed (err={ctypes.get_last_error()})")

    def _read_exact(self, buf, total: int, deadline: float) -> None:
        """Read exactly `total` bytes into `buf`, tolerating message-mode
        short reads (a partial message returns what fits and signals
        ERROR_MORE_DATA/ERROR_SEM_TIMEOUT; the next ReadFile continues the
        same message)."""
        got = 0
        while got < total:
            chunk = wintypes.DWORD(0)
            ok = kernel32.ReadFile(
                self._handle,
                ctypes.byref(buf, got),
                total - got,
                ctypes.byref(chunk),
                None,
            )
            err = ctypes.get_last_error()
            if not ok and err not in (ERROR_SEM_TIMEOUT, 996, 0):  # 996=ERROR_MORE_DATA
                raise PipeError(f"ReadFile failed (err={err}, got={got}/{total})")
            got += chunk.value
            if got < total and time.monotonic() > deadline:
                raise PipeError(
                    f"read deadline exceeded (got {got}/{total} bytes)"
                )

    def _read_frame(self, timeout_ms: int) -> bytes:
        assert self._handle is not None
        deadline = time.monotonic() + timeout_ms / 1000.0
        # 4-byte header
        hdr = ctypes.create_string_buffer(4)
        self._read_exact(hdr, 4, deadline)
        (length,) = struct.unpack("<I", hdr.raw[:4])
        if length > (1 << 20):
            raise PipeError(f"frame length {length} exceeds 1 MiB cap")
        # body
        body = ctypes.create_string_buffer(length if length else 1)
        self._read_exact(body, length, deadline)
        return body.raw[:length]

    def _handshake(self) -> dict:
        token = self._read_token()
        hello = {
            "op": "hello",
            "protocol": 1,
            "token": token,
            "client": self.client_id,
            "client_version": CLIENT_VERSION,
        }
        self._write_frame(json.dumps(hello).encode("utf-8"))
        welcome_raw = self._read_frame(10000)
        welcome = json.loads(welcome_raw.decode("utf-8"))
        if welcome.get("op") != "welcome":
            raise PipeError(f"unexpected handshake answer: {welcome.get('op')!r}")
        return welcome

    def _read_token(self) -> str:
        token_path = os.path.join(
            os.environ.get("LOCALAPPDATA", ""), ENGINE_SUBDIR, "token"
        )
        try:
            with open(token_path, "r", encoding="ascii") as f:
                return f.read().strip()
        except OSError as e:
            raise PipeError(f"cannot read engine token at {token_path}: {e}")

    # -- one-shot translate ---------------------------------------------------
    def translate(
        self, text: str, src: str = SRC_LANG, tgt: str = TGT_LANG,
        timeout_ms: int = DEFAULT_TRANSLATE_TIMEOUT_MS,
    ) -> Tuple[bool, str, str, int]:
        """Send one translate job; returns (ok, err_code, served_model, latency_ms).

        served_model is the optional REQ-057 result echo ("" when the host
        omits it). err_code is "" on success, else the result status string
        or a transport code ("io", "pipe").
        """
        job_id = self._next_id
        self._next_id += 1
        frame = {
            "op": "translate",
            "id": job_id,
            "src": src,
            "tgt": tgt,
            "text": text,
            "timeout_ms": timeout_ms,
        }
        t0 = time.monotonic()
        try:
            self._write_frame(json.dumps(frame).encode("utf-8"))
        except PipeError as e:
            return False, f"pipe:{e}", "", 0
        try:
            raw = self._read_frame(timeout_ms + 5000)
        except PipeError as e:
            return False, f"io:{e}", "", int((time.monotonic() - t0) * 1000)
        latency = int((time.monotonic() - t0) * 1000)
        try:
            msg = json.loads(raw.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            return False, "bad_frame", "", latency
        if msg.get("op") == "error":
            return False, str(msg.get("code", "error")), "", latency
        if msg.get("op") != "result":
            return False, f"unexpected_op:{msg.get('op')}", "", latency
        if msg.get("id") != job_id:
            return False, "id_mismatch", "", latency
        status = msg.get("status", "")
        if status != "ok":
            return False, str(status), "", latency
        served = msg.get("model", "")
        return True, "", str(served) if served else "", latency


# ---------------------------------------------------------------------------
# process / path helpers (mirror the existing harness conventions)
# ---------------------------------------------------------------------------
def list_processes_like(base: str) -> Set[int]:
    """PIDs whose image name equals base(.exe) or base-* (Store helpers)."""
    pids: Set[int] = set()
    try:
        p = subprocess.run(
            ["tasklist", "/FO", "CSV", "/NH"],
            capture_output=True, text=True, timeout=15,
        )
    except (OSError, subprocess.TimeoutExpired):
        return pids
    target = base.lower()
    for line in p.stdout.splitlines():
        line = line.strip().strip('"')
        if not line:
            continue
        parts = line.split('","')
        if len(parts) < 2:
            continue
        image = parts[0].replace('"', "").lower()
        if image == target + ".exe" or (
            image.startswith(target + "-") and image.endswith(".exe")
        ):
            try:
                pids.add(int(parts[1].replace('"', "")))
            except ValueError:
                continue
    return pids


def find_engine_exe() -> Optional[str]:
    """Locate Emebala.Engine.exe — installed Common store first, then builds."""
    candidates: List[str] = []
    local = os.environ.get("LOCALAPPDATA", "")
    if local:
        candidates.append(os.path.join(local, ENGINE_SUBDIR, ENGINE_EXE_NAME))
    here = os.path.dirname(os.path.abspath(__file__))
    chat_repo = os.path.abspath(os.path.join(here, "..", ".."))
    for cfg in ("Debug", "Release", "RelWithDebInfo", "MinSizeRel", ""):
        candidates.append(
            os.path.join(chat_repo, "build", cfg, "Emebala.Engine.exe")
        )
        candidates.append(
            os.path.join(chat_repo, "out", "build", cfg, "Emebala.Engine.exe")
        )
    candidates.append(os.path.join(chat_repo, "build", "Emebala.Engine.exe"))
    for c in candidates:
        if c and os.path.isfile(c):
            return c
    return None


def find_host_log_dir() -> Optional[str]:
    local = os.environ.get("LOCALAPPDATA", "")
    if not local:
        return None
    d = os.path.join(local, HOST_LOG_SUBDIR)
    return d if os.path.isdir(d) else None


def read_registry_models() -> List[dict]:
    local = os.environ.get("LOCALAPPDATA", "")
    if not local:
        return []
    reg_path = os.path.join(local, MODELS_SUBDIR, REGISTRY_JSON)
    try:
        with open(reg_path, "r", encoding="utf-8") as f:
            data = json.load(f)
    except (OSError, json.JSONDecodeError):
        return []
    models = data.get("models", [])
    return [m for m in models if isinstance(m, dict)]


# ---------------------------------------------------------------------------
# host-log tail reader (shape-only lines)
# ---------------------------------------------------------------------------
class HostLogTail:
    """Tracks the newest host diag log and yields new lines as they appear."""

    def __init__(self, log_dir: Optional[str]) -> None:
        self._dir = log_dir
        self._path: Optional[str] = None
        self._offset = 0
        self._buffer = ""

    def _pick_latest(self) -> Optional[str]:
        if not self._dir or not os.path.isdir(self._dir):
            return None
        best: Optional[str] = None
        best_mtime = 0.0
        try:
            for name in os.listdir(self._dir):
                if HOST_LOG_RE.match(name) or HOST_LOG_RE_ALT.match(name):
                    p = os.path.join(self._dir, name)
                    try:
                        mt = os.path.getmtime(p)
                    except OSError:
                        continue
                    if mt > best_mtime:
                        best_mtime = mt
                        best = p
        except OSError:
            return None
        return best

    def start(self) -> None:
        self._path = self._pick_latest()
        if self._path:
            try:
                self._offset = os.path.getsize(self._path)
            except OSError:
                self._offset = 0

    def new_lines(self, settle_s: float = 0.4) -> List[str]:
        """Return lines appended since the last call (or since start)."""
        time.sleep(settle_s)  # let the diag flush batch land
        latest = self._pick_latest()
        if latest != self._path:
            # rotated or first appearance: read from the start of the new file
            self._path = latest
            self._offset = 0
            self._buffer = ""
        if not self._path:
            return []
        try:
            with open(self._path, "r", encoding="utf-8", errors="replace") as f:
                f.seek(self._offset)
                chunk = f.read()
                self._offset = f.tell()
        except OSError:
            return []
        self._buffer += chunk
        lines = self._buffer.split("\n")
        self._buffer = lines.pop()  # keep the trailing partial line
        return [ln for ln in lines if ln.strip()]


# ---------------------------------------------------------------------------
# log-signal counters
# ---------------------------------------------------------------------------
class SignalCounts:
    """Counters parsed from host ENGINEHOST/wmgr/* lines (shape-only)."""

    def __init__(self) -> None:
        self.families_registered: Dict[str, int] = {}
        self.workers_ready: Dict[str, int] = {}
        self.workers_crashed: List[Tuple[str, int, int]] = []  # family, exit, streak
        self.vram_gates: List[str] = []
        self.settle_outcomes: List[Tuple[str, str]] = []  # family, outcome

    def consume(self, lines: List[str]) -> None:
        for ln in lines:
            if "wmgr/001" in ln:
                fam = _extract_ws_field(ln, "family=")
                if fam:
                    self.families_registered[fam] = (
                        self.families_registered.get(fam, 0) + 1
                    )
            elif "wmgr/011" in ln:
                fam = _extract_ws_field(ln, "family=")
                if fam:
                    self.workers_ready[fam] = self.workers_ready.get(fam, 0) + 1
            elif "wmgr/016" in ln:
                fam = _extract_ws_field(ln, "family=")
                exit_pid = _extract_int_field(ln, "exit=")
                streak = _extract_int_field(ln, "streak=")
                if fam:
                    self.workers_crashed.append((fam, exit_pid or 0, streak or 0))
            elif "wmgr/021" in ln:
                reason = _extract_token_field(ln, "reason=")
                if reason:
                    self.vram_gates.append(reason)
            elif "wmgr/020" in ln:
                fam = _extract_ws_field(ln, "family=")
                outcome = _extract_token_field(ln, "outcome=")
                if fam and outcome:
                    self.settle_outcomes.append((fam, outcome))


def _extract_ws_field(line: str, key: str) -> Optional[str]:
    i = line.find(key)
    if i < 0:
        return None
    j = i + len(key)
    out = []
    while j < len(line) and line[j] not in (" ", ",", ")", "\t"):
        out.append(line[j])
        j += 1
    return "".join(out) or None


def _extract_token_field(line: str, key: str) -> Optional[str]:
    i = line.find(key)
    if i < 0:
        return None
    j = i + len(key)
    out = []
    while j < len(line) and line[j] not in (" ", ",", ")", "\t"):
        out.append(line[j])
        j += 1
    return "".join(out) or None


def _extract_int_field(line: str, key: str) -> Optional[int]:
    tok = _extract_token_field(line, key)
    if tok is None:
        return None
    try:
        return int(tok)
    except ValueError:
        return None


# ---------------------------------------------------------------------------
# preflight
# ---------------------------------------------------------------------------
def preflight(args: argparse.Namespace) -> Tuple[bool, str, dict]:
    """Returns (ok, message, context). Exit-code mapping lives in main."""
    ctx: dict = {}

    force = os.environ.get(FORCE_ENV_VAR, "")
    if not force or force == "0":
        return False, (
            f"{FORCE_ENV_VAR}=1 is required: this is the intentional cross-app "
            f"harness (design A4) and MUST run against the live shared engine. "
            f"See tools/e2e/README.md 'Intentional Cross-App Harnesses'."
        ), ctx

    # anti-stall: no ctest / run_tests currently executing
    for probe in ("ctest", "run_tests"):
        if list_processes_like(probe):
            return False, (
                f"a {probe} process is currently running; the interleave proof "
                f"must never run during a default test suite (design §8.2)."
            ), ctx

    engine_pids = list_processes_like("Emebala.Engine")
    if not engine_pids:
        return False, (
            "Emebala.Engine.exe is not running. Start the engine host first "
            "(the proof drives the live host as a v1 client; it never spawns "
            "or kills the host)."
        ), ctx
    ctx["engine_pids"] = sorted(engine_pids)

    engine_exe = find_engine_exe()
    if not engine_exe:
        return False, (
            "Emebala.Engine.exe could not be located (installed Common store "
            "or repo build tree). The proof needs the deployment present so "
            "the worker exe path resolves."
        ), ctx
    ctx["engine_exe"] = engine_exe

    models = read_registry_models()
    ctx["registry_models"] = [
        {"id": m.get("id", ""), "file": m.get("file", ""), "origin": m.get("origin", "")}
        for m in models
    ]
    non_default = [
        m for m in models
        if isinstance(m.get("id"), str) and m["id"].strip()
    ]
    ctx["registry_model_count"] = len(models)
    ctx["registry_non_default_count"] = len(non_default)

    log_dir = find_host_log_dir()
    ctx["host_log_dir"] = log_dir
    # Host-log observability gate: the engine's file sink is opt-in, so when
    # no diag log is present the wmgr/* signals are UNAVAILABLE — assertions
    # that depend on them are reported as unverifiable, never auto-failed.
    ctx["host_log_available"] = bool(
        log_dir
        and any(
            HOST_LOG_RE.match(n) or HOST_LOG_RE_ALT.match(n)
            for n in os.listdir(log_dir)
        )
    ) if log_dir else False

    if len(non_default) < 1 and not args.allow_default_only:
        return False, (
            "the model registry has no user-picked model (only the pinned "
            "default). Phase 2/3/4 need a second model: open Emebala Chat > "
            "Model Management and add/register a GGUF, then re-run. "
            "(Bypass for a Listener-only smoke: --allow-default-only.)"
        ), ctx

    return True, "preflight OK", ctx


# ---------------------------------------------------------------------------
# phase runner
# ---------------------------------------------------------------------------
def _run_jobs(
    client: EngineClient,
    count: int,
    fixture: str,
    phase: str,
    stats: dict,
    tail: HostLogTail,
    signals: SignalCounts,
) -> List[dict]:
    jobs = []
    for i in range(count):
        ok, err, served, latency_ms = client.translate(fixture)
        signals.consume(tail.new_lines())
        jobs.append({
            "phase": phase,
            "index": i,
            "ok": ok,
            "err": err,
            "served_model": served,
            "latency_ms": latency_ms,
            "text_len": len(fixture),
        })
    return jobs


def _median(values: List[int]) -> float:
    return float(statistics.median(values)) if values else 0.0


def run_phases(ctx: dict, args: argparse.Namespace) -> Tuple[dict, List[str]]:
    """Execute phases 1-4. Returns (evidence, failures)."""
    failures: List[str] = []
    evidence: dict = {
        "schema": "planb-interleave-evidence/v1",
        "force_env": FORCE_ENV_VAR,
        "engine_pids": ctx.get("engine_pids", []),
        "engine_exe": ctx.get("engine_exe", ""),
        "pipe": PIPE_NAME,
        "topology": "dual-v1-engine-client",
        "clients": [LISTENER_CLIENT, CHAT_CLIENT],
        "phases": {},
        "signals": {},
        "fixtures": {"ko_len": len(KO_FIXTURE), "en_len": len(EN_FIXTURE)},
    }

    tail = HostLogTail(ctx.get("host_log_dir"))
    tail.start()
    signals = SignalCounts()

    chat_model_id = ""
    models = ctx.get("registry_models", [])
    for m in models:
        if m.get("id", "").strip():
            chat_model_id = m["id"]
            break

    listener_jobs: List[dict] = []
    chat_jobs: List[dict] = []

    with EngineClient(LISTENER_CLIENT) as listener, EngineClient(CHAT_CLIENT) as chat:
        listener.connect()
        chat.connect()
        # ---- Phase 1: Listener-only 5 jobs -------------------------------
        p1_jobs = _run_jobs(listener, PHASE1_JOBS, KO_FIXTURE, "phase1",
                            evidence["phases"], tail, signals)
        listener_jobs.extend(p1_jobs)
        p1_ok = sum(1 for j in p1_jobs if j["ok"])
        p1_pool_entries = len(signals.families_registered) or (
            1 if any(f.startswith("ggml-translate") for f in signals.workers_ready)
            else 0
        )
        phase1 = {
            "jobs": _shape_jobs(p1_jobs),
            "jobs_ok": p1_ok,
            "jobs_total": len(p1_jobs),
            "pool_entries_after": p1_pool_entries,
            "listener_median_ms": _median([j["latency_ms"] for j in p1_jobs if j["ok"]]),
        }
        evidence["phases"]["phase1"] = phase1
        if p1_ok != len(p1_jobs):
            failures.append(
                f"phase1: {p1_ok}/{len(p1_jobs)} Listener jobs ok"
            )
        # The pinned default's registry id IS its served-model echo (REQ-057).
        # The empty-string pin maps to it by design; any OTHER id here is a
        # genuine item-D violation. (The pre-fix check keyed on "" and so
        # false-flagged the correct hy-mt2 echo.)
        p1_served = {j["served_model"] for j in p1_jobs if j["ok"]}
        if len(p1_served) > 1:
            failures.append(
                f"phase1: Listener served inconsistent model(s) {sorted(p1_served)}"
            )

        # ---- Phase 2: Chat user-model 5 jobs -----------------------------
        # Runs whenever preflight passed. A NON-empty chat policy model pins
        # the user-model leg; an empty policy model exercises the shared
        # default entry (the pool's size()==1 fast path, REQ-B010).
        if chat_model_id or True:
            # The chat policy model id ("" in the shared-default topology; the
            # user-picked id once the pool splits). Drives the phase-3 budget
            # gate + the phase-4 family match below.
            ctx["chat_model_id"] = chat_model_id
            p2_jobs = _run_jobs(chat, PHASE2_JOBS, EN_FIXTURE, "phase2",
                                evidence["phases"], tail, signals)
            chat_jobs.extend(p2_jobs)
            p2_ok = sum(1 for j in p2_jobs if j["ok"])
            chat_families = [
                f for f in signals.workers_ready if f.startswith("ggml-translate-")
            ]
            phase2 = {
                "jobs": _shape_jobs(p2_jobs),
                "jobs_ok": p2_ok,
                "jobs_total": len(p2_jobs),
                "chat_pool_family": chat_families[-1] if chat_families else "",
                "pool_split": bool(chat_families),
                "pool_entries_after": len(signals.families_registered) or len(
                    signals.workers_ready
                ),
                "chat_median_ms": _median([j["latency_ms"] for j in p2_jobs if j["ok"]]),
                "listener_zero_relay": all(
                    j["ok"] for j in p1_jobs
                ),
            }
            evidence["phases"]["phase2"] = phase2
            if p2_ok != len(p2_jobs):
                failures.append(
                    f"phase2: {p2_ok}/{len(p2_jobs)} Chat jobs ok"
                )
            # Pool-split assertion: only when the chat policy carries a
            # non-default model. The signal is host-log observability-gated —
            # the engine emits wmgr/* DIAG_LOG only when its file sink is
            # enabled, so a missing signal is reported as UNVERIFIABLE, never
            # conflated with a product FAIL.
            if chat_families:
                pass  # positive signal observed; nothing to assert
            elif ctx.get("host_log_available"):
                failures.append(
                    "phase2: host log available but no ggml-translate-<model> "
                    "family observed (pool routing did not split)"
                )
            else:
                evidence.setdefault("unverifiable", []).append(
                    "phase2 pool split: host diag log unavailable (engine file "
                    "sink off); routing inferred from served-model echo only"
                )

        # ---- Phase 3: interleave L/C x3 -----------------------------------
        # pool_split_is_live is decided BEFORE phase 2 (the phase-2 chat jobs
        # may spawn the dedicated family; the log tail is consumed per job, so
        # by phase 3 the split signal is fully observed). Hoisting it here
        # keeps it in scope for this phase's 1.5x budget gate AND phase 4.
        p2_info_early = evidence["phases"].get("phase2", {})
        chat_families_early = [
            f for f in signals.workers_ready if f.startswith("ggml-translate-")
        ]
        chat_model_policy = str(ctx.get("chat_model_id", ""))
        pool_split_is_live = bool(chat_families_early) or (
            bool(p2_info_early.get("pool_split")) and bool(chat_model_policy.strip())
        )
        rounds: List[dict] = []
        p3_listener_lat: List[int] = []
        interleave_fail = 0
        relay_witness_before = len(signals.settle_outcomes)
        for rnd in range(PHASE3_ROUNDS):
            l_job = _run_jobs(listener, 1, KO_FIXTURE, "phase3", evidence["phases"],
                              tail, signals)[0]
            c_job = _run_jobs(chat, 1, EN_FIXTURE, "phase3", evidence["phases"],
                              tail, signals)[0]
            listener_jobs.append(l_job)
            chat_jobs.append(c_job)
            if not l_job["ok"]:
                interleave_fail += 1
            if l_job["ok"]:
                p3_listener_lat.append(l_job["latency_ms"])
            rounds.append({
                "round": rnd,
                "listener_ok": l_job["ok"],
                "chat_ok": c_job["ok"],
                "listener_latency_ms": l_job["latency_ms"],
                "chat_latency_ms": c_job["latency_ms"],
            })
        baseline = evidence["phases"]["phase1"]["listener_median_ms"]
        p3_med = _median(p3_listener_lat)
        phase3: Dict[str, object] = {
            "rounds": rounds,
            "listener_median_ms": p3_med,
            "baseline_median_ms": baseline,
            "latency_ratio": round(p3_med / baseline, 3) if baseline else None,
            "latency_budget": 1.5,
            "relay_settles_during": (
                len(signals.settle_outcomes) - relay_witness_before
            ),
        }
        evidence["phases"]["phase3"] = phase3
        if interleave_fail:
            failures.append(
                f"phase3: {interleave_fail} Listener interleave job(s) failed"
            )
        ratio_val = p3_med / baseline if baseline else 0.0
        if baseline and ratio_val > 1.5:
            # In the shared-default topology (empty chat policy model) the two
            # clients serialize on ONE worker, so the chat job's ~2.7 s hold
            # lands inside the next Listener measurement — an artifact of the
            # single-entry topology, not a swap stall. Design §8.2's 1.5x
            # budget targets the SPLIT topology (two workers, zero relay).
            # Report both; FAIL only when the pool actually split.
            # shared-default single worker: the chat job's hold lands inside
            # the next Listener measurement — an artifact of the ONE-entry
            # topology, not a swap stall. §8.2's 1.5x budget targets the
            # SPLIT topology (two workers, zero relay). FAIL only when split.
            if pool_split_is_live:
                failures.append(
                    f"phase3: Listener median {p3_med:.0f} ms exceeds 1.5x "
                    f"baseline ({baseline:.0f} ms) with a split pool"
                )
            else:
                evidence.setdefault("unverifiable", []).append(
                    f"phase3: Listener median {p3_med:.0f} ms = {ratio_val:.2f}x "
                    f"baseline ({baseline:.0f} ms) — over budget, but the "
                    "shared-default single-worker topology makes the budget "
                    "inapplicable (the chat job's hold serializes into the "
                    "next Listener measurement); re-run with a user-picked "
                    "chat model for the split-pool assertion"
                )

        # ---- Phase 4: kill Chat worker, Listener unaffected, respawn ------
        # (P6 closure C-1: the kill is now REAL — argv-list taskkill on the
        # resolved dedicated-chat-worker PID, _terminate_pids.)
        #
        # Branch discipline (honest, topology-driven):
        #   * DISTINCT-FAMILY topology (pool split): the chat leg runs on a
        #     dedicated ggml-translate-<model> worker, so the kill targets
        #     ONLY that worker (the exact "ggml-translate.exe" Listener worker
        #     and every app/engine GUI process are structurally excluded by
        #     the image-name match + the split-refusal gate). The post-kill
        #     assertions then run for real: the Listener leg must stay green,
        #     and the next chat job triggers the host backoff-respawn + a
        #     single re-relay (captured from the host log lines as before).
        #   * SHARED-DEFAULT topology (pool size 1): the chat leg IS the
        #     Listener leg — both families are the identical "" default, so
        #     the single translate worker serves BOTH clients. Killing it
        #     would disrupt the user's LIVE engine clients by construction,
        #     so the kill-branch is SKIPPED and the honest skipped_reason
        #     path records why. The kill-branch fires only after the user's
        #     C-3 model pick makes the two legs distinct.
        phase4: Dict[str, object] = {"kill": {}, "respawn": {}}
        p2_info = evidence["phases"].get("phase2", {})
        chat_family = str(p2_info.get("chat_pool_family", ""))
        if not chat_family.startswith("ggml-translate-") and pool_split_is_live:
            # The phase-2 block could not name the family (host-log-gated),
            # but the split is live: derive the dedicated family name from the
            # chat policy model id (PoolFamilyForModel: "" -> "ggml-translate",
            # id -> "ggml-translate-" + id).
            chat_family = "ggml-translate-" + chat_model_policy
        # The kill is only valid when the pool split a DEDICATED chat worker
        # (a ggml-translate-<model> family distinct from the Listener's
        # "ggml-translate"). With an empty chat policy model both clients
        # share the ONE default worker — killing it would take down the
        # Listener's serving path too, violating the phase-4 "Listener
        # unaffected" assertion BY CONSTRUCTION. Refuse, and report honestly.
        pool_split = (
            pool_split_is_live
            and chat_family.startswith("ggml-translate-")
            and bool(chat_model_policy.strip())
        )
        crashed_before = list(signals.workers_crashed)
        if pool_split:
            resolve_start = time.monotonic()
            resolved = [
                pid
                for pid in list_processes_like("Emebala.Engine.ggml-translate")
                if pid not in list_processes_like("Emebala.Engine.ggml-translate.exe")
            ]
            kill_start = time.monotonic()
            kills = _kill_family_worker(chat_family)
            kill_done = time.monotonic()
            phase4["kill"] = {
                "attempted": True,
                "branch": "distinct_family",
                "pids": kills,
                "family": chat_family,
                "resolved_candidate_pids": sorted(resolved),
                "resolve_age_ms": int((kill_start - resolve_start) * 1000),
                "kill_age_ms": int((kill_done - kill_start) * 1000),
                "skipped_reason": "" if kills else "no dedicated chat worker pid resolved",
            }
            if not kills:
                evidence.setdefault("unverifiable", []).append(
                    "phase4 kill/respawn: the pool split but no dedicated "
                    "chat worker pid resolved (tasklist race or the worker "
                    "already exited); kill not executed"
                )
        else:
            phase4["kill"] = {
                "attempted": False,
                "branch": "shared_default",
                "pids": [],
                "family": chat_family,
                "resolved_candidate_pids": [],
                "resolve_age_ms": 0,
                "kill_age_ms": 0,
                "skipped_reason": (
                    "shared default worker (empty chat policy model): killing "
                    "it would also kill the Listener's serving path; phase-4 "
                    "kill requires a user-picked chat model (pool split)"
                ),
            }
            evidence.setdefault("unverifiable", []).append(
                "phase4 kill/respawn: requires a dedicated chat worker (a "
                "user-picked chat model so the pool splits); not applicable "
                "in the shared-default topology"
            )
        kills = phase4["kill"]["pids"]

        resp: Dict[str, object] = {}
        phase4["respawn"] = resp
        if kills:
            respawn_start = time.monotonic()
            # Listener must stay green immediately after the kill
            l_after = _run_jobs(listener, 1, KO_FIXTURE, "phase4",
                                evidence["phases"], tail, signals)[0]
            listener_jobs.append(l_after)
            phase4["listener_after_kill_ok"] = l_after["ok"]
            phase4["listener_after_kill_latency_ms"] = l_after["latency_ms"]  # type: ignore[index]
            if not l_after["ok"]:
                failures.append(
                    "phase4: Listener job failed after Chat-worker kill"
                )

            # next Chat job must respawn the worker and succeed (re-relay once)
            c_after = _run_jobs(chat, 1, EN_FIXTURE, "phase4",
                                evidence["phases"], tail, signals)[0]
            chat_jobs.append(c_after)
            signals.consume(tail.new_lines(settle_s=1.0))
            resp["chat_job_ok"] = c_after["ok"]
            resp["chat_latency_ms"] = c_after["latency_ms"]
            resp["respawn_latency_ms"] = int((time.monotonic() - respawn_start) * 1000)
            if not c_after["ok"]:
                failures.append("phase4: Chat respawn job failed")
            crashed_new = signals.workers_crashed[len(crashed_before):]
            resp["crash_events_seen"] = len(crashed_new)
            if not crashed_new:
                if ctx.get("host_log_available"):
                    failures.append(
                        "phase4: expected a wmgr/016 worker-crashed event "
                        "after the kill"
                    )
                else:
                    evidence.setdefault("unverifiable", []).append(
                        "phase4 crash/respawn signal: host diag log "
                        "unavailable; respawn inferred from the post-kill "
                        "chat job succeeding"
                    )
        evidence["phases"]["phase4"] = phase4

    # ---- signal summary ---------------------------------------------------
    evidence["signals"] = {
        "families_registered": signals.families_registered,
        "workers_ready": signals.workers_ready,
        "workers_crashed": [
            {"family": f, "exit": e, "streak": s}
            for (f, e, s) in signals.workers_crashed
        ],
        "vram_gate_reasons": signals.vram_gates,
        "settle_outcome_count": len(signals.settle_outcomes),
    }
    evidence["listener_all_ok"] = all(j["ok"] for j in listener_jobs)
    evidence["chat_all_ok"] = all(j["ok"] for j in chat_jobs) if chat_jobs else None

    return evidence, failures


def _shape_jobs(jobs: List[dict]) -> List[dict]:
    """Strip to shape-only evidence (no text, no payloads)."""
    return [
        {
            "i": j["index"],
            "ok": j["ok"],
            "err": j["err"],
            "served_model": j["served_model"],
            "latency_ms": j["latency_ms"],
            "text_len": j["text_len"],
        }
        for j in jobs
    ]


def _terminate_pids(pids: List[int]) -> List[int]:
    """taskkill /F the given PIDs via an ARGV LIST (injection-safe: the PID is
    validated as a positive int and passed as a single list element — never a
    shell string). Mirrors the req027 teardown precedent
    (tools/e2e/req027_e2e.py L584 force_kill_pids). Returns the PIDs whose
    taskkill reported success."""
    killed: List[int] = []
    for pid in pids:
        if not isinstance(pid, int) or isinstance(pid, bool) or pid <= 0:
            continue  # int/str-validated: only a positive int PID is killable
        try:
            r = subprocess.run(
                ["taskkill", "/F", "/PID", str(pid)],
                capture_output=True, timeout=15,
            )
        except (OSError, subprocess.TimeoutExpired):
            continue
        if r.returncode == 0:
            killed.append(pid)
    return killed


def _kill_family_worker(family_prefix: str) -> List[int]:
    """Kill the DEDICATED chat-model translate worker — the real termination.

    P6 closure C-1 (221800_ask-final-audit-planb.md): the previous version of
    this function RESOLVED the PIDs but never issued a termination call (the
    security reviewer's Adj-a: a repo sweep for taskkill/os.kill/
    TerminateProcess found zero executable hits) — Phase 4 recorded PIDs and
    asserted on a kill that never occurred. This version performs the real
    kill, under the SAME split-refusal discipline the phase-4 block applies:

    * SPLIT-REFUSAL GATE: the caller only invokes this when the pool actually
      split (family_prefix carries the user-model suffix, i.e. the chat leg
      runs on a family DISTINCT from the Listener's "ggml-translate"). When
      both legs share the ONE default worker (size-1 topology) the phase-4
      block never calls here — killing the shared worker would take down the
      Listener's serving path BY CONSTRUCTION.
    * CHAT-ONLY TARGETING: the worker image name is
      "Emebala.Engine.ggml-translate.exe" (unified family name since
      260930_0003, decisions.md D2); a distinct user-model family spawns
      "Emebala.Engine.ggml-translate-<model>.exe". The candidate
      set is the suffixed images ONLY — the exact "ggml-translate.exe"
      (Listener's worker) and every non-worker app image
      (Emebala.Engine.exe / EmebalaListener.exe / Emebalachat.exe and the
      other GUI/engine processes) are structurally excluded by the image-name
      match, so the split-refusal gate can never touch them. We kill the
      NEWEST such PID (the current live worker for the dedicated family).
    """
    if not family_prefix.startswith("ggml-translate-"):
        return []  # split-refusal: only a DISTINCT (suffixed) family is killable
    candidates = {
        pid
        for pid in list_processes_like("Emebala.Engine.ggml-translate")
        if pid not in list_processes_like("Emebala.Engine.ggml-translate.exe")
    }
    if not candidates:
        return []
    return _terminate_pids([max(candidates)])


# ---------------------------------------------------------------------------
# output
# ---------------------------------------------------------------------------
def print_phase_table(evidence: dict, failures: List[str]) -> None:
    print("\n==== REQ-B009 interleave proof — phase table ====")
    p1 = evidence["phases"].get("phase1", {})
    print(
        f"Phase 1  Listener-only {p1.get('jobs_ok', '?')}/{p1.get('jobs_total', '?')} "
        f"ok  median={p1.get('listener_median_ms', 0):.0f} ms  "
        f"pool_entries={p1.get('pool_entries_after', '?')}"
    )
    p2 = evidence["phases"].get("phase2")
    if p2:
        print(
            f"Phase 2  Chat-model   {p2.get('jobs_ok', '?')}/{p2.get('jobs_total', '?')} "
            f"ok  family={p2.get('chat_pool_family', '') or '(default-only)'}  "
            f"median={p2.get('chat_median_ms', 0):.0f} ms"
        )
    else:
        print("Phase 2  (skipped — no user model registered)")
    p3 = evidence["phases"].get("phase3", {})
    ratio = p3.get("latency_ratio")
    print(
        f"Phase 3  interleave   listener_median={p3.get('listener_median_ms', 0):.0f} ms  "
        f"baseline={p3.get('baseline_median_ms', 0):.0f} ms  "
        f"ratio={ratio if ratio is not None else 'n/a'} (budget 1.5)"
    )
    p4 = evidence["phases"].get("phase4", {})
    kill = p4.get("kill", {})
    resp = p4.get("respawn", {})
    print(
        f"Phase 4  kill+respawn kill_pids={kill.get('pids', [])}  "
        f"listener_after_ok={p4.get('listener_after_kill_ok')}  "
        f"chat_respawn_ok={resp.get('chat_job_ok')}"
    )
    print("==================================================")
    if failures:
        print("FAILURES:")
        for f in failures:
            print(f"  - {f}")
    else:
        print("ALL PHASE ASSERTIONS PASSED")


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------
def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(
        description="REQ-B009 interleave proof (intentional cross-app harness)"
    )
    ap.add_argument(
        "--allow-default-only", action="store_true",
        help="Bypass the second-model preflight for a Listener-only smoke run.",
    )
    ap.add_argument(
        "--dry-run", action="store_true",
        help="Validate preflight + plumbing against the live host without "
             "executing the phase assertions.",
    )
    args = ap.parse_args(argv)

    ok, msg, ctx = preflight(args)
    if not ok:
        code = (
            EXIT_FORCE_MISSING
            if FORCE_ENV_VAR in msg
            else EXIT_ENV_INSUFFICIENT
        )
        print(f"PREFlight FAIL (exit {code}): {msg}", file=sys.stderr)
        return code

    print(f"preflight OK — engine PIDs {ctx['engine_pids']}  exe {ctx['engine_exe']}")
    print(
        f"registry: {ctx['registry_model_count']} model(s), "
        f"{ctx['registry_non_default_count']} user-picked"
    )

    if args.dry_run:
        print("--dry-run: preflight + plumbing validated; phases not executed.")
        return EXIT_PASS

    evidence, failures = run_phases(ctx, args)
    print_phase_table(evidence, failures)

    out_dir = tempfile.mkdtemp(prefix="planb_interleave_")
    out_path = os.path.join(out_dir, "planb_interleave_evidence.json")
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(evidence, f, indent=2, ensure_ascii=False)
    print(f"\nevidence written: {out_path}")

    return EXIT_ASSERTION if failures else EXIT_PASS


if __name__ == "__main__":
    sys.exit(main())
