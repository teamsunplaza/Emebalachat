// ===========================================================================
// 261116_0003 P9a (language-menu redesign, plan §3 D5/D6 + §5 P9):
// user-defined languages and the capability probe, CORE LAYER ONLY.
//
// Covered, per the P9a unit contract:
//   (1) script inference + the script-token round trip (D5, fail-open)
//   (2) code resolution: alias exact -> locale fallback -> no code (D5)
//   (3) probe evaluation for EVERY verdict branch (D6, pure)
//   (4) probe-cache transitions: result / override / re-test / cap
//   (5) config round-trip: add -> save -> reload -> merge
//   (6) missing-block backward compatibility + malformed-block tolerance
//   (7) newer-schema forward compat (verbatim preservation)
//   (8) PlanLanguageSync raw-token allowance (registered yes / unknown no)
//   (9) the free-form prompt pins that must NOT regress
//  (10) the D5/D7 menu-view merge (user group in cloud views, not local)
//
// Zero network, zero UI: the D6 transport is the caller's seam (P9b), and the
// only live Win32 calls are the read-only locale queries D5's ladder needs.
// ===========================================================================
namespace p9a_user_language_tests {

// The injectable D5 stage-2 source (a LocaleLanguageSource function pointer):
// a deterministic fake install, so the name -> code rule is tested without
// depending on which system locales this machine happens to have. The rows
// mirror real Windows traps: an alias-only-unresolvable name (Samplelandic),
// a name the alias stage already owns (Korean), the recorded yue placeholder
// trap, an unnamed locale entry, and a locale whose primary subtag has no
// registry row (zh).
std::vector<LocaleLanguageName> FakeLocaleSource() {
    return {
        {"fi-FI", "Samplelandic", "samplelandic"},
        {"ko-KR", "Korean", "한국어"},
        {"yue", "unknown locale (yue)", "알 수 없는 로캘 (yue)"},
        {"zz-ZZ", "", ""},
        {"zh-Hans-CN", "Chinese", "中文"},
    };
}

bool RegistryEquals(const std::vector<const LanguageInfo*>& a,
                    const std::vector<const LanguageInfo*>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i] != b[i]) {
            return false;
        }
    }
    return true;
}

// How many rows in a merged view carry `code` (registry section + user group),
// which is the "a language appears once per menu" count.
int CountRowsWithCode(const LanguageMenuView& view, std::string_view code) {
    int count = 0;
    for (const LanguageInfo* row : view.registry) {
        if (row != nullptr && row->code == code) {
            ++count;
        }
    }
    for (const LanguageMenuRow& row : view.user_group) {
        if (row.registry != nullptr && row.registry->code == code) {
            ++count;
        }
    }
    return count;
}

bool HasRegistryRow(const std::vector<const LanguageInfo*>& rows, std::string_view code) {
    for (const LanguageInfo* row : rows) {
        if (row != nullptr && row->code == code) {
            return true;
        }
    }
    return false;
}

std::string ManyEntriesJson(int count) {
    std::string json = "{\"user_languages_version\": 1, \"user_languages\": [";
    for (int i = 0; i < count; ++i) {
        if (i != 0) {
            json += ",";
        }
        json += "{\"native_name\": \"Samplelang" + std::to_string(i) + "\"}";
    }
    json += "]}";
    return json;
}

} // namespace p9a_user_language_tests

