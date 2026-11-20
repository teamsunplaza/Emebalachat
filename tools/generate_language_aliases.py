#!/usr/bin/env python3
"""261116_0003 P4 (language-menu redesign) - generate src/language_alias.cpp.

WHAT THIS EMITS
    The CLDR-derived multi-locale alias matrix (89 UI locales x 89 registry
    targets + the AUTO pseudo-language), the hand-curated script-name table
    (plan D4), and the folded-text lookup table used by the search matcher
    (src/language_alias.hpp). The emitted file is static `const` data only: no
    CLDR JSON, no network access and no Python is needed at build or run time.

    Since 261119_0001 1B the matrix indexes ALL 89 UI locales of
    src/i18n.cpp kLocaleMappings (1A widened the table from 37 to 89 rows).
    Two of those languages have no `cldr-localenames-full` directory at the
    pinned CLDR tag: ht (Haitian Creole) and gom (Goan Konkani - CLDR carries
    Konkani under "kok", but there is no Goan-Konkani UI locale file). They are
    declared in SNAPSHOT_UNAVAILABLE_LOCALES, keep their slot in
    kLanguageAliasLocales (the locale list stays index-parallel to
    kLocaleMappings), and contribute no exonym column: every other locale's
    data is unaffected.

ONE-TIME SNAPSHOT (the exact fetch command)
    The snapshot is the unicode-org/cldr-json repo at the pinned release tag,
    one `languages.json` per UI locale:

        python tools/generate_language_aliases.py --fetch \\
            --snapshot-dir tools_tmp_261119_cldr

    which downloads, per CLDR locale id <loc>, exactly this URL with Python's
    urllib.request (standard library - no curl or other tool is involved):

        https://raw.githubusercontent.com/unicode-org/cldr-json/49.0.0/\\
cldr-json/cldr-localenames-full/main/<loc>/languages.json

    Fetched AND cached bytes are verified against the pinned
    EXPECTED_SNAPSHOT_SHA256 manifest below (fail-closed, PR4-05), so a
    tampered snapshot can never reach the generated C++. A locale declared in
    SNAPSHOT_UNAVAILABLE_LOCALES has no file to verify: --fetch does not even
    request it, and a file appearing for it is a hard error (stale declaration).

    (`cldr-localenames-full` is the alias-resolved package: a language name
    CLDR does not translate falls back to root inside the JSON, so every value
    we read is a real CLDR name for that locale.)

DETERMINISM
    Same snapshot files in, byte-identical `src/language_alias.cpp` out. The
    provenance header carries CONSTANTS (tag, release date, snapshot fetch
    date), never `datetime.now()`, so a regeneration months later is still
    byte-identical. `--check` re-renders and diffs against the file on disk
    (exit 1 on drift) - use it as a CI gate after any manual edit.

    The snapshot files' SHA-256 digests are embedded in the emitted file's
    provenance block, so the committed table can be audited against the exact
    CLDR bytes it came from.

PROVENANCE / LICENSE
    CLDR data is Unicode-licensed: Copyright (c) 1991-2025 Unicode, Inc. All
    rights reserved. Distributed under the Terms of Use in
    https://www.unicode.org/copyright.html (Unicode License v3). Only the
    DERIVED language-name strings are embedded (plan section 4 / risk list:
    "embed the generated table, not the dependency").

WHAT IS NOT GENERATED
    The matcher itself (folding + ranking + SearchLanguages) is hand-written in
    src/language_alias.hpp and deliberately outside this generator's output:
    regeneration must never be able to clobber reviewed logic.

USAGE
    python tools/generate_language_aliases.py                    # render from the cached snapshot
    python tools/generate_language_aliases.py --fetch            # fetch missing snapshot files first
    python tools/generate_language_aliases.py --check            # fail if the file on disk is stale

    Options: --snapshot-dir DIR, --out PATH, --repo-root DIR.
    Python 3.8+, standard library only.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import sys
import unicodedata
import urllib.error
import urllib.request

# ---------------------------------------------------------------------------
# Pinned CLDR snapshot (provenance constants - deliberately NOT "today")
# ---------------------------------------------------------------------------
CLDR_TAG = "49.0.0"
CLDR_RELEASE_DATE = "2026-10-22"
# 261119_0001 1B: the snapshot dir is new (tools_tmp_261119_cldr) and every
# file in it was fetched on this date, so the constant is the real fetch date
# of the bytes this file embeds - not "today" at render time.
SNAPSHOT_FETCH_DATE = "2026-11-19"
CLDR_RAW_URL = (
    "https://raw.githubusercontent.com/unicode-org/cldr-json/"
    "{tag}/cldr-json/cldr-localenames-full/main/{locale}/languages.json"
)
# 261119_0001 FIX-R1 R1-09: the fetched body is streamed and capped before the
# digest pin runs (verification happens after transfer, so an un-capped read()
# let a compromised pinned endpoint exhaust dev-machine memory). Real snapshots
# are ~200-400 KiB; 8 MiB is ~20x the largest one.
MAX_SNAPSHOT_BYTES = 8 * 1024 * 1024
SNAPSHOT_READ_CHUNK = 64 * 1024

# ---------------------------------------------------------------------------
# Pinned supply-chain expectations (261116_0003 P4 fix round, PR4-05).
# ---------------------------------------------------------------------------
# Expected SHA-256 of every pinned snapshot file. These are the same digests
# the generated file records in its provenance block, promoted to a committed
# pin: fetch AND load verify bytes against this manifest and fail closed on a
# mismatch, so a tampered or re-pointed snapshot can never be rendered into
# src/language_alias.cpp (the embedded digest block alone could not detect
# this - it was computed from the very bytes it certified).
EXPECTED_SNAPSHOT_SHA256 = {
    "ko": "8cb88085df66df3eac87402f0f6d41ea18a54af62e0c6c2d1c1e32ae38e2f10d",
    "en": "6180f817207decbc0f9cd0deb34ed0bad3256ff4d03c3c4252706285e8636b88",
    "vi": "7ae2140f647769aa48ca7642c9855fb34cb7ec38b6c5b24e9f4c9bde07e9d8dd",
    "zh": "d48013fa97860489f72d7ed1fc870d6c27aa3de87581694ce2991ff0271cbf7e",
    "zh-Hant": "f8162cee42121fdffb9b36ed20ccb85193559732d0b69422daf335c3d9a4b450",
    "ja": "c3ad61ac4738e2879e116c16a1a8d20f610941dfc7544e75b84a027a4d068715",
    "es": "e2d84c932d359372c395fbee876f822158430632f51c3c025209600eab068841",
    "fr": "49c4f489e321f62ec64e3950045225a6aa2d4accc71584dfde262083679215d9",
    "de": "712b2659074ef04e75e54c0cb7502e3a63dcd473c47e989a65300175b6ef9f83",
    "ru": "0a07b689bb212c78fd3865e4c3035922b2b3d68c1ea941fd658461c6e118fd70",
    "th": "e5fe0944c5d7183c6fbd3f1f55fbdad59d5be28366343e62e822eba3cc98b05d",
    "ar": "8d58b2577ce5334298fa64d12ce76ab41e945e1008067ee96a470bae05714ddf",
    "pt": "d20672dba7ed5ba3bfb89e8d2a969a2de0cde858ce7265aa03695e8ec14e77f8",
    "it": "b7d56e70b5d867b44f5e177919fe1f08019cae2e714eefa7c9805fb2851e32c4",
    "id": "dad7be179313dde259ba0d7d16bee8ccd6934a138dd52c6bd1606f8198a29ae5",
    "ms": "3c419a519887ce9bb48e70b085616eeeaf62d622357dce7255d85562d736e200",
    "fil": "fac111d0309e805497dd3a432ed8589eb47952884a9aafb3438bc2abdd696a72",
    "km": "3e6c61ddf4a553b778b1485c905b36150adbba2adf34215b5418211073d078a2",
    "lo": "d1574c7fa6965f050f23914384829eb3b4eb6a8b92390ec06dc26c7a2bc96599",
    "hi": "ab7dae0561a31c55fd9ca6698d6483482f831cf5c68a3342b22fae2b994ec8ed",
    "bn": "ebf1b6bd9465f67be42b18e37b2dc9970c58ae1ffe9b42246eb4834cdc48cdda",
    "tr": "d288817b707464b59a566ce058feef127eead2c18f2eebfca7f90607e48c29ad",
    "pl": "4717ff668c5bfcc5dc883d4cd94fa73b21934892ed8f76b4eb55794b600909c0",
    "nl": "a476582e90ec89b30f6d75e3a8012680be3cf0c3e17e6fcfea850ea9d8b7aeda",
    "uk": "7d8d43350725a292e84a7ad08e02d7fe6679b4c67507d73538d0bb138006d7d8",
    "fa": "f5344a7949ab39fe7b405959ec3ad684d14ff46ff64e73dd879e15de58322a0e",
    "ur": "f61f476f62ed2b5e003dc0444e222e6f315d8c1e43e38dde2c297a59d9b6dfac",
    "he": "56e7e7734e8768041ff907a15bb6612083cf0779ec5d693293f4f34ab2f693ee",
    "cs": "eeb27189ca92179bb387a70b8ecd66eebf4cf41cea60cb33393b1b6a7a7e092b",
    "hu": "203ee8cce0b847dd4504eae0c1b3e8b458b497383343d9a9f435e8db54ea7674",
    "sv": "60d69b254abf8c194c115a7d021b12eeef56e19b5840f66c8c0836930a09bcf1",
    "el": "f71b8546eadcff91c04bdc4551934af2b93304a5f9dc6594e009d7d8126e41b9",
    "ro": "89e6b9df1d1b82fa204801909f4b72b4bd248eed1c9561a4a8cff0a58c05404a",
    "da": "9e22114d29b76b72927a0fceee4871c2c73c3228716aafa0a3f7044991c030dd",
    "fi": "eb017af857a5ece0040771e8c9e8b63df1c2b78daaf086402384b0280643b351",
    "no": "14a7e1bd43f0d84fc2201e40fe832f7a9ee17d723414c8a60ee49bb89ae0a52c",
    "my": "67b9cd8977c5bcf6b077faf2e1bc1012bcbf49671b2016499dc5a810d8abd53e",
    # ---- 261119_0001 1B: the 50 additional UI locales whose CLDR file
    # exists at 49.0.0 (real digests of the fetched bytes; the other two new
    # UI locales, ht and gom, are declared in SNAPSHOT_UNAVAILABLE_LOCALES).
    "gu": "64a8bd1001191eb54f98a67ed8220210937fb1af0cf8089e391fd0a9673061b5",
    "te": "6ae0e94b6f541455c2f59afa8714a65faf95b067cd32799b8712893e8003c2dd",
    "mr": "edb168f95906826a92ca5412b47573fdc197da79e7443283f31f6cc137717af6",
    "ta": "2f1f54f0c18618e1ff9281b1bea1d4b6efd18cf1f2acf3184a2f538d184554f2",
    "bo": "02b813d3196278c37817b57d5e7d2ceef585448bb82164ca6ffe33c1fb7a1b71",
    "kk": "68dad17b403d1b220a0d44ef342abb439264b3d44fda2edd11a06d68917f9e6b",
    "mn": "66d0e7c1a53e4171c1c032ef73f855a8f4bc7a06ca6a2d55fff687246fb63a7f",
    "ug": "66012d554301e67490a972e5a33e49908586951d1f4afa8193de50e8d015e706",
    "yue": "cbb3993e679ff084c48b2aa804e1e3006799aa152193207b3636d0bd22194737",
    "bs": "ce68b4aaf6077b558a8ae85538e57c2619822df845902b8f8f908eeb5f985d23",
    "sr": "e904cb592f8488a2932a4f55577829e7b4a34bb0587107ea220e4a2816b21ea2",
    "hr": "a5cf5df15a4f4dbafbc019f847694770ccb7e0b12d7b5b1553d1189b7ca8660d",
    "sl": "76ff6882cbea7a23ead331261ec56f3c4209da5b768f1c0a8f0b166a64d18176",
    "sk": "d95394366dc6da039ae61eb4c651cb0c849672fc255bc0fdbd287137bbb915a5",
    "bg": "79c5187eaf6c09be2cec9e86f3c1c8f200ceb177c573111620c07437d855b393",
    "lt": "770bd1632cd56af8a02f908bb3ac825cc4d1d38e0db0804376c87335757c1176",
    "lv": "c4dd050fc413b584949128301c3e1653ab0257fd8345fcd5df616267c4a11e66",
    "et": "4dcf0c78eaa808a635ae90687e546c26222540dabc9048fb9c86567c93c2be6b",
    "af": "b6990a0b3a5b6784fb4374ca88b766bd07d8f79ba11acf3710b360b990ed1e86",
    "ca": "ccf535332483220cbb38f35d03d63cd93433b0b843ba5aaddfa6d021ec5ef4b8",
    "is": "c731876ae51dcc3c67b5ea571cf5166d66eef7187116fa34cc136a40e88fa19f",
    "sq": "a830abfd41f1606e8867255b8ed70804ba28a17da9b1d330f66c1f8ba856bef8",
    "mk": "5f7646837aa12909eab55789a775aa0c2f69ebc7630294d05fa8f36f634aa6e3",
    "cy": "d99197d532e6323a266decbeb6a6643098f9eb2076c6077ef8dcf6a9b7e0e6b8",
    "ga": "3c5bb4b881f318f3ccc19a26de187a54af9b6878fce2b1e61e3eb37cc1fa61e5",
    "eu": "d7dffd19b8fb14fece3549a3dd0a15168ee93cbfcd15f5509c946278a24653c7",
    "gl": "8b970faac13033b3f2b7964b37fd9f231d4454c44ed5f54f1e31be6b61c5d0ce",
    "sw": "70e586e2fa969f70e7dad9cb75a89c0cbd3bf9971d341ef20289213ccd19065d",
    "uz": "6fce68f74765433d0e2d83fe40648bfe1d8aa0a1e4d5cdf9be3803f29faa61bc",
    "az": "5c065dfe319e0d1dc4a648ecbf7ed2af4334603e7406c01fe0ce09a4159b59f6",
    "be": "9be33eff6733d46925415ceaa61ee6a84a4e994224d60a67dfb1eefa1819384f",
    "ky": "6d96217bfd984d25850ac0a884f75d5be41d776f795aec99ec1a9e1821fbcb76",
    "tg": "520d6aa6a73358acabdce2e4cb6d911b8011c6584bf809bc47675de15620e627",
    "eo": "f36bcdb9fa08f2182fe4abcc8660434962549d19f858729e5144b4c4016cdf64",
    "hy": "3596a4f3306d76f1c2bc14708c0f6d2115c93eaf93a9a834a4e3f00488feedd8",
    "ka": "7a9d54e2ece2f89f2b39a81424c9ad97f123fcc5fa335c4855706c7852e53edf",
    "si": "d558991ce457254f3d4f9edb9b3e9a1bd947921d1af979e3fc1e7e9531a9218a",
    "ml": "e28a482a6f76f89a7e528ab8a31f34a716ceb4b440cf29f11136153dfbd9b5d5",
    "kn": "e2a206df9f4f6c9ad144f921a7035cad35c85e21c7fdd55ffc884658838ad294",
    "pa": "ba4492cff38c39f5b0dd2afb8915ce3198eed95ba30ec825c531f0a71123b2b5",
    "or": "e5910172959b8e4067a2545e70c8920341ddfd96325b5ddff0e00788ae846240",
    "am": "1946c9bdcdf694c7209485231fc0f59b7f8b060fa9be7473f643d80b30544336",
    "ti": "0b3c473a354e04c577463755e74c7f49c345888ae1c2c04a49d93ad742a42e7b",
    "ps": "0da884f1c97a0690502862a681f8460207c7f8532c18ed60a42ee25cbec44146",
    "sd": "4fa15df6187d80bda1d18287741d3e06d952ec8cbb1036866ef7294a4c403bb8",
    "ne": "f79f21375575a7c774b9d3e107e709fc2667d245bb6d27769371c4ab9c979ee7",
    "as": "553053b31828bda1224aded7c27f188dd757308bc879fb86a59ff51349da834f",
    "dz": "fcee351df427b50191e503355f88a61f37c5ae4b4e0facfbeed66ece89a4da50",
    "sa": "9acaeb1d4caf31fff70a78d66173f4ea2410ae6d8b60096bf37e461dbb2cb1ac",
    "mai": "ddf4bcd3a0344013391e622e774e0085b0f703c9a5a916b4771a670306b5540a",
}

# UI locales whose `cldr-localenames-full/main/<id>/languages.json` does not
# exist at the pinned tag (verified: HTTP 404 on CLDR 49.0.0 for both, probed
# 2026-11-19). They are UI locales in every other respect - they keep their
# slot in kLanguageAliasLocales, index-parallel to kLocaleMappings - but have
# no CLDR name data, so they contribute no exonym column and no digest entry.
# Declaring a locale here is the ONLY documented fallback for a missing
# UI-locale file: no data is fabricated for it, and every other locale's column
# is rendered exactly as if this locale did not exist. Remove an entry only
# together with a real manifest digest once CLDR ships the file.
SNAPSHOT_UNAVAILABLE_LOCALES = {
    # Haitian Creole: no "ht" directory (CLDR carries no Haitian locale names).
    "ht",
    # Goan Konkani: no "gom" directory (Konkani names live under "kok", which
    # would be a DIFFERENT language view - the registry's GOM target already
    # reaches those names through CLDR_KEY_FALLBACKS, but a gom UI column
    # cannot be sourced from kok without lying about the locale).
    "gom",
}

# The Unicode version the NFD-derived fold entries were derived from. Pinned
# and asserted at generation time (PR4-05): a newer Python could silently
# change which codepoints NFD-strip to an ASCII base, so the version must be
# re-derived and re-pinned deliberately, never drift. NOTE: 14.0.0 is what
# CPython 3.11 reports; coder-report-p4.md once claimed 16.0.0 - the artifact
# (14.0.0) was always right and the report claim was corrected in the P4
# fix-round report.
EXPECTED_UNICODATA_VERSION = "14.0.0"

DEFAULT_SNAPSHOT_DIR = "tools_tmp_261119_cldr"
DEFAULT_OUT = "src/language_alias.cpp"

# ---------------------------------------------------------------------------
# The app's own tables (parsed, never duplicated - drift is a hard error)
# ---------------------------------------------------------------------------
CONFIG_CPP = os.path.join("src", "config.cpp")
I18N_CPP = os.path.join("src", "i18n.cpp")

# kAllLanguages row: {"CODE", "name_en", "name_native", "display_short", "bcp47", MASK},
CONFIG_ROW_RE = re.compile(
    r'^\s*\{\s*"([^"\\]+)"\s*,\s*"([^"\\]*)"\s*,\s*"([^"\\]*)"\s*,\s*"([^"\\]*)"\s*,'
    r'\s*"([^"\\]*)"\s*,\s*([A-Za-z0-9_|\s]+?)\s*\}\s*,?\s*$'
)
# kLocaleMappings row: { UiLocale::Korean, "ko", L"ko", nullptr, L"ko-KR", LANG_KOREAN, true },
I18N_ROW_RE = re.compile(r'^\s*\{\s*UiLocale::([A-Za-z]+)\s*,\s*"([^"\\]+)"\s*,')

EXPECTED_TARGET_COUNT = 89
EXPECTED_UI_LOCALE_COUNT = 89

# config_code (i18n LocaleMapping.config_code) -> cldr-localenames directory id.
# Traps: the two Chinese UI locales are CLDR's script trees ("zh" is the
# Simplified/default tree, "zh-Hant" the Traditional one - there is no "zh-CN"
# or "zh-TW" directory); Norwegian is CLDR's macro locale "no" (the "nb"
# directory carries the same names; "no" is the canonical one). Cantonese
# (261119_0001 1B) sits in the same family but NOT as a trap any more: its
# config code is "yue" - the registry/CLDR language key itself (identity
# mapping since 261119_0001 FIX-R1 R1-01; the config code used to be the
# DWrite tag "zh-HK", which never joined the registry's "YUE" row). Its BCP-47
# tags stay zh / zh-HK in i18n.cpp, so the UI column is still the
# Cantonese-language view - "zh-Hant-HK" would be a Chinese view, not this
# locale. Every remaining code is its own CLDR id.
CLDR_LOCALE_FOR_CONFIG_CODE = {
    "ko": "ko",
    "en": "en",
    "vi": "vi",
    "zh-CN": "zh",
    "zh-TW": "zh-Hant",
    "ja": "ja",
    "es": "es",
    "fr": "fr",
    "de": "de",
    "ru": "ru",
    "th": "th",
    "ar": "ar",
    "pt": "pt",
    "it": "it",
    "id": "id",
    "ms": "ms",
    "fil": "fil",
    "km": "km",
    "lo": "lo",
    "hi": "hi",
    "bn": "bn",
    "tr": "tr",
    "pl": "pl",
    "nl": "nl",
    "uk": "uk",
    "fa": "fa",
    "ur": "ur",
    "he": "he",
    "cs": "cs",
    "hu": "hu",
    "sv": "sv",
    "el": "el",
    "ro": "ro",
    "da": "da",
    "fi": "fi",
    "no": "no",
    "my": "my",
    # ---- 261119_0001 1B: the 52 UI languages added by phase 1A. Identity
    # mapping except Cantonese (see the trap note above) and the two locales
    # declared in SNAPSHOT_UNAVAILABLE_LOCALES (ht, gom) - the mapping is
    # still required for them because it is what proves the locale id is
    # deliberate, and their slot stays in the rendered locale list.
    "gu": "gu",
    "te": "te",
    "mr": "mr",
    "ta": "ta",
    "bo": "bo",
    "kk": "kk",
    "mn": "mn",
    "ug": "ug",
    "yue": "yue",
    "bs": "bs",
    "sr": "sr",
    "hr": "hr",
    "sl": "sl",
    "sk": "sk",
    "bg": "bg",
    "lt": "lt",
    "lv": "lv",
    "et": "et",
    "af": "af",
    "ca": "ca",
    "is": "is",
    "sq": "sq",
    "mk": "mk",
    "cy": "cy",
    "ga": "ga",
    "eu": "eu",
    "gl": "gl",
    "sw": "sw",
    "ht": "ht",
    "uz": "uz",
    "az": "az",
    "be": "be",
    "ky": "ky",
    "tg": "tg",
    "eo": "eo",
    "hy": "hy",
    "ka": "ka",
    "si": "si",
    "ml": "ml",
    "kn": "kn",
    "pa": "pa",
    "or": "or",
    "am": "am",
    "ti": "ti",
    "ps": "ps",
    "sd": "sd",
    "ne": "ne",
    "as": "as",
    "dz": "dz",
    "sa": "sa",
    "mai": "mai",
    "gom": "gom",
}

# registry code -> CLDR language key.  Derived from LanguageInfo::bcp47 (the
# CLDR key IS the bcp47 subtag for 86 of the 89 targets); every divergence is
# enumerated here and re-verified by `verify_key_overrides` at run time.
CLDR_KEY_OVERRIDES = {
    # CLDR splits Chinese by SCRIPT, not by region: the bcp47 tags we feed to
    # DWrite ("zh-CN"/"zh-TW") are not CLDR keys, the script keys are.
    "ZH-CN": "zh-Hans",
    "ZH-TW": "zh-Hant",
    # YUE's LanguageInfo::bcp47 is the DWrite tag "zh-HK" (the 261116_0002
    # locale-probe trap), which is not a CLDR language key; CLDR carries
    # Cantonese under "yue".
    "YUE": "yue",
    # GOM (Goan Konkani): CLDR has no "gom" key in ANY of the 37 locales
    # (verified on the 49.0.0 snapshot). The key is kept here for provenance;
    # CLDR_KEY_FALLBACKS below supplies the "kok" (Konkani) entry so the
    # language is still reachable by exonym.
    "GOM": "gom",
}

# Extra CLDR keys tried, in order, when the primary key is absent in a locale.
# Documented fallback chain (per locale):
#   1. CLDR_KEY_OVERRIDES.get(code) or bcp47   (the exact language name)
#   2. CLDR_KEY_FALLBACKS[code]               (long/alt form, then macro language)
#   3. LanguageInfo::name_en                  (English name - always present)
#   4. LanguageInfo::code                     (the code itself - always present)
CLDR_KEY_FALLBACKS = {
    "ZH-CN": ["zh-Hans-alt-long", "zh"],
    "ZH-TW": ["zh-Hant-alt-long", "zh"],
    "GOM": ["kok"],
}

# ---------------------------------------------------------------------------
# Hand-curated script-name table (plan D4 + the P3 script classes)
# One row per language, lowest rank tier.  Terms are indexed globally (a
# query is matched against every row), so a user typing a script name finds
# the language that writes it.
# ---------------------------------------------------------------------------
# Script terms shared by EVERY language that writes that script (the table is
# language-keyed; a shared term makes the script name find all of its
# languages, and registry order decides which one ranks first - so the D4
# representative (RU for Cyrillic, AR for Arabic, HI for Devanagari, ...) wins
# every tie).
ARABIC_SCRIPT_TERMS = ["arabic script", "arabic alphabet"]

SCRIPT_ALIASES = {
    "AUTO": [],
    # (AUTO's pseudo-language English extras live in EXTRA_ENGLISH below.)
    "EN": ["latin script", "latin alphabet"],
    "KO": ["한글", "hangul", "hangeul"],
    "VI": [],
    "ZH-CN": ["汉字", "简体字", "hanzi"],
    "ZH-TW": ["漢字", "繁體字", "hanzi"],
    "JA": ["kana", "かな", "カナ", "hiragana", "ひらがな", "katakana", "カタカナ"],
    "ES": [],
    "FR": [],
    "DE": [],
    "RU": ["cyrillic", "cyrillic script", "кириллица"],
    "TH": ["thai script", "อักษรไทย"],
    "AR": ARABIC_SCRIPT_TERMS,
    "PT": [],
    "IT": [],
    "ID": [],
    "MS": [],
    "FIL": [],
    "KM": ["khmer script"],
    "LO": ["lao script"],
    "HI": ["devanagari", "देवनागरी"],
    "BN": ["bengali script", "bangla script"],
    "TR": [],
    "PL": [],
    "NL": [],
    "UK": ["cyrillic", "кириллиця"],
    "FA": ARABIC_SCRIPT_TERMS,
    "UR": ARABIC_SCRIPT_TERMS,
    "HE": ["hebrew script", "hebrew alphabet", "כתב עברי"],
    "CS": [],
    "HU": [],
    "SV": [],
    "EL": ["greek script", "greek alphabet", "ελληνικό αλφάβητο"],
    "RO": [],
    "DA": [],
    "FI": [],
    "NO": [],
    "MY": ["myanmar script", "burmese script"],
    "GU": ["gujarati script"],
    "TE": ["telugu script"],
    "MR": ["devanagari", "देवनागरी"],
    "TA": ["tamil script"],
    "BO": ["tibetan script", "བོད་ཡིག"],
    "KK": ["cyrillic"],
    "MN": ["cyrillic"],
    "UG": ARABIC_SCRIPT_TERMS,
    "YUE": ["hanzi"],
    "BS": [],
    "SR": ["cyrillic", "ћирилица"],
    "HR": [],
    "SL": [],
    "SK": [],
    "BG": ["cyrillic", "кирилица"],
    "LT": [],
    "LV": [],
    "ET": [],
    "AF": [],
    "CA": [],
    "IS": [],
    "SQ": [],
    "MK": ["cyrillic", "кирилица"],
    "CY": [],
    "GA": [],
    "EU": [],
    "GL": [],
    "SW": [],
    "HT": [],
    "UZ": [],
    "AZ": [],
    "BE": ["cyrillic", "кірыліца"],
    "KY": ["cyrillic", "кириллица"],
    "TG": ["cyrillic"],
    "EO": [],
    "HY": ["armenian script"],
    "KA": ["georgian script", "ქართული დამწერლობა"],
    "SI": ["sinhala script", "sinhalese script"],
    "ML": ["malayalam script"],
    "KN": ["kannada script"],
    "PA": ["gurmukhi", "ਗੁਰਮੁਖੀ"],
    "OR": ["odia script", "oriya script"],
    "AM": ["ethiopic", "geez", "ግዕዝ"],
    "TI": ["ethiopic script"],
    "PS": ARABIC_SCRIPT_TERMS,
    "SD": ARABIC_SCRIPT_TERMS,
    "NE": ["devanagari", "देवनागरी"],
    "AS": ["bengali script"],
    "DZ": ["tibetan script"],
    "SA": ["devanagari", "देवनागरी"],
    "MAI": ["devanagari", "देवनागरी"],
    "GOM": ["devanagari", "देवनागरी"],
}

# Extra English-tier spellings for the AUTO pseudo-language (the registry name
# is "Auto Detect"; these make "automatic" / "detect" work verbatim).
EXTRA_ENGLISH = {
    "AUTO": ["Automatic", "Detect"],
}

# ---------------------------------------------------------------------------
# Folding tables (case-fold + diacritic fold + ASCII fold + Cyrillic/Greek
# transliteration + whitespace/invisible-char normalization)
# ---------------------------------------------------------------------------
# Codepoint ranges whose NFD decomposition yields an ASCII base letter.  Covers
# Latin-1 Supplement, Latin Extended-A/B, Latin Extended Additional (the
# Vietnamese tone block), and Latin Extended-C/D (folded in via the same rule
# where UnicodeData gives a clean ASCII base).
LATIN_DIACRITIC_RANGES = (
    (0x00C0, 0x00FF),
    (0x0100, 0x017F),
    (0x0180, 0x024F),
    (0x1E00, 0x1EFF),
    (0x2C60, 0x2C7F),
    (0xA720, 0xA7FF),
)

# Letters UnicodeData does NOT decompose (stroke/ligature letters): a
# hand-authored base is the only way to fold them.  Covers every one of the 89
# targets' orthographies (Polish l-stroke, Vietnamese d-stroke, Norwegian/
# Danish o-slash + ae, German sharp s, Icelandic eth/thorn, French oe, Turkic
# dotless i).
LATIN_FOLD_SPECIALS = {
    0x00C6: "ae", 0x00E6: "ae",
    0x00D0: "d", 0x00F0: "d",
    0x00D8: "o", 0x00F8: "o",
    0x00DE: "th", 0x00FE: "th",
    0x00DF: "ss",
    0x0110: "d", 0x0111: "d",
    0x0126: "h", 0x0127: "h",
    0x0131: "i",
    0x0138: "k",
    0x0141: "l", 0x0142: "l",
    0x014A: "n", 0x014B: "n",
    0x0152: "oe", 0x0153: "oe",
    0x0166: "t", 0x0167: "t",
    0x017F: "s",
    0x0180: "b", 0x0183: "b", 0x0188: "c", 0x018C: "d", 0x0192: "f",
    0x0199: "k", 0x019A: "l", 0x019E: "n", 0x01A5: "p", 0x01AB: "t",
    0x01AD: "t", 0x01B4: "y", 0x01B6: "z", 0x01DD: "e", 0x01F3: "dz",
    0x0221: "d", 0x0234: "l", 0x0235: "n", 0x0236: "t", 0x0237: "j",
    0x0238: "db", 0x0239: "qp",
    0x0247: "e", 0x0249: "j", 0x024B: "q", 0x024D: "r", 0x024F: "y",
    0x1D79: "g", 0x1D7D: "p", 0x1D89: "r", 0x1E96: "h", 0x1E97: "t",
    0x1E98: "w", 0x1E99: "y", 0x1E9A: "a", 0x1E9B: "s", 0x1E9E: "ss",
    0x1EFB: "t",
}

# Cyrillic -> Latin (approximate, search-only).  One entry per letter used by
# the registry's Cyrillic languages (RU UK BE BG MK SR KK KY TG MN).
CYRILLIC_FOLD = {
    # Russian / pan-Slavic block (U+0410-U+044F, both cases)
    0x0410: "a", 0x0430: "a", 0x0411: "b", 0x0431: "b", 0x0412: "v", 0x0432: "v",
    0x0413: "g", 0x0433: "g", 0x0414: "d", 0x0434: "d", 0x0415: "e", 0x0435: "e",
    0x0401: "e", 0x0451: "e", 0x0416: "zh", 0x0436: "zh", 0x0417: "z", 0x0437: "z",
    0x0418: "i", 0x0438: "i", 0x0419: "j", 0x0439: "j", 0x041A: "k", 0x043A: "k",
    0x041B: "l", 0x043B: "l", 0x041C: "m", 0x043C: "m", 0x041D: "n", 0x043D: "n",
    0x041E: "o", 0x043E: "o", 0x041F: "p", 0x043F: "p", 0x0420: "r", 0x0440: "r",
    0x0421: "s", 0x0441: "s", 0x0422: "t", 0x0442: "t", 0x0423: "u", 0x0443: "u",
    0x0424: "f", 0x0444: "f", 0x0425: "h", 0x0445: "h", 0x0426: "c", 0x0446: "c",
    0x0427: "ch", 0x0447: "ch", 0x0428: "sh", 0x0448: "sh", 0x0429: "shch",
    0x0449: "shch", 0x042A: "a", 0x044A: "a", 0x042B: "y", 0x044B: "y",
    0x042C: "", 0x044C: "", 0x042D: "e", 0x044D: "e", 0x042E: "ju", 0x044E: "ju",
    0x042F: "ja", 0x044F: "ja",
    # Ukrainian / Belarusian
    0x0406: "i", 0x0456: "i", 0x0407: "ji", 0x0457: "ji", 0x0404: "je", 0x0454: "je",
    0x0490: "g", 0x0491: "g", 0x040E: "u", 0x045E: "u",
    # Serbian / Macedonian
    0x0408: "j", 0x0458: "j", 0x0409: "lj", 0x0459: "lj", 0x040A: "nj", 0x045A: "nj",
    0x040B: "c", 0x045B: "c", 0x0402: "dj", 0x0452: "dj", 0x040F: "dz", 0x045F: "dz",
    0x0403: "gj", 0x0453: "gj", 0x040C: "kj", 0x045C: "kj", 0x0405: "dz", 0x0455: "dz",
    # Kazakh / Kyrgyz / Tajik / Mongolian
    0x04D8: "a", 0x04D9: "a", 0x0492: "g", 0x0493: "g", 0x049A: "q", 0x049B: "q",
    0x04A2: "n", 0x04A3: "n", 0x04E8: "o", 0x04E9: "o", 0x04B0: "u", 0x04B1: "u",
    0x04AE: "u", 0x04AF: "u", 0x04BA: "h", 0x04BB: "h", 0x04E2: "i", 0x04E3: "i",
    0x04EE: "u", 0x04EF: "u", 0x04B2: "h", 0x04B3: "h", 0x04B6: "j", 0x04B7: "j",
}

# Greek -> Latin (approximate, search-only): monotonic transliteration, so
# "Ελληνικά" and "ellinika" meet at the same folded key.
GREEK_FOLD = {
    0x0391: "a", 0x03B1: "a", 0x0392: "v", 0x03B2: "v", 0x0393: "g", 0x03B3: "g",
    0x0394: "d", 0x03B4: "d", 0x0395: "e", 0x03B5: "e", 0x0396: "z", 0x03B6: "z",
    0x0397: "i", 0x03B7: "i", 0x0398: "th", 0x03B8: "th", 0x0399: "i", 0x03B9: "i",
    0x039A: "k", 0x03BA: "k", 0x039B: "l", 0x03BB: "l", 0x039C: "m", 0x03BC: "m",
    0x039D: "n", 0x03BD: "n", 0x039E: "x", 0x03BE: "x", 0x039F: "o", 0x03BF: "o",
    0x03A0: "p", 0x03C0: "p", 0x03A1: "r", 0x03C1: "r", 0x03A3: "s", 0x03C3: "s",
    0x03C2: "s", 0x03A4: "t", 0x03C4: "t", 0x03A5: "y", 0x03C5: "y", 0x03A6: "f",
    0x03C6: "f", 0x03A7: "ch", 0x03C7: "ch", 0x03A8: "ps", 0x03C8: "ps",
    0x03A9: "o", 0x03C9: "o",
    # Tonos / dialytika forms (precomposed)
    0x0386: "a", 0x03AC: "a", 0x0388: "e", 0x03AD: "e", 0x0389: "i", 0x03AE: "i",
    0x038A: "i", 0x03AF: "i", 0x038C: "o", 0x03CC: "o", 0x038E: "y", 0x03CD: "y",
    0x038F: "o", 0x03CE: "o", 0x03AA: "i", 0x03CA: "i", 0x03AB: "y", 0x03CB: "y",
    0x0390: "i", 0x03B0: "y",
}

# Whitespace / invisible / punctuation normalization (query == alias space).
# ASCII tab/CR/LF are deliberately NOT in this table (PR4-04): the generated
# fold table is only consulted for codepoints >= 0x80 (the C++ matcher's
# sub-0x80 fast path and this Python mirror handle ASCII inline), so a
# sub-0x80 entry would be dead data. Both sides fold '\t'/'\r'/'\n' to ' '
# inline; the table starts at U+00A0.
FOLD_WHITESPACE = {
    0x00A0: " ", 0x1680: " ", 0x2000: " ", 0x2001: " ", 0x2002: " ", 0x2003: " ",
    0x2004: " ", 0x2005: " ", 0x2006: " ", 0x2007: " ", 0x2008: " ", 0x2009: " ",
    0x200A: " ", 0x202F: " ", 0x205F: " ", 0x3000: " ",
}
FOLD_DROP = {
    0x00AD: "", 0x034F: "", 0x061C: "", 0x200B: "", 0x200C: "", 0x200D: "",
    0x200E: "", 0x200F: "", 0x202A: "", 0x202B: "", 0x202C: "", 0x202D: "",
    0x202E: "", 0x2060: "", 0x2061: "", 0x2062: "", 0x2063: "", 0x2064: "",
    0xFEFF: "",
}
FOLD_HYPHEN = {0x2010: "-", 0x2011: "-", 0x2012: "-", 0x2013: "-", 0x2014: "-",
               0x2015: "-", 0x2212: "-"}
FOLD_APOSTROPHE = {0x02B9: "'", 0x02BB: "'", 0x02BC: "'", 0x2018: "'",
                   0x2019: "'", 0x2032: "'"}

# Combining diacritical marks (U+0300-U+036F): dropped so decomposed input
# (α + U+0301, e + U+0301) folds exactly like its precomposed form.
COMBINING_RANGE = (0x0300, 0x036F)


def nfd_ascii_base(ch: str):
    """NFD-strip `ch`; return the lowercase ASCII base letter, else None."""
    base = None
    for part in unicodedata.normalize("NFD", ch):
        if unicodedata.category(part) == "Mn" or unicodedata.combining(part):
            continue
        if base is not None:
            return None  # more than one base character: not a simple fold
        base = part
    if base is None or len(base) != 1:
        return None
    low = base.lower()
    if len(low) == 1 and "a" <= low <= "z":
        return low
    return None


def build_fold_table():
    """Return the sorted [(codepoint, replacement, note)] fold table."""
    table = {}

    def put(cp, repl, note):
        if cp in table and table[cp][0] != repl:
            raise SystemExit(
                "fold table conflict at U+%04X: %r vs %r" % (cp, table[cp][0], repl)
            )
        table[cp] = (repl, note)

    for lo, hi in LATIN_DIACRITIC_RANGES:
        for cp in range(lo, hi + 1):
            base = nfd_ascii_base(chr(cp))
            if base is not None:
                put(cp, base, unicodedata.name(chr(cp), "?"))

    for cp, repl in LATIN_FOLD_SPECIALS.items():
        put(cp, repl, unicodedata.name(chr(cp), "?"))

    for cp, repl in CYRILLIC_FOLD.items():
        put(cp, repl, "CYRILLIC -> LATIN")

    for cp, repl in GREEK_FOLD.items():
        put(cp, repl, "GREEK -> LATIN")

    lo, hi = COMBINING_RANGE
    for cp in range(lo, hi + 1):
        if unicodedata.category(chr(cp)) == "Mn":
            put(cp, "", "COMBINING MARK (dropped)")

    for cp, repl in FOLD_WHITESPACE.items():
        put(cp, repl, "WHITESPACE -> SPACE")
    for cp, repl in FOLD_DROP.items():
        put(cp, repl, "INVISIBLE (dropped)")
    for cp, repl in FOLD_HYPHEN.items():
        put(cp, repl, "HYPHEN -> '-'")
    for cp, repl in FOLD_APOSTROPHE.items():
        put(cp, repl, "APOSTROPHE -> \"'\"")

    # Full-width ASCII forms (U+FF01-U+FF5E) -> ASCII, so a CJK input method's
    # full-width Latin query still finds the Latin alias. LETTERS FOLD TO
    # LOWERCASE ASCII (PR4-02): the C++ matcher case-folds only codepoints
    # that are ASCII at decode time and appends table replacements verbatim,
    # so an uppercase replacement would escape the case fold and a fullwidth
    # query like "KO" would miss the lowercase-folded alias "ko". Emitting
    # lowercase here makes the fullwidth fold case-insensitive, matching the
    # rest of the fold table.
    for cp in range(0xFF01, 0xFF5F):
        repl = chr(cp - 0xFEE0)
        if "A" <= repl <= "Z":
            repl = repl.lower()
            put(cp, repl, "FULLWIDTH -> ASCII (lowercase)")
        else:
            put(cp, repl, "FULLWIDTH -> ASCII")

    out = [(cp, table[cp][0], table[cp][1]) for cp in sorted(table)]
    for cp, repl, _ in out:
        if any(ord(c) > 0x7F for c in repl):
            raise SystemExit("fold replacement must be ASCII at U+%04X" % cp)
    return out


def fold_text(text: str, fold_table) -> str:
    """Python mirror of FoldLanguageAliasText (used for dedupe + sanity).

    Mirrors the C++ fold exactly (PR4-04): ASCII case fold, inline ASCII
    whitespace fold ('\\t'/\\r'/\\n' -> ' ' - the generated table is only
    consulted for codepoints >= 0x80), table fold for the rest, then interior
    space-run collapsing + trimming.
    """
    lookup = {cp: repl for cp, repl, _ in fold_table}
    out = []
    for ch in text:
        cp = ord(ch)
        if "A" <= ch <= "Z":
            out.append(ch.lower())
            continue
        if ch in "\t\r\n":
            out.append(" ")
            continue
        if cp < 0x80:
            out.append(ch)
            continue
        if cp in lookup:
            out.append(lookup[cp])
            continue
        out.append(ch)
    folded = "".join(out)
    # Collapse interior space runs and trim both ends - the same semantics as
    # the C++ matcher (" ".join(split(" ")) was a no-op and let interior runs
    # through, so the dedupe key and the match key disagreed; fixed here).
    return re.sub(" +", " ", folded).strip(" ")


# ---------------------------------------------------------------------------
# Source parsing (single source of truth: the app's own tables)
# ---------------------------------------------------------------------------
def read_text(path):
    with open(path, "r", encoding="utf-8") as handle:
        return handle.read()


def parse_registry(repo_root):
    """Parse kAllLanguages from src/config.cpp -> [(code, name_en, native, bcp47)]."""
    text = read_text(os.path.join(repo_root, CONFIG_CPP))
    start = text.find("kAllLanguages = {")
    if start < 0:
        raise SystemExit("could not find kAllLanguages in src/config.cpp")
    end = text.find("\n};", start)
    if end < 0:
        raise SystemExit("could not find the end of kAllLanguages in src/config.cpp")
    rows = []
    skipped = []
    for line in text[start:end].splitlines():
        match = CONFIG_ROW_RE.match(line)
        if match is None:
            if line.strip().startswith("{"):
                skipped.append(line.strip())
            continue
        code = match.group(1)
        rows.append((code, match.group(2), match.group(3), match.group(5)))
    if skipped:
        raise SystemExit(
            "kAllLanguages rows did not parse (%d): %s - update CONFIG_ROW_RE"
            % (len(skipped), skipped[0])
        )
    if len(rows) != EXPECTED_TARGET_COUNT + 1:
        raise SystemExit(
            "parsed %d kAllLanguages rows, expected %d (AUTO + %d targets); "
            "src/config.cpp layout changed - update CONFIG_ROW_RE"
            % (len(rows), EXPECTED_TARGET_COUNT + 1, EXPECTED_TARGET_COUNT)
        )
    if rows[0][0] != "AUTO":
        raise SystemExit("the first kAllLanguages row must be AUTO")
    if len({row[0] for row in rows}) != len(rows):
        raise SystemExit("duplicate codes in kAllLanguages")
    return rows


def parse_ui_locales(repo_root):
    """Parse kLocaleMappings from src/i18n.cpp -> [config_code] in table order."""
    text = read_text(os.path.join(repo_root, I18N_CPP))
    start = text.find("const LocaleMapping kLocaleMappings[] = {")
    if start < 0:
        raise SystemExit("could not find kLocaleMappings in src/i18n.cpp")
    end = text.find("\n};", start)
    if end < 0:
        raise SystemExit("could not find the end of kLocaleMappings in src/i18n.cpp")
    codes = []
    for line in text[start:end].splitlines():
        match = I18N_ROW_RE.match(line)
        if match is not None:
            codes.append(match.group(2))
    if len(codes) != EXPECTED_UI_LOCALE_COUNT:
        raise SystemExit(
            "parsed %d UI locales, expected %d; src/i18n.cpp layout changed"
            % (len(codes), EXPECTED_UI_LOCALE_COUNT)
        )
    missing = [code for code in codes if code not in CLDR_LOCALE_FOR_CONFIG_CODE]
    if missing:
        raise SystemExit(
            "no CLDR locale mapping for UI locale(s) %s - extend "
            "CLDR_LOCALE_FOR_CONFIG_CODE" % ", ".join(missing)
        )
    return codes


# ---------------------------------------------------------------------------
# Snapshot access
# ---------------------------------------------------------------------------
def snapshot_path(snapshot_dir, locale):
    return os.path.join(snapshot_dir, locale + ".json")


def verify_snapshot_bytes(locale, raw):
    """Fail closed (PR4-05): hash raw snapshot bytes against the pinned pin."""
    if locale in SNAPSHOT_UNAVAILABLE_LOCALES:
        raise SystemExit(
            "locale %r is declared in SNAPSHOT_UNAVAILABLE_LOCALES and has no "
            "CLDR file; no bytes may be verified or rendered for it" % locale
        )
    expected = EXPECTED_SNAPSHOT_SHA256.get(locale)
    if expected is None:
        raise SystemExit(
            "no pinned SHA-256 for locale %r - extend EXPECTED_SNAPSHOT_SHA256"
            % locale
        )
    actual = hashlib.sha256(raw).hexdigest()
    if actual != expected:
        raise SystemExit(
            "snapshot digest mismatch for %s:\n  got    %s\n  pinned %s\n"
            "refusing to render a snapshot that fails the digest pin"
            % (locale, actual, expected)
        )


def load_snapshot(snapshot_dir, locale):
    path = snapshot_path(snapshot_dir, locale)
    with open(path, "rb") as handle:
        raw = handle.read()
    verify_snapshot_bytes(locale, raw)
    data = json.loads(raw.decode("utf-8"))
    names = data["main"][locale]["localeDisplayNames"]["languages"]
    return names, hashlib.sha256(raw).hexdigest()


def fetch_snapshot(snapshot_dir, locales):
    os.makedirs(snapshot_dir, exist_ok=True)
    for locale in locales:
        path = snapshot_path(snapshot_dir, locale)
        if locale in SNAPSHOT_UNAVAILABLE_LOCALES:
            # Documented fallback: no file exists at the pinned tag, so there is
            # nothing to fetch. A file showing up here would make the
            # declaration stale, so refuse instead of silently using it.
            if os.path.exists(path):
                raise SystemExit(
                    "%s exists but %r is declared in SNAPSHOT_UNAVAILABLE_LOCALES; "
                    "pin its digest and remove the declaration" % (path, locale)
                )
            print("skipping %s (declared unavailable: no CLDR file at %s)"
                  % (locale, CLDR_TAG))
            continue
        if os.path.exists(path):
            continue
        url = CLDR_RAW_URL.format(tag=CLDR_TAG, locale=locale)
        print("fetching %s" % url)
        try:
            with urllib.request.urlopen(url, timeout=60) as response:
                chunks = []
                received = 0
                while True:
                    chunk = response.read(SNAPSHOT_READ_CHUNK)
                    if not chunk:
                        break
                    received += len(chunk)
                    if received > MAX_SNAPSHOT_BYTES:
                        raise SystemExit(
                            "fetching %s exceeded the %d-byte cap; refusing an "
                            "oversized body (the digest pin could only reject it "
                            "after the whole transfer)"
                            % (url, MAX_SNAPSHOT_BYTES)
                        )
                    chunks.append(chunk)
                payload = b"".join(chunks)
        except urllib.error.HTTPError as exc:
            raise SystemExit(
                "fetching %s failed with HTTP %s; do NOT invent data - either "
                "pin the real digest once the file exists, or declare the "
                "locale in SNAPSHOT_UNAVAILABLE_LOCALES (it then contributes "
                "no exonym column)" % (locale, exc.code)
            )
        # PR4-05: verify BEFORE caching - a mismatched fetch is never written
        # to the snapshot dir.
        verify_snapshot_bytes(locale, payload)
        with open(path, "wb") as handle:
            handle.write(payload)


# ---------------------------------------------------------------------------
# CLDR key resolution (the documented mapping table + fallback chain)
# ---------------------------------------------------------------------------
def cldr_key_for(code, bcp47, en_keys):
    """Return the primary CLDR key, validating every divergence from bcp47."""
    if code in CLDR_KEY_OVERRIDES:
        return CLDR_KEY_OVERRIDES[code]
    if bcp47 in en_keys:
        return bcp47
    raise SystemExit(
        "%s: bcp47 %r is not a CLDR language key and no override is declared; "
        "add it to CLDR_KEY_OVERRIDES" % (code, bcp47)
    )


def cldr_name_for(code, keys, names, used):
    """First candidate key present in `names`; records which tier was used."""
    for index, key in enumerate(keys):
        if key in names and names[key]:
            used.append((code, index, key))
            return names[key]
    return None


# ---------------------------------------------------------------------------
# Emission
# ---------------------------------------------------------------------------
HEADER = '''\
// ===========================================================================
// 261116_0003 P4 (language-menu redesign, plan section 3 D4 / section 4
// "Aliases") - GENERATED FILE, DO NOT EDIT BY HAND.
//
// Generated by:  python tools/generate_language_aliases.py --snapshot-dir {snapshot_dir}
//
// Provenance
//   CLDR version ........ {cldr_tag} (released {cldr_release})
//   Snapshot ............ unicode-org/cldr-json @ {cldr_tag},
//                         cldr-localenames-full/main/<locale>/languages.json
//   Snapshot fetched .... {fetch_date}  ({file_count} files, SHA-256 listed at the end)
//   Snapshot coverage ... {file_count} of {locale_count} UI locales; {unavailable_count} declared
//                         unavailable (no CLDR file at this tag, no data
//                         fabricated, no exonym column)
//   Source ............. https://raw.githubusercontent.com/unicode-org/cldr-json/
//                         {cldr_tag}/cldr-json/cldr-localenames-full/main/<locale>/languages.json
//
// License / attribution
//   CLDR data: Copyright (c) 1991-2025 Unicode, Inc. All rights reserved.
//   Distributed under the Terms of Use in
//   https://www.unicode.org/copyright.html (Unicode License v3).
//   Only the DERIVED language-name strings below are embedded; CLDR JSON is
//   NOT a build or run-time dependency of this application.
//
// Contents (all static `const` data - no logic, so regeneration can never
// clobber reviewed code; the matcher lives in src/language_alias.hpp)
//   1. kLanguageAliasLocales       - the app's {locale_count} UI locales as CLDR ids
//   2. kLanguageAliasAtoms         - one alias string per row, grouped by
//                                    language, tagged with its rank tier
//                                    (Code < Native < English < Exonym <
//                                    Script) and its source locale
//   3. kLanguageAliasGroups        - code -> [first, count) slice of the atoms
//   4. kLanguageAliasFoldTable     - codepoint -> ASCII fold/transliteration
//                                    pairs (binary-searched by the matcher)
//   5. kLanguageAlias              - the table header the matcher reads
//
// Registry rows: {target_count} targets + AUTO, in kAllLanguages order (the
// order is cycle-pin load-bearing). Aliases per language: {atom_count} total.
// ===========================================================================

#include "language_alias.hpp"

namespace emebalachat {{

namespace {{

// ---------------------------------------------------------------------------
// 1) The {locale_count} UI locales as CLDR locale ids (src/i18n.cpp
//    kLocaleMappings order, mapped through CLDR_LOCALE_FOR_CONFIG_CODE:
//    zh-CN -> "zh" (the CLDR Simplified tree), zh-TW -> "zh-Hant",
//    yue -> "yue" (Cantonese: the config code IS its CLDR language key, not
//    a Chinese script tree - identity since 261119_0001 FIX-R1), no -> "no").
{unavailable_comment}// ---------------------------------------------------------------------------
const char* const kLanguageAliasLocales[] = {{
{locale_lines}}};

constexpr std::size_t kLanguageAliasLocaleCount =
    sizeof(kLanguageAliasLocales) / sizeof(kLanguageAliasLocales[0]);

// ---------------------------------------------------------------------------
// 2) Alias atoms, in registry order. Tiers (best first):
//      Code    - the registry code and, when it folds differently, the bcp47 tag
//      Native  - LanguageInfo::name_native (the language's own name)
//      English - LanguageInfo::name_en + the CLDR English name
//      Exonym  - the CLDR name in each of the other {exonym_locale_count}
//                snapshot-carrying UI locales
//      Script  - the hand-curated script-name table (plan D4)
//    kAliasNoLocale marks an atom with no source locale; every other atom's
//    locale index points at kLanguageAliasLocales (the "en" column is tagged
//    English tier because a CLDR English name IS an English name).
//    Fallback chain per (language, locale): primary CLDR key -> documented
//    alt/macro keys (CLDR_KEY_FALLBACKS) -> English name -> code.
// ---------------------------------------------------------------------------
const LanguageAliasAtom kLanguageAliasAtoms[] = {{
{atom_lines}}};

constexpr std::size_t kLanguageAliasAtomCount =
    sizeof(kLanguageAliasAtoms) / sizeof(kLanguageAliasAtoms[0]);

// ---------------------------------------------------------------------------
// 3) Per-language slices of (2), registry order. `first + count` of one row
// equals `first` of the next; the last reaches kLanguageAliasAtomCount
// (pinned by TestLanguageAliasCoverage).
// ---------------------------------------------------------------------------
const LanguageAliasGroup kLanguageAliasGroups[] = {{
{group_lines}}};

constexpr std::size_t kLanguageAliasGroupCount =
    sizeof(kLanguageAliasGroups) / sizeof(kLanguageAliasGroups[0]);

// ---------------------------------------------------------------------------
// 4) Fold table: codepoint -> ASCII replacement, sorted by codepoint (the
//    matcher binary-searches it). Derived from unicodedata {unicode_version}
//    NFD (diacritics) plus the hand-authored Latin specials / Cyrillic and
//    Greek transliterations / whitespace-and-invisible normalization. An
//    empty replacement drops the codepoint (zero-width marks, combining
//    diacritics).
// ---------------------------------------------------------------------------
const LanguageAliasFoldEntry kLanguageAliasFoldTable[] = {{
{fold_lines}}};

constexpr std::size_t kLanguageAliasFoldCount =
    sizeof(kLanguageAliasFoldTable) / sizeof(kLanguageAliasFoldTable[0]);

// ---------------------------------------------------------------------------
// 5) Snapshot digests: SHA-256 of every languages.json this file was built
//    from. Regenerating against a different snapshot changes this block (and
//    possibly the table above) - the audit trail for "is this real CLDR data".
// ---------------------------------------------------------------------------
// {digest_lines}

}} // namespace

const LanguageAliasTable kLanguageAlias = {{
    kLanguageAliasLocales,
    kLanguageAliasLocaleCount,
    kLanguageAliasAtoms,
    kLanguageAliasAtomCount,
    kLanguageAliasGroups,
    kLanguageAliasGroupCount,
    kLanguageAliasFoldTable,
    kLanguageAliasFoldCount,
}};

}} // namespace emebalachat
'''

TIER_NAMES = {
    "code": "AliasTier::Code",
    "native": "AliasTier::Native",
    "english": "AliasTier::English",
    "exonym": "AliasTier::Exonym",
    "script": "AliasTier::Script",
}


def cpp_string(text):
    # PR4-05: fail closed on control bytes. '"' and '\\' are escaped (no C++
    # injection is possible), but a raw C0 control byte or DEL would emit an
    # ill-formed or implementation-defined literal from a hostile/tampered
    # snapshot - reject it at generation time instead.
    out = []
    for ch in text:
        if ch == '"':
            out.append('\\"')
        elif ch == "\\":
            out.append("\\\\")
        elif ord(ch) < 0x20 or ord(ch) == 0x7F:
            raise SystemExit(
                "refusing to emit control character U+%04X into a C++ literal"
                % ord(ch)
            )
        else:
            out.append(ch)
    return '"' + "".join(out) + '"'


def lang_comment(code, name_en):
    return "    // ---- %s (%s) ----" % (code, name_en)


def render(repo_root, snapshot_dir):
    registry = parse_registry(repo_root)
    ui_codes = parse_ui_locales(repo_root)
    locales = [CLDR_LOCALE_FOR_CONFIG_CODE[code] for code in ui_codes]
    if len(set(locales)) != len(locales):
        raise SystemExit("duplicate CLDR locale ids in the UI locale mapping")

    missing_script_rows = [row[0] for row in registry if row[0] not in SCRIPT_ALIASES]
    if missing_script_rows:
        raise SystemExit(
            "SCRIPT_ALIASES has no row for %s (one row per registry language, "
            "an empty list when the language writes no distinct script)"
            % ", ".join(missing_script_rows)
        )

    snapshots = {}
    digests = []
    unavailable_locales = [loc for loc in locales if loc in SNAPSHOT_UNAVAILABLE_LOCALES]
    for locale in locales:
        if locale in SNAPSHOT_UNAVAILABLE_LOCALES:
            # Documented fallback: no CLDR file exists at this tag, so this
            # locale keeps its index slot but contributes no exonym column.
            # A cached file would mean the declaration is stale.
            if os.path.exists(snapshot_path(snapshot_dir, locale)):
                raise SystemExit(
                    "%s exists but %r is declared in SNAPSHOT_UNAVAILABLE_LOCALES; "
                    "pin its digest and remove the declaration"
                    % (snapshot_path(snapshot_dir, locale), locale)
                )
            snapshots[locale] = {}
            continue
        names, digest = load_snapshot(snapshot_dir, locale)
        snapshots[locale] = names
        digests.append((locale, digest))
    if "en" not in snapshots or not snapshots["en"]:
        raise SystemExit("the en snapshot is mandatory (it validates CLDR keys)")
    available_locale_count = len(locales) - len(unavailable_locales)
    exonym_locale_count = available_locale_count - 1  # every available locale but "en"

    en_keys = set(snapshots["en"].keys())
    en_index = locales.index("en")
    atom_lines = []
    group_lines = []
    atoms_total = 0
    per_code_atoms = {}
    fallback_report = []

    for code, name_en, name_native, bcp47 in registry:
        primary = cldr_key_for(code, bcp47, en_keys) if code != "AUTO" else None
        candidates = []
        if primary is not None:
            candidates.append(primary)
            candidates.extend(CLDR_KEY_FALLBACKS.get(code, []))

        seen = {}  # folded text -> tier (dedupe keeps the BEST tier)
        atoms = []  # (text, tier_key, locale_index)

        def add(text, tier_key, locale_index=0xFF):
            if not text:
                return
            folded = fold_text(text, FOLD_TABLE)
            if not folded:
                return
            if folded in seen:
                return  # a better (or equal) tier already covers this key
            seen[folded] = tier_key
            atoms.append((text, tier_key, locale_index))

        # Code tier: the registry code, then the bcp47 tag when it folds
        # differently (YUE's zh-HK, EN's en, FIL's fil ... collapse as dupes).
        # PR4-01: the AUTO pseudo-row's bcp47 is the DWrite PIVOT tag ("en")
        # - a font-selection concern, not a name users type for AUTO. Emitting
        # it ranked AUTO above EN for the query "en" in every AUTO-inclusive
        # source view (AUTO is registry row 0 and wins the exact-tier tie by
        # pool order), so the pivot atom is dropped for AUTO only.
        add(code, "code")
        if (code != "AUTO" and bcp47 and
                fold_text(bcp47, FOLD_TABLE) != fold_text(code, FOLD_TABLE)):
            add(bcp47, "code")
        add(name_native, "native")
        add(name_en, "english")
        for extra in EXTRA_ENGLISH.get(code, []):
            add(extra, "english")

        if primary is not None:
            # English column: a CLDR English name is an English name.
            used = []
            english_cldr = cldr_name_for(code, candidates, snapshots["en"], used)
            if used and used[0][1] > 0:
                fallback_report.append(("en", code, used[0][2], candidates[0]))
            if english_cldr:
                add(english_cldr, "english", en_index)
            # Exonym columns: the remaining snapshot-carrying UI locales, in
            # table order (en keeps the English tier above; a declared-
            # unavailable locale has no names and is skipped).
            for index, locale in enumerate(locales):
                if locale == "en" or locale in SNAPSHOT_UNAVAILABLE_LOCALES:
                    continue
                used = []
                name = cldr_name_for(code, candidates, snapshots[locale], used)
                if used and used[0][1] > 0:
                    fallback_report.append(
                        (locale, code, used[0][2], candidates[0])
                    )
                if name:
                    add(name, "exonym", index)

        for term in SCRIPT_ALIASES.get(code, []):
            add(term, "script")

        if len(atoms) < 2:
            raise SystemExit("%s: fewer than 2 aliases after dedupe" % code)

        atom_lines.append(lang_comment(code, name_en))
        for text, tier_key, locale_index in atoms:
            if locale_index == 0xFF:
                atom_lines.append(
                    '    {%s, %s, kAliasNoLocale},'
                    % (cpp_string(text), TIER_NAMES[tier_key])
                )
            else:
                atom_lines.append(
                    '    {%s, %s, %d}, // %s'
                    % (cpp_string(text), TIER_NAMES[tier_key], locale_index,
                       locales[locale_index])
                )
        group_lines.append(
            '    {%s, %d, %d},' % (cpp_string(code), atoms_total, len(atoms))
        )
        per_code_atoms[code] = atoms
        atoms_total += len(atoms)

    fold_lines = []
    for cp, repl, note in FOLD_TABLE:
        fold_lines.append(
            '    {0x%04X, %s}, // %s' % (cp, cpp_string(repl), note)
        )

    locale_lines = "".join(
        ('    "%s", // %s\n' % (locale, ui_codes[index])) if locale != ui_codes[index]
        else ('    "%s",\n' % locale)
        for index, locale in enumerate(locales)
    )
    digest_lines = "\n// ".join(
        "%s %s" % (locale, digest) for locale, digest in digests
    )
    if unavailable_locales:
        unavailable_comment = (
            "//    Locale(s) declared in SNAPSHOT_UNAVAILABLE_LOCALES (no CLDR\n"
            "//    file at %s, HTTP 404): %s. Their slots above keep the locale\n"
            "//    list index-parallel to kLocaleMappings, but they contribute NO\n"
            "//    exonym column - no data is fabricated for them.\n"
            % (CLDR_TAG, ", ".join(unavailable_locales))
        )
    else:
        unavailable_comment = ""

    rendered = HEADER.format(
        snapshot_dir=snapshot_dir.replace("\\", "/"),
        cldr_tag=CLDR_TAG,
        cldr_release=CLDR_RELEASE_DATE,
        fetch_date=SNAPSHOT_FETCH_DATE,
        target_count=len(registry) - 1,
        atom_count=atoms_total,
        locale_count=len(locales),
        file_count=len(digests),
        unavailable_count=len(unavailable_locales),
        exonym_locale_count=exonym_locale_count,
        unavailable_comment=unavailable_comment,
        locale_lines=locale_lines,
        atom_lines="\n".join(atom_lines) + "\n",
        group_lines="\n".join(group_lines) + "\n",
        fold_lines="\n".join(fold_lines) + "\n",
        unicode_version=unicodedata.unidata_version,
        digest_lines=digest_lines,
    )

    # ---- report -----------------------------------------------------------
    tier_counts = {}
    locale_coverage = {}
    for code, atoms in per_code_atoms.items():
        for _text, tier_key, locale_index in atoms:
            tier_counts[tier_key] = tier_counts.get(tier_key, 0) + 1
            if tier_key == "exonym":
                locale_coverage[locale_index] = locale_coverage.get(locale_index, 0) + 1
    no_exonym = sorted(
        code for code, atoms in per_code_atoms.items()
        if not any(t == "exonym" for _x, t, _i in atoms)
    )
    no_script = sorted(
        code for code, atoms in per_code_atoms.items()
        if not any(t == "script" for _x, t, _i in atoms)
    )
    fallback_langs = sorted({code for _loc, code, _used, _prim in fallback_report})
    print("registry rows ........ %d (AUTO + %d targets)" % (len(registry), len(registry) - 1))
    print("UI locales ........... %d (%d snapshot files, %d declared unavailable: %s)" % (
        len(locales), len(digests), len(unavailable_locales),
        ", ".join(unavailable_locales) if unavailable_locales else "none"))
    print("alias atoms .......... %d" % atoms_total)
    print("atoms by tier ........ %s" % ", ".join(
        "%s=%d" % (tier, tier_counts.get(tier, 0))
        for tier in ("code", "native", "english", "exonym", "script")))
    print("exonym atoms/locale .. min=%d max=%d" % (
        min(locale_coverage.values()), max(locale_coverage.values())))
    print("languages w/o exonym . %d %s" % (len(no_exonym), no_exonym))
    print("languages w/o script . %d" % len(no_script))
    print("fallback languages ... %d %s" % (len(fallback_langs), fallback_langs))
    print("fallback cells ....... %d (of %d target x locale cells)" % (
        len(fallback_report), (len(registry) - 1) * available_locale_count))
    if fallback_report:
        by_key = {}
        for _locale, code, used_key, primary in fallback_report:
            by_key.setdefault((primary, used_key), []).append(code)
        for (primary, used_key), codes in sorted(by_key.items()):
            print("  %-22s -> %-22s %d langs (%s)" % (
                primary, used_key, len(codes), ", ".join(sorted(set(codes)))))
    print("fold table entries ... %d" % len(FOLD_TABLE))
    return rendered


FOLD_TABLE = build_fold_table()


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Generate src/language_alias.cpp from a pinned CLDR snapshot."
    )
    parser.add_argument("--snapshot-dir", default=DEFAULT_SNAPSHOT_DIR,
                        help="cached snapshot directory (default: %s)" % DEFAULT_SNAPSHOT_DIR)
    parser.add_argument("--out", default=DEFAULT_OUT,
                        help="output file (default: %s)" % DEFAULT_OUT)
    parser.add_argument("--repo-root", default=None,
                        help="repository root (default: the parent of tools/)")
    parser.add_argument("--fetch", action="store_true",
                        help="download missing snapshot files from the pinned tag first")
    parser.add_argument("--check", action="store_true",
                        help="exit 1 if the file on disk differs from the rendered output")
    args = parser.parse_args(argv)

    repo_root = args.repo_root
    if repo_root is None:
        repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    snapshot_dir = args.snapshot_dir
    if not os.path.isabs(snapshot_dir):
        snapshot_dir = os.path.join(repo_root, snapshot_dir)

    ui_codes = parse_ui_locales(repo_root)
    locales = [CLDR_LOCALE_FOR_CONFIG_CODE[code] for code in ui_codes]
    if args.fetch:
        fetch_snapshot(snapshot_dir, locales)

    missing = [
        loc for loc in locales
        if loc not in SNAPSHOT_UNAVAILABLE_LOCALES
        and not os.path.exists(snapshot_path(snapshot_dir, loc))
    ]
    if missing:
        raise SystemExit(
            "snapshot incomplete under %s (missing %s); re-run with --fetch"
            % (snapshot_dir, ", ".join(missing))
        )

    # PR4-05: the fold table's NFD-derived entries depend on the host
    # unicodedata version; refuse to render under a different one (re-derive
    # + re-pin deliberately instead of drifting).
    if unicodedata.unidata_version != EXPECTED_UNICODATA_VERSION:
        raise SystemExit(
            "unicodedata %s != pinned %s: the NFD-derived fold entries could "
            "differ; re-derive the fold table deliberately and re-pin "
            "EXPECTED_UNICODATA_VERSION"
            % (unicodedata.unidata_version, EXPECTED_UNICODATA_VERSION)
        )

    rendered = render(repo_root, args.snapshot_dir)

    out_path = args.out
    if not os.path.isabs(out_path):
        out_path = os.path.join(repo_root, out_path)

    if args.check:
        try:
            current = read_text(out_path)
        except FileNotFoundError:
            print("%s is missing (regenerate it)" % out_path)
            return 1
        if current != rendered:
            print("%s is STALE (regenerate with the command in its header)" % out_path)
            return 1
        print("%s matches the generator output" % out_path)
        return 0

    tmp_path = out_path + ".tmp"
    with open(tmp_path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(rendered)
    os.replace(tmp_path, out_path)
    print("wrote %s (%d bytes)" % (out_path, len(rendered.encode("utf-8"))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
