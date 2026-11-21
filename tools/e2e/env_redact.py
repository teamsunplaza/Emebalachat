#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Environment-neutral redaction of user-identifying local paths.

261122_0001 A4 (F11.2, security seat SR5 F-07): the E2E harnesses printed and
persisted ABSOLUTE paths, and the persisted run-manifest.json carried them into
`docs/` reports and pasted transcripts - which embeds the operator's Windows
account name (C:\\Users\\<name>\\...) in every artifact.

`redact_user_path()` rewrites the profile-anchored part of a path (or of any
free text that contains one) to a %ENV%-style token, so a transcript stays
byte-comparable across machines while no account name survives:

    C:\\Users\\alice\\AppData\\Local\\Emebalachat\\config.json
        -> %LOCALAPPDATA%\\Emebalachat\\config.json
    C:\\Users\\alice\\OneDrive\\Projects\\x\\build\\Emebala_chat.exe
        -> %USERPROFILE%\\OneDrive\\Projects\\x\\build\\Emebala_chat.exe
    D:\\CI\\emebala\\build\\Emebala_chat.exe   (no profile segment)
        -> unchanged (it identifies a checkout, not a person)

Only WHO identifies the path is removed; the shape stays readable, which is
what the manifest needs (a `%USERPROFILE%\\...` value still tells a reviewer
that the run used the default profile layout). Stdlib only, Python 3.8+.
"""

import os
import re

# (env var, token). LOCALAPPDATA/APPDATA/TEMP normally live UNDER USERPROFILE,
# so the replacement order is longest-prefix-first (computed below) - otherwise
# %LOCALAPPDATA% would always be rewritten as %USERPROFILE%\AppData\Local.
_PROFILE_TOKENS = (
    ("USERPROFILE", "%USERPROFILE%"),
    ("LOCALAPPDATA", "%LOCALAPPDATA%"),
    ("APPDATA", "%APPDATA%"),
    ("TEMP", "%TEMP%"),
    ("TMP", "%TEMP%"),
)

# Fallback for a profile that is NOT the current user's (another drive, a UNC
# share, a saved log from a different operator). Well-known machine profiles
# are skipped so "%USERPROFILE%\\Public\\..." style noise is not invented.
_USERS_RE = re.compile(
    r"(?i)(?:[A-Za-z]:)?[\\/]+(?:Users|Documents and Settings)[\\/]"
    r"(?!(?:Public|Default|Default User|All Users)\b)[^\\/\s'\"]+")


def _root_pairs():
    """(absolute prefix, token) pairs, longest prefix first."""
    pairs = []
    for var, token in _PROFILE_TOKENS:
        raw = os.environ.get(var) or ""
        if len(raw) > 3:  # never rewrite a bare "C:\" or an empty var
            pairs.append((raw.rstrip("\\/"), token))
    home = os.path.expanduser("~")
    if len(home) > 3:
        pairs.append((home.rstrip("\\/"), "%USERPROFILE%"))
    return sorted(pairs, key=lambda p: len(p[0]), reverse=True)


def redact_user_path(value):
    """Return `value` with every profile-anchored path rewritten to a token.

    Accepts a bare path OR any text containing paths (a manifest note, an
    exception message); non-str input is returned untouched so a caller can
    pass None/ints through without a type check. Idempotent: a value that is
    already tokenized has no profile prefix left to match.
    """
    if not isinstance(value, str) or not value:
        return value
    out = value
    for root, token in _root_pairs():
        # A separator (or end of string) must follow, so C:\Users\bob2 is not
        # mistaken for C:\Users\bob.
        pattern = re.escape(root) + r"(?=[\\/]|$)"
        out = re.sub(pattern, token, out, flags=re.IGNORECASE)
    return _USERS_RE.sub("%USERPROFILE%", out)


def redact_mapping(mapping):
    """Redact every string value of a flat dict (manifest section helper)."""
    return {k: redact_user_path(v) for k, v in mapping.items()}


if __name__ == "__main__":  # manual probe: python env_redact.py <path>...
    import sys

    for arg in sys.argv[1:]:
        print(redact_user_path(arg))
