#!/usr/bin/env python3
"""kExpectedModelSha256 3-way sync gate (REQ-044, P4-1).

Purpose
-------
The Hy-MT2-1.8B-Q8_0.gguf SHA-256 pin is defined in THREE places that must
stay byte-identical, or a model rotation will silently break either the
worker/host runtime verification or the installer's download verification
(with no C++ test able to see the installer side):

  1. src/engine_core/engine_core_helpers.hpp  — kExpectedModelSha256 (char[])
       runtime pin used by VerifyModelSha256() in the engine worker/host.
  2. src/engine_host_client.hpp               — kExpectedModelSha256 (string_view)
       client-side pin checked against the host's welcome.model_sha256.
  3. installer/setup.iss                      — EXPECTED_MODEL_SHA256 (Pascal)
       installer's download + VerifyDownloadedModel() pin.

This script is a READ-ONLY gate. It parses all three literals, verifies
each is exactly 64 lowercase hex chars, and verifies all three agree.
Any parse failure, format violation, or mismatch exits non-zero so the
script can be wired into a pre-release / pre-build pipeline.

Deterministic: performs ONLY static 3-way comparison. The optional
"certutil -hashfile <model>" live-file check mentioned in the REQ-044 P3
design doc is deliberately NOT implemented here — keeping the gate
hermetic (no dependency on a 2 GB model file being present).

Usage:
  python tools/check_model_sha_sync.py            # gate mode (exit 0 = pass)
  python tools/check_model_sha_sync.py --self-test
      # synthetic regression check: exercises the diff/mismatch
      # detection logic against in-memory strings WITHOUT touching any
      # source file. Proves the gate would catch a real drift.
"""
from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path

WS = Path(__file__).resolve().parent.parent

SRC_ENGINE_CORE = WS / "src" / "engine_core" / "engine_core_helpers.hpp"
SRC_CLIENT = WS / "src" / "engine_host_client.hpp"
SRC_INSTALLER = WS / "installer" / "setup.iss"

HEX64_RE = re.compile(r"\A[0-9a-f]{64}\Z")

# --- Parsing rules (REQ-044 P3 design §item2, verified against real sources) ---

# (1) engine_core_helpers.hpp: `inline constexpr char kExpectedModelSha256[] =`
#     followed by the literal on the SAME or NEXT line. We accept an optional
#     `[]` (char array) and then capture everything between the first `"` after
#     `=` and the terminating `;` — this naturally handles C++ adjacent-string
#     concatenation ("..." "...") which the design doc explicitly calls out.
RE_ENGINE_CORE = re.compile(
    r"kExpectedModelSha256(?:\s*\[\s*\])?\s*=\s*\"(?P<body>.*?)\"\s*;",
    re.DOTALL,
)

# (2) engine_host_client.hpp: `inline constexpr std::string_view
#     kExpectedModelSha256 = "...";`. Same body capture so a future refactor
#     that adds concatenation still parses. We intentionally do NOT anchor on
#     `std::string_view` so the gate keeps working if the type is swapped for
#     `const char*` etc. — the name is the contract, not the type.
RE_CLIENT = re.compile(
    r"kExpectedModelSha256(?:\s*\[\s*\])?\s*=\s*\"(?P<body>.*?)\"\s*;",
    re.DOTALL,
)

# (3) installer/setup.iss: Pascal single-quoted constant inside [Code].
#     `EXPECTED_MODEL_SHA256 = '...';`
RE_INSTALLER = re.compile(
    r"EXPECTED_MODEL_SHA256\s*=\s*'(?P<body>[0-9a-fA-F]{0,64})'\s*;",
    re.DOTALL,
)


@dataclass
class Source:
    """One hash-literal source file."""

    key: str           # short identifier used in reports
    path: Path         # absolute path
    regex: re.Pattern  # extraction regex (must define a `body` group)
    joiner: str        # character stripped from body to handle C++ concat
    description: str   # human-readable role


SOURCES: list[Source] = [
    Source(
        key="engine_core",
        path=SRC_ENGINE_CORE,
        regex=RE_ENGINE_CORE,
        joiner='"',
        description="engine_core_helpers.hpp kExpectedModelSha256 (char[], "
                    "worker/host runtime pin)",
    ),
    Source(
        key="client",
        path=SRC_CLIENT,
        regex=RE_CLIENT,
        joiner='"',
        description="engine_host_client.hpp kExpectedModelSha256 "
                    "(string_view, welcome-pin check)",
    ),
    Source(
        key="installer",
        path=SRC_INSTALLER,
        regex=RE_INSTALLER,
        joiner="",
        description="installer/setup.iss EXPECTED_MODEL_SHA256 "
                    "(download verification)",
    ),
]


def extract_hash(src: Source, problems: list[str]) -> str | None:
    """Read `src.path`, apply `src.regex`, return the normalized 64-char hash.

    Returns None on any failure and appends a descriptive entry to `problems`.
    """
    try:
        text = src.path.read_text(encoding="utf-8")
    except FileNotFoundError:
        problems.append(f"{src.key}: FILE NOT FOUND: {src.path}")
        return None
    except UnicodeDecodeError as e:
        problems.append(f"{src.key}: NOT VALID UTF-8: {src.path}: {e}")
        return None
    except OSError as e:
        problems.append(f"{src.key}: READ ERROR: {src.path}: {e}")
        return None

    m = src.regex.search(text)
    if not m:
        problems.append(
            f"{src.key}: PARSE FAILED — could not find "
            f"{src.regex.pattern!r} in {src.path}"
        )
        return None

    body = m.group("body")
    # Normalize: for C++ sources, drop interior quote chars so adjacent-string
    # concatenation ("abc" "def") collapses to "abcdef". For the installer
    # (Pascal single-quoted, no concat), joiner is empty and this is a no-op.
    normalized = body.replace(src.joiner, "")
    normalized = "".join(normalized.split())  # strip any interior whitespace

    if not HEX64_RE.match(normalized):
        problems.append(
            f"{src.key}: FORMAT VIOLATION in {src.path} — value is not "
            f"exactly 64 lowercase hex chars "
            f"(got {len(normalized)} chars: {normalized!r})"
        )
        return None
    return normalized


