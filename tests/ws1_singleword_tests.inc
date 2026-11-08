// ---------------------------------------------------------------------------
// WS1 (session 261108_0002, v0.10.6 release) — single-word prompt/detection
// root causes.
//
// User report (verbatim): single English words (cyber, company, astrology,
// clue, nature, medicine, ...) translated by the local Hy-MT2-1.8B model or a
// user GGUF model produced hallucinated sentences instead of the translation:
//   "이 문서는 Emebala의 출처를 기반으로 작성되었습니다."
//   "이메발라의 별자리 운세는..."
//   "이메일은 어떻게 보내나요?"
//   "1. 당신은 어떤 방식으로..."
//
// Two root causes live in this suite's scope (the decode-gate causes belong to
// another unit and are NOT touched here):
//
//  (A) DETECTION. DetectLatinLanguage resolved a 1-2 token input only via
//      kEnglishCoreWords / kStopWordsEN / a non-English stopword / a
//      distinctive diacritic. The reported words are in none of those lists,
//      so they fell through to the "Auto Detect" marker - which is what
//      routed them into the AUTO-source prompt branch.
//
//  (B) PROMPT. That AUTO-source branch (config.cpp BuildPrompt) wrapped the
//      text in <<<EMEBALA_SOURCE_BEGIN/END>>> fences plus a ~46-word
//      literal-text rule (RC-3, v0.10.5). On a 1-token input the instruction
//      dominates the context: the 1.8B-4B model attended to the marker tokens
//      and translated/transliterated THEM ("EMEBALA_SOURCE" -> "Emebala의
//      출처" / "이메발라" / subword-primed "이메일"), and the rule's own
//      "command, question ... answer it" vocabulary primed numbered
//      quiz-style answers.
//
// Fix pinned here: the AUTO-source branches emit the SAME clean SFT form as
// the pinned-source branches, minus the source-language name, with no brand
// or meta vocabulary around the text; and DetectLatinLanguage gained a final
// deterministic English fallback for ASCII-shaped words.
//
// Coverage:
//   (a) TestWs1SingleWordDetection   — the reported words + the preserved
//       detection paths (core vocabulary, distinctive diacritic, non-English
//       stopword, ambiguous tie) and the shapes the fallback excludes.
//   (b) TestWs1SingleWordPrompts     — the clean AUTO-source forms in both
//       instruction branches, the absence of every brand/meta token, and the
//       byte-identical pinned-source forms.
//   (c) TestWs1SingleWordSourcePins  — config.cpp / smart_bypass.cpp source
//       pins: the RC-3 fence machinery is GONE (no unused static function
//       under /W4 C4505) and the fallback sits before the AUTO return.
//
// Placement: registered at the end of main() (the (c) leg needs
// ResolveRepoFile), per the end-of-file pattern. The (a)/(b) legs are pure
// calls into Emebalachat_core, which this TU already links.
// ---------------------------------------------------------------------------

