#!/usr/bin/env python3
# tools/generate_probe_manifest.py
# Emits tools/probe_manifest.txt (UTF-8, no BOM) for session 260913_0002
# REQ-005, per the architect grid (153220_architect-report.md §2.3, numbers
# authoritative): 150 RUN records = 5 pairs x 10 texts x 3 temps, grouped as
# 15 cells x 10 records (cell = one (temp, (top_p, top_k)) combo applied to
# the owning pair's 10-sentence set). rep_pen held at shipped 1.05 for every
# record (architect: "rep_pen is held at 1.05 for all primary cells"; the two
# secondary rep-pen cells 14-15 are deferred -- deviation documented in the
# batch-1 code report, rationale: full temp-0.0 coverage for all 5 pairs
# (user's core 0.0/0.1/0.3 question) takes precedence within the fixed
# 150-run budget).
#
# Per-record sampler tokens (top_p=/top_k=/rep_pen=) ride on the temp_probe
# manifest extension; at temp=0.0 the probe builds llama_sampler_init_greedy()
# and never reads them (temp_probe.cpp L306-307 == engine.cpp L1002-1020), so
# cells 1-5 double as the architect's combo-inertness proof (cells 1-3).
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
CORPUS = ROOT / "probe_corpus.md"
MANIFEST = ROOT / "probe_manifest.txt"
CELLS_MD = ROOT / "probe_manifest_cells.md"

# English names exactly as the app registry injects them into BuildPrompt
# (src/config.cpp L40-53 kAllLanguages name_en column).
PAIRS = [
    ("P1", "FR", "French", "Arabic", "FR->AR"),
    ("P2", "DE", "German", "Chinese Simplified", "DE->ZH"),
    ("P3", "EN", "English", "Russian", "EN->RU"),
    ("P4", "ES", "Spanish", "Japanese", "ES->JA"),
    ("P5", "KO", "Korean", "Vietnamese", "KO->VI"),
]
# Home (top_p, top_k) per pair: P1 keeps the shipped/official values; P2..P5
# rotate the remaining grid combos (architect §2.3.2 combos).
HOME = {
    "P1": (0.60, 20),  # Hy-MT2 official (0.6, 20)
    "P2": (0.80, 20),  # tuned-1
    "P3": (0.60, 40),  # tuned-2
    "P4": (0.90, 50),  # llama-general
    "P5": (0.95, 40),  # llama-general-alt
}
TEMPS = [(0.0, "t0"), (0.1, "t1"), (0.3, "t3")]
SLOTS = ["L1", "L2", "L3", "L4", "L5", "S1", "S2", "S3", "S4", "S5"]
REP_PEN = 1.05  # shipped value, held constant (architect §2.3.2 note)

ID_RE = re.compile(r"^# ((?:FR|DE|EN|ES|KO)-(?:L[1-5]|S[1-5]))\s*$")
CLOSERS = "\"')]}»」'"

def count_sentences(text: str) -> int:
    # 1. mask dots inside email-bearing tokens
    masked = []
    for tok in text.split(" "):
        if "@" in tok:
            masked.append(tok.replace(".", "\x00"))
        else:
            masked.append(tok)
    s = " ".join(masked)
    # 2. mask decimal dots (digit.digit) -- covers 12.4MB / 0.4퍼센트 styles
    s = re.sub(r"(?<=\d)\.(?=\d)", "\x00", s)
    # 3. terminator counts only if, skipping closing quotes/brackets, the
    #    next char is whitespace or end-of-text (so `?"라며` is embedded,
    #    `?" ` / `format ? »` / `migration." ` terminate).
    n = 0
    i, L = 0, len(s)
    while i < L:
        ch = s[i]
        if ch in ".!?…":
            j = i + 1
            while j < L and s[j] in CLOSERS:
                j += 1
            if j >= L or s[j].isspace():
                n += 1
                i = j
                continue
        i += 1
    return n

def parse_corpus() -> dict:
    blocks, cur_id, body = {}, None, []
    for raw in CORPUS.read_text(encoding="utf-8").splitlines():
        line = raw.rstrip("\r")
        m = ID_RE.match(line)
        if m:
            if cur_id:
                blocks[cur_id] = " ".join(x.strip() for x in body if x.strip())
            cur_id, body = m.group(1), []
        elif line.startswith("#"):
            continue  # comment line: never body, also closes nothing open
        elif cur_id is not None:
            body.append(line)
    if cur_id:
        blocks[cur_id] = " ".join(x.strip() for x in body if x.strip())
    return blocks

