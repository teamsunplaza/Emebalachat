// ---------------------------------------------------------------------------
// 261108_0002 (WS2) - v0.10.6 "appended sentences stay untranslated" bundle.
//
// User report (Korean, verbatim): "1차 번역 후 같은 입력창에서 여러 문장을 연달아
// 이어 쓰고 엔터를 쳤을 때 뒷부분(한국어 다중 문장)이 번역되지 않는 현상" - after a
// first translation, typing more sentences into the SAME input box and pressing
// Enter left the appended part untranslated.
//
// Root cause class: the capture is ONE logical line = [already-translated
// text][newly typed source text]. FindCurrentBlockStart can only split on
// '\r'/'\n', so a single-line capture never split; ShouldTranslate() then saw a
// mixed-script line whose dominant script was the TARGET one and smart-bypassed
// the whole line (src/smart_bypass.cpp TargetScriptDominant).
//
// This suite pins the three pure seams the workstream added:
//   (a) SplitSameLineTargetPrefix  - the deterministic same-line splitter
//       (src/win32_input.hpp): the user's exact repro line, the emoji /
//       shortcode / ZWSP / BOM / ellipsis boundary family, and every
//       must-NOT-fire shape (homogeneous script, multi-line, AUTO target,
//       source-free suffix, target-free prefix, clock-shaped "12:30:45").
//   (b) ScriptMaskOfText / ScriptMaskForLanguageCode - the script-mask seam.
//       Pinned equal to engine_core's TextScriptClasses / TargetScriptClasses
//       for EVERY registered language code, so the two tables cannot drift.
//       (The masks cannot COME from engine_core: worker.cpp is compiled into
//       Emebalachat_core, which links no engine_core - see the header note.)
//   (c) TrimLeadingHorizontalSpace + the tail-level bypass composition - the
//       item-1 / item-2 seams: the separator space the ledger split leaves at
//       the tail front never reaches the engine, and the translation decision
//       reads the TAIL-level ShouldTranslate verdict when a tail exists while
//       the whole-line flag keeps its legacy consumers.
//   (d) AnalyzeCaptureVsLastPaste edge used by the item-3 ledger re-anchor:
//       the strict canonical byte-prefix test (ADR-001: no fuzzy matching).
//   (e) Structural pins on src/worker.cpp: the new seams are wired, and the
//       pre-existing invariants the workstream must not break still hold
//       (the boundary-space recomposition reads the ORIGINAL tail character,
//       not the trimmed one).
//
// Self-contained: no main(), no new includes beyond what tests/run_tests.cpp
// already provides (it includes ../src/win32_input.hpp, ../src/worker.hpp and
// ../src/engine_core/engine_core_helpers.hpp; this inc adds only the
// translation_common.hpp mid-TU include it needs for the drift pins, the
// req051_decode_budget_tests.inc precedent).
// ---------------------------------------------------------------------------

#include "../src/engine_core/translation_common.hpp" // 261108_0002: TextScriptClasses / TargetScriptClasses drift pins (mid-TU include, pragma-once safe)