def diff_report(values: dict[str, str]) -> list[str]:
    """Render a per-source diff-style report when hashes disagree."""
    lines: list[str] = []
    uniq = sorted(set(values.values()))
    lines.append(f"  distinct values: {len(uniq)}")
    for i, v in enumerate(uniq, 1):
        lines.append(f"    variant[{i}] = {v}")
    for key, v in values.items():
        lines.append(f"    {key:12s} -> variant[{uniq.index(v) + 1}]  ({v})")
    return lines


def run_gate() -> int:
    problems: list[str] = []
    values: dict[str, str] = {}

    print("REQ-044 P4-1 kExpectedModelSha256 3-way sync gate")
    print("=" * 60)

    for src in SOURCES:
        v = extract_hash(src, problems)
        if v is not None:
            values[src.key] = v
            print(f"  [OK ] {src.key:12s} {v}  ({src.path.name})")
        else:
            print(f"  [ERR] {src.key:12s}  ({src.description})")

    print("-" * 60)

    # Any parse/format problem already fails the gate.
    if problems:
        print("FAIL — gate could not read/parse all three sources:")
        for p in problems:
            print("  " + p)
        return 1

    # All three parsed; now compare.
    distinct = set(values.values())
    if len(distinct) == 1:
        only = next(iter(distinct))
        print(f"PASS — all three sources agree on {only}")
        print("       (engine_core_helpers.hpp == engine_host_client.hpp "
              "== setup.iss EXPECTED_MODEL_SHA256)")
        return 0

    print(f"FAIL — {len(distinct)} distinct hash values across 3 sources:")
    for line in diff_report(values):
        print(line)
    return 1


def run_self_test() -> int:
    """Prove mismatch detection works WITHOUT touching any source file.

    Uses in-memory synthetic strings that mimic the three real file shapes
    (char[] with concatenation, string_view, Pascal single-quote) and feeds
    them through the SAME extraction/comparison logic the gate uses.
    """
    print("REQ-044 P4-1 self-test (synthetic, no source files touched)")
    print("=" * 60)

    # Pull the real hash value so the equality case is also exercised.
    real = extract_hash(SOURCES[0], [])
    if real is None:
        print("SELF-TEST ABORT — could not read real anchor from "
              f"{SOURCES[0].path}")
        return 2

    good_core = f'kExpectedModelSha256[] =\n    "{real}";'
    good_client = f'kExpectedModelSha256 =\n    "{real}";'
    good_iss = f"EXPECTED_MODEL_SHA256 = '{real}';"

    # Case A: concatenated C++ literal must normalize identically.
    half = len(real) // 2
    concat_core = (
        f'kExpectedModelSha256[] =\n    "{real[:half]}"\n    "{real[half:]}";'
    )

    # Case B: drifted installer value — the realistic failure mode.
    drifted = ("a" if real[0] != "a" else "b") + real[1:]
    bad_iss = f"EXPECTED_MODEL_SHA256 = '{drifted}';"

    # Case C: wrong length (rotated but truncated).
    short_iss = f"EXPECTED_MODEL_SHA256 = '{real[:32]}';"

    cases = [
        ("all-match (baseline)", good_core, good_client, good_iss, True),
        ("all-match (C++ concat)", concat_core, good_client, good_iss, True),
        ("installer drift", good_core, good_client, bad_iss, False),
        ("installer truncated", good_core, good_client, short_iss, False),
    ]

    failures = 0
    for name, core, client, iss, expect_pass in cases:
        problems: list[str] = []
        values: dict[str, str] = {}
        for src, text in (
            (SOURCES[0], core),
            (SOURCES[1], client),
            (SOURCES[2], iss),
        ):
            m = src.regex.search(text)
            if not m:
                problems.append(f"{src.key}: PARSE FAILED on synthetic input")
                continue
            norm = "".join(m.group("body").replace(src.joiner, "").split())
            if not HEX64_RE.match(norm):
                problems.append(
                    f"{src.key}: FORMAT VIOLATION (len={len(norm)})"
                )
                continue
            values[src.key] = norm

        if problems:
            actual_pass = False
        else:
            actual_pass = len(set(values.values())) == 1

        ok = actual_pass == expect_pass
        status = "PASS" if ok else "FAIL"
        arrow = "expected" if ok else f"EXPECTED {expect_pass} GOT {actual_pass}"
        print(f"  [{status}] {name:32s} -> gate_would_pass={actual_pass} "
              f"({arrow})")
        if not ok:
            failures += 1
            for p in problems:
                print(f"         {p}")

    print("-" * 60)
    if failures:
        print(f"SELF-TEST FAILED — {failures} case(s) mis-detected")
        return 1
    print(f"SELF-TEST PASSED — all {len(cases)} synthetic cases detected "
          "correctly (mismatch/truncation caught, equality + concat "
          "accepted).")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description="REQ-044 P4-1: gate that pins kExpectedModelSha256 "
                    "to be identical across engine_core_helpers.hpp, "
                    "engine_host_client.hpp, and setup.iss."
    )
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="run synthetic in-memory regression cases instead of the "
             "real gate (proves mismatch detection without touching "
             "source files).",
    )
    args = parser.parse_args()

    if args.self_test:
        return run_self_test()
    return run_gate()


if __name__ == "__main__":
    sys.exit(main())
