"""P4 B-3 authoring gate: count aggregate-initializer fields per locale table.

Parses every `const LocalizedStrings kStringsX = { ... };` block in
src/i18n.cpp, splits the initializer at top-level commas (outside string
literals), and counts the segments. Adjacent string-literal pieces (the
multi-line cheat-sheet bodies) form ONE field each. Every table MUST have
exactly 47 fields (the struct's member count) or the aggregate would compile
with value-initialized nullptr tails - the silent-killer this probe pins.
"""
import re
import sys

SRC = "src/i18n.cpp"
EXPECTED = 47  # struct LocalizedStrings members; == StringId::EnumCount

def split_fields(body: str):
    fields = []
    depth = 0
    cur = []
    i = 0
    n = len(body)
    while i < n:
        c = body[i]
        if c == '"':
            # consume string literal (handles \" escapes)
            cur.append(c)
            i += 1
            while i < n:
                if body[i] == "\\":
                    cur.append(body[i])
                    if i + 1 < n:
                        cur.append(body[i + 1])
                    i += 2
                    continue
                cur.append(body[i])
                if body[i] == '"':
                    i += 1
                    break
                i += 1
            continue
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
        elif c == "," and depth == 0:
            fields.append("".join(cur))
            cur = []
            i += 1
            continue
        cur.append(c)
        i += 1
    fields.append("".join(cur))
    return [f for f in fields if f.strip()]

def main():
    text = open(SRC, encoding="utf-8").read()
    tables = re.findall(r"const LocalizedStrings (kStrings\w+) = \{(.*?)\n\};", text, re.S)
    print(f"tables found: {len(tables)}")
    bad = 0
    for name, body in tables:
        fields = split_fields(body)
        mark = "OK " if len(fields) == EXPECTED else "BAD"
        if len(fields) != EXPECTED:
            bad += 1
        print(f"{mark} {name}: {len(fields)} fields")
    print("RESULT:", "PASS" if bad == 0 and len(tables) == 37 else f"FAIL bad={bad} count={len(tables)}")
    sys.exit(0 if bad == 0 and len(tables) == 37 else 1)

main()
