// ---------------------------------------------------------------------------
// v0.10.7 (session 261111_0001 U2) - LANGUAGE-FAIRNESS fix in source-language
// detection. CEO standing requirement: the app serves speakers of 30+
// languages equally, so detection must not favour Korean or English.
//
// Two defects are pinned here, both in src/smart_bypass.cpp:
//
//  (1) ANY-CONTAINS script priority. DetectLanguage ran
//      `if (ContainsKorean(trimmed)) return "Korean";` first, so ONE Hangul
//      syllable anywhere in an otherwise English sentence ("The configuration
//      file is corrupted. 한") labeled the WHOLE selection Korean. With a
//      Korean target that became a Korean->Korean identity request (typing
//      prompt: "Translate the following Korean text into Korean"); on the drag
//      path it became a spurious ResolveEffectiveTarget pivot (engine got
//      Korean->English for English text). The same bias existed one branch
//      down for Thai/Arabic/Cyrillic/... text.
//      Fix: a DOMINANT-SCRIPT model - letters are counted per script family
//      (spaces, punctuation, digits, symbols are skipped) and a family owns
//      the label only with an absolute majority AND a clear lead over the
//      runner-up. Near-ties (runner-up within 10% of the leader) and
//      pluralities (leader <= 50%) fall back to the historical priority
//      chain, so pure-script input keeps its exact pre-v0.10.7 answer.
//
//  (2) ASCII SHAPE == ENGLISH. DetectLatinLanguage resolved any single
//      pure-ASCII word of >= 3 letters carrying a vowel to "English"
//      (IsAsciiEnglishShapedToken). A French/Spanish/Vietnamese/Indonesian
//      user dragging a short Latin-script word toward an English target got
//      "already English" -> ShouldTranslate false -> SILENT skip (no
//      translation, no error). Word identity cannot be decided from letter
//      shape, so English now requires positive lexical evidence
//      (kEnglishSingleWordEvidence); every other single word keeps the AUTO
//      marker and the engine's built-in language ID decides.
//
//  (3) R1-C (fix-list round 1) - sub-language corroboration: a SINGLE
//      Vietnamese-exclusive / Ukrainian / Urdu / Persian marker letter no
//      longer relabels foreign text (section 7 below). Wiring unchanged.
//
//  (4) R2-G-04 / R2-G-05 (fix-list round 2, section 8): the short-Latin
//      counter-evidence scan now includes the English core vocabulary
//      ("hello senor" is no longer asserted Spanish), and the corroboration
//      stopword lists are restricted to language-EXCLUSIVE words (Ukrainian
//      та/не/для were Russian; Persian هم was Arabic; Vietnamese em is
//      Portuguese).
//
// Included by tests/run_tests.cpp (with the other .inc files) and dispatched
// as RunV107LangDetectTests(g_failed_count) from the runner's suite table.
//
// Self-contained: uses only the public smart_bypass.hpp API, the TEST_CHECK
// harness (g_test_count/g_failed_count) and ResolveRepoFile for the one
// structural pin, exactly like the ws1/ws2 suites.
// ---------------------------------------------------------------------------

namespace {

// Builds a string of `count` copies of one code point, so the ratio-model
// checks state their letter counts instead of relying on hand-counted prose.
std::wstring V107Repeat(const wchar_t* unit, int count) {
    std::wstring out;
    for (int i = 0; i < count; ++i) {
        out += unit;
    }
    return out;
}

} // namespace