namespace {

// Read a production source through the runner's ResolveRepoFile helper (the
// REQ-052 / REQ-051 structural-pin precedent). Empty when unresolvable, which
// makes the calling test SKIP loudly instead of passing silently.
std::string Ws2ReadSource(const wchar_t* rel) {
    const std::filesystem::path p = ResolveRepoFile(rel);
    if (p.empty()) {
        return {};
    }
    std::ifstream in(p, std::ios::binary);
    if (!in) {
        return {};
    }
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::size_t Ws2CountOcc(const std::string& hay, const std::string& needle) {
    std::size_t n = 0;
    for (std::size_t pos = 0; (pos = hay.find(needle, pos)) != std::string::npos; pos += needle.size()) {
        ++n;
    }
    return n;
}

// The user's exact repro line (live report): the English first sentence (what
// the FIRST Enter translated), then ":laughing:", then the Korean sentences the
// user appended before the SECOND Enter.
const wchar_t* const kWs2ReproLine =
    L"Ah... I fell asleep while writing a fairly long article yesterday. :laughing: "
    L"사실 저는 astrology에 대해서는 전혀 모릅니다. 다만, 2000년 전에 쓰여진 의학서적이 "
    L"'경험'을 근간으로 했으며, 최신의학에서의 '면역학'에 상당히 가깝다는 점을 주목했을 뿐입니다.";

} // namespace

// ---------------------------------------------------------------------------
// (a) the same-line target-language prefix splitter
// ---------------------------------------------------------------------------
void TestWs2SameLinePrefixSplitter() {
    std::cout << "[TEST] 261108_0002 WS2 same-line target-prefix splitter" << std::endl;
    const int failures_before = g_failed_count;

    const std::uint32_t ko = kScriptMaskHangul;
    const std::uint32_t en = kScriptMaskLatin;

    // ---- the user's repro: Korean target, English prefix + Korean suffix ----
    // Under a KOREAN target the repro line does NOT split: the leading run is
    // English (not the target language), so the contract's prefix test
    // refuses and the pre-WS2 whole-line path owns it. This is the fail-safe
    // direction - the splitter never guesses. The direction that matters for
    // the reported symptom is the mirror one (target-language prefix + source
    // suffix), covered by the cell below.
    {
        const std::wstring line(kWs2ReproLine);
        const SameLinePrefixSplit s = SplitSameLineTargetPrefix(line, ko);
        TEST_CHECK(!s.split,
                   "WS2 (a): the repro line under a KOREAN target does not split (English prefix is not target language)");
    }
    // The mirror direction: an ENGLISH target with the Korean tail. Here the
    // leading English IS the target language and the Korean suffix is the
    // source - exactly the reported "appended part stays untranslated" shape.
    {
        const std::wstring line(kWs2ReproLine);
        const SameLinePrefixSplit s = SplitSameLineTargetPrefix(line, en);
        TEST_CHECK(s.split, "WS2 (a): the live repro line splits under an English target");
        if (s.split) {
            const std::wstring prefix = line.substr(0, s.prefix_len);
            const std::wstring suffix = line.substr(s.prefix_len);
            TEST_CHECK(prefix.find(L":laughing:") != std::wstring::npos,
                       "WS2 (a): the ':laughing:' shortcode stays on the PREFIX side");
            TEST_CHECK(suffix.find(L":laughing:") == std::wstring::npos,
                       "WS2 (a): the shortcode never leaks into the translated suffix");
            TEST_CHECK(suffix.find(L"사실 저는") != std::wstring::npos,
                       "WS2 (a): the appended Korean sentences are the suffix the engine translates");
            TEST_CHECK((SameLineLinguisticMask(suffix) & ~en) != 0,
                       "WS2 (a): the suffix carries the source script (Hangul under an English target)");
            TEST_CHECK(prefix.size() + suffix.size() == line.size(),
                       "WS2 (a): prefix + suffix cover the capture exactly (recomposition invariant)");
        }
    }

    // ---- the opposite direction: Korean prefix, English target ----
    {
        const std::wstring line = L"이미 번역된 문장입니다. Newly typed English sentence";
        const SameLinePrefixSplit s = SplitSameLineTargetPrefix(line, ko);
        TEST_CHECK(s.split, "WS2 (a): a Korean prefix + English suffix splits under a Korean target");
        if (s.split) {
            TEST_CHECK(line.substr(0, s.prefix_len) == L"이미 번역된 문장입니다. ",
                       "WS2 (a): the prefix keeps its own trailing separator space");
            TEST_CHECK(line.substr(s.prefix_len) == L"Newly typed English sentence",
                       "WS2 (a): the suffix starts at the first source-script character");
        }
    }

    // ---- boundary family: every marker the contract names ----
    // Target = Korean, so the prefix must be Korean and the suffix must carry
    // a non-Korean script (Latin here).
    struct BoundaryCell { const wchar_t* line; const wchar_t* expected_suffix; const char* what; };
    const BoundaryCell boundary_cells[] = {
        {L"번역된 문장입니다. 새 English tail", L"새 English tail",
         "period + space"},
        {L"번역된 문장입니다! 새 English tail", L"새 English tail",
         "exclamation + space"},
        {L"번역된 문장입니다? 새 English tail", L"새 English tail",
         "question + space"},
        {L"번역된 문장입니다\u2026 새 English tail", L"새 English tail",
         "horizontal ellipsis U+2026"},
        {L"번역된 문장입니다\u200B새 English tail", L"새 English tail",
         "ZWSP U+200B (no space after)"},
        {L"번역된 문장입니다\uFEFF새 English tail", L"새 English tail",
         "BOM / ZWNBSP U+FEFF"},
        {L"번역된 문장입니다. :tada: 새 English tail", L"새 English tail",
         "':tada:' shortcode after a terminator"},
        {L"번역된 문장입니다. :+1: 새 English tail", L"새 English tail",
         "signed shortcode ':+1:'"},
        {L"번역된 문장입니다. 😀 새 English tail", L"새 English tail",
         "emoji U+1F600"},
        {L"번역된 문장입니다. \u2728 새 English tail", L"새 English tail",
         "dingbat U+2728 (BMP symbol block)"},
        {L"번역된 문장입니다.\u00A0새 English tail", L"새 English tail",
         "NBSP separator absorbed into the prefix"},
    };
    for (const BoundaryCell& c : boundary_cells) {
        const std::wstring line(c.line);
        const SameLinePrefixSplit s = SplitSameLineTargetPrefix(line, ko);
        TEST_CHECK(s.split && line.substr(s.prefix_len) == std::wstring(c.expected_suffix),
                   std::string("WS2 (a) boundary: ") + c.what + " splits with the expected suffix");
    }

    // A shortcode that ENDS the prefix with no sentence terminator before it
    // still splits: the shortcode body ("tada") is Latin, but it is filtered
    // out of the linguistic mask (SameLineLinguisticMask), so the Korean
    // prefix stays a clean target-language run.
    {
        const std::wstring line = L"번역된 문장입니다 :tada: 새 English tail";
        const SameLinePrefixSplit s = SplitSameLineTargetPrefix(line, ko);
        TEST_CHECK(s.split && line.substr(s.prefix_len) == std::wstring(L"새 English tail"),
                   "WS2 (a): a bare ':tada:' boundary splits (shortcode body excluded from the mask)");
    }

    // ---- must-NOT-fire shapes ----
    struct RefuseCell { const wchar_t* line; std::uint32_t mask; const char* what; };
    const RefuseCell refuse_cells[] = {
        // Homogeneous single-script lines: nothing to split (the whole line is
        // already in one language; translating it is the pre-WS2 behavior).
        {L"안녕하세요. 오늘 날씨가 좋습니다. 산책하러 갈까요?", ko,
         "homogeneous Korean line"},
        {L"Hello there. How are you today? I am fine.", en,
         "homogeneous English line"},
        {L"Hello there. How are you today? I am fine.", ko,
         "English-only line under a Korean target (no source-script suffix)"},
        // A multi-line capture belongs to FindCurrentBlockStart, never here.
        {L"번역된 문장\r\n새 English tail", ko, "multi-line capture (CRLF)"},
        {L"번역된 문장\n새 English tail", ko, "multi-line capture (LF)"},
        // AUTO / unknown target: fail open to the legacy whole-line path.
        {L"번역된 문장입니다. New English tail", 0u, "unknown target mask (AUTO)"},
        // The suffix carries no source-script character.
        {L"번역된 문장입니다. 또 다른 문장입니다.", ko,
         "target-only suffix (nothing to translate)"},
        // The prefix carries no target-script character.
        {L"😀😀 hello", ko, "emoji-only prefix"},
        {L"Plain English. More English.", ko,
         "source-only line under a Korean target (prefix is not target language)"},
        // A clock/ratio shape must not be read as a shortcode.
        {L"번역된 문장입니다 12:30:45 English tail", ko,
         "clock shape '12:30:45' is not a shortcode"},
        // Empty input.
        {L"", ko, "empty capture"},
    };
    for (const RefuseCell& c : refuse_cells) {
        const std::wstring line(c.line);
        const SameLinePrefixSplit s = SplitSameLineTargetPrefix(line, c.mask);
        TEST_CHECK(!s.split && s.prefix_len == 0,
                   std::string("WS2 (a) refuse: ") + c.what + " never splits");
    }

    // ---- the prefix may not itself mix scripts ----
    // "번역된 English 문장. 새 English tail" - the leading run already carries
    // the source script, so it is not a clean target-language prefix and the
    // splitter must refuse (the whole-line path handles it, as before).
    {
        const std::wstring line = L"번역된 English 문장. 새 English tail";
        TEST_CHECK(!SplitSameLineTargetPrefix(line, ko).split,
                   "WS2 (a): a mixed-script leading run is not a clean target prefix -> refuse");
    }

    // ---- purity: the input is never modified ----
    {
        std::wstring line(kWs2ReproLine);
        const std::wstring copy = line;
        (void)SplitSameLineTargetPrefix(line, ko);
        TEST_CHECK(line == copy, "WS2 (a): the splitter is pure (input unmodified)");
    }

    // ---- last-match-wins: the split keeps the LONGEST qualifying prefix ----
    // Two candidate boundaries; the splitter must take the RIGHT one, so the
    // already-translated run stays verbatim instead of being re-translated.
    {
        const std::wstring line = L"번역문 하나. 번역문 둘. New English tail";
        const SameLinePrefixSplit s = SplitSameLineTargetPrefix(line, ko);
        TEST_CHECK(s.split && line.substr(0, s.prefix_len) == L"번역문 하나. 번역문 둘. ",
                   "WS2 (a): the LAST qualifying boundary wins (maximal verbatim prefix)");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] 261108_0002 WS2 same-line splitter passed." << std::endl;
    } else {
        std::cout << "[FAIL] 261108_0002 WS2 same-line splitter: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---------------------------------------------------------------------------
// (b) the script-mask seam: pinned equal to engine_core for every language
// ---------------------------------------------------------------------------
void TestWs2ScriptMaskParity() {
    std::cout << "[TEST] 261108_0002 WS2 script-mask parity with engine_core" << std::endl;
    const int failures_before = g_failed_count;

    // The bit VALUES must equal engine_core's ScriptClass ordinals: the masks
    // are exchanged between the two headers, so a renumbering on either side
    // would silently mis-split.
    TEST_CHECK(kScriptMaskLatin == ToMask(ScriptClass::Latin) &&
                   kScriptMaskHangul == ToMask(ScriptClass::Hangul) &&
                   kScriptMaskHan == ToMask(ScriptClass::Han) &&
                   kScriptMaskKana == ToMask(ScriptClass::Kana) &&
                   kScriptMaskCyrillic == ToMask(ScriptClass::Cyrillic) &&
                   kScriptMaskThai == ToMask(ScriptClass::Thai) &&
                   kScriptMaskArabic == ToMask(ScriptClass::Arabic) &&
                   kScriptMaskDevanagari == ToMask(ScriptClass::Devanagari) &&
                   kScriptMaskBengali == ToMask(ScriptClass::Bengali) &&
                   kScriptMaskKhmer == ToMask(ScriptClass::Khmer) &&
                   kScriptMaskLao == ToMask(ScriptClass::Lao) &&
                   kScriptMaskHebrew == ToMask(ScriptClass::Hebrew) &&
                   kScriptMaskGreek == ToMask(ScriptClass::Greek) &&
                   kScriptMaskMyanmar == ToMask(ScriptClass::Myanmar),
               "WS2 (b): every kScriptMask* bit equals its engine_core ScriptClass ordinal");

    // EVERY registered language code must resolve to the SAME mask through both
    // tables. A language added to the registry without a row in
    // ScriptMaskForLanguageCode fails HERE (the drift guard).
    for (const LanguageInfo& lang : GetSupportedLanguages()) {
        const std::uint32_t via_core = TargetScriptClasses(lang.code);
        const std::uint32_t via_ws2 = ScriptMaskForLanguageCode(lang.code);
        TEST_CHECK(via_core == via_ws2,
                   std::string("WS2 (b): ScriptMaskForLanguageCode(\"") + lang.code +
                       "\") equals engine_core TargetScriptClasses");
        // Case-insensitivity: the config layer hands us NormalizeLanguageCode
        // output, but the helper must not depend on that.
        std::string lower = lang.code;
        for (char& ch : lower) {
            if (ch >= 'A' && ch <= 'Z') {
                ch = static_cast<char>(ch - 'A' + 'a');
            }
        }
        TEST_CHECK(ScriptMaskForLanguageCode(lower) == via_ws2,
                   std::string("WS2 (b): ScriptMaskForLanguageCode is case-insensitive for \"") +
                       lang.code + "\"");
    }

    // TextScriptClasses parity over a representative multi-script grid.
    const wchar_t* const text_cells[] = {
        L"hello", L"안녕하세요", L"日本語です", L"简体中文", L"안녕 hello",
        L"12345 !!!", L"", L"Türkçe", L"Русский", L"ไทย", L"العربية", L"עברית",
        L"हिन्दी", L"বাংলা", L"ភាសាខ្មែរ", L"ພາສາລາວ", L"မြန်မာစာ", L"Ελληνικά",
        L"😀 :laughing:", L"번역된 문장입니다. New English tail",
    };
    for (const wchar_t* cell : text_cells) {
        TEST_CHECK(ScriptMaskOfText(cell) == TextScriptClasses(cell),
                   "WS2 (b): ScriptMaskOfText equals engine_core TextScriptClasses");
    }

    // Unknown / AUTO / empty codes resolve to 0 (the splitter's fail-open
    // signal) on BOTH sides.
    TEST_CHECK(ScriptMaskForLanguageCode("AUTO") == 0 &&
                   ScriptMaskForLanguageCode("") == 0 &&
                   ScriptMaskForLanguageCode("NotALanguage") == 0,
               "WS2 (b): AUTO / empty / unknown codes resolve to mask 0 (splitter fails open)");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] 261108_0002 WS2 script-mask parity passed." << std::endl;
    } else {
        std::cout << "[FAIL] 261108_0002 WS2 script-mask parity: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---------------------------------------------------------------------------
// (c) the tail trim + the tail-level bypass composition
// ---------------------------------------------------------------------------
void TestWs2TailTrimAndBypassComposition() {
    std::cout << "[TEST] 261108_0002 WS2 tail trim + tail-level bypass" << std::endl;
    const int failures_before = g_failed_count;

    // ---- TrimLeadingHorizontalSpace ----
    TEST_CHECK(TrimLeadingHorizontalSpace(L" :laughing: 사실") == L":laughing: 사실",
               "WS2 (c): the ledger separator space is stripped from the engine input");
    TEST_CHECK(TrimLeadingHorizontalSpace(L"\t\t x") == L"x",
               "WS2 (c): a leading tab run is stripped too");
    TEST_CHECK(TrimLeadingHorizontalSpace(L"\u00A0x") == L"x",
               "WS2 (c): a leading NBSP is stripped (CanonicalFormForLedger treats it as space)");
    TEST_CHECK(TrimLeadingHorizontalSpace(L"   ") == L"",
               "WS2 (c): an all-whitespace tail trims to empty");
    TEST_CHECK(TrimLeadingHorizontalSpace(L"") == L"",
               "WS2 (c): an empty tail stays empty");
    TEST_CHECK(TrimLeadingHorizontalSpace(L"x y") == L"x y",
               "WS2 (c): interior and trailing spaces are untouched");
    TEST_CHECK(TrimLeadingHorizontalSpace(L"\r\nx") == L"\r\nx",
               "WS2 (c): a leading LINE BREAK is not horizontal space (never stripped here)");
    {
        const std::wstring tail = L" :laughing: 사실";
        const std::wstring copy = tail;
        (void)TrimLeadingHorizontalSpace(tail);
        TEST_CHECK(tail == copy, "WS2 (c): the trim is pure (input unmodified)");
    }

    // ---- the recorded-original-character contract (item 5) ----
    // The recomposition must decide the boundary space from the ORIGINAL tail
    // first character, because the engine input is now trimmed. Pin the
    // composition the worker performs:
    //   translated = prefix + [space if tail_had_leading_ws] + translation
    {
        auto recompose = [](const std::wstring& prefix, const std::wstring& tail_original,
                            const std::wstring& translation) {
            const bool tail_had_leading_ws =
                !tail_original.empty() && IsSameLineBoundarySpace(tail_original.front());
            const std::wstring engine_input = TrimLeadingHorizontalSpace(tail_original);
            std::wstring out = translation;
            auto is_ws = [](wchar_t c) {
                return c == L' ' || c == L'\n' || c == L'\r' || c == L'\t';
            };
            if (!prefix.empty() && !is_ws(prefix.back()) && !out.empty() &&
                !is_ws(out.front()) && tail_had_leading_ws) {
                out.insert(out.begin(), L' ');
            }
            out.insert(out.begin(), prefix.begin(), prefix.end());
            return std::make_pair(out, engine_input);
        };

        const auto spaced = recompose(L"번역된 문장입니다.", L" 새 English tail", L"새로 번역된 꼬리");
        TEST_CHECK(spaced.second == L"새 English tail",
                   "WS2 (c): the engine receives the TRIMMED tail");
        TEST_CHECK(spaced.first == L"번역된 문장입니다. 새로 번역된 꼬리",
                   "WS2 (c): the boundary space is preserved in the recomposition (item 5)");

        const auto unspaced = recompose(L"번역된 문장입니다.", L"새 English tail", L"새로 번역된 꼬리");
        TEST_CHECK(unspaced.first == L"번역된 문장입니다.새로 번역된 꼬리",
                   "WS2 (c): no boundary space is invented when the user typed none");

        const auto prefix_ends_space = recompose(L"번역된 문장입니다. ", L" 새 English", L"새 꼬리");
        TEST_CHECK(prefix_ends_space.first == L"번역된 문장입니다. 새 꼬리",
                   "WS2 (c): a prefix already ending in space gets no second space");

        const auto tail_break = recompose(L"번역된 문장입니다.", L"\r\n새 English", L"새 꼬리");
        TEST_CHECK(tail_break.first == L"번역된 문장입니다.새 꼬리",
                   "WS2 (c): a leading LINE BREAK is not boundary space (legacy behavior kept)");
    }

    // ---- the tail-level bypass composition (item 2) ----
    // translate_decision_bypassed = tail.empty() ? whole_line : tail_level.
    // The whole-line flag keeps its legacy consumers; only the TRANSLATION
    // decision reads the tail-level verdict.
    {
        const std::string ko_target = "Korean";
        // The reported shape, measured: a LONG Korean run (the already-
        // translated text) followed by a SHORT English tail. As a whole line
        // the Korean target sees a Korean-dominant letter mix and
        // TargetScriptDominant bypasses it (ShouldTranslate == false) - the
        // bug. The TAIL alone is English under a Korean target, so the
        // tail-level verdict is TRUE and the engine must run.
        const std::wstring ko_long_en_tail =
            L"사실 저는 astrology에 대해서는 전혀 모릅니다. 다만, 2000년 전에 쓰여진 의학서적이 "
            L"'경험'을 근간으로 했으며, 최신의학에서의 '면역학'에 상당히 가깝다는 점을 주목했을 뿐입니다. Hi.";
        const std::wstring tail = L"Hi.";
        const bool whole_bypassed = !ShouldTranslate(ko_long_en_tail, ko_target, "Auto Detect");
        const bool tail_bypassed = !ShouldTranslate(tail, ko_target, "Auto Detect");
        TEST_CHECK(whole_bypassed,
                   "WS2 (c): a Korean-dominant line with an English tail IS whole-line bypassed (the reported gate)");
        TEST_CHECK(!tail_bypassed,
                   "WS2 (c): the English tail alone is NOT bypassed under a Korean target -> the engine runs");

        // The composition rule itself: empty tail -> the whole-line flag; a
        // non-empty tail -> the tail-level flag. This is the exact expression
        // worker.cpp evaluates.
        auto decision_bypassed = [](bool whole, bool tail_bypassed_flag, bool tail_empty) {
            return tail_empty ? whole : tail_bypassed_flag;
        };
        TEST_CHECK(decision_bypassed(whole_bypassed, tail_bypassed, true) == true,
                   "WS2 (c): empty tail -> the whole-line flag owns the decision (legacy byte-identical)");
        TEST_CHECK(decision_bypassed(whole_bypassed, tail_bypassed, false) == false,
                   "WS2 (c): non-empty tail -> the tail-level flag overrides a whole-line bypass (the fix)");
        TEST_CHECK(decision_bypassed(false, true, false) == true,
                   "WS2 (c): non-empty tail -> a tail-level bypass is honoured even when the whole line would translate");
        TEST_CHECK(decision_bypassed(false, false, false) == false,
                   "WS2 (c): non-empty tail, both flags clear -> translate (unchanged)");
        TEST_CHECK(decision_bypassed(true, true, false) == true,
                   "WS2 (c): non-empty tail, both flags set -> still bypassed (no spurious engine call)");
    }

    // ---- the shortcode recognizer edge used by the splitter ----
    {
        const std::wstring ok = L"a :laughing: b";
        TEST_CHECK(SameLineShortcodeEnd(ok, 2) == 12,
                   "WS2 (c): ':laughing:' is recognized and its end index is one past the closing ':'");
        const std::wstring clock = L"a 12:30:45 b";
        TEST_CHECK(SameLineShortcodeEnd(clock, 4) == std::wstring_view::npos,
                   "WS2 (c): a clock body ('30') is not a shortcode");
        const std::wstring signed_code = L"a :+1: b";
        TEST_CHECK(SameLineShortcodeEnd(signed_code, 2) == 6,
                   "WS2 (c): a signed shortcode (':+1:') is recognized");
        const std::wstring unterminated = L"a :laughing b";
        TEST_CHECK(SameLineShortcodeEnd(unterminated, 2) == std::wstring_view::npos,
                   "WS2 (c): an unterminated ':' run is not a shortcode");
        const std::wstring long_body = L"a :abcdefghijklmnopqrstuvwxyz012345678: b";
        TEST_CHECK(SameLineShortcodeEnd(long_body, 2) == std::wstring_view::npos,
                   "WS2 (c): a body over the 32-character cap is not a shortcode");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] 261108_0002 WS2 tail trim + bypass composition passed." << std::endl;
    } else {
        std::cout << "[FAIL] 261108_0002 WS2 tail trim + bypass composition: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---------------------------------------------------------------------------
// (d) the ledger-re-anchor edge: the strict canonical byte-prefix test
// ---------------------------------------------------------------------------
void TestWs2LedgerReanchorPrefixEdge() {
    std::cout << "[TEST] 261108_0002 WS2 ledger re-anchor prefix edge" << std::endl;
    const int failures_before = g_failed_count;

    // The re-anchor fires only when the live capture still STARTS WITH the
    // remembered ledger (strict canonical byte prefix, ADR-001). These pins
    // exercise the exact test the worker performs, on the shapes the emoji
    // popup Enter produces.
    auto reanchor_would_fire = [](std::wstring_view line, std::wstring_view ledger_canonical) {
        if (ledger_canonical.empty()) {
            return false;
        }
        const std::wstring line_canonical = CanonicalFormForLedger(line);
        return line_canonical.size() >= ledger_canonical.size() &&
               line_canonical.compare(0, ledger_canonical.size(), ledger_canonical) == 0;
    };

    // The popup-accept Enter: the user typed ":laughing:" after our paste, so
    // the capture is [our translation][separator][shortcode]. The ledger is
    // still a prefix -> re-anchor fires and the next Enter sees the shortcode
    // as part of the remembered text.
    {
        const std::wstring ledger_raw = L"번역된 문장입니다.";
        const std::wstring ledger_canonical = CanonicalFormForLedger(ledger_raw);
        TEST_CHECK(reanchor_would_fire(L"번역된 문장입니다. :laughing:", ledger_canonical),
                   "WS2 (d): the emoji-popup capture still starts with the ledger -> re-anchor fires");
        TEST_CHECK(reanchor_would_fire(L"번역된 문장입니다. :laughing: 사실 저는 모릅니다.", ledger_canonical),
                   "WS2 (d): the re-anchored ledger prefixes the next capture (the chain continues)");
    }

    // A capture that DIVERGED must not re-anchor (the ledger self-invalidates
    // and the next Enter routes to F3 exactly as before).
    {
        const std::wstring ledger_canonical = CanonicalFormForLedger(L"번역된 문장입니다.");
        TEST_CHECK(!reanchor_would_fire(L"사용자가 고친 문장입니다. :laughing:", ledger_canonical),
                   "WS2 (d): an edited prefix never re-anchors (strict byte prefix, no fuzzy match)");
        TEST_CHECK(!reanchor_would_fire(L"번역", ledger_canonical),
                   "WS2 (d): a SHORTER capture than the ledger never re-anchors");
        TEST_CHECK(!reanchor_would_fire(L"번역된 문장입니다. :laughing:", L""),
                   "WS2 (d): no ledger -> no re-anchor");
    }

    // Canonical space absorbs the editor re-serialization (S2 contract): a
    // trailing space the editor added must not defeat the prefix test.
    {
        const std::wstring ledger_canonical = CanonicalFormForLedger(L"translated line");
        TEST_CHECK(reanchor_would_fire(L"translated line  :laughing:", ledger_canonical),
                   "WS2 (d): editor trailing whitespace is absorbed by the canonical form");
    }

    // The re-anchor keeps the ledger pointing at LIVE text. After the popup
    // Enter the live text is [our translation][separator][shortcode]; the
    // re-anchor moves the ledger end past the shortcode, so the NEXT Enter's
    // tail is only the newly typed text (the shortcode is never re-sent to
    // the engine). Both verdicts stay PrefixWithTail - the re-anchor shortens
    // the TAIL, it does not change the verdict class.
    {
        const std::wstring reanchored = CanonicalFormForLedger(L"번역된 문장입니다. :laughing:");
        const std::wstring stale = CanonicalFormForLedger(L"번역된 문장입니다.");
        const std::wstring next_capture = L"번역된 문장입니다. :laughing: 사실 저는 모릅니다.";
        const std::wstring next_canonical = CanonicalFormForLedger(next_capture);
        TEST_CHECK(AnalyzeCaptureVsLastPaste(next_canonical, reanchored) ==
                       PasteLedgerVerdict::PrefixWithTail,
                   "WS2 (d): after the re-anchor the next Enter is a clean PrefixWithTail");
        TEST_CHECK(AnalyzeCaptureVsLastPaste(next_canonical, stale) ==
                       PasteLedgerVerdict::PrefixWithTail,
                   "WS2 (d): the stale ledger is still a byte prefix (verdict class unchanged)");
        // The difference the re-anchor makes is the TAIL SIZE: without it the
        // shortcode is part of the tail and would be re-sent to the engine.
        const std::wstring tail_reanchored = next_canonical.substr(reanchored.size());
        const std::wstring tail_stale = next_canonical.substr(stale.size());
        TEST_CHECK(tail_reanchored == L" 사실 저는 모릅니다.",
                   "WS2 (d): with the re-anchor the tail is only the newly typed Korean");
        TEST_CHECK(tail_stale.find(L":laughing:") != std::wstring::npos,
                   "WS2 (d): without it the shortcode rides along in the tail (re-sent to the engine)");
        TEST_CHECK(tail_stale.size() > tail_reanchored.size(),
                   "WS2 (d): the re-anchor strictly shortens the tail (less engine input, no re-translation)");
    }
    // A genuinely diverged ledger IS a NoMatch, and the re-anchor refuses it
    // (the strict prefix test above), so the next Enter routes to F3 as before.
    {
        const std::wstring diverged = CanonicalFormForLedger(L"사용자가 고친 문장입니다.");
        const std::wstring next_capture = L"번역된 문장입니다. :laughing: 사실 저는 모릅니다.";
        TEST_CHECK(AnalyzeCaptureVsLastPaste(CanonicalFormForLedger(next_capture), diverged) ==
                       PasteLedgerVerdict::NoMatch,
                   "WS2 (d): a diverged ledger is a NoMatch and the re-anchor refuses it (F3 owns the capture)");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] 261108_0002 WS2 ledger re-anchor edge passed." << std::endl;
    } else {
        std::cout << "[FAIL] 261108_0002 WS2 ledger re-anchor edge: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---------------------------------------------------------------------------
// (e) structural pins on src/worker.cpp
// ---------------------------------------------------------------------------
void TestWs2WorkerWiringPins() {
    std::cout << "[TEST] 261108_0002 WS2 worker.cpp wiring pins" << std::endl;
    const int failures_before = g_failed_count;

    const std::string src = Ws2ReadSource(L"src\\worker.cpp");
    if (src.empty()) {
        std::cout << "[SKIP] src/worker.cpp not resolvable from the test CWD; WS2 wiring pins skipped."
                  << std::endl;
        return;
    }

    // Item 1: the engine input is the TRIMMED tail, and the original first
    // character is recorded BEFORE the trim (item 5 depends on it).
    TEST_CHECK(src.find("const std::wstring& engine_input = untranslated_tail.empty() ? line : tail_for_engine;") !=
                   std::string::npos,
               "WS2 (e) item 1: engine_input uses tail_for_engine (the trimmed tail)");
    TEST_CHECK(src.find("TrimLeadingHorizontalSpace(untranslated_tail)") != std::string::npos,
               "WS2 (e) item 1: the tail is trimmed through the shared helper");
    TEST_CHECK(src.find("const bool tail_had_leading_ws =") != std::string::npos,
               "WS2 (e) item 1/5: tail_had_leading_ws records the ORIGINAL first character");
    {
        // The record MUST precede the trim (it reads the untrimmed string).
        const std::size_t record = src.find("const bool tail_had_leading_ws =");
        const std::size_t trim = src.find("TrimLeadingHorizontalSpace(untranslated_tail)");
        TEST_CHECK(record != std::string::npos && trim != std::string::npos && record < trim,
                   "WS2 (e) item 1/5: tail_had_leading_ws is computed BEFORE the trim");
    }

    // Item 5: the recomposition reads the recorded flag, never the trimmed
    // string (reading the trimmed one would drop the separator space).
    TEST_CHECK(src.find("tail_had_leading_ws) {") != std::string::npos,
               "WS2 (e) item 5: the boundary-space gate reads tail_had_leading_ws");
    TEST_CHECK(src.find("is_ws(untranslated_tail.front())") == std::string::npos,
               "WS2 (e) item 5: the recomposition no longer reads the (now trimmed) tail directly");

    // Item 2: the tail-level bypass exists, is computed only for a non-empty
    // tail, and drives the translation decision while the whole-line flag
    // keeps its own consumers.
    TEST_CHECK(src.find("const bool tail_smart_bypassed =") != std::string::npos,
               "WS2 (e) item 2: tail_smart_bypassed is computed");
    TEST_CHECK(src.find("const bool translate_decision_bypassed =") != std::string::npos,
               "WS2 (e) item 2: the translation decision has its own named flag");
    TEST_CHECK(src.find("const bool should_translate = !line.empty() && !translate_decision_bypassed;") !=
                   std::string::npos,
               "WS2 (e) item 2: should_translate reads the composed decision");
    TEST_CHECK(src.find("!ShouldTranslate(tail_for_engine, snap.type_target_language, snap.type_source_language)") !=
                   std::string::npos,
               "WS2 (e) item 2: the tail verdict is judged on the TRIMMED tail the engine receives");
    // The whole-line flag keeps every legacy consumer byte-identical.
    TEST_CHECK(src.find("const bool was_smart_bypassed =") != std::string::npos &&
                   src.find("!line.empty() && !ShouldTranslate(line, snap.type_target_language,") !=
                       std::string::npos,
               "WS2 (e) item 2: the whole-line flag is still computed on the whole line (unchanged)");
    TEST_CHECK(src.find("EmptyCaptureNeedsHold(line.empty(), was_smart_bypassed)") != std::string::npos,
               "WS2 (e) item 2: the empty-capture hold keeps the whole-line flag (legacy consumer)");
    TEST_CHECK(src.find("EmptyCapturePromotesToSend(line.empty(), was_smart_bypassed, last_paste_valid,") !=
                   std::string::npos,
               "WS2 (e) item 2: the promote gate keeps the whole-line flag (legacy consumer)");
    TEST_CHECK(src.find("EqualsSourceNeedsSendThrough(line.empty(), was_smart_bypassed)") != std::string::npos,
               "WS2 (e) item 2: the identity-outcome predicate keeps the whole-line flag (legacy consumer)");
    TEST_CHECK(src.find("PastedPrefixNeedsSkip(true, was_smart_bypassed, false)") != std::string::npos,
               "WS2 (e) item 2: the ExactMatch skip keeps the whole-line flag (legacy consumer)");

    // Item 3: the ledger re-anchor lives INSIDE the smart-bypass send-through
    // arm, before the send-through helper, and refreshes BOTH ledger halves.
    {
        const std::size_t arm = src.find("if (line.empty() || !should_translate) {");
        const std::size_t reanchor = src.find("stage=ledger_maint decision=reanchor");
        const std::size_t helper = src.find("SendThroughWithNewlineTracking(task.target_hwnd", arm);
        TEST_CHECK(arm != std::string::npos && reanchor != std::string::npos &&
                       helper != std::string::npos && arm < reanchor && reanchor < helper,
                   "WS2 (e) item 3: the re-anchor sits inside the bypass arm, before the send-through helper");
        TEST_CHECK(src.find("line_canonical_reanchor.compare(0, last_paste_canonical_.size(),") !=
                       std::string::npos,
                   "WS2 (e) item 3: the re-anchor uses a strict canonical byte-prefix test (ADR-001)");
        // Both halves are refreshed adjacent (the S2 §7.1.1 lifecycle rule:
        // no path may write one without the other).
        const std::size_t text_store = src.find("last_paste_text_ = line;");
        const std::size_t canon_store = src.find("last_paste_canonical_ = line_canonical_reanchor;");
        TEST_CHECK(text_store != std::string::npos && canon_store != std::string::npos &&
                       canon_store - text_store < 200,
                   "WS2 (e) item 3: the re-anchor writes last_paste_text_ and last_paste_canonical_ adjacently");
    }

    // Item 4: the splitter is wired after the F3 refine and before the tail
    // handling, guarded by the empty-tail / non-ExactMatch / non-empty-block
    // conditions, and its prefix/tail assignment covers the capture exactly.
    {
        const std::size_t f3_call = src.find("f3 = F3BlockSliceRefine(");
        const std::size_t splitter = src.find("SplitSameLineTargetPrefix(line, target_script_mask)");
        const std::size_t trim = src.find("TrimLeadingHorizontalSpace(untranslated_tail)");
        TEST_CHECK(f3_call != std::string::npos && splitter != std::string::npos &&
                       trim != std::string::npos && f3_call < splitter && splitter < trim,
                   "WS2 (e) item 4: the splitter runs after the F3 refine and before the tail trim");
        TEST_CHECK(src.find("ledger_verdict != PasteLedgerVerdict::ExactMatch") != std::string::npos,
                   "WS2 (e) item 4: the splitter refuses the ExactMatch verdict (our own output)");
        TEST_CHECK(src.find("pasted_prefix_text.assign(line, 0, same_line.prefix_len);") != std::string::npos &&
                       src.find("untranslated_tail.assign(line, same_line.prefix_len,") != std::string::npos,
                   "WS2 (e) item 4: the split assigns prefix and tail from the RAW capture (verbatim prefix)");
        TEST_CHECK(src.find("stage=same_line_split") != std::string::npos,
                   "WS2 (e) item 4: the split is observable in the DIAG log (shape-only: lengths + mask)");
    }

    // The pre-existing ledger-maintenance contract is untouched: still exactly
    // TWO clears (the has_text=0 fast-path + the maintenance wipe).
    TEST_CHECK(Ws2CountOcc(src, "last_paste_text_.clear()") == 2,
               "WS2 (e): the two last_paste_text_ clears are unchanged (the re-anchor adds no clear)");
    TEST_CHECK(src.find("stage=ledger_maint decision=clear") != std::string::npos,
               "WS2 (e): the task-end ledger-maintenance wipe is unchanged");

    // The workstream adds no new send-through terminal and no new release
    // site: every pre-existing structural pin keeps its count.
    TEST_CHECK(Ws2CountOcc(src, "SendThroughWithNewlineTracking(task.target_hwnd") == 8,
               "WS2 (e): still 8 send-through helper call sites (the workstream adds none)");
    TEST_CHECK(Ws2CountOcc(src, "ReleaseSelectionOnce();") == 5,
               "WS2 (e): still 5 ReleaseSelectionOnce sites (the workstream adds none)");
    TEST_CHECK(Ws2CountOcc(src, "if (SelectionReleaseRequired(pasted)) {") == 1,
               "WS2 (e): still exactly ONE REQ-R03 release gate");
    TEST_CHECK(Ws2CountOcc(src, "if (RescueLiveSelectionNeedsConsume(") == 5,
               "WS2 (e): still 5 guarded consume sites");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] 261108_0002 WS2 worker wiring pins passed." << std::endl;
    } else {
        std::cout << "[FAIL] 261108_0002 WS2 worker wiring pins: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}