def main() -> int:
    blocks = parse_corpus()
    expected = {f"{lang}-{slot}" for _, lang, _, _, _ in PAIRS for slot in SLOTS}
    missing = sorted(expected - set(blocks))
    extra = sorted(set(blocks) - expected)
    if missing or extra:
        print(f"FATAL corpus block mismatch missing={missing} extra={extra}")
        return 2
    if any("\x00" in t for t in blocks.values()):
        print("FATAL NUL byte leaked into a block text")
        return 2

    stats, failures = [], []
    for pid, lang, _, _, _ in PAIRS:
        for slot in SLOTS:
            text = blocks[f"{lang}-{slot}"]
            ns, nw = count_sentences(text), len(text.split())
            kind = "long" if slot.startswith("L") else "short"
            ok = (8 <= ns <= 12) if kind == "long" else (ns == 1)
            if not ok:
                failures.append(f"{lang}-{slot}: sentences={ns} (kind={kind})")
            if kind == "short" and nw < 12:
                failures.append(f"{lang}-{slot}: short word count={nw} < 12")
            stats.append((f"{lang}-{slot}", kind, ns, nw, len(text.encode('utf-8'))))

    records = []  # (cell_no, cell_label, header, text)
    cell_no = 0
    for temp, tcode in TEMPS:
        for pid, lang, src_en, tgt_en, pair_disp in PAIRS:
            cell_no += 1
            top_p, top_k = HOME[pid]
            label = (f"CELL {cell_no}: temp={temp} top_p={top_p} top_k={top_k} "
                     f"rep_pen={REP_PEN} pair={pair_disp} ({pid} 10-sentence set)")
            for slot in SLOTS:
                ns = count_sentences(blocks[f"{lang}-{slot}"])
                header = (f"@@@RUN id={pid}-{slot}-{tcode} pair={pair_disp} "
                          f"src={src_en} tgt={tgt_en} sent={ns} temp={temp} "
                          f"top_p={top_p:.2f} top_k={top_k} rep_pen={REP_PEN:.2f}")
                records.append((cell_no, label, header, blocks[f"{lang}-{slot}"]))

    assert len(records) == 150, f"expected 150 records, got {len(records)}"
    with MANIFEST.open("w", encoding="utf-8", newline="\n") as f:
        f.write("# probe_manifest.txt -- session 260913_0002 REQ-005 grid\n")
        f.write("# 150 RUN records = 5 pairs x 10 texts x 3 temps; 15 cells x 10.\n")
        f.write("# Generated by tools/generate_probe_manifest.py; UTF-8 no BOM.\n")
        prev_cell = 0
        for cell_no, label, header, text in records:
            if cell_no != prev_cell:
                f.write(f"\n# === {label} ===\n")
                prev_cell = cell_no
            f.write(header + "\n")
            f.write(text + "\n")
            f.write("@@@END\n")

    with CELLS_MD.open("w", encoding="utf-8", newline="\n") as f:
        f.write("# Probe manifest cell index (15 cells x 10 runs = 150)\n\n")
        f.write("| Cell | temp | top_p | top_k | rep_pen | Pair | Texts |\n")
        f.write("|---|---|---|---|---|---|---|\n")
        seen = {}
        for cell_no, label, header, _ in records:
            if cell_no not in seen:
                m = re.match(r"@@@RUN id=(P\d)-", header)
                if m:
                    seen[cell_no] = (label, m.group(1))
        for cell_no, (label, pid) in sorted(seen.items()):
            parts = dict(p.split("=", 1) for p in label.split() if "=" in p)
            f.write(f"| {cell_no} | {parts['temp']} | {parts['top_p']} | "
                    f"{parts['top_k']} | {parts['rep_pen']} | "
                    f"{dict((p[0], p[4]) for p in PAIRS)[pid]} | 10 |\n")

    print(f"OK: 50 blocks, {len(records)} records, {cell_no} cells -> {MANIFEST.name}")
    print(f"{'ID':<8}{'kind':<6}{'sent':<6}{'words':<7}{'utf8_bytes':<11}")
    for sid, kind, ns, nw, nb in stats:
        print(f"{sid:<8}{kind:<6}{ns:<6}{nw:<7}{nb:<11}")
    if failures:
        print("VALIDATION FAILURES:")
        for x in failures:
            print("  " + x)
        return 2
    print("VALIDATION: all long blocks 8-12 sentences; all shorts exactly 1 sentence.")
    return 0

if __name__ == "__main__":
    sys.exit(main())