int RunV107LangDetectTests(int& failures) {
    std::cout << "[RUN] Testing v0.10.7 dominant-script detection (261111_0001 U2)..."
              << std::endl;
    const int failures_before = g_failed_count;

    // =====================================================================
    // 1) The reported defect: one Korean character at the end of English text
    //    must not turn the whole selection Korean.
    // =====================================================================
    {
        // 31 Latin letters, 1 Hangul syllable -> Latin dominates.
        const wchar_t* mixed = L"The configuration file is corrupted. \ud55c";
        const std::string detected = DetectLanguage(mixed);
        TEST_CHECK(detected != "Korean",
                   (std::string("V107: one trailing Hangul syllable no longer labels English text "
                                "Korean [got ") + detected + "]").c_str());
        // Latin-dominant with 2 English stop-word hits (N4 threshold is 3) =>
        // the documented Latin-uncertain label, i.e. the model's own language
        // ID decides downstream. The point is that it is NOT script-Korean.
        TEST_CHECK(detected == "Auto Detect",
                   (std::string("V107: 'The configuration file is corrupted. 한' reads Latin-dominant "
                                "(Auto Detect), not Korean [got ") + detected + "]").c_str());
        TEST_CHECK(NormalizeLanguageCode(detected) == "AUTO",
                   "V107: the mixed sentence sends NO Korean source token to the engine (AUTO)");
        // The same shape WITH real English evidence resolves a concrete
        // English label (3 English stop words: this, is, for).
        TEST_CHECK(DetectLanguage(L"This is for testing purposes \ud55c") == "English",
                   "V107: 3 English stop words + 1 Hangul syllable reads English");
        // The smart-bypass contract: mixed English+Korean text under a Korean
        // target is TRANSLATED (before the fix the label said Korean; the
        // 70%-target-script gate refused the bypass, but the prompt then asked
        // for Korean->Korean). Here the label itself is already correct.
        TEST_CHECK(ShouldTranslate(mixed, "Korean"),
                   "V107: mixed EN+1 Hangul -> Korean still translates (no KO->KO request)");
        TEST_CHECK(ShouldTranslate(mixed, "Korean", "Auto Detect"),
                   "V107: same under the default AUTO source pin (typing path)");
    }

    // =====================================================================
    // 2) Pure-script samples keep their exact labels (regression matrix).
    //    Every one of these is also pinned in tests/run_tests.cpp; the local
    //    copies keep the fairness fix honest across the whole script surface.
    // =====================================================================
    {
        struct V107Sample { const wchar_t* text; const char* want; const char* tag; };
        const V107Sample samples[] = {
            { L"\uc548\ub155\ud558\uc138\uc694", "Korean", "Korean" },
            { L"\u3053\u3093\u306b\u3061\u306f", "Japanese", "Japanese kana" },
            { L"\u4f60\u597d\u4e16\u754c", "Chinese Simplified", "Chinese hanzi" },
            { L"\u3053\u3093\u306b\u3061\u306f\u4e16\u754c", "Japanese",
              "Japanese kana+hanzi (kana wins over hanzi)" },
            { L"\u0e2a\u0e27\u0e31\u0e2a\u0e14\u0e35\u0e0a\u0e32\u0e27\u0e42\u0e25\u0e01",
              "Thai", "Thai" },
            { L"\u0645\u0631\u062d\u0628\u0627 \u0628\u0627\u0644\u0639\u0627\u0644\u0645",
              "Arabic", "Arabic" },
            { L"\u0686\u0637\u0648\u0631 \u0647\u0633\u062a\u06cc", "Persian", "Persian" },
            { L"\u0622\u067e \u06a9\u06cc\u0633\u06d2 \u06c1\u06cc\u06ba", "Urdu", "Urdu" },
            { L"\u041f\u0440\u0438\u0432\u0435\u0442 \u043c\u0438\u0440", "Russian", "Cyrillic (Russian)" },
            { L"\u041f\u0440\u0438\u0432\u0456\u0442 \u0441\u0432\u0456\u0442", "Ukrainian",
              "Cyrillic (Ukrainian variant letters)" },
            { L"\u0906\u091c \u092e\u094c\u0938\u092e \u0905\u091a\u094d\u091b\u093e \u0939\u0948",
              "Hindi", "Devanagari" },
            { L"\u0986\u099c \u0986\u09ac\u09b9\u09be\u0993\u09af\u09bc\u09be \u09ad\u09be\u09b2\u09cb",
              "Bengali", "Bengali" },
            { L"\u0393\u03b5\u03b9\u03b1 \u03c3\u03b1\u03c2 \u03c0\u03ce\u03c2 \u03b5\u03af\u03c3\u03c4\u03b5",
              "Greek", "Greek" },
            { L"\u05e9\u05dc\u05d5\u05dd \u05e2\u05d5\u05dc\u05dd", "Hebrew", "Hebrew" },
            { L"Xin ch\u00e0o th\u1ebf gi\u1edbi", "Vietnamese", "Vietnamese (VI tone marks)" },
            { L"Vi\u1ec7t Nam th\u1eadt tuy\u1ec7t v\u1eddi", "Vietnamese",
              "Vietnamese (second sample)" },
            { L"Liebe Gr\u00fc\u00dfe", "German", "Latin with distinctive diacritic (German)" },
            { L"se\u00f1or", "Spanish", "Latin with distinctive diacritic (Spanish)" },
            { L"hello", "English", "English core vocabulary" },
            { L"This is for testing purposes", "English", "English 3-stop-word sentence" },
            // Shared-accent Latin stays the script-certain/language-ambiguous
            // AUTO marker (F1/F5 contract, unchanged by the ratio model).
            { L"r\u00e1pido coraz\u00f3n", "Auto Detect", "shared-accent Latin stays AUTO" },
            { L"\u00e9trange m\u00e9diterran\u00e9en", "Auto Detect",
              "shared-accent French stays AUTO" },
        };
        for (const auto& s : samples) {
            const std::string got = DetectLanguage(s.text);
            TEST_CHECK(got == s.want,
                       (std::string("V107 script matrix: ") + s.tag + " -> " + s.want +
                        " [got " + got + "]").c_str());
        }
        // Non-Latin identity contract survives: script-certain labels still
        // drive the already-target bypass, language-neutrally.
        TEST_CHECK(!ShouldTranslate(L"\ud5e4\ube0c\ub9ac\uc5b4 \uc548\ub155", "Korean"),
                   "V107: pure Korean under a Korean target still bypasses");
        TEST_CHECK(!ShouldTranslate(L"\u05e9\u05dc\u05d5\u05dd \u05e2\u05d5\u05dc\u05dd", "Hebrew"),
                   "V107: Hebrew under a Hebrew target still bypasses");
        TEST_CHECK(!ShouldTranslate(L"\u0906\u091c \u092e\u094c\u0938\u092e \u0905\u091a\u094d\u091b\u093e \u0939\u0948",
                                    "Hindi"),
                   "V107: Hindi under a Hindi target still bypasses");
    }

    // =====================================================================
    // 3) The ratio model: dominance, near-tie fallback, plurality fallback.
    //    Counts are exact, so each case states why it lands where it does.
    // =====================================================================
    {
        // 30 Latin + 1 Hangul: absolute majority, runner-up far below the
        // leader -> the Latin path decides (AUTO, no English evidence).
        std::wstring latin_dominant = V107Repeat(L"a", 30);
        latin_dominant += L" ";
        latin_dominant += L"\ud55c";
        TEST_CHECK(DetectLanguage(latin_dominant) == "Auto Detect",
                   "V107 ratio: 30 Latin + 1 Hangul is Latin-dominant (not Korean)");

        // 25 Hangul + 2 Latin: the mirror case keeps Korean (dominance, not a
        // Korean preference - the majority really is Hangul).
        std::wstring korean_dominant = V107Repeat(L"\uac00", 25);
        korean_dominant += L" ab";
        TEST_CHECK(DetectLanguage(korean_dominant) == "Korean",
                   "V107 ratio: 25 Hangul + 2 Latin stays Korean (true majority)");

        // NEAR-TIE (runner-up within 10% of the leader) keeps the historical
        // priority order BY DESIGN: 20 vs 19 is not a majority decision, so
        // the legacy chain answers - and it answers "Korean" in both
        // directions because ContainsKorean runs first.
        std::wstring near_tie_ko;
        near_tie_ko += V107Repeat(L"\uac00", 20);
        near_tie_ko += V107Repeat(L"a", 19);
        TEST_CHECK(DetectLanguage(near_tie_ko) == "Korean",
                   "V107 ratio: 20 Hangul + 19 Latin is a near-tie -> legacy priority order (Korean)");

        std::wstring near_tie_latin;
        near_tie_latin += V107Repeat(L"a", 20);
        near_tie_latin += V107Repeat(L"\uac00", 19);
        TEST_CHECK(DetectLanguage(near_tie_latin) == "Korean",
                   "V107 ratio: 20 Latin + 19 Hangul is a near-tie -> legacy priority order (Korean), "
                   "documented preservation of pre-v0.10.7 behavior");

        // NO MAJORITY (three scripts, 10/10/8): falls back to the priority
        // order as well - the model never guesses between balanced scripts.
        std::wstring plurality;
        plurality += V107Repeat(L"a", 10);
        plurality += V107Repeat(L"\uac00", 10);
        plurality += V107Repeat(L"\u4f60", 8);
        TEST_CHECK(DetectLanguage(plurality) == "Korean",
                   "V107 ratio: 10 Latin / 10 Hangul / 8 Hanzi has no majority -> legacy priority order");

        // Punctuation, digits and spaces are NOT letters: a Korean sentence
        // padded with them keeps its Hangul majority.
        std::wstring padded = V107Repeat(L"\uac00", 6);
        padded += L" 12345 !!! --- ??? ";
        padded += V107Repeat(L"a", 2);
        TEST_CHECK(DetectLanguage(padded) == "Korean",
                   "V107 ratio: digits/punctuation/spaces carry no language signal (6 Hangul vs 2 Latin)");
    }

    // =====================================================================
    // 4) English positive-evidence rule (the shape rule is RETIRED).
    // =====================================================================
    {
        // No English evidence -> AUTO marker -> the text is TRANSLATED toward
        // an English target instead of being silently skipped as "already
        // English". These are the exact fairness cases from the requirement.
        const wchar_t* no_evidence[] = {
            L"bonjour",       // French
            L"informazione",  // Italian
            L"merci",         // French
            L"weather",       // English-looking, but not in the evidence list
        };
        for (const wchar_t* word : no_evidence) {
            const std::string got = DetectLanguage(word);
            TEST_CHECK(got != "English",
                       (std::string("V107 English rule: a bare ASCII word with no English evidence "
                                    "is not asserted English [got ") + got + "]").c_str());
            TEST_CHECK(got == "Auto Detect",
                       (std::string("V107 English rule: it keeps the AUTO marker [got ") + got +
                        "]").c_str());
        }
        TEST_CHECK(ShouldTranslate(L"bonjour", "English"),
                   "V107 English rule: 'bonjour' -> English TRANSLATES (was a silent skip)");
        TEST_CHECK(ShouldTranslate(L"informazione", "English"),
                   "V107 English rule: 'informazione' -> English TRANSLATES (was a silent skip)");

        // Real English evidence still resolves English, so the identity bypass
        // keeps working for genuine English input.
        TEST_CHECK(DetectLanguage(L"hello") == "English",
                   "V107 English rule: 'hello' (core vocabulary) still resolves English");
        TEST_CHECK(DetectLanguage(L"thanks") == "English",
                   "V107 English rule: 'thanks' (core vocabulary) still resolves English");
        TEST_CHECK(DetectLanguage(L"the") == "English",
                   "V107 English rule: 'the' (English function word / stop word) still resolves English");
        TEST_CHECK(DetectLanguage(L"cyber") == "English",
                   "V107 English rule: 'cyber' (lexical evidence list) still resolves English");
        TEST_CHECK(DetectLanguage(L"Hotel") == "English",
                   "V107 English rule: 'Hotel' (lexical evidence list) still resolves English");
        TEST_CHECK(!ShouldTranslate(L"hello", "English"),
                   "V107 English rule: real English under an English target still bypasses");
        TEST_CHECK(ShouldTranslate(L"hello", "Korean"),
                   "V107 English rule: real English under a Korean target still translates");

        // The shape rule itself is GONE: a 2-token ASCII phrase was already
        // excluded before, and the single-token fallback is now lexical.
        TEST_CHECK(DetectLanguage(L"cyber security") == "Auto Detect",
                   "V107 English rule: a 2-token ASCII phrase keeps the AUTO marker (F5 invariant)");
        TEST_CHECK(ShouldTranslate(L"Sisitiki sana", "English"),
                   "V107 English rule: Swahili -> English still translates (F5 invariant)");
    }

    // =====================================================================
    // 5) Smart-bypass contract around the change (what counts as
    //    translatable must not move beyond the intended fairness fix).
    // =====================================================================
    {
        // Mostly-English text with Korean words under a Korean target: the
        // label is Latin-side, so the request reaches the engine (no KO->KO).
        TEST_CHECK(ShouldTranslate(L"Please review the \uae30\ud68d document", "Korean"),
                   "V107 contract: mixed EN+KO sentence translates to Korean");
        TEST_CHECK(ShouldTranslate(L"Please \uae30\ud68d review", "Korean"),
                   "V107 contract: two Hangul words in English text still translate");
        // Mostly-Korean text with an English tail keeps the Korean label, so
        // the target-script dominance gate (BUG-7) still owns that decision.
        TEST_CHECK(DetectLanguage(L"\uc624\ub298 \ud68c\uc758 \uc790\ub8cc\ub97c \uac80\ud1a0\ud574 \uc8fc\uc138\uc694 Hi.")
                       == "Korean",
                   "V107 contract: Korean-dominant text with an English tail stays Korean");
        TEST_CHECK(!ShouldTranslate(L"\uc624\ub298 \ud68c\uc758 \uc790\ub8cc\ub97c \uac80\ud1a0\ud574 \uc8fc\uc138\uc694 Hi.",
                                    "Korean"),
                   "V107 contract: that Korean-dominant line is still whole-line bypassed (BUG-7)");
        // Unchanged cross-language direction.
        TEST_CHECK(ShouldTranslate(L"\u4f60\u597d\u4e16\u754c", "Korean"),
                   "V107 contract: Chinese -> Korean still translates");
        TEST_CHECK(ShouldTranslate(L"This is for testing purposes", "Korean"),
                   "V107 contract: English -> Korean still translates");
        TEST_CHECK(!ShouldTranslate(L"This is for testing purposes", "English"),
                   "V107 contract: 3-stop-word English -> English still bypasses");
    }

    // =====================================================================
    // 6) Structural pin: the retired shape rule cannot come back silently.
    // =====================================================================
    {
        const std::filesystem::path sb_cpp = ResolveRepoFile(L"src\\smart_bypass.cpp");
        if (sb_cpp.empty()) {
            std::cout << "[SKIP] src/smart_bypass.cpp not resolvable; V107 source pins skipped."
                      << std::endl;
        } else {
            std::ifstream in(sb_cpp, std::ios::binary);
            const std::string src((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
            // The helper DEFINITION must be gone (prose comments may still
            // name it when explaining the retraction).
            TEST_CHECK(src.find("bool IsAsciiEnglishShapedToken(") == std::string::npos,
                       "V107 pin: the ASCII-shape English helper is removed (no shape-only English read)");
            TEST_CHECK(src.find("kEnglishSingleWordEvidence") != std::string::npos,
                       "V107 pin: the positive-evidence English lexicon exists");
            TEST_CHECK(src.find("single-token English evidence") != std::string::npos,
                       "V107 pin: the single-token fallback logs its lexical evidence");
            // The ratio model and its fallback are present and ordered: the
            // dominant-script decision precedes the priority chain call.
            const size_t tally = src.find("TallyScriptFamilies");
            const size_t dominant = src.find("TryDominantScriptFamily");
            const size_t fallback_call = src.find("return DetectLanguageByScriptPriority(trimmed);");
            TEST_CHECK(tally != std::string::npos && dominant != std::string::npos &&
                           fallback_call != std::string::npos && tally < dominant &&
                           dominant < fallback_call,
                       "V107 pin: dominant-script tally -> decision -> priority fallback order");
        }
    }

    // =====================================================================
    // 7) R1-C (261111_0001 fix-list round 1): sub-language corroboration.
    //    A SINGLE exclusive/variant marker must not relabel foreign text;
    //    the claim needs a second signal (>= 2 shared markers, >= 2 variant
    //    occurrences, or a matching stopword). The run_tests.cpp:981 pin
    //    ("Đừng ăn đó": 1 exclusive + 3 shared) must stay true.
    // =====================================================================
    {
        // (a) English sentence + one Vietnamese-exclusive o-horn: NOT
        //     Vietnamese. Latin-dominant, no English stopword evidence here
        //     (30 plain letters + the marker), so the expected label is the
        //     AUTO marker - the point is that it is not "Vietnamese".
        std::wstring english_plus_one =
            L"This is a clearly English sentence with one mark: \u01a1";
        {
            const std::string got = DetectLanguage(english_plus_one);
            TEST_CHECK(got != "Vietnamese",
                       (std::string("V107 R1-C: one o-horn in an English sentence is not "
                                    "Vietnamese [got ") + got + "]").c_str());
            // The sentence carries 3 English stop words (this, is, with), so
            // after the corroboration refusal the Latin path resolves its
            // real label: English. (The defect was the "Vietnamese" label,
            // which let the already-target gate silently skip a VI target.)
            TEST_CHECK(got == "English",
                       (std::string("V107 R1-C: the uncorroborated marker falls through to the "
                                    "Latin decision [got ") + got + "]").c_str());
        }
        // The underlying predicate, directly: 1 exclusive, no shared, no
        // stopword -> false.
        TEST_CHECK(!ContainsVietnamese(L"clearly english text \u01a1"),
                   "V107 R1-C: ContainsVietnamese refuses a lone exclusive marker");
        // (b) THE TRAP (tests/run_tests.cpp:981): "Đừng ăn đó" = 1 exclusive
        //     (ừ) + 3 shared (Đ, ă, đ) -> Vietnamese. Exclusive-only counting
        //     would break this pin; corroboration keeps it.
        TEST_CHECK(ContainsVietnamese(L"\u0110\u1eebng \u0103n \u0111\u00f3"),
                   "V107 R1-C: 'Đừng ăn đó' stays Vietnamese (1 exclusive + 3 shared corroborate)");
        // (c) Pure Vietnamese -> Vietnamese (both the 2-exclusive shape and
        //     the exclusive+stopword shape).
        TEST_CHECK(DetectLanguage(L"Xin ch\u00e0o th\u1ebf gi\u1edbi") == "Vietnamese",
                   "V107 R1-C: pure Vietnamese (2 exclusive markers) still Vietnamese");
        TEST_CHECK(DetectLanguage(L"Vi\u1ec7t Nam th\u1eadt tuy\u1ec7t v\u1eddi") == "Vietnamese",
                   "V107 R1-C: pure Vietnamese (many markers) still Vietnamese");
        TEST_CHECK(ContainsVietnamese(L"kh\u00f4ng \u0111\u01b0\u1ee3c"),
                   "V107 R1-C: exclusive + stopword corroborates Vietnamese");
        // (d) Russian text + one Ukrainian-exclusive і -> Russian (not
        //     Ukrainian). 2+ occurrences still resolve Ukrainian.
        {
            const wchar_t* ru_plus_i = L"\u041f\u0440\u0438\u0432\u0435\u0442 \u043c\u0438\u0440 \u0456";
            const std::string got = DetectLanguage(ru_plus_i);
            TEST_CHECK(got == "Russian",
                       (std::string("V107 R1-C: Russian + one Ukrainian і stays Russian [got ") +
                        got + "]").c_str());
            const wchar_t* ru_plus_ii = L"\u041f\u0440\u0438\u0432\u0456\u0442 \u043c\u0456\u0440";
            TEST_CHECK(DetectLanguage(ru_plus_ii) == "Ukrainian",
                       "V107 R1-C: two Ukrainian markers still resolve Ukrainian");
            TEST_CHECK(DetectLanguage(L"\u041f\u0440\u0438\u0432\u0456\u0442 \u0441\u0432\u0456\u0442") == "Ukrainian",
                       "V107 R1-C: pure Ukrainian (2+ markers) still Ukrainian");
        }
        // (e) Arabic text + one Urdu-exclusive letter -> Arabic (not Urdu);
        //     two occurrences resolve Urdu; pure Persian/Urdu keep labels.
        {
            // Pure Arabic sentence + one Urdu ے (0x06D2).
            const wchar_t* ar_plus_urdu =
                L"\u0645\u0631\u062d\u0628\u0627 \u0628\u0627\u0644\u0639\u0627\u0644\u0645 \u06d2";
            const std::string got = DetectLanguage(ar_plus_urdu);
            TEST_CHECK(got == "Arabic",
                       (std::string("V107 R1-C: Arabic + one Urdu-exclusive letter stays Arabic [got ") +
                        got + "]").c_str());
            // Two Urdu markers corroborate (with a Persian marker absent).
            const wchar_t* urdu_two =
                L"\u0622\u067e \u06a9\u06cc\u0633\u06d2 \u06c1\u06cc\u06ba \u06d2";
            TEST_CHECK(DetectLanguage(urdu_two) == "Urdu",
                       "V107 R1-C: two Urdu markers resolve Urdu");
            // Pure Persian / pure Urdu keep their labels (regression matrix
            // copies; also pinned in section 2 above).
            TEST_CHECK(DetectLanguage(L"\u0686\u0637\u0648\u0631 \u0647\u0633\u062a\u06cc") == "Persian",
                       "V107 R1-C: pure Persian stays Persian");
            TEST_CHECK(DetectLanguage(L"\u0622\u067e \u06a9\u06cc\u0633\u06d2 \u06c1\u06cc\u06ba") == "Urdu",
                       "V107 R1-C: pure Urdu stays Urdu");
            TEST_CHECK(DetectLanguage(L"\u0645\u0631\u062d\u0628\u0627 \u0628\u0627\u0644\u0639\u0627\u0644\u0645") == "Arabic",
                       "V107 R1-C: pure Arabic stays Arabic");
        }
        // (f) The smart-bypass contract: the English sentence with one stray
        //     marker must TRANSLATE under a Vietnamese target (before the
        //     fix the "Vietnamese" label + TargetScriptDominant silently
        //     skipped it).
        TEST_CHECK(ShouldTranslate(english_plus_one, "VI", "Auto Detect"),
                   "V107 R1-C: English + one o-horn under a Vietnamese target still translates");
        TEST_CHECK(!ShouldTranslate(L"Xin ch\u00e0o th\u1ebf gi\u1edbi", "VI", "Auto Detect"),
                   "V107 R1-C: pure Vietnamese under a Vietnamese target still bypasses");
    }

    // =====================================================================
    // 8) R2-G-04 / R2-G-05 (261111_0001 fix-list round 2):
    //    (a) short-Latin counter-evidence must include the English CORE
    //        VOCABULARY, not only the kLatinLangs stopwords;
    //    (b) corroboration stopword lists must hold language-EXCLUSIVE words
    //        only (Ukrainian та/не/для are Russian; Persian هم is Arabic;
    //        Vietnamese em is Portuguese).
    // =====================================================================
    {
        // ---- (a) G-04: "hello senor" ------------------------------------
        // "hello" is in kEnglishCoreWords and in NO stopword list, so the
        // 1-2 token diacritic path used to label the pair "Spanish" from the
        // n-tilde alone. Under a Spanish target that label + TargetScriptDominant
        // made ShouldTranslate return false: the English speaker's request was
        // silently skipped (no translation, no message). The English core word
        // is now counter-evidence, so the sample keeps the AUTO marker and the
        // engine's built-in language ID decides.
        {
            const std::string got = DetectLanguage(L"hello se\u00f1or");
            TEST_CHECK(got != "Spanish",
                       (std::string("V107 G-04: 'hello senor' is NOT asserted Spanish [got ") +
                        got + "]").c_str());
            TEST_CHECK(got == "Auto Detect",
                       (std::string("V107 G-04: mixed English core word + diacritic token keeps "
                                    "the AUTO marker [got ") + got + "]").c_str());
            // The user-visible defect, direction by direction.
            TEST_CHECK(ShouldTranslate(L"hello se\u00f1or", "Spanish"),
                       "V107 G-04: 'hello senor' -> Spanish TRANSLATES (was a silent already-target skip)");
            TEST_CHECK(ShouldTranslate(L"hello se\u00f1or", "English"),
                       "V107 G-04: 'hello senor' -> English still translates (mixed evidence)");
            // Symmetry: the English word can be in either position - the scan
            // is over the whole 1-2 token sample.
            const std::string flipped = DetectLanguage(L"se\u00f1or hello");
            TEST_CHECK(flipped != "Spanish",
                       (std::string("V107 G-04: 'senor hello' is not asserted Spanish either [got ") +
                        flipped + "]").c_str());
        }
        // The RC-1 / WS1 pins this fix must NOT move: a lone distinctive
        // diacritic is still sufficient, with no counter-evidence present.
        TEST_CHECK(DetectLanguage(L"se\u00f1or") == "Spanish",
                   "V107 G-04: 'señor' alone still resolves Spanish (run_tests.cpp:941 pin)");
        TEST_CHECK(DetectLanguage(L"Liebe Gr\u00fc\u00dfe") == "German",
                   "V107 G-04: 'Liebe Grüße' still resolves German (run_tests.cpp:1002 pin)");
        TEST_CHECK(DetectLanguage(L"hello") == "English",
                   "V107 G-04: 'hello' alone still resolves English (no diacritic claim to refuse)");

        // ---- (b) G-05: Ukrainian corroboration --------------------------
        // та/не/для are ordinary Russian words. With them in the list, Russian
        // prose carrying ONE stray і (a Latin i typed on the wrong layout)
        // corroborated the Ukrainian claim and the whole selection relabeled
        // Ukrainian - a Ukrainian->X prompt for Russian text.
        {
            const wchar_t* ru_with_stray_i =
                L"\u042d\u0442\u043e \u043d\u0435 \u0434\u043b\u044f \u0442\u0435\u0431\u044f \u0456";
            const std::string got = DetectLanguage(ru_with_stray_i);
            TEST_CHECK(got == "Russian",
                       (std::string("V107 G-05: Russian + не/для + one stray і stays Russian "
                                    "[got ") + got + "]").c_str());
        }
        // Two genuine Ukrainian markers still resolve Ukrainian (the count
        // rule is untouched), and the Ukrainian-only function words still
        // corroborate a lone marker.
        TEST_CHECK(DetectLanguage(L"\u041f\u0440\u0438\u0432\u0456\u0442 \u0441\u0432\u0456\u0442") ==
                       "Ukrainian",
                   "V107 G-05: pure Ukrainian (2+ markers) still Ukrainian");
        TEST_CHECK(DetectLanguage(L"\u0446\u0435 \u0456 \u0434\u043e\u0431\u0440\u0435") == "Ukrainian",
                   "V107 G-05: one marker + the Ukrainian-only word 'це' still corroborates");

        // ---- (b) G-05: Persian corroboration ----------------------------
        // هم ("also") is also ordinary Arabic, so Arabic text with one stray
        // Persian-exclusive گ relabeled the whole selection Persian.
        {
            const wchar_t* arabic_with_stray_ge =
                L"\u0645\u0631\u062d\u0628\u0627 \u0628\u0627\u0644\u0639\u0627\u0644\u0645 \u06af \u0647\u0645";
            const std::string got = DetectLanguage(arabic_with_stray_ge);
            TEST_CHECK(got == "Arabic",
                       (std::string("V107 G-05: Arabic + one stray گ + هم stays Arabic [got ") +
                        got + "]").c_str());
        }
        // A Persian-only function word still corroborates a lone marker, and
        // pure Persian keeps its label.
        TEST_CHECK(DetectLanguage(L"\u0686\u0637\u0648\u0631 \u0647\u0633\u062a\u06cc") == "Persian",
                   "V107 G-05: pure Persian still Persian ('chotor' corroborates the lone گ)");

        // ---- (b) G-05: Vietnamese corroboration -------------------------
        // "em" is the most frequent Portuguese word; keeping it in the VI
        // stopword list let a Portuguese sentence carrying ONE stray o-horn
        // corroborate itself into "Vietnamese", and under a Vietnamese target
        // the already-target gate then silently skipped the translation.
        {
            const wchar_t* pt_with_o_horn =
                L"n\u00e3o presto muita aten\u00e7\u00e3o em \u01a1 futebol";
            const std::string got = DetectLanguage(pt_with_o_horn);
            TEST_CHECK(got != "Vietnamese",
                       (std::string("V107 G-05: Portuguese + one o-horn + em is NOT Vietnamese "
                                    "[got ") + got + "]").c_str());
            TEST_CHECK(got == "Portuguese",
                       (std::string("V107 G-05: it keeps its real Portuguese label via "
                                    "stopwords [got ") + got + "]").c_str());
            TEST_CHECK(ShouldTranslate(pt_with_o_horn, "Vietnamese", "Auto Detect"),
                       "V107 G-05: Portuguese + one o-horn under a Vietnamese target TRANSLATES");
            // The trap stays: 1 exclusive + 3 shared letters is still VI.
            TEST_CHECK(ContainsVietnamese(L"\u0110\u1eebng \u0103n \u0111\u00f3"),
                       "V107 G-05: 'Đừng ăn đó' stays Vietnamese (run_tests.cpp:981 trap)");
            // And a VI-only stopword still rescues the single-marker case.
            TEST_CHECK(ContainsVietnamese(L"kh\u00f4ng \u0111\u01b0\u1ee3c"),
                       "V107 G-05: 'không được' still corroborates (exclusive + VI-only stopword)");
        }
    }

    const int failed = g_failed_count - failures_before;
    // Report the suite delta to the caller WITHOUT double counting: when the
    // caller passes the harness global itself, it is already authoritative.
    if (&failures != &g_failed_count) {
        failures += failed;
    }
    if (failed == 0) {
        std::cout << "[PASS] v0.10.7 dominant-script detection passed." << std::endl;
    } else {
        std::cout << "[FAIL] v0.10.7 dominant-script detection: " << failed
                  << " check(s) failed." << std::endl;
    }
    return failed;
}