void TestP9aUserLanguages() {
    std::cout << "[RUN] Testing 261116_0003 P9a user languages + capability probe..." << std::endl;
    const int failures_before = g_failed_count;

    using p9a_user_language_tests::CountRowsWithCode;
    using p9a_user_language_tests::FakeLocaleSource;
    using p9a_user_language_tests::HasRegistryRow;
    using p9a_user_language_tests::ManyEntriesJson;
    using p9a_user_language_tests::RegistryEquals;

    // ---- (1) D5 script inference + the config script-token round trip ------
    TEST_CHECK(InferUserLanguageScript("한국어") == kScriptMaskHangul,
               "P9a (1): a Hangul native name infers the hangul script");
    TEST_CHECK(InferUserLanguageScript("Twi") == kScriptMaskLatin,
               "P9a (1): a Latin native name infers the latin script");
    TEST_CHECK(InferUserLanguageScript("日本語") == kScriptMaskHan,
               "P9a (1): '日本語' infers han (ideographs only)");
    TEST_CHECK(InferUserLanguageScript("ひらがな") == kScriptMaskKana,
               "P9a (1): kana-only text infers kana");
    TEST_CHECK(InferUserLanguageScript("Русский") == kScriptMaskCyrillic,
               "P9a (1): a Cyrillic native name infers cyrillic");
    TEST_CHECK(InferUserLanguageScript("العربية") == kScriptMaskArabic,
               "P9a (1): an Arabic native name infers arabic");
    TEST_CHECK(InferUserLanguageScript("हिन्दी") == kScriptMaskDevanagari,
               "P9a (1): a Devanagari native name infers devanagari");
    TEST_CHECK(InferUserLanguageScript("한국어 Korean") == (kScriptMaskHangul | kScriptMaskLatin),
               "P9a (1): a mixed name ORs its classes instead of guessing one");
    TEST_CHECK(InferUserLanguageScript("") == 0,
               "P9a (1): an empty name fails open (mask 0), no crash");
    TEST_CHECK(InferUserLanguageScript("   ") == 0,
               "P9a (1): a whitespace-only name fails open (mask 0)");
    TEST_CHECK(InferUserLanguageScript("1234 !? --") == 0,
               "P9a (1): a digit/punctuation-only name fails open (mask 0)");
    TEST_CHECK(InferUserLanguageScript(std::string_view()) == 0,
               "P9a (1): an empty string_view is safe");
    {
        // Every mask bit must survive the config token round trip. The list is
        // the win32_input.hpp vocabulary (pinned against engine_core by the
        // ws2 suite), so a missing row here is a schema gap.
        const std::uint32_t bits[] = {
            kScriptMaskLatin,      kScriptMaskHangul,    kScriptMaskHan,
            kScriptMaskKana,       kScriptMaskCyrillic,  kScriptMaskThai,
            kScriptMaskArabic,     kScriptMaskDevanagari, kScriptMaskBengali,
            kScriptMaskKhmer,      kScriptMaskLao,       kScriptMaskHebrew,
            kScriptMaskGreek,      kScriptMaskMyanmar,   kScriptMaskTamil,
            kScriptMaskTelugu,     kScriptMaskGujarati,  kScriptMaskTibetan,
            kScriptMaskArmenian,   kScriptMaskGeorgian,  kScriptMaskSinhala,
            kScriptMaskMalayalam,  kScriptMaskKannada,   kScriptMaskGurmukhi,
            kScriptMaskOdia,       kScriptMaskEthiopic,
        };
        bool all_round_trip = true;
        bool all_named = true;
        for (const std::uint32_t bit : bits) {
            const std::string token = UserLanguageScriptToken(bit);
            if (token.empty()) {
                all_named = false;
                continue;
            }
            if (UserLanguageScriptMaskFromToken(token) != bit) {
                all_round_trip = false;
            }
        }
        TEST_CHECK(all_named, "P9a (1): all 26 script bits have a config token");
        TEST_CHECK(all_round_trip, "P9a (1): every script token round-trips to its bit");
        TEST_CHECK(UserLanguageScriptToken(kScriptMaskHan | kScriptMaskKana) == "han+kana",
                   "P9a (1): a composite mask emits ascending-bit tokens joined by '+'");
        TEST_CHECK(UserLanguageScriptMaskFromToken("KANA + HAN") == (kScriptMaskHan | kScriptMaskKana),
                   "P9a (1): token parsing folds case and tolerates spaces");
        TEST_CHECK(UserLanguageScriptToken(0).empty(),
                   "P9a (1): mask 0 emits the empty token");
        TEST_CHECK(UserLanguageScriptMaskFromToken("") == 0,
                   "P9a (1): the empty token is mask 0");
        TEST_CHECK(UserLanguageScriptMaskFromToken("klingon") == 0,
                   "P9a (1): an unknown token fails open to mask 0");
        TEST_CHECK(UserLanguageScriptMaskFromToken("han+garbage") == kScriptMaskHan,
                   "P9a (1): a partially unknown composite keeps the known bits");
    }

    // ---- (2) D5 code resolution: alias -> locale -> no code ----------------
    {
        const UserLanguageResolution ko_en = ResolveUserLanguageCode("Korean", "");
        TEST_CHECK(ko_en.code == "KO" && ko_en.from_alias && !ko_en.from_locale,
                   "P9a (2): 'Korean' resolves to KO through the alias matrix");
        TEST_CHECK(ko_en.english_name == "Korean",
                   "P9a (2): the resolution carries the registry English name");
        const UserLanguageResolution ko_native = ResolveUserLanguageCode("한국어", "");
        TEST_CHECK(ko_native.code == "KO" && ko_native.from_alias,
                   "P9a (2): the native name '한국어' resolves to KO");
        const UserLanguageResolution via_english =
            ResolveUserLanguageCode("Korean", "Korean");
        TEST_CHECK(via_english.code == "KO" && via_english.from_alias,
                   "P9a (2): the optional English field is a resolution candidate");
        const UserLanguageResolution via_english_only =
            ResolveUserLanguageCode("Samplelandic", "Korean");
        TEST_CHECK(via_english_only.code == "KO" && via_english_only.from_alias,
                   "P9a (2): the English field rescues a native name the matrix does not hold");
        const UserLanguageResolution none = ResolveUserLanguageCode("Twi", "");
        TEST_CHECK(none.code.empty() && !none.from_alias && !none.from_locale,
                   "P9a (2): an unregistered name resolves to no code (free-form only)");
        const UserLanguageResolution no_source = ResolveUserLanguageCode("Twi", "", nullptr);
        TEST_CHECK(no_source.code.empty(),
                   "P9a (2): a null locale source is legal (ladder stops after the alias stage)");
        const UserLanguageResolution klingon = ResolveUserLanguageCode("Klingon", "");
        TEST_CHECK(klingon.code.empty(),
                   "P9a (2): 'Klingon' stays free-form (the prompt raw-token pin's language)");

        // Fuzzy search must NOT resolve: a prefix hit is a picker affordance,
        // not a recorded code decision.
        const std::vector<LanguageSearchHit> prefix_hits =
            SearchLanguages("Kore", GetLanguagesForEngine(LanguageEngine::Google, false));
        bool korean_prefix_hit = false;
        for (const LanguageSearchHit& hit : prefix_hits) {
            if (hit.language != nullptr && hit.language->code == "KO" &&
                hit.match != AliasMatchKind::Exact) {
                korean_prefix_hit = true;
                break;
            }
        }
        TEST_CHECK(korean_prefix_hit,
                   "P9a (2): 'Kore' is a prefix hit for KO (the search affordance exists)");
        TEST_CHECK(ResolveUserLanguageCode("Kore", "").code.empty(),
                   "P9a (2): ... but resolution requires an EXACT alias match");

        // Stage 2 (the locale seam): a name only the installed-locale table
        // knows, whose primary subtag IS a registry row, resolves there.
        const UserLanguageResolution locale_hit =
            ResolveUserLanguageCode("Samplelandic", "", &FakeLocaleSource);
        TEST_CHECK(locale_hit.code == "FI" && locale_hit.from_locale && !locale_hit.from_alias,
                   "P9a (2): the locale fallback resolves a name the matrix misses");
        TEST_CHECK(locale_hit.english_name == "Finnish",
                   "P9a (2): the locale resolution also carries the registry name");
        const UserLanguageResolution alias_wins =
            ResolveUserLanguageCode("Korean", "", &FakeLocaleSource);
        TEST_CHECK(alias_wins.code == "KO" && alias_wins.from_alias && !alias_wins.from_locale,
                   "P9a (2): the alias stage wins when both stages could answer");
        TEST_CHECK(ResolveCodeFromLocaleLanguages("samplelandic", "", FakeLocaleSource()) == "FI",
                   "P9a (2): locale name matching is ASCII-case-insensitive");
        TEST_CHECK(ResolveCodeFromLocaleLanguages("", "", FakeLocaleSource()).empty(),
                   "P9a (2): a blank query never matches an unnamed locale entry");
        TEST_CHECK(ResolveCodeFromLocaleLanguages("Chinese", "", FakeLocaleSource()).empty(),
                   "P9a (2): a locale whose primary subtag has no registry row yields no code");
        TEST_CHECK(ResolveCodeFromLocaleLanguages("unknown locale (yue)", "",
                                                  FakeLocaleSource()) == "YUE",
                   "P9a (2): the yue placeholder row can only match its own literal name");

        // Live wiring proof of the real enumeration (no machine-dependent
        // assertion about WHICH languages are installed beyond en-*).
        const std::vector<LocaleLanguageName> live = EnumerateSystemLocaleLanguages();
        TEST_CHECK(!live.empty(),
                   "P9a (2): EnumSystemLocalesEx returns the installed locales");
        bool live_has_en = false;
        bool live_has_resolvable = false;
        bool live_codes_valid = true;
        for (const LocaleLanguageName& entry : live) {
            if (entry.locale.rfind("en-", 0) == 0) {
                live_has_en = true;
            }
            const std::string code =
                ResolveCodeFromLocaleLanguages(entry.english, entry.native, {entry});
            if (!code.empty()) {
                live_has_resolvable = true;
                if (FindLanguageByCode(code) == nullptr) {
                    live_codes_valid = false;
                }
            }
        }
        TEST_CHECK(live_has_en,
                   "P9a (2): the live enumeration contains en-US (every Windows install)");
        TEST_CHECK(live_has_resolvable,
                   "P9a (2): at least one live locale name maps to a registry code");
        TEST_CHECK(live_codes_valid,
                   "P9a (2): every live locale name maps to a REAL registry code, never a guess");

        // MakeUserLanguage: the builder fills the derived fields.
        const UserLanguage twi = MakeUserLanguage("Twi", "");
        TEST_CHECK(twi.native_name == "Twi" && twi.english_name.empty(),
                   "P9a (2): a free-form entry keeps the user's English field empty");
        TEST_CHECK(twi.code.empty() && twi.script_mask == kScriptMaskLatin,
                   "P9a (2): ... with no code and the inferred script");
        const UserLanguage ko_user = MakeUserLanguage("한국어", "");
        TEST_CHECK(ko_user.code == "KO",
                   "P9a (2): MakeUserLanguage records the resolved code");
        TEST_CHECK(ko_user.english_name == "Korean",
                   "P9a (2): ... and fills the registry English name for the prompt");
        TEST_CHECK(UserLanguageStoredName(ko_user) == "Korean",
                   "P9a (2): the stored form is the English name when present");
        TEST_CHECK(UserLanguageStoredName(twi) == "Twi",
                   "P9a (2): the stored form falls back to the native name");
        TEST_CHECK(MakeUserLanguage("한국어", "Korean (KO)").english_name == "Korean (KO)",
                   "P9a (2): an explicit English name is preserved verbatim");
        const UserLanguage unnamed = MakeUserLanguage("   ", "X");
        TEST_CHECK(unnamed.native_name.empty(),
                   "P9a (2): an empty native name yields an entry callers must reject");
        TEST_CHECK(MakeUserLanguage("Tibetan", "").code == "BO",
                   "P9a (2): BO resolves (the row with no Google bit, used by the merge pin)");
        const UserLanguage trimmed = MakeUserLanguage("  Twi  ", " Twi ");
        TEST_CHECK(trimmed.native_name == "Twi" && trimmed.english_name == "Twi",
                   "P9a (2): the builder trims both fields");
    }

    // ---- (3) D6 probe evaluation: every verdict branch ---------------------
    {
        const std::string korean_output = "내일 아침까지 보고서를 보내 드리겠습니다.";
        const ProbeEvaluation ok = EvaluateProbeResult(
            kUserLanguageProbeSentence, korean_output, kScriptMaskHangul);
        TEST_CHECK(ok.status == UserLanguageProbeStatus::Supported,
                   "P9a (3): a translated Hangul output is Supported");
        TEST_CHECK(ok.non_empty && ok.differs && ok.script_ok,
                   "P9a (3): all three checks report their bits");
        TEST_CHECK(ok.detected_mask == kScriptMaskHangul,
                   "P9a (3): the detected mask is the output's script");
        TEST_CHECK(ok.evidence.find("nonempty=1 differs=1 script=1") != std::string::npos,
                   "P9a (3): the evidence records the three check bits");
        TEST_CHECK(ok.evidence.find("expected=hangul detected=hangul") != std::string::npos,
                   "P9a (3): the evidence names the scripts, not the text");
        TEST_CHECK(ok.evidence.find("보고서") == std::string::npos &&
                       ok.evidence.find("report") == std::string::npos,
                   "P9a (3): the evidence carries no probe/engine text (shape-only, no PII)");

        const ProbeEvaluation empty = EvaluateProbeResult(
            kUserLanguageProbeSentence, "", kScriptMaskHangul);
        TEST_CHECK(empty.status == UserLanguageProbeStatus::Unsupported && !empty.non_empty,
                   "P9a (3): an empty output is Unsupported (branch: empty)");
        TEST_CHECK(empty.evidence.find("reason=empty") != std::string::npos,
                   "P9a (3): ... and says why");

        const ProbeEvaluation blank = EvaluateProbeResult(
            kUserLanguageProbeSentence, " \r\n\t  ", kScriptMaskHangul);
        TEST_CHECK(blank.status == UserLanguageProbeStatus::Unsupported && !blank.non_empty,
                   "P9a (3): a whitespace-only output is Unsupported (branch: empty)");
        TEST_CHECK(blank.detected_mask == 0,
                   "P9a (3): a blank output detects no script");

        const ProbeEvaluation echo = EvaluateProbeResult(
            kUserLanguageProbeSentence, kUserLanguageProbeSentence, kScriptMaskHangul);
        TEST_CHECK(echo.status == UserLanguageProbeStatus::Unsupported && !echo.differs,
                   "P9a (3): an unchanged echo is Unsupported (branch: echo)");
        TEST_CHECK(echo.evidence.find("reason=echo") != std::string::npos,
                   "P9a (3): ... and says why");

        const std::string padded_echo =
            std::string("  ") + kUserLanguageProbeSentence + "\r\n";
        const ProbeEvaluation padded = EvaluateProbeResult(
            kUserLanguageProbeSentence, padded_echo, kScriptMaskHangul);
        TEST_CHECK(padded.status == UserLanguageProbeStatus::Unsupported && !padded.differs,
                   "P9a (3): trimming makes a padded echo an echo");

        const ProbeEvaluation wrong_script = EvaluateProbeResult(
            kUserLanguageProbeSentence,
            "Please send me the report by tomorrow afternoon.", kScriptMaskHangul);
        TEST_CHECK(wrong_script.status == UserLanguageProbeStatus::Unsupported,
                   "P9a (3): a differing output in the wrong script is Unsupported");
        TEST_CHECK(wrong_script.non_empty && wrong_script.differs && !wrong_script.script_ok,
                   "P9a (3): ... failing ONLY the script check (branch: script)");
        TEST_CHECK(wrong_script.evidence.find("reason=script") != std::string::npos,
                   "P9a (3): ... and says why");

        const ProbeEvaluation unknown_script = EvaluateProbeResult(
            kUserLanguageProbeSentence, korean_output, 0);
        TEST_CHECK(unknown_script.status == UserLanguageProbeStatus::Supported,
                   "P9a (3): mask 0 fails OPEN: the content checks alone decide");
        TEST_CHECK(unknown_script.script_ok &&
                       unknown_script.evidence.find("script=na") != std::string::npos &&
                       unknown_script.evidence.find("expected=-") != std::string::npos,
                   "P9a (3): ... and the evidence records the skipped check");

        const ProbeEvaluation mixed_script = EvaluateProbeResult(
            kUserLanguageProbeSentence, "내일 아침까지 (tomorrow morning)", kScriptMaskHangul);
        TEST_CHECK(mixed_script.status == UserLanguageProbeStatus::Supported,
                   "P9a (3): an output carrying the expected script plus Latin still passes");
        TEST_CHECK(mixed_script.detected_mask == (kScriptMaskHangul | kScriptMaskLatin),
                   "P9a (3): ... reported as a composite mask");

        const ProbeEvaluation kana = EvaluateProbeResult(
            kUserLanguageProbeSentence, "レポートを送ってください",
            kScriptMaskHan | kScriptMaskKana);
        TEST_CHECK(kana.status == UserLanguageProbeStatus::Supported,
                   "P9a (3): a kana output passes a han|kana expectation (JA dual-script row)");

        const ProbeEvaluation no_input = EvaluateProbeResult("", korean_output, kScriptMaskHangul);
        TEST_CHECK(no_input.status == UserLanguageProbeStatus::Unsupported &&
                       no_input.evidence.find("reason=noinput") != std::string::npos,
                   "P9a (3): an empty input sentence is an internal-invariant breach, reported");

        TEST_CHECK(ok.status != UserLanguageProbeStatus::Unknown &&
                       ok.status != UserLanguageProbeStatus::Checking &&
                       wrong_script.status != UserLanguageProbeStatus::Unknown,
                   "P9a (3): the evaluator never returns Unknown/Checking");
    }

    // ---- (4) D6 probe-cache transitions ------------------------------------
    {
        TEST_CHECK(UserLanguageEngineId(LanguageEngine::Google) == "google",
                   "P9a (4): Google's probe-cache id");
        TEST_CHECK(UserLanguageEngineId(LanguageEngine::OpenAi) == "openai",
                   "P9a (4): OpenAI's probe-cache id");
        TEST_CHECK(UserLanguageEngineId(LanguageEngine::UserGguf) == "user_gguf",
                   "P9a (4): the user-.gguf engine's probe-cache id");
        TEST_CHECK(UserLanguageEngineId(LanguageEngine::HyMt2).empty() &&
                       UserLanguageEngineId(LanguageEngine::Auto).empty(),
                   "P9a (4): the local built-in engine has NO probe id (D7)");

        UserLanguage language = MakeUserLanguage("한국어", "");
        const std::string engine = UserLanguageEngineId(LanguageEngine::OpenAi);
        const std::string model = "gpt-4o-mini";
        TEST_CHECK(FindUserLanguageProbe(language, engine, model) == nullptr,
                   "P9a (4): an unprobed (engine, model) has no cache entry");

        const ProbeEvaluation echo = EvaluateProbeResult(
            kUserLanguageProbeSentence, kUserLanguageProbeSentence, kScriptMaskHangul);
        RecordUserLanguageProbeResult(language, engine, model, echo, "2026-11-17T22:00:00Z");
        const UserLanguageProbe* probe = FindUserLanguageProbe(language, engine, model);
        TEST_CHECK(probe != nullptr && language.probes.size() == 1,
                   "P9a (4): recording a result creates one cache entry");
        TEST_CHECK(probe != nullptr &&
                       probe->status == UserLanguageProbeStatus::Unsupported,
                   "P9a (4): ... with the evaluator's verdict");
        TEST_CHECK(probe != nullptr && probe->checked_at == "2026-11-17T22:00:00Z" &&
                       probe->evidence.find("reason=echo") != std::string::npos,
                   "P9a (4): ... and the timestamp + shape-only evidence");
        TEST_CHECK(probe != nullptr &&
                       EffectiveUserLanguageProbeStatus(*probe) ==
                           UserLanguageProbeStatus::Unsupported,
                   "P9a (4): the effective status starts as the probe verdict");
        TEST_CHECK(probe != nullptr &&
                       DisplayUserLanguageProbeStatus(*probe) ==
                           UserLanguageProbeStatus::Unsupported,
                   "P9a (4): the display fold matches while no override is set");

        RecordUserLanguageProbeOverride(language, engine, model,
                                        UserLanguageProbeStatus::Supported,
                                        "2026-11-17T22:05:00Z");
        probe = FindUserLanguageProbe(language, engine, model);
        TEST_CHECK(probe != nullptr &&
                       probe->status == UserLanguageProbeStatus::Unsupported &&
                       probe->override_status == UserLanguageProbeStatus::Supported,
                   "P9a (4): the override keeps the probe evidence and records the answer");
        TEST_CHECK(probe != nullptr &&
                       EffectiveUserLanguageProbeStatus(*probe) ==
                           UserLanguageProbeStatus::Supported,
                   "P9a (4): the user's answer wins (D6 final authority)");
        TEST_CHECK(probe != nullptr &&
                       DisplayUserLanguageProbeStatus(*probe) ==
                           UserLanguageProbeStatus::UserOverridden,
                   "P9a (4): ... and the display state says it was overridden");

        RecordUserLanguageProbeOverride(language, engine, model,
                                        UserLanguageProbeStatus::Checking, "");
        probe = FindUserLanguageProbe(language, engine, model);
        TEST_CHECK(probe != nullptr &&
                       probe->override_status == UserLanguageProbeStatus::Supported,
                   "P9a (4): a non-answer override status is rejected (no-op)");

        ClearUserLanguageProbeOverride(language, engine, model);
        probe = FindUserLanguageProbe(language, engine, model);
        TEST_CHECK(probe != nullptr &&
                       probe->override_status == UserLanguageProbeStatus::Unknown &&
                       EffectiveUserLanguageProbeStatus(*probe) ==
                           UserLanguageProbeStatus::Unsupported,
                   "P9a (4): clearing the override returns to the probe verdict");

        const ProbeEvaluation ok = EvaluateProbeResult(
            kUserLanguageProbeSentence, "내일 아침까지 보고서를 보냅니다.", kScriptMaskHangul);
        RecordUserLanguageProbeOverride(language, engine, model,
                                        UserLanguageProbeStatus::Unsupported, "");
        RecordUserLanguageProbeResult(language, engine, model, ok, "2026-11-17T23:00:00Z");
        probe = FindUserLanguageProbe(language, engine, model);
        TEST_CHECK(probe != nullptr &&
                       probe->status == UserLanguageProbeStatus::Supported &&
                       probe->override_status == UserLanguageProbeStatus::Unsupported,
                   "P9a (4): an explicit re-test replaces the verdict; the user's override STANDS "
                   "(261116_0003 P9a fix PR9-06: a probe run never retires the final authority)");
        TEST_CHECK(language.probes.size() == 1,
                   "P9a (4): re-recording the same key updates in place (no duplicate)");

        RecordUserLanguageProbeResult(language, engine, "other-model", ok, "");
        TEST_CHECK(language.probes.size() == 2 &&
                       FindUserLanguageProbe(language, engine, "other-model") != nullptr,
                   "P9a (4): a different model is a different cache key (D6 per model)");
        RecordUserLanguageProbeResult(language, "google", "", ok, "");
        TEST_CHECK(language.probes.size() == 3 &&
                       FindUserLanguageProbe(language, "google", "") != nullptr,
                   "P9a (4): an empty model is a legal, distinct cache key");
        RecordUserLanguageProbeResult(language, "", "", ok, "");
        TEST_CHECK(language.probes.size() == 3,
                   "P9a (4): an empty engine id is not addressable and is ignored");

        UserLanguage capped = MakeUserLanguage("Twi", "");
        for (int i = 0; i < 12; ++i) {
            RecordUserLanguageProbeResult(capped, engine, "model-" + std::to_string(i), ok, "");
        }
        TEST_CHECK(capped.probes.size() == kMaxUserLanguageProbes,
                   "P9a (4): the per-language cache is capped at kMaxUserLanguageProbes");
    }

    // ---- (5) config round-trip: add -> save -> reload -> merge -------------
    {
        AppConfig config;
        TEST_CHECK(config.user_languages.empty(),
                   "P9a (5): a fresh config has no user languages");
        config.user_languages.push_back(MakeUserLanguage("한국어", ""));
        config.user_languages.push_back(MakeUserLanguage("Twi", ""));
        const ProbeEvaluation ok = EvaluateProbeResult(
            kUserLanguageProbeSentence, "내일 아침까지 보고서를 보냅니다.", kScriptMaskHangul);
        RecordUserLanguageProbeResult(config.user_languages[0], "openai", "gpt-4o-mini", ok,
                                      "2026-11-17T23:30:00Z");
        RecordUserLanguageProbeOverride(config.user_languages[0], "openai", "gpt-4o-mini",
                                        UserLanguageProbeStatus::Supported,
                                        "2026-11-17T23:31:00Z");
        // A second entry carries a RESULT-ONLY cache entry, so both writer paths
        // (verdict without override, verdict with override) are covered.
        RecordUserLanguageProbeResult(config.user_languages[1], "openai", "gpt-4o-mini", ok,
                                      "2026-11-17T22:59:00Z");

        const std::string json = config.ToJsonString();
        TEST_CHECK(json.find("\"user_languages_version\": 1") != std::string::npos,
                   "P9a (5): the writer emits the schema version");
        TEST_CHECK(json.find("\"native_name\": \"Twi\"") != std::string::npos,
                   "P9a (5): the writer emits the native name");
        TEST_CHECK(json.find("\"script\": \"latin\"") != std::string::npos,
                   "P9a (5): the writer emits the inferred script as a token");
        TEST_CHECK(json.find("\"script\": \"hangul\"") != std::string::npos,
                   "P9a (5): ... and the Hangul token for the Korean entry");
        TEST_CHECK(json.find("\"status\": \"supported\"") != std::string::npos &&
                       json.find("\"override\": \"supported\"") != std::string::npos,
                   "P9a (5): the writer emits both the probe verdict and the override");
        TEST_CHECK(json.find("\"checked_at\": \"2026-11-17T22:59:00Z\"") != std::string::npos,
                   "P9a (5): the writer emits a result-only checked-at stamp");

        AppConfig reloaded;
        TEST_CHECK(reloaded.FromJsonString(json),
                   "P9a (5): the emitted document re-parses");
        TEST_CHECK(reloaded.user_languages.size() == 2,
                   "P9a (5): ... with both entries");
        const UserLanguage& reloaded_ko = reloaded.user_languages[0];
        TEST_CHECK(reloaded_ko.native_name == "한국어" && reloaded_ko.code == "KO",
                   "P9a (5): the Korean entry survives the round trip (native + code)");
        TEST_CHECK(reloaded_ko.english_name == "Korean",
                   "P9a (5): ... with the filled registry English name");
        TEST_CHECK(reloaded_ko.script_mask == kScriptMaskHangul,
                   "P9a (5): ... and the script token");
        TEST_CHECK(reloaded_ko.probes.size() == 1,
                   "P9a (5): ... and its probe cache entry");
        TEST_CHECK(reloaded_ko.probes[0].status == UserLanguageProbeStatus::Supported &&
                       reloaded_ko.probes[0].override_status ==
                           UserLanguageProbeStatus::Supported,
                   "P9a (5): ... with verdict + override intact");
        TEST_CHECK(reloaded_ko.probes[0].checked_at == "2026-11-17T23:31:00Z",
                   "P9a (5): ... and the override's timestamp (last write wins)");
        TEST_CHECK(reloaded_ko.probes[0].evidence.find("nonempty=1") != std::string::npos,
                   "P9a (5): ... and the evidence");
        const UserLanguage& reloaded_twi = reloaded.user_languages[1];
        TEST_CHECK(reloaded_twi.native_name == "Twi" && reloaded_twi.code.empty(),
                   "P9a (5): the free-form entry survives with an empty code");
        TEST_CHECK(reloaded_twi.probes.size() == 1 &&
                       reloaded_twi.probes[0].status == UserLanguageProbeStatus::Supported &&
                       reloaded_twi.probes[0].override_status ==
                           UserLanguageProbeStatus::Unknown &&
                       reloaded_twi.probes[0].checked_at == "2026-11-17T22:59:00Z",
                   "P9a (5): ... with its result-only cache entry and no override");
        TEST_CHECK(reloaded_twi.script_mask == kScriptMaskLatin,
                   "P9a (5): ... and its inferred script");

        // The menu merge is the consumer of the reloaded data (P9b renders it).
        const LanguageMenuView google_after_reload =
            BuildLanguageMenuView(LanguageEngine::Google, false, reloaded.user_languages);
        TEST_CHECK(google_after_reload.user_group.size() == 1 &&
                       google_after_reload.user_group[0].registry != nullptr &&
                       google_after_reload.user_group[0].registry->code == "KO",
                   "P9a (5): reload -> merge offers the resolved user language in the cloud view");

        // Disk path: save -> load through the real filesystem plumbing.
        std::error_code ec;
        const std::filesystem::path tmp =
            std::filesystem::temp_directory_path(ec) / "emebalachat_p9a_user_languages.json";
        std::filesystem::remove(tmp, ec); // stale leftovers must not mask a save failure
        TEST_CHECK(config.SaveToFile(tmp), "P9a (5): SaveToFile writes the block");
        AppConfig from_disk;
        TEST_CHECK(from_disk.LoadFromFile(tmp), "P9a (5): LoadFromFile reads it back");
        TEST_CHECK(from_disk.user_languages.size() == 2 &&
                       from_disk.user_languages[0].code == "KO" &&
                       from_disk.user_languages[1].native_name == "Twi",
                   "P9a (5): the on-disk round trip preserves both entries");
        TEST_CHECK(from_disk.user_languages[0].probes.size() == 1 &&
                       from_disk.user_languages[0].probes[0].override_status ==
                           UserLanguageProbeStatus::Supported,
                   "P9a (5): ... including the probe cache");
        std::filesystem::remove(tmp, ec); // cleanup (best-effort, our own artifact)
    }

    // ---- (6) missing-block backward compat + malformed-block tolerance -----
    {
        const std::string legacy =
            "{\"ui_language\": \"ko\", \"engine_type\": \"auto\", "
            "\"target_language\": \"English\"}";
        AppConfig legacy_config;
        TEST_CHECK(legacy_config.FromJsonString(legacy),
                   "P9a (6): a pre-P9a document still parses");
        TEST_CHECK(legacy_config.user_languages.empty(),
                   "P9a (6): a MISSING user_languages block means no user languages");
        TEST_CHECK(legacy_config.ui_language == "ko",
                   "P9a (6): ... and the other keys are unaffected");
        const std::string legacy_rewritten = legacy_config.ToJsonString();
        TEST_CHECK(legacy_rewritten.find("\"user_languages\": []") != std::string::npos,
                   "P9a (6): the next save writes an explicit empty array");
        AppConfig legacy_again;
        TEST_CHECK(legacy_again.FromJsonString(legacy_rewritten) &&
                       legacy_again.user_languages.empty(),
                   "P9a (6): ... which reads back as an empty registry");

        const char* malformed[] = {
            "{\"user_languages\": 42}",
            "{\"user_languages\": {}}",
            "{\"user_languages\": [{}]}",
            "{\"user_languages\": [{\"native_name\": \"\"}]}",
            "{\"user_languages\": [{\"english_name\": \"Ghost\"}]}",
            "{\"user_languages\": [{\"native_name\": \"Twi\", \"probes\": \"nope\"}]}",
        };
        bool malformed_ok = true;
        for (const char* document : malformed) {
            AppConfig config;
            if (!config.FromJsonString(document) || config.user_languages.size() > 1) {
                malformed_ok = false;
            }
        }
        TEST_CHECK(malformed_ok,
                   "P9a (6): malformed blocks parse without crashing and never invent entries");

        AppConfig probes_not_array;
        TEST_CHECK(probes_not_array.FromJsonString(
                       "{\"user_languages\": [{\"native_name\": \"Twi\", \"probes\": \"nope\"}]}") &&
                       probes_not_array.user_languages.size() == 1 &&
                       probes_not_array.user_languages[0].probes.empty(),
                   "P9a (6): a non-array probes value yields an entry with no cached verdicts");

        AppConfig bogus_code;
        TEST_CHECK(bogus_code.FromJsonString(
                       "{\"user_languages\": [{\"native_name\": \"Twi\", \"code\": \"nonsense\"}]}") &&
                       bogus_code.user_languages.size() == 1 &&
                       bogus_code.user_languages[0].code.empty(),
                   "P9a (6): an unknown code is normalized to the free-form empty code");

        AppConfig brace_in_string;
        TEST_CHECK(brace_in_string.FromJsonString(
                       "{\"user_languages\": [{\"native_name\": \"Twi } { we\"}]}") &&
                       brace_in_string.user_languages.size() == 1 &&
                       brace_in_string.user_languages[0].native_name == "Twi } { we",
                   "P9a (6): a brace inside a string cannot truncate the entry scan");

        AppConfig hand_written;
        TEST_CHECK(hand_written.FromJsonString(
                       "{\"user_languages\": [{\"native_name\": \"\\ud55c\\uad6d\\uc5b4\"}]}") &&
                       hand_written.user_languages.size() == 1 &&
                       hand_written.user_languages[0].code == "KO" &&
                       hand_written.user_languages[0].script_mask == kScriptMaskHangul,
                   "P9a (6): a hand-written entry with no derived keys re-derives code + script");

        AppConfig explicit_empty_code;
        TEST_CHECK(explicit_empty_code.FromJsonString(
                       "{\"user_languages\": [{\"native_name\": \"\\ud55c\\uad6d\\uc5b4\", "
                       "\"code\": \"\"}]}") &&
                       explicit_empty_code.user_languages.size() == 1 &&
                       explicit_empty_code.user_languages[0].code.empty(),
                   "P9a (6): an EXPLICIT empty code is trusted (recorded decision, not re-derived)");

        AppConfig capped;
        TEST_CHECK(capped.FromJsonString(ManyEntriesJson(70)) &&
                       capped.user_languages.size() == kMaxUserLanguages,
                   "P9a (6): the reader caps the registry at kMaxUserLanguages");

        std::string many_probes = "{\"user_languages\": [{\"native_name\": \"Twi\", \"probes\": [";
        for (int i = 0; i < 12; ++i) {
            if (i != 0) {
                many_probes += ",";
            }
            many_probes += "{\"engine\": \"openai\", \"model\": \"m" + std::to_string(i) +
                           "\", \"status\": \"supported\"}";
        }
        many_probes += "]}]}";
        AppConfig capped_probes;
        TEST_CHECK(capped_probes.FromJsonString(many_probes) &&
                       capped_probes.user_languages.size() == 1 &&
                       capped_probes.user_languages[0].probes.size() == kMaxUserLanguageProbes,
                   "P9a (6): the reader caps one language's probe cache");

        // Transient/display-only status tokens never survive a load: a crashed
        // probe ("checking") or a directionless hand-edited "user_overridden"
        // must not freeze a language in a state nobody can clear.
        AppConfig transient_status;
        TEST_CHECK(transient_status.FromJsonString(
                       "{\"user_languages\": [{\"native_name\": \"Twi\", \"probes\": ["
                       "{\"engine\": \"openai\", \"model\": \"m\", \"status\": \"checking\"},"
                       "{\"engine\": \"google\", \"model\": \"\", \"status\": \"user_overridden\"}"
                       "]}]}") &&
                       transient_status.user_languages.size() == 1 &&
                       transient_status.user_languages[0].probes.size() == 2 &&
                       transient_status.user_languages[0].probes[0].status ==
                           UserLanguageProbeStatus::Unknown &&
                       transient_status.user_languages[0].probes[1].status ==
                           UserLanguageProbeStatus::Unknown,
                   "P9a (6): transient status tokens read back as Unknown (no stuck state)");

        // The probe sentence is a fixed non-personal constant: cached verdicts
        // are only comparable if the input never drifts, and the probe must
        // never carry user content.
        const std::string probe_sentence(kUserLanguageProbeSentence);
        bool probe_sentence_ascii = !probe_sentence.empty();
        for (const char c : probe_sentence) {
            if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7E) {
                probe_sentence_ascii = false;
            }
        }
        TEST_CHECK(probe_sentence_ascii,
                   "P9a (6): the probe sentence is a fixed printable-ASCII constant");
    }

    // ---- (7) newer-schema forward compat ----------------------------------
    {
        const std::string future_document =
            "{\n  \"user_languages_version\": 2,\n"
            "  \"user_languages\": [{\"native_name\": \"Twi\", \"future_field\": {\"a\": [1, 2]}}]\n}";
        AppConfig future;
        TEST_CHECK(future.FromJsonString(future_document),
                   "P9a (7): a newer-schema document still loads");
        TEST_CHECK(future.user_languages.empty(),
                   "P9a (7): ... without guessing at fields this build does not know");
        const std::string rewritten = future.ToJsonString();
        TEST_CHECK(rewritten.find("\"user_languages_version\": 2") != std::string::npos,
                   "P9a (7): the declared version is preserved on the next save");
        TEST_CHECK(rewritten.find("\"future_field\"") != std::string::npos &&
                       rewritten.find("[1, 2]") != std::string::npos,
                   "P9a (7): ... and the whole block is re-emitted verbatim (no data loss)");
        AppConfig future_again;
        TEST_CHECK(future_again.FromJsonString(rewritten) &&
                       future_again.ToJsonString().find("\"future_field\"") != std::string::npos,
                   "P9a (7): preservation is idempotent across a second round trip");

        AppConfig back_to_v1;
        TEST_CHECK(back_to_v1.FromJsonString(
                       "{\"user_languages_version\": 1, \"user_languages\": "
                       "[{\"native_name\": \"Twi\"}]}") &&
                       back_to_v1.user_languages.size() == 1 &&
                       back_to_v1.ToJsonString().find("\"user_languages_version\": 1") !=
                           std::string::npos,
                   "P9a (7): returning to a v1 block parses it and drops the carrier");
    }

    // ---- (8) PlanLanguageSync raw-token allowance -------------------------
    {
        std::vector<UserLanguage> users;
        users.push_back(MakeUserLanguage("Twi", ""));       // free-form, stored "Twi"
        users.push_back(MakeUserLanguage("한국어", ""));     // resolved, stored "Korean"

        LanguageSyncPlan plan = PlanLanguageSync(LanguageContext::Type, "Auto Detect", "English",
                                                 std::string_view(), "Twi", users);
        TEST_CHECK(plan.valid && plan.target_language == "Twi" && plan.changed,
                   "P9a (8): a REGISTERED user language is accepted as a target");
        TEST_CHECK(plan.surface_updates.size() == 3,
                   "P9a (8): ... and refreshes the three views like any valid mutation");

        plan = PlanLanguageSync(LanguageContext::Type, "Auto Detect", "English",
                                std::string_view(), "Elvish", users);
        TEST_CHECK(!plan.valid && plan.target_language == "English" && !plan.changed,
                   "P9a (8): an UNREGISTERED free-form token is still refused");

        plan = PlanLanguageSync(LanguageContext::Type, "Auto Detect", "English",
                                std::string_view(), "Klingon", users);
        TEST_CHECK(!plan.valid,
                   "P9a (8): 'Klingon' is still refused (prompt raw-token is not a config write)");

        plan = PlanLanguageSync("Auto Detect", "English", std::string_view(), "Twi");
        TEST_CHECK(!plan.valid,
                   "P9a (8): the registry-only overload keeps the refusal (allowance is SCOPED)");

        plan = PlanLanguageSync(LanguageContext::Type, "Auto Detect", "English",
                                std::string_view(), "한국어", users);
        TEST_CHECK(plan.valid && plan.target_language == "Korean",
                   "P9a (8): a user entry canonicalizes to its stored (registry) form");

        plan = PlanLanguageSync(LanguageContext::Type, "Twi", "English",
                                std::string_view(), std::string_view(), users);
        TEST_CHECK(plan.valid && !plan.changed && plan.source_language == "Twi" &&
                       plan.target_language == "English",
                   "P9a (8): a persisted user-language pin reports changed=false (no disk churn)");

        plan = PlanLanguageSync(LanguageContext::Type, "Auto Detect", "Twi",
                                std::string_view(), std::string_view(), users);
        TEST_CHECK(plan.valid && !plan.changed && plan.target_language == "Twi",
                   "P9a (8): ... in the TARGET slot too");

        std::vector<UserLanguage> auto_trap = users;
        auto_trap.push_back(MakeUserLanguage("Auto Detect", std::string_view()));
        plan = PlanLanguageSync(LanguageContext::Type, "Auto Detect", "English",
                                std::string_view(), "Auto Detect", auto_trap);
        TEST_CHECK(!plan.valid,
                   "P9a (8): AUTO stays a non-target even with a user entry spelled like it");

        std::vector<UserLanguage> english_named;
        {
            UserLanguage entry;
            entry.native_name = "Samplelandic";
            entry.english_name = "Samplelish";
            entry.script_mask = kScriptMaskLatin;
            english_named.push_back(entry);
        }
        plan = PlanLanguageSync(LanguageContext::Type, "Auto Detect", "English",
                                std::string_view(), "samplelish", english_named);
        TEST_CHECK(plan.valid && plan.target_language == "Samplelish",
                   "P9a (8): the user entry's English name is an accepted spelling");

        plan = PlanLanguageSync(LanguageContext::Type, "Samplelandic", "English",
                                std::string_view(), std::string_view(), english_named);
        TEST_CHECK(plan.valid && plan.changed && plan.source_language == "Samplelish",
                   "P9a (8): a persisted NATIVE-name pin rewrites to the stored form once");
        plan = PlanLanguageSync(LanguageContext::Type, "Samplelish", "English",
                                std::string_view(), std::string_view(), english_named);
        TEST_CHECK(plan.valid && !plan.changed && plan.source_language == "Samplelish",
                   "P9a (8): ... and the rewritten pin then reports no churn");

        plan = PlanLanguageSync(LanguageContext::Type, "Auto Detect", "English",
                                std::string_view(), "Auto Detect", users);
        TEST_CHECK(!plan.valid, "P9a (8): the AUTO target refusal is unchanged for plain users");
        (void)0;
    }

    // ---- (9) free-form prompt pins must NOT regress ------------------------
    {
        TEST_CHECK(BuildPrompt("hi", "Klingon").rfind(
                       "Translate the following text into Klingon", 0) == 0,
                   "P9a (9): BuildPrompt still injects an unresolvable token raw");
        TEST_CHECK(BuildCompletionPrompt("hello", "Klingon", "English") ==
                       "Translate this from English to Klingon:\nEnglish: hello\nKlingon:",
                   "P9a (9): BuildCompletionPrompt's raw-token form is unchanged");
        TEST_CHECK(BuildPrompt("hi", "Korean").rfind(
                       "Translate the following text into Korean", 0) == 0,
                   "P9a (9): the resolved registry path is unchanged");
    }

    // ---- (10) D5/D7 menu merge --------------------------------------------
    {
        std::vector<UserLanguage> users;
        users.push_back(MakeUserLanguage("한국어", ""));   // KO (resolved)
        users.push_back(MakeUserLanguage("Twi", ""));      // free-form
        users.push_back(MakeUserLanguage("Tibetan", ""));  // BO (no Google bit)

        const std::vector<const LanguageInfo*>& raw_local =
            GetLanguagesForEngine(LanguageEngine::HyMt2, false);
        const LanguageMenuView local = BuildLanguageMenuView(LanguageEngine::HyMt2, false, users);
        TEST_CHECK(local.user_group.empty(),
                   "P9a (10): the LOCAL view has no '내 언어' group (D7: no custom row)");
        TEST_CHECK(RegistryEquals(local.registry, raw_local),
                   "P9a (10): ... and its registry section is the raw official view");

        const std::vector<const LanguageInfo*>& raw_google =
            GetLanguagesForEngine(LanguageEngine::Google, false);
        const LanguageMenuView google_view =
            BuildLanguageMenuView(LanguageEngine::Google, false, users);
        TEST_CHECK(google_view.user_group.size() == 1,
                   "P9a (10): the Google view offers only the publisher-declared user entry");
        TEST_CHECK(google_view.user_group[0].registry != nullptr &&
                       google_view.user_group[0].registry->code == "KO" &&
                       google_view.user_group[0].user != nullptr &&
                       google_view.user_group[0].user->native_name == "한국어",
                   "P9a (10): ... carrying the registry row AND the user entry");
        TEST_CHECK(google_view.registry.size() == raw_google.size() - 1,
                   "P9a (10): the claimed row leaves the registry section");
        TEST_CHECK(CountRowsWithCode(google_view, "KO") == 1,
                   "P9a (10): ... so KO appears exactly once in the menu");
        TEST_CHECK(!HasRegistryRow(google_view.registry, "KO") &&
                       !HasRegistryRow(google_view.registry, "BO"),
                   "P9a (10): KO moved to the user group and BO was never a Google row");
        TEST_CHECK(!HasRegistryRow(google_view.registry, "AUTO"),
                   "P9a (10): a target view (include_auto=false) drops AUTO");

        const LanguageMenuView google_with_auto =
            BuildLanguageMenuView(LanguageEngine::Google, true, users);
        TEST_CHECK(!google_with_auto.registry.empty() &&
                       google_with_auto.registry.front()->code == "AUTO",
                   "P9a (10): a source view keeps AUTO at the registry head");
        TEST_CHECK(google_with_auto.user_group.size() == google_view.user_group.size(),
                   "P9a (10): ... and the user group is unaffected by include_auto");

        const LanguageMenuView openai_view =
            BuildLanguageMenuView(LanguageEngine::OpenAi, false, users);
        TEST_CHECK(openai_view.user_group.size() == 3,
                   "P9a (10): the OpenAI view offers every registered entry (free-form)");
        TEST_CHECK(openai_view.user_group[1].registry == nullptr &&
                       openai_view.user_group[1].user != nullptr &&
                       openai_view.user_group[1].user->native_name == "Twi",
                   "P9a (10): a code-less entry is a free-form row (registry == nullptr)");
        TEST_CHECK(openai_view.user_group[2].registry != nullptr &&
                       openai_view.user_group[2].registry->code == "BO",
                   "P9a (10): BO is offered by the free-form engines");

        const LanguageMenuView gguf_view =
            BuildLanguageMenuView(LanguageEngine::UserGguf, false, users);
        TEST_CHECK(gguf_view.user_group.size() == 3,
                   "P9a (10): the user-.gguf view offers every registered entry too");
        TEST_CHECK(CountRowsWithCode(gguf_view, "KO") == 1 &&
                       CountRowsWithCode(gguf_view, "BO") == 1,
                   "P9a (10): the free-form views keep one row per claimed language");

        const LanguageMenuView auto_view =
            BuildLanguageMenuView(LanguageEngine::Auto, false, users);
        TEST_CHECK(auto_view.user_group.size() == google_view.user_group.size() &&
                       auto_view.registry.size() == google_view.registry.size(),
                   "P9a (10): the menu view under auto equals the Google view (P8b tray idiom)");
        TEST_CHECK(auto_view.user_group.size() == 1,
                   "P9a (10): ... which means the default engine setting shows '내 언어'");

        const LanguageMenuView plain =
            BuildLanguageMenuView(LanguageEngine::Google, false, {});
        TEST_CHECK(plain.user_group.empty() && RegistryEquals(plain.registry, raw_google),
                   "P9a (10): with no user languages the view is the raw registry view");

        std::vector<UserLanguage> duplicates;
        duplicates.push_back(MakeUserLanguage("한국어", ""));
        duplicates.push_back(MakeUserLanguage("Korean", ""));
        const LanguageMenuView deduped =
            BuildLanguageMenuView(LanguageEngine::Google, false, duplicates);
        TEST_CHECK(deduped.user_group.size() == 1 &&
                       CountRowsWithCode(deduped, "KO") == 1,
                   "P9a (10): two entries naming one language collapse to one row");

        std::vector<UserLanguage> unnamed_entries;
        unnamed_entries.push_back(UserLanguage{});
        const LanguageMenuView unnamed_view =
            BuildLanguageMenuView(LanguageEngine::OpenAi, false, unnamed_entries);
        TEST_CHECK(unnamed_view.user_group.empty(),
                   "P9a (10): an unnamed (invalid) entry is never rendered");

        // 261116_0003 P9b-3 PR9-11 (supersedes the pre-P9b pin that recorded
        // the pathological case): an "Auto Detect"-named entry is now REFUSED
        // at creation - it was offered by the OpenAI/user-.gguf menus but
        // refused by the planner (offers=1 / valid=0, a dead menu row). The
        // merge half stays pinned as defense in depth for a config.json that
        // already carries such an entry (hand-edited, or written by a build
        // that predates the rule): it never maps to the AUTO registry row.
        TEST_CHECK(ValidateUserLanguageName("Auto Detect", "", {}) ==
                       UserLanguageNameVerdict::Reserved,
                   "P9b-3 PR9-11: an 'Auto Detect'-named user entry is refused at creation");
        const LanguageMenuView auto_trap_view = BuildLanguageMenuView(
            LanguageEngine::OpenAi, false, {MakeUserLanguage("Auto Detect", "")});
        TEST_CHECK(auto_trap_view.user_group.size() == 1 &&
                       auto_trap_view.user_group[0].registry == nullptr,
                   "P9a (10): a hand-edited AUTO-named entry never maps to the AUTO row");
    }

    // ---- 261116_0003 P9a fix round (PR9-01..PR9-09, fix-list-round9.md) ----
    {
        // PR9-01: code-level first-occurrence-wins dedupe. Two entries
        // carrying the SAME code under DIFFERENT stored names must collapse
        // (the old name-level dedupe could not distinguish them; live-proven:
        // the Google user group rendered both code=="KO" rows).
        std::vector<UserLanguage> same_code;
        UserLanguage first_ko;
        first_ko.native_name = "한국어";
        first_ko.code = "KO";
        same_code.push_back(first_ko);
        UserLanguage second_ko;
        second_ko.native_name = "Korean custom";
        second_ko.english_name = "Korean Custom";
        second_ko.code = "KO";
        same_code.push_back(second_ko);
        const LanguageMenuView code_deduped =
            BuildLanguageMenuView(LanguageEngine::OpenAi, false, same_code);
        TEST_CHECK(code_deduped.user_group.size() == 1 &&
                       code_deduped.user_group[0].user == &same_code[0],
                   "PR9-01: same-code entries with different names collapse to the FIRST row");

        // code-less entries fall back to name-level dedupe.
        std::vector<UserLanguage> same_freeform_name;
        UserLanguage form_a;
        form_a.native_name = "Twi";
        same_freeform_name.push_back(form_a);
        UserLanguage form_b;
        form_b.native_name = "twi";
        same_freeform_name.push_back(form_b);
        const LanguageMenuView name_deduped =
            BuildLanguageMenuView(LanguageEngine::OpenAi, false, same_freeform_name);
        TEST_CHECK(name_deduped.user_group.size() == 1,
                   "PR9-01: code-less entries fall back to name-level dedupe");

        // PR9-02: a name that folds to EMPTY (ZWSP-only U+200B, NBSP-only
        // U+00A0) must not resolve through the picker's empty-query branch to
        // the first pool row (EN).
        const std::string zwsp_only = "\xE2\x80\x8B";
        const std::string nbsp_only = "\xC2\xA0";
        const UserLanguageResolution zwsp = ResolveUserLanguageCode(zwsp_only, "");
        TEST_CHECK(zwsp.code.empty() && !zwsp.from_alias && !zwsp.from_locale,
                   "PR9-02: a ZWSP-only name resolves to no code");
        const UserLanguageResolution nbsp = ResolveUserLanguageCode(nbsp_only, "");
        TEST_CHECK(nbsp.code.empty() && !nbsp.from_alias && !nbsp.from_locale,
                   "PR9-02: an NBSP-only name resolves to no code");

        // PR9-03: the sanitizer strips C0/C1/DEL + bidi overrides/isolates at
        // BOTH entry paths, and caps field lengths at a UTF-8 boundary.
        const std::string bidi_marks = std::string("Twi") + "\xE2\x80\x8E" /* U+200E LRM */ +
                                       "\xE2\x80\xAA" /* U+202A LRE */ +
                                       "\xE2\x81\xA7" /* U+2067 RLI */ +
                                       "\xE2\x80\x8F" /* U+200F RLM */;
        TEST_CHECK(MakeUserLanguage(bidi_marks, "").native_name == "Twi",
                   "PR9-03: bidi override/isolate marks are stripped at the build path");
        TEST_CHECK(MakeUserLanguage("T\x01w\x7Fi", "").native_name == "Twi",
                   "PR9-03: C0 controls and DEL are stripped at the build path");
        TEST_CHECK(MakeUserLanguage(std::string("A\xC2\x85") + "B", "").native_name == "AB",
                   "PR9-03: C1 controls are stripped at the build path");

        AppConfig sanitized_load;
        TEST_CHECK(sanitized_load.FromJsonString(
                       "{\"user_languages\": [{\"native_name\": \"Tw\\u0001i \\u202ab\\u202c\"}]}") &&
                       sanitized_load.user_languages.size() == 1 &&
                       sanitized_load.user_languages[0].native_name == "Twi b",
                   "PR9-03: controls and bidi marks are stripped at the parse path");

        const std::string overlong_name(kMaxUserLanguageNameBytes + 32, 'A');
        const UserLanguage capped_name = MakeUserLanguage(overlong_name, "");
        TEST_CHECK(capped_name.native_name.size() == kMaxUserLanguageNameBytes,
                   "PR9-03: a name past the byte cap is truncated at the cap");
        std::string hangul_over_cap;
        while (hangul_over_cap.size() <= kMaxUserLanguageNameBytes) {
            hangul_over_cap += "\xED\x95\x9C"; // one Hangul syllable, 3 bytes
        }
        const std::string capped_hangul = MakeUserLanguage(hangul_over_cap, "").native_name;
        TEST_CHECK(capped_hangul.size() <= kMaxUserLanguageNameBytes &&
                       (capped_hangul.size() % 3) == 0,
                   "PR9-03: the byte cap never splits a UTF-8 code point");

        // PR9-05: probe string fields are byte-capped on load.
        const std::string long_model(kMaxUserLanguageProbeModelBytes + 16, 'm');
        const std::string long_evidence(kMaxUserLanguageProbeEvidenceBytes + 16, 'e');
        const std::string long_stamp(kMaxUserLanguageProbeCheckedAtBytes + 16, 'c');
        AppConfig fat_probe;
        TEST_CHECK(fat_probe.FromJsonString(
                       "{\"user_languages\": [{\"native_name\": \"Twi\", \"probes\": [{"
                       "\"engine\": \"openai\", \"model\": \"" +
                       long_model + "\", \"status\": \"supported\", \"checked_at\": \"" +
                       long_stamp + "\", \"evidence\": \"" + long_evidence + "\"}]}]}") &&
                       fat_probe.user_languages.size() == 1 &&
                       fat_probe.user_languages[0].probes.size() == 1 &&
                       fat_probe.user_languages[0].probes[0].model.size() ==
                           kMaxUserLanguageProbeModelBytes &&
                       fat_probe.user_languages[0].probes[0].evidence.size() ==
                           kMaxUserLanguageProbeEvidenceBytes &&
                       fat_probe.user_languages[0].probes[0].checked_at.size() ==
                           kMaxUserLanguageProbeCheckedAtBytes,
                   "PR9-05: probe model/evidence/checked_at are byte-capped on load");

        // PR9-05: duplicate (engine, model) keys within one entry keep the
        // FIRST verdict (no shadowing, no slot-cap exhaustion).
        AppConfig dup_probes;
        TEST_CHECK(dup_probes.FromJsonString(
                       "{\"user_languages\": [{\"native_name\": \"Twi\", \"probes\": ["
                       "{\"engine\": \"openai\", \"model\": \"m\", \"status\": \"supported\"},"
                       "{\"engine\": \"openai\", \"model\": \"m\", \"status\": \"unsupported\"},"
                       "{\"engine\": \"openai\", \"model\": \"n\", \"status\": \"unsupported\"}"
                       "]}]}") &&
                       dup_probes.user_languages.size() == 1 &&
                       dup_probes.user_languages[0].probes.size() == 2 &&
                       dup_probes.user_languages[0].probes[0].status ==
                           UserLanguageProbeStatus::Supported,
                   "PR9-05: duplicate (engine, model) keys dedupe on load (first wins)");

        // PR9-06: a probe result recorded over an override-only entry must not
        // retire the user's final-authority answer.
        UserLanguage override_only = MakeUserLanguage("Twi", "");
        const ProbeEvaluation twi_ok = EvaluateProbeResult(
            kUserLanguageProbeSentence, "Twi output text", kScriptMaskLatin);
        RecordUserLanguageProbeOverride(override_only, "openai", "m",
                                        UserLanguageProbeStatus::Unsupported,
                                        "2026-11-17T23:59:00Z");
        RecordUserLanguageProbeResult(override_only, "openai", "m", twi_ok,
                                      "2026-11-18T00:00:00Z");
        const UserLanguageProbe* guarded = FindUserLanguageProbe(override_only, "openai", "m");
        TEST_CHECK(guarded != nullptr &&
                       guarded->status == UserLanguageProbeStatus::Supported &&
                       guarded->override_status == UserLanguageProbeStatus::Unsupported &&
                       EffectiveUserLanguageProbeStatus(*guarded) ==
                           UserLanguageProbeStatus::Unsupported,
                   "PR9-06: a probe run never retires the user's override");

        // PR9-07: a locale-fallback resolution back-fills the registry English
        // name too (not only alias-matrix resolutions).
        // 261116_0003 P9a fix2 PR10-05: the pin runs on the deterministic
        // FakeLocaleSource install instead of searching the LIVE locale table.
        // The old probe needed a host install carrying a stage-2-only name and
        // hard-failed on one that had none; the seam makes the rule
        // deterministic ("Samplelandic" resolves only through the locale row
        // fi-FI, and the back-fill must still fill the registry name).
        const UserLanguage fake_locale_made =
            MakeUserLanguage("Samplelandic", "", &FakeLocaleSource);
        TEST_CHECK(fake_locale_made.code == "FI" && fake_locale_made.english_name == "Finnish",
                   "PR10-05: a locale-fallback resolution back-fills the registry English name");

        // PR9-08: a brace inside an array comment cannot truncate the entry
        // scan (the live finding: one commented "}" swallowed every following
        // entry and the next save persisted the loss).
        AppConfig commented;
        TEST_CHECK(commented.FromJsonString(
                       "{\"user_languages\": ["
                       "{\"native_name\": \"Twi\"}, // a stray } brace in a note\n"
                       "{\"native_name\": \"Twi Two\"}"
                       "]}") &&
                       commented.user_languages.size() == 2 &&
                       commented.user_languages[0].native_name == "Twi" &&
                       commented.user_languages[1].native_name == "Twi Two",
                   "PR9-08: a brace inside a line comment cannot truncate the entry scan");

        // PR9-04: a STRING-typed newer block fails closed. The raw capture
        // keeps the value's quotes and escapes, and the carrier refuses
        // non-arrays, so the decoded-string re-emission that fabricated
        // sibling config keys is impossible.
        AppConfig string_carrier;
        TEST_CHECK(string_carrier.FromJsonString(
                       "{\"user_languages_version\": 2, "
                       "\"user_languages\": \"\\\"fabricated_sibling\\\": 1\"}") &&
                       string_carrier.user_languages.empty(),
                   "PR9-04: a string-typed newer block loads as an empty registry");
        const std::string string_carrier_rewrite = string_carrier.ToJsonString();
        TEST_CHECK(string_carrier_rewrite.find("fabricated_sibling") == std::string::npos &&
                       string_carrier_rewrite.find("\"user_languages\": []") !=
                           std::string::npos &&
                       string_carrier_rewrite.find("\"user_languages_version\": 1") !=
                           std::string::npos,
                   "PR9-04: fail closed: the save writes a clean empty v1 block, no fabricated keys");

        // ... and an ARRAY-typed newer block is still preserved verbatim.
        AppConfig array_carrier;
        TEST_CHECK(array_carrier.FromJsonString(
                       "{\"user_languages_version\": 2, "
                       "\"user_languages\": [{\"native_name\": \"Twi\", \"future_key\": [7]}]}") &&
                       array_carrier.user_languages.empty(),
                   "PR9-04: an array-typed newer block loads as a preserved carrier");
        TEST_CHECK(array_carrier.ToJsonString().find("\"future_key\": [7]") != std::string::npos,
                   "PR9-04: ... and the whole array survives the save verbatim");

        // PR9-09: strict digit-only version parse; an unparseable ("2junk",
        // "abc") or newer ("999") version is never guessed at as current: the
        // block and the version text round-trip verbatim.
        // 261116_0003 P9a fix2 PR10-01: a NON-NUMERIC version text now
        // round-trips as a QUOTED, escaped JSON string (a numeric one stays a
        // bare integer), so the expected emission is built per version kind.
        for (const char* bad_version : {"2junk", "abc", "999"}) {
            const std::string version_literal(bad_version);
            const bool quoted =
                version_literal.find_first_not_of("0123456789") != std::string::npos;
            const std::string document =
                "{\"user_languages_version\": " +
                (quoted ? "\"" + version_literal + "\"" : version_literal) +
                ", \"user_languages\": [{\"native_name\": \"Twi\"}]}";
            AppConfig preserved_version;
            TEST_CHECK(preserved_version.FromJsonString(document) &&
                           preserved_version.user_languages.empty(),
                       "PR9-09: an unparseable/newer version never guesses at the block");
            const std::string rewritten = preserved_version.ToJsonString();
            const std::string expected_version =
                "\"user_languages_version\": " +
                (quoted ? "\"" + version_literal + "\"" : version_literal);
            TEST_CHECK(rewritten.find(expected_version) != std::string::npos &&
                           rewritten.find("\"native_name\": \"Twi\"") != std::string::npos,
                       "PR9-09: ... the block and the version text round-trip verbatim");
        }
    }

    // ---- 261116_0003 P9a fix2 round (fix-list-round10.md): PR10-01/02/03/04/
    // 06/08/09/10 here; PR10-05 sits with the other PR9-07 pins above and
    // PR10-07 is P9b's (no production probe-record writer exists yet) ---------
    {
        // PR10-01: the preserved NON-NUMERIC version text is emitted as a
        // QUOTED, escaped JSON string. Re-emitting the raw text unquoted let a
        // crafted version value fabricate sibling config keys on the next save:
        // the payload below closes its own quote, re-declares two keys that the
        // writer emits EARLIER (last-wins would hand them the attacker's value)
        // and closes with an empty string, so the pre-fix rewrite was a VALID
        // document that pointed model_path at an arbitrary file.
        AppConfig quoted_version_carrier;
        const std::string fabrication_document =
            "{\"user_languages_version\": \"2, \\\"model_path\\\": \\\"C:/evil.gguf\\\", "
            "\\\"ui_language\\\": \\\"zz\\\", \\\"x\\\": \\\"\\\"\", "
            "\"user_languages\": [{\"native_name\": \"Twi\"}]}";
        TEST_CHECK(quoted_version_carrier.FromJsonString(fabrication_document) &&
                       quoted_version_carrier.user_languages.empty(),
                   "PR10-01: a quoted non-numeric version preserves the block");
        const std::string fabricated_rewrite = quoted_version_carrier.ToJsonString();
        TEST_CHECK(fabricated_rewrite.find("\"user_languages_version\": \"2, \\\"model_path") !=
                       std::string::npos,
                   "PR10-01: ... the version is re-emitted quoted and escaped");
        AppConfig fabrication_reload;
        TEST_CHECK(fabrication_reload.FromJsonString(fabricated_rewrite) &&
                       fabrication_reload.model_path == "models/Hy-MT2-1.8B-Q8_0.gguf" &&
                       fabrication_reload.ui_language == "auto" &&
                       fabrication_reload.user_languages.empty(),
                   "PR10-01: ... so the next load fabricates no sibling key");

        // PR10-02: an empty or whitespace-only version must not arm the carrier
        // with the 0 sentinel (the next load reads 0 as "current" and
        // half-parses the preserved newer block as v1). Both shapes take the
        // quoted-preserved round trip instead: the save emits the quoted empty
        // string, so the block stays preserved on every later load.
        for (const char* blank_version : {"", "   "}) {
            AppConfig blank_version_carrier;
            const std::string document =
                std::string("{\"user_languages_version\": \"") + blank_version +
                "\", \"user_languages\": [{\"native_name\": \"Twi\"}]}";
            TEST_CHECK(blank_version_carrier.FromJsonString(document) &&
                           blank_version_carrier.user_languages.empty(),
                       "PR10-02: an empty/whitespace-only version preserves the block");
            const std::string blank_rewrite = blank_version_carrier.ToJsonString();
            TEST_CHECK(blank_rewrite.find("\"user_languages_version\": 0") == std::string::npos &&
                           blank_rewrite.find("\"user_languages_version\": \"\"") !=
                               std::string::npos,
                       "PR10-02: ... never the 0 sentinel (quoted empty version instead)");
            AppConfig blank_version_reload;
            TEST_CHECK(blank_version_reload.FromJsonString(blank_rewrite) &&
                           blank_version_reload.user_languages.empty() &&
                           blank_version_reload.ToJsonString() == blank_rewrite,
                       "PR10-02: ... the save/load cycle is stable (still preserved)");
        }

        // PR10-03: a missing comma after the block value must not absorb the
        // next key. Pre-fix the capture re-entered string mode at the next key's
        // quote, the parse succeeded, and the next save persisted the loss of
        // ui_language.
        AppConfig missing_comma;
        TEST_CHECK(!missing_comma.FromJsonString(
                       "{\"user_languages\": [{\"native_name\": \"Twi\"}] "
                       "\"ui_language\": \"ko\"}"),
                   "PR10-03: a block value without a trailing comma fails the parse closed");
        TEST_CHECK(missing_comma.ui_language == "auto" && missing_comma.user_languages.empty(),
                   "PR10-03: ... the rejected document leaves every field at its default");

        // PR10-04: a trailing note after the v2 array is trivia, not a reason to
        // drop the future block (the pre-fix front/back byte test failed on it);
        // the note is not part of the value, so it is not re-emitted inside it.
        AppConfig commented_carrier;
        TEST_CHECK(commented_carrier.FromJsonString(
                       "{\"user_languages_version\": 2, "
                       "\"user_languages\": [{\"native_name\": \"Twi\", \"future_key\": [7]}] "
                       "// written by a newer build\n}") &&
                       commented_carrier.user_languages.empty(),
                   "PR10-04: a trailing note does not disqualify the preserved array");
        const std::string commented_rewrite = commented_carrier.ToJsonString();
        TEST_CHECK(commented_rewrite.find("\"future_key\": [7]") != std::string::npos &&
                       commented_rewrite.find("// written by a newer build") == std::string::npos,
                   "PR10-04: ... the array survives and the note stays out of the value");
        AppConfig multi_group_carrier;
        TEST_CHECK(!multi_group_carrier.FromJsonString(
                       "{\"user_languages_version\": 2, \"user_languages\": [1] [2]}"),
                   "PR10-04: a multi-group span is rejected");

        // PR10-06: the probe engine byte cap and BOTH english_name sanitize
        // sites (parse and build) are pinned; deleting either sanitize call or
        // the engine cap used to keep the gate green.
        const std::string long_engine(kMaxUserLanguageProbeEngineBytes + 16, 'e');
        AppConfig fat_engine;
        TEST_CHECK(fat_engine.FromJsonString(
                       "{\"user_languages\": [{\"native_name\": \"Twi\", \"probes\": [{"
                       "\"engine\": \"" + long_engine +
                       "\", \"status\": \"supported\"}]}]}") &&
                       fat_engine.user_languages.size() == 1 &&
                       fat_engine.user_languages[0].probes.size() == 1 &&
                       fat_engine.user_languages[0].probes[0].engine.size() ==
                           kMaxUserLanguageProbeEngineBytes,
                   "PR10-06: the probe engine id is byte-capped on load");
        AppConfig parsed_english_name;
        TEST_CHECK(parsed_english_name.FromJsonString(
                       "{\"user_languages\": [{\"native_name\": \"Twi\", "
                       "\"english_name\": \"Ex\\u202aample\"}]}") &&
                       parsed_english_name.user_languages.size() == 1 &&
                       parsed_english_name.user_languages[0].english_name == "Example",
                   "PR10-06: english_name is sanitized at the parse path");
        TEST_CHECK(MakeUserLanguage("Twi", std::string("Ex") + "\xE2\x80\xAA" + "ample")
                           .english_name == "Example",
                   "PR10-06: english_name is sanitized at the build path");

        // PR10-08: the sanitizer strip set also covers the remaining invisible
        // characters (ALM, the three zero-widths, the line/paragraph
        // separators, the BOM and the soft hyphen).
        const char* const invisible_marks[] = {
            "\xD8\x9C",      // U+061C ARABIC LETTER MARK
            "\xE2\x80\x8B",  // U+200B ZERO WIDTH SPACE
            "\xE2\x80\x8C",  // U+200C ZERO WIDTH NON-JOINER
            "\xE2\x80\x8D",  // U+200D ZERO WIDTH JOINER
            "\xE2\x80\xA8",  // U+2028 LINE SEPARATOR
            "\xE2\x80\xA9",  // U+2029 PARAGRAPH SEPARATOR
            "\xEF\xBB\xBF",  // U+FEFF ZERO WIDTH NO-BREAK SPACE
            "\xC2\xAD",      // U+00AD SOFT HYPHEN
        };
        bool invisible_marks_stripped = true;
        for (const char* mark : invisible_marks) {
            if (MakeUserLanguage(std::string("Twi") + mark, "").native_name != "Twi") {
                invisible_marks_stripped = false;
            }
        }
        TEST_CHECK(invisible_marks_stripped,
                   "PR10-08: ALM, zero-widths, separators, BOM and soft hyphen strip at the "
                   "build path");
        AppConfig stripped_load;
        TEST_CHECK(stripped_load.FromJsonString(
                       "{\"user_languages\": [{\"native_name\": "
                       "\"Tw\\u061ci\\u200b\\u200c\\u200d\\u2028\\u2029\\ufeff\\u00ad\"}]}") &&
                       stripped_load.user_languages.size() == 1 &&
                       stripped_load.user_languages[0].native_name == "Twi",
                   "PR10-08: ... and at the parse path");

        // PR10-09: an intra-entry note PARSES (SkipWhitespace is comment-aware),
        // which is what the corrected SplitTopLevelJsonObjects comment states.
        AppConfig noted_entry;
        TEST_CHECK(noted_entry.FromJsonString(
                       "{\"user_languages\": [{ /* a note */ \"native_name\": \"Twi\" "
                       "// trailing note\n}]}") &&
                       noted_entry.user_languages.size() == 1 &&
                       noted_entry.user_languages[0].native_name == "Twi",
                   "PR10-09: an intra-entry note parses instead of dropping the entry");

        // PR10-10: the v1 (current) STRING-typed block shape reads as 0 entries
        // and re-saves as a clean empty v1 block, exactly like the pre-fix
        // reader: no fabrication, no hard failure.
        AppConfig v1_string_block;
        TEST_CHECK(v1_string_block.FromJsonString(
                       "{\"user_languages_version\": 1, "
                       "\"user_languages\": \"\\\"fabricated_sibling\\\": 1\"}") &&
                       v1_string_block.user_languages.empty(),
                   "PR10-10: a v1 string-typed block loads as an empty registry");
        const std::string v1_string_rewrite = v1_string_block.ToJsonString();
        TEST_CHECK(v1_string_rewrite.find("\"user_languages\": []") != std::string::npos &&
                       v1_string_rewrite.find("\"user_languages_version\": 1") !=
                           std::string::npos &&
                       v1_string_rewrite.find("fabricated_sibling") == std::string::npos,
                   "PR10-10: ... the save writes a clean empty v1 block");
    }

    // ---- 261116_0003 P9a fix3 round (fix-list-round11.md): PR11-01/02/03 ----
    {
        // PR11-01 (case A, the verification instance's PoC): an OBJECT-typed
        // user_languages_version whose value carries a "// }" line comment.
        // Pre-fix the group scan was comment-blind at the group level: the
        // note's '}' closed the value early, the following line bound as
        // top-level sibling keys, and the next save laundered the edit
        // (cloud_fallback flipped, openai.base_url redirected).
        AppConfig version_object_poc;
        const std::string poc_document =
            "{\n  \"user_languages_version\": {\"meta\": \"pack info\" // }\n"
            "  , \"cloud_fallback_enabled\": true,\n"
            "  \"openai\": {\"base_url\": \"https://attacker.example/v1\"}\n}";
        TEST_CHECK(!version_object_poc.FromJsonString(poc_document),
                   "PR11-01: the version-key PoC fails the parse closed");
        TEST_CHECK(!version_object_poc.cloud_fallback_enabled &&
                       version_object_poc.openai.base_url.empty() &&
                       version_object_poc.ui_language == "auto",
                   "PR11-01: ... no sibling key binds, every field stays at its default");

        // PR11-01 (case B): the same comment truncation through a top-level
        // engine_host object value (the probe-proven second vector). The
        // attacker's "enabled": false is the discriminator: only the
        // truncated parse can flip it off its default.
        AppConfig engine_host_poc;
        TEST_CHECK(!engine_host_poc.FromJsonString(
                       "{\n  \"engine_host\": {\"enabled\": false // }\n"
                       "  , \"cloud_fallback_enabled\": true\n}"),
                   "PR11-01: the engine_host vector fails the parse closed too");
        TEST_CHECK(engine_host_poc.engine_host.enabled &&
                       !engine_host_poc.cloud_fallback_enabled,
                   "PR11-01: ... engine_host keeps its default and cloud_fallback stays off");

        // PR11-01 hard rule: a WELL-FORMED document with comments still parses
        // (comments are a supported reader feature by design). The engine_host
        // note below carries a real '}' inside a line comment; only the
        // comment-aware capture reaches the block's true end.
        AppConfig commented_wellformed;
        TEST_CHECK(commented_wellformed.FromJsonString(
                       "{\n  // a leading note\n"
                       "  \"ui_language\": \"ko\", // a trailing note\n"
                       "  \"user_languages_version\": 1,\n"
                       "  \"user_languages\": [ /* an inner note */ {\"native_name\": \"Twi\"} ],\n"
                       "  \"engine_host\": {\n"
                       "    \"enabled\": false // a note with a } brace\n  }\n}"),
                   "PR11-01: a well-formed document with comments still parses");
        TEST_CHECK(commented_wellformed.ui_language == "ko" &&
                       commented_wellformed.user_languages.size() == 1 &&
                       !commented_wellformed.engine_host.enabled,
                   "PR11-01: ... keys, block, and commented object values all bind");
        // 261116_0003 P9a fix4 PR12-02: the pin now covers ALL FOUR document
        // keys. The version key has no live in-memory field when it reads as
        // current, so its bind is pinned through the round trip: the save
        // keeps the visible sticky version key and the live-parsed block
        // entry (a version that failed to bind would park the block in the
        // verbatim carrier and the registry would read empty).
        const std::string commented_wellformed_rewrite = commented_wellformed.ToJsonString();
        TEST_CHECK(commented_wellformed_rewrite.find("\"user_languages_version\": 1") !=
                       std::string::npos &&
                       commented_wellformed_rewrite.find("\"native_name\": \"Twi\"") !=
                           std::string::npos,
                   "PR12-02: the fourth key (the version) binds too: current-version round trip");

        // PR11-01 compat: a benign OBJECT-typed version is still never guessed
        // at (the block stays preserved verbatim), only the attacker's
        // truncation is closed.
        AppConfig object_version;
        TEST_CHECK(object_version.FromJsonString(
                       "{\"user_languages_version\": {\"meta\": \"pack info\"}, "
                       "\"user_languages\": [{\"native_name\": \"Twi\"}]}"),
                   "PR11-01: a well-formed object-typed version parses");
        TEST_CHECK(object_version.user_languages.empty(),
                   "PR11-01: ... and the block stays preserved (not half-read as v1)");
        TEST_CHECK(object_version.ToJsonString().find("\"native_name\": \"Twi\"") !=
                       std::string::npos,
                   "PR11-01: ... the block survives the save verbatim");

        // 261116_0003 P9a fix4 PR12-04: pin the DOCUMENTED trailing-trivia
        // capture for an object-typed version text. CaptureRawValue keeps
        // trivia between the value's own close and the separator inside the
        // raw text (its "value start through the enclosing separator"
        // contract), so the note below lands in the preserved version text
        // and is re-emitted inside the quoted version string. This round
        // trip is stable by design; trimming the trivia would change the
        // capture contract.
        AppConfig trailing_note_version;
        TEST_CHECK(trailing_note_version.FromJsonString(
                       "{\"user_languages_version\": {\"meta\": \"pack info\"} // trailing note\n"
                       ", \"user_languages\": [{\"native_name\": \"Twi\"}]}") &&
                       trailing_note_version.user_languages.empty(),
                   "PR12-04: an object version with trailing trivia parses as a preserved carrier");
        const std::string trailing_note_rewrite = trailing_note_version.ToJsonString();
        TEST_CHECK(trailing_note_rewrite.find("// trailing note") != std::string::npos &&
                       trailing_note_rewrite.find("pack info") != std::string::npos,
                   "PR12-04: ... the captured trivia survives into the quoted version text");
        AppConfig trailing_note_second;
        TEST_CHECK(trailing_note_second.FromJsonString(trailing_note_rewrite) &&
                       trailing_note_second.ToJsonString() == trailing_note_rewrite,
                   "PR12-04: ... the round trip is stable (second save byte-identical)");

        // PR11-02: a mismatched closer is rejected, not carried verbatim and
        // round-tripped as strict-JSON-invalid output. Both shapes travel the
        // user_languages carrier path; the engine_host shape covers the
        // shared top-level scanner.
        for (const char* bad_block : {"[1}", "[{\"a\":1]"}) {
            AppConfig mismatched;
            TEST_CHECK(!mismatched.FromJsonString(
                           std::string("{\"user_languages_version\": 2, "
                                       "\"user_languages\": ") +
                           bad_block + "}, \"ui_language\": \"ko\"}"),
                       "PR11-02: a mismatched closer fails the parse closed");
        }
        AppConfig mismatched_engine_host;
        TEST_CHECK(!mismatched_engine_host.FromJsonString(
                       "{\"engine_host\": [{\"a\":1], \"ui_language\": \"ko\"}"),
                   "PR11-02: the same mismatch through a top-level object key fails closed");

        // PR11-03 (verified against the code before fixing: the pre-fix loop
        // tolerated ANY non-comma tail after a value, so the next quoted key
        // bound as a sibling; the pins below failed on the unfixed parser):
        // a key with no separator after a generic value must fail the parse.
        AppConfig no_separator;
        TEST_CHECK(!no_separator.FromJsonString(
                       "{\"ui_language\": \"ko\" \"cloud_fallback_enabled\": true}"),
                   "PR11-03: a missing comma after a string value fails the parse closed");
        TEST_CHECK(no_separator.ui_language == "auto" && !no_separator.cloud_fallback_enabled,
                   "PR11-03: ... neither the value nor the fabricated sibling binds");
        AppConfig no_separator_bool;
        TEST_CHECK(!no_separator_bool.FromJsonString(
                       "{\"cloud_fallback_enabled\": true \"sound_enabled\": false}"),
                   "PR11-03: a missing comma after a bool value fails the parse closed");
        // 261116_0003 P9a fix4 PR12-03: pin the fail-closed binding for the
        // no-separator primitive shapes: neither the value nor the would-be
        // sibling key binds, every field stays at its default (the string
        // sibling of this pair is pinned right above).
        TEST_CHECK(!no_separator_bool.cloud_fallback_enabled &&
                       no_separator_bool.sound_enabled,
                   "PR12-03: ... the bool values fail closed at their defaults");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] 261116_0003 P9a user languages + capability probe passed."
                  << std::endl;
    } else {
        std::cout << "[FAIL] 261116_0003 P9a user languages + capability probe: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}