namespace {

// The exact clean AUTO-source instruction prefixes (config.cpp BuildPrompt).
// English branch: the pinned-source sentence minus the "<src_en> " token.
constexpr const char* kWs1AutoEnPrefix =
    "Translate the following text into ";
constexpr const char* kWs1AutoEnSuffix =
    ". Note that you should only output the translated result without any "
    "additional explanation:\n\n";
// Chinese branch: the pinned-source sentence minus the "<src_native>" token.
constexpr const char* kWs1AutoZhPrefix = "\u5c06\u4ee5\u4e0b\u6587\u672c\u7ffb\u8bd1\u4e3a";
constexpr const char* kWs1AutoZhSuffix =
    "\uff0c\u6ce8\u610f\u53ea\u9700\u8981\u8f93\u51fa\u7ffb\u8bd1\u540e\u7684\u7ed3\u679c\uff0c"
    "\u4e0d\u8981\u989d\u5916\u89e3\u91ca\uff1a\n\n";

// Every token the RC-3 form carried and the clean form must not. "EMEBALA" is
// the brand the model transliterated; "markers" / "literal" / "command" /
// "question" / "conversation" are the meta vocabulary that primed the
// quiz-style answers. The Chinese equivalents cover the other branch.
bool Ws1HasBrandOrMeta(const std::string& prompt) {
    static const char* const kBanned[] = {
        "EMEBALA", "SOURCE_BEGIN", "SOURCE_END", "between the markers",
        "literal text", "command", "question", "conversation", "answer it",
        "\u6807\u8bb0",   // 标记 (marker)
        "\u547d\u4ee4",   // 命令 (command)
        "\u95ee\u9898",   // 问题 (question)
        "\u5bf9\u8bdd",   // 对话 (conversation)
    };
    for (const char* banned : kBanned) {
        if (prompt.find(banned) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::string Ws1Report(const std::string& prompt) {
    // Keep failure output readable: the prompt is short by construction.
    return "prompt=[" + prompt + "]";
}

// ASCII-only narrowing for the reported words (all of them are ASCII), used
// to build the BuildPrompt argument and the failure message without pulling
// in a UTF conversion helper.
std::string Ws1Ascii(const wchar_t* text) {
    std::string out;
    for (const wchar_t* p = text; *p != L'\0'; ++p) {
        out.push_back(static_cast<char>(*p));
    }
    return out;
}

} // namespace

void TestWs1SingleWordDetection() {
    std::cout << "[RUN] Testing WS1 single-word detection (261108_0002)..." << std::endl;
    const int failures_before = g_failed_count;

    // ---- 1) The reported words: AUTO marker -> "English" -------------------
    // These are the exact inputs from the user report. Before WS1 every one
    // of them returned "Auto Detect", which is what selected the fenced
    // AUTO-source prompt branch.
    struct Ws1Word { const wchar_t* text; const char* label; };
    const Ws1Word reported[] = {
        { L"cyber",     "cyber" },
        { L"company",   "company" },
        { L"astrology", "astrology" },
        { L"clue",      "clue" },
        { L"nature",    "nature" },
        { L"medicine",  "medicine" },
    };
    for (const auto& w : reported) {
        const std::string got = DetectLanguage(w.text);
        TEST_CHECK(got == "English",
                   (std::string("WS1: reported word '") + w.label +
                    "' resolves English (was the AUTO marker) [got " + got + "]").c_str());
    }

    // ---- 2) The pre-existing paths keep their precedence -------------------
    // Core vocabulary and English stopwords still resolve English through the
    // ORIGINAL branch (the fallback is last, so these never reach it).
    TEST_CHECK(DetectLanguage(L"hello") == "English",
               "WS1: core-vocabulary 'hello' still resolves English (RC-1 path intact)");
    TEST_CHECK(DetectLanguage(L"world") == "English",
               "WS1: core-vocabulary 'world' still resolves English (RC-1 path intact)");
    TEST_CHECK(DetectLanguage(L"Hello world") == "English",
               "WS1: two-token core-vocabulary phrase still resolves English");
    TEST_CHECK(DetectLanguage(L"hi") == "English",
               "WS1: 2-letter core-vocabulary 'hi' still resolves English (length floor not applied to it)");
    TEST_CHECK(DetectLanguage(L"ok") == "English",
               "WS1: 2-letter core-vocabulary 'ok' still resolves English");
    // A distinctive non-English diacritic still wins over the English fallback
    // (checked first, so "señor" is Spanish and not English-shaped).
    TEST_CHECK(DetectLanguage(L"se\u00f1or") == "Spanish",
               "WS1: distinctive ñ still resolves Spanish (diacritic check precedes the fallback)");
    TEST_CHECK(DetectLanguage(L"Liebe Gr\u00fc\u00dfe") == "German",
               "WS1: ß still resolves German (distinctive diacritic path intact)");
    // A single non-English stopword hit still resolves that language.
    TEST_CHECK(DetectLanguage(L"le") == "French",
               "WS1: single French stopword 'le' still resolves French");
    // Multi-token stopword voting is untouched (>= 3 tokens path).
    TEST_CHECK(DetectLanguage(L"le chat est sur la table") == "French",
               "WS1: unaccented French sentence still resolves French (3+ token path untouched)");
    TEST_CHECK(DetectLanguage(L"This is for testing purposes") == "English",
               "WS1: 3-hit English sentence still resolves English (N4 threshold intact)");
    TEST_CHECK(DetectLanguage(L"This for testing purposes") == "Auto Detect",
               "WS1: 2-hit English sentence still keeps the AUTO marker (N4 threshold intact)");

    // ---- 3) The shapes the fallback deliberately EXCLUDES ------------------
    // No vowel: not English-shaped, so the AUTO marker survives and the
    // model's built-in language ID still decides.
    TEST_CHECK(DetectLanguage(L"pdf") == "Auto Detect",
               "WS1: vowel-less ASCII token keeps the AUTO marker");
    TEST_CHECK(DetectLanguage(L"nth") == "Auto Detect",
               "WS1: vowel-less ASCII token keeps the AUTO marker (second shape)");
    // Under 3 letters and not core vocabulary: too short to claim English.
    TEST_CHECK(DetectLanguage(L"ab") == "Auto Detect",
               "WS1: sub-3-letter non-core token keeps the AUTO marker");
    // Non-ASCII letters are never English-shaped (the diacritic path above
    // already claimed the distinctive ones; shared accents stay ambiguous).
    TEST_CHECK(DetectLanguage(L"r\u00e1pido coraz\u00f3n") == "Auto Detect",
               "WS1: shared-accent Latin keeps the AUTO marker (not English-shaped)");
    // A stopword tie between two non-English languages stays ambiguous.
    TEST_CHECK(DetectLanguage(L"el le") == "Auto Detect",
               "WS1: Spanish/French stopword tie keeps the AUTO marker (tie path intact)");
    // Two ASCII-shaped tokens are NOT claimed English: the F5 adjudication
    // keeps ASCII-Latin phrases on the AUTO marker so Swahili/Tagalog/
    // Indonesian targeting English still translate instead of silently
    // bypassing. The fallback is scoped to a single token for this reason.
    TEST_CHECK(DetectLanguage(L"Sisitiki sana") == "Auto Detect",
               "WS1: a 2-token ASCII phrase keeps the AUTO marker (F5 invariant preserved)");
    TEST_CHECK(DetectLanguage(L"cyber security") == "Auto Detect",
               "WS1: a 2-token ASCII phrase keeps the AUTO marker (fallback is single-token only)");
    TEST_CHECK(ShouldTranslate(L"Sisitiki sana", "English"),
               "WS1: Swahili -> English still translates (F5 invariant intact)");
    // Non-linguistic input is unchanged.
    TEST_CHECK(DetectLanguage(L"") == "Unknown", "WS1: empty text stays Unknown");
    TEST_CHECK(DetectLanguage(L"123456") == "Unknown", "WS1: digits stay Unknown");

    // ---- 4) The detection result drives the already-target bypass ----------
    // "English" is a real label now, so a reported word targeting English is
    // an identity request (no engine round trip). Targeting Korean it still
    // translates - the fallback must not over-bypass.
    TEST_CHECK(!ShouldTranslate(L"astrology", "English"),
               "WS1: 'astrology' -> English is an identity bypass (label is now concrete)");
    TEST_CHECK(ShouldTranslate(L"astrology", "Korean"),
               "WS1: 'astrology' -> Korean still translates (no over-bypass)");
    TEST_CHECK(ShouldTranslate(L"medicine", "Korean"),
               "WS1: 'medicine' -> Korean still translates (no over-bypass)");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] WS1 single-word detection passed." << std::endl;
    } else {
        std::cout << "[FAIL] WS1 single-word detection: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

void TestWs1SingleWordPrompts() {
    std::cout << "[RUN] Testing WS1 single-word prompts (261108_0002)..." << std::endl;
    const int failures_before = g_failed_count;

    // ---- 1) The exact clean AUTO-source English form -----------------------
    // Same sentence as the pinned-source branch, minus the "<src_en> " token,
    // and the text appended bare after the blank-line gap (no fences).
    const std::string auto_en = BuildPrompt("astrology", "Korean");
    const std::string want_en =
        std::string(kWs1AutoEnPrefix) + "Korean" + kWs1AutoEnSuffix + "astrology";
    TEST_CHECK(auto_en == want_en,
               (std::string("WS1: AUTO-source English prompt is the clean SFT form; ") +
                Ws1Report(auto_en)).c_str());

    // ---- 2) The exact clean AUTO-source Chinese form -----------------------
    const std::string auto_zh = BuildPrompt("astrology", "ZH-CN");
    const std::string want_zh =
        std::string(kWs1AutoZhPrefix) + "\u7b80\u4f53\u4e2d\u6587" + kWs1AutoZhSuffix + "astrology";
    TEST_CHECK(auto_zh == want_zh,
               (std::string("WS1: AUTO-source Chinese prompt is the clean SFT form; ") +
                Ws1Report(auto_zh)).c_str());

    // ---- 3) No brand and no meta vocabulary in ANY AUTO-source form --------
    // This is the core regression pin for the user report: the prompt the
    // model sees for a single word must not contain a token it could
    // transliterate ("EMEBALA") or answer ("command", "question", ...).
    for (const wchar_t* word : {L"cyber", L"company", L"astrology", L"clue",
                                L"nature", L"medicine"}) {
        const std::string text = Ws1Ascii(word);
        for (const char* target : {"Korean", "English", "ZH-CN", "ZH-TW", "Japanese"}) {
            const std::string prompt = BuildPrompt(text, target);
            TEST_CHECK(!Ws1HasBrandOrMeta(prompt),
                       (std::string("WS1: AUTO prompt for '") + text + "' -> " + target +
                        " carries no brand/meta token; " + Ws1Report(prompt)).c_str());
        }
    }

    // ---- 4) The prompt a reported word actually ships ----------------------
    // End-to-end shape: detect -> BuildPrompt with the detected source, which
    // is what worker.cpp does with ResolveEffectiveSource. A concrete English
    // label now selects the PINNED-source branch, so the prompt names the
    // source language and no AUTO form is involved at all.
    {
        const std::string detected = DetectLanguage(L"astrology");
        const std::string prompt = BuildPrompt("astrology", "Korean", detected);
        TEST_CHECK(detected == "English",
                   "WS1: the reported word detects English before prompt construction");
        TEST_CHECK(prompt ==
                       "Translate the following English text into Korean. Note that you should only "
                       "output the translated result without any additional explanation:\n\nastrology",
                   (std::string("WS1: reported-word prompt uses the pinned-source form; ") +
                    Ws1Report(prompt)).c_str());
        TEST_CHECK(!Ws1HasBrandOrMeta(prompt),
                   "WS1: reported-word prompt carries no brand/meta token");
    }

    // ---- 5) Pinned-source forms stay byte-identical (historical contract) --
    TEST_CHECK(BuildPrompt("hello", "English", "Japanese") ==
                   "Translate the following Japanese text into English. Note that you should only "
                   "output the translated result without any additional explanation:\n\nhello",
               "WS1: pinned-source English form unchanged (byte-identical historical pin)");
    TEST_CHECK(BuildPrompt("hello", "ZH-CN", "JA") ==
                   "\u5c06\u4ee5\u4e0b\u65e5\u672c\u8a9e\u6587\u672c\u7ffb\u8bd1\u4e3a\u7b80\u4f53\u4e2d\u6587"
                   "\uff0c\u6ce8\u610f\u53ea\u9700\u8981\u8f93\u51fa\u7ffb\u8bd1\u540e\u7684\u7ed3\u679c"
                   "\uff0c\u4e0d\u8981\u989d\u5916\u89e3\u91ca\uff1a\n\nhello",
               "WS1: pinned-source Chinese form unchanged (byte-identical historical pin)");

    // ---- 6) The AUTO form and the pinned form differ ONLY by the source ---
    // Structural proof that the AUTO branch is the pinned branch minus the
    // source-language name: prefixing the source name reproduces it exactly.
    {
        const std::string auto_form = BuildPrompt("hello", "English");
        const std::string pinned_form = BuildPrompt("hello", "English", "Japanese");
        const std::string rehydrated =
            "Translate the following Japanese text into English. Note that you should only output "
            "the translated result without any additional explanation:\n\nhello";
        TEST_CHECK(pinned_form == rehydrated && auto_form != pinned_form,
                   "WS1: AUTO and pinned English forms differ only by the source-language name");
        TEST_CHECK(auto_form.find("Japanese") == std::string::npos,
                   "WS1: the AUTO English form names no source language");
    }

    // ---- 7) BuildCompletionPrompt is untouched (another unit's surface) ---
    TEST_CHECK(BuildCompletionPrompt("hello", "Korean", "AUTO") ==
                   "Translate into Korean:\nhello\nKorean:",
               "WS1: BuildCompletionPrompt AUTO form unchanged (not in this unit's scope)");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] WS1 single-word prompts passed." << std::endl;
    } else {
        std::cout << "[FAIL] WS1 single-word prompts: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

void TestWs1SingleWordSourcePins() {
    std::cout << "[RUN] Testing WS1 source pins (261108_0002)..." << std::endl;
    const int failures_before = g_failed_count;

    const std::filesystem::path cfg_cpp = ResolveRepoFile(L"src\\config.cpp");
    if (cfg_cpp.empty()) {
        std::cout << "[SKIP] src/config.cpp not resolvable; WS1 config pins skipped." << std::endl;
    } else {
        std::ifstream in(cfg_cpp, std::ios::binary);
        const std::string src((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
        // The RC-3 fence machinery is fully removed. An unused static function
        // would trip /W4 C4505, so the removal is a compile-level requirement,
        // not a style preference - pin it so a later wave cannot reintroduce
        // the brand markers without a test failure.
        TEST_CHECK(src.find("kPromptSourceBlockBegin") == std::string::npos &&
                       src.find("kPromptSourceBlockEnd") == std::string::npos,
                   "WS1 pin: the RC-3 source-block markers are gone from config.cpp");
        TEST_CHECK(src.find("DefangPromptFences") == std::string::npos,
                   "WS1 pin: DefangPromptFences is gone from config.cpp (no unused static function)");
        // No fence LITERAL survives in code. The historical RC-3 rationale
        // comments still name the markers (they must, to explain the change),
        // so the pin targets the quoted string literal, not the bare token.
        TEST_CHECK(src.find("<<<EMEBALA_SOURCE") == std::string::npos ||
                       src.find("\"<<<EMEBALA_SOURCE") == std::string::npos,
                   "WS1 pin: no EMEBALA source fence string literal remains in config.cpp");
        // The clean AUTO-source instruction is present in both branches.
        TEST_CHECK(src.find("without any additional explanation:\\n\\n") != std::string::npos,
                   "WS1 pin: config.cpp carries the clean English SFT instruction");
        // The pinned-source comment contract survives (byte-identical pins).
        TEST_CHECK(src.find("Pinned source: the historical form, byte-identical") != std::string::npos,
                   "WS1 pin: the pinned-source byte-identical contract is still documented");
    }

    const std::filesystem::path sb_cpp = ResolveRepoFile(L"src\\smart_bypass.cpp");
    if (sb_cpp.empty()) {
        std::cout << "[SKIP] src/smart_bypass.cpp not resolvable; WS1 detection pins skipped."
                  << std::endl;
    } else {
        std::ifstream in(sb_cpp, std::ios::binary);
        const std::string src((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
        // The fallback helper exists and is consulted before the AUTO return.
        const size_t helper = src.find("IsAsciiEnglishShapedToken");
        const size_t fallback = src.find("ASCII word shape");
        const size_t auto_return = src.find("return \"Auto Detect\";");
        TEST_CHECK(helper != std::string::npos,
                   "WS1 pin: smart_bypass.cpp carries the English word-shape helper");
        TEST_CHECK(fallback != std::string::npos,
                   "WS1 pin: the fallback logs its decision (shape-only DIAG_F line)");
        TEST_CHECK(helper < fallback && fallback < auto_return,
                   "WS1 pin: the fallback sits after the helper and BEFORE the AUTO return");
        // The pre-existing decision order is intact: diacritic, then English
        // core/stopword, then the non-English stopword vote, then the fallback.
        const size_t diacritic = src.find("distinctive diacritic)");
        const size_t english = src.find("1-2 token path detected=English\\n");
        const size_t vote = src.find("1-2 token path detected=%s with %d hits");
        TEST_CHECK(diacritic != std::string::npos && english != std::string::npos &&
                       vote != std::string::npos && diacritic < english && english < vote &&
                       vote < fallback,
                   "WS1 pin: detection order is diacritic -> English core -> stopword vote -> shape fallback");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] WS1 source pins passed." << std::endl;
    } else {
        std::cout << "[FAIL] WS1 source pins: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}
