// ---------------------------------------------------------------------------
// WS1-gates (session 261108_0002, v0.10.6 release) - DECODE-SIDE root causes.
//
// User report (verbatim, Korean):
//   "단일 영단어(cyber, company, astrology, clue, nature, medicine 등) 번역 시
//    ... assistant: 역할 헤더 노출 및 Emebala의 출처 / 이메발라 / 이메일 문장
//    환각 ... 근본 원인 제거"
//   "1차 번역 후 같은 입력창에서 여러 문장을 연달아 이어 쓰고 엔터를 쳤을 때
//    뒷부분(한국어 다중 문장)이 번역되지 않는 현상의 근본 원인 제거"
//
// The prompt/detection causes of the same report were fixed in config.cpp /
// smart_bypass.cpp by another unit (tests/ws1_singleword_tests.inc). THIS suite
// pins the decode-side causes owned by this unit, all in
// src/engine_core/translation_common.{hpp,cpp}:
//   (1) the leaked assistant/role header ("assistant: 사이버") is stripped
//       before the rung acceptance compares, source- and label-guarded;
//   (2) the meta-leakage needle table gained dialogue-role labels ("assistant:",
//       "user:", "system:", "model:"), ChatML markers ("<|im_start|>",
//       "<|im_end|>") and the brand ("emebala", "emebala_source", "이메발라"),
//       with the source guard intact;
//   (3) the tiny-source amplification floors were re-calibrated
//       (Tier 1 = max(16, 4x); a single word <= 15 cp = max(20, 4x));
//   (4) the structural one-word-to-sentence gate (its own reject reason);
//   (5) the wrong-script LEAK DOMINANCE clause (v0.10.9: 20 code points at 75%
//       retention, was 10 at 40% - the old bounds rejected legitimate
//       translations that kept proper nouns and technical terms);
//   (6) the single-word generation cap (v0.10.9: 128 tokens, was 16) at the
//       header level;
//   (7) the leading-colon / emoji-shortcode decision;
//   (8) the echo-probe mixed-script rule.
//
// NOT pinnable purely (covered by the structural source pins in
// tests/run_tests.cpp, WS1 section):
//   * the single-line source answer normalization (v0.10.9 replaced the
//     first-newline BREAK with a post-decode flatten; the pure decision
//     V109NormalizeSingleLineOutput IS pinned here, the call site is not),
//   * the ChatML CONTROL-token guard (needs a loaded GGUF vocab),
//   * the chat-template-model "never degrade to untemplated rung 2" arm
//     (in-loop, needs a decode).
// Those three are behaviour of the llama-coupled ladder; the pure decisions
// they call ARE pinned here.
//
// Self-contained: no main(), no ResolveRepoFile, only public API of
// translation_common.hpp (plus ToUtf16 from unicode_utils.hpp, both already
// included by the runner). Wired by the orchestrator as one call:
//     TestWs1Gates();
// ---------------------------------------------------------------------------

void TestWs1Gates() {
    std::cout << "[RUN] Testing WS1 decode gates (261108_0002)..." << std::endl;
    const int failures_before = g_failed_count;

    // =====================================================================
    // (1) StripLeadingRoleHeader - the reported "assistant: <answer>" leak.
    // =====================================================================
    {
        auto strip = [](const char* in, const char* src) {
            std::string text(in);
            const bool changed = StripLeadingRoleHeader(text, src);
            return std::make_pair(changed, text);
        };
        // Positives: the header and its separator go, the answer survives.
        {
            const auto r = strip("assistant: 사이버", "cyber");
            TEST_CHECK(r.first && r.second == "사이버",
                       "WS1 strip: 'assistant: 사이버' -> '사이버'");
        }
        {
            const auto r = strip("Assistant:사이버", "cyber");
            TEST_CHECK(r.first && r.second == "사이버",
                       "WS1 strip: case-insensitive prefix, no space after the colon");
        }
        {
            const auto r = strip("ASSISTANT:   사이버", "cyber");
            TEST_CHECK(r.first && r.second == "사이버",
                       "WS1 strip: all-caps prefix and every horizontal space after it");
        }
        {
            const auto r = strip("model: company", "company");
            TEST_CHECK(r.first && r.second == "company", "WS1 strip: 'model:' is a role header");
        }
        {
            const auto r = strip("ai: nature", "nature");
            TEST_CHECK(r.first && r.second == "nature", "WS1 strip: 'ai:' is a role header");
        }
        {
            const auto r = strip("bot: medicine", "medicine");
            TEST_CHECK(r.first && r.second == "medicine", "WS1 strip: 'bot:' is a role header");
        }
        {
            const auto r = strip("<|assistant|>\n사이버", "cyber");
            TEST_CHECK(r.first && r.second == "사이버",
                       "WS1 strip: the ChatML role marker plus the line break it leaves");
        }
        {
            const auto r = strip("<|assistant|>사이버", "cyber");
            TEST_CHECK(r.first && r.second == "사이버",
                       "WS1 strip: the ChatML role marker with no separator");
        }
        {
            const auto r = strip("assistant: ", "hello");
            TEST_CHECK(r.first && r.second.empty(),
                       "WS1 strip: a header with nothing behind it strips to empty");
        }
        // Negatives: never strip content.
        {
            const auto r = strip("사이버", "cyber");
            TEST_CHECK(!r.first && r.second == "사이버",
                       "WS1 strip: an answer without a header is untouched");
        }
        {
            const auto r = strip("assistant: 안녕", "assistant: 안녕");
            TEST_CHECK(!r.first && r.second == "assistant: 안녕",
                       "WS1 strip: source guard - a prefix the SOURCE begins with is content");
        }
        {
            const auto r = strip("Assistant: Hello", "  ASSISTANT: hello");
            TEST_CHECK(!r.first && r.second == "Assistant: Hello",
                       "WS1 strip: source guard is case-insensitive and whitespace-tolerant");
        }
        {
            const auto r = strip("Model: GPT-4", "모델: GPT-4");
            TEST_CHECK(!r.first && r.second == "Model: GPT-4",
                       "WS1 strip: R2-7 label guard - a TRANSLATED label is content");
        }
        {
            const auto r = strip("user: hello", "hello");
            TEST_CHECK(!r.first && r.second == "user: hello",
                       "WS1 strip: 'user:' / 'system:' are rejected as leakage, not salvaged");
        }
        {
            const auto r = strip("system: hello", "hello");
            TEST_CHECK(!r.first && r.second == "system: hello",
                       "WS1 strip: 'system:' is not a strippable role");
        }
        {
            const auto r = strip("assistant", "hello");
            TEST_CHECK(!r.first && r.second == "assistant",
                       "WS1 strip: a bare word without the colon is untouched");
        }
        {
            const auto r = strip("assistant : x", "hello");
            TEST_CHECK(!r.first && r.second == "assistant : x",
                       "WS1 strip: a space before the colon is not a role header");
        }
        {
            const auto r = strip("", "hello");
            TEST_CHECK(!r.first && r.second.empty(), "WS1 strip: empty text is a no-op");
        }
        // End to end: the stripped answer passes the gate the unstripped one
        // fails (that is the whole point of stripping before the compares).
        {
            std::string salvaged("assistant: 사이버");
            TEST_CHECK(StripLeadingRoleHeader(salvaged, "cyber") &&
                           ClassifyTranslationOutput(ToUtf16(salvaged), L"cyber", "KO") ==
                               OutputRejectReason::None,
                       "WS1 strip: 'assistant: 사이버' is a good Korean answer once salvaged");
            TEST_CHECK(ClassifyTranslationOutput(L"assistant: 사이버", L"cyber", "KO") ==
                           OutputRejectReason::MetaLeakage,
                       "WS1 strip: ... and the UNSTRIPPED form is meta leakage");
        }
    }

    // =====================================================================
    // (2) The new meta-leakage needles (role labels, ChatML, brand).
    // =====================================================================
    {
        TEST_CHECK(HasMetaLeakage(L"assistant: 사이버", L"cyber"),
                   "WS1 needles: 'assistant:' label fires");
        TEST_CHECK(HasMetaLeakage(L"사이버\n\nassistant: 안녕하세요", L"cyber"),
                   "WS1 needles: 'assistant:' fires at any line start, not only line 1");
        TEST_CHECK(HasMetaLeakage(L"user: hello", L"hello"),
                   "WS1 needles: 'user:' label fires");
        TEST_CHECK(HasMetaLeakage(L"system: hello", L"hi"),
                   "WS1 needles: 'system:' label fires");
        TEST_CHECK(HasMetaLeakage(L"model: hello", L"hi"),
                   "WS1 needles: 'model:' label fires");
        TEST_CHECK(HasMetaLeakage(L"사이버 <|im_end|>", L"cyber"),
                   "WS1 needles: '<|im_end|>' fires ANYWHERE (a turn marker is never content)");
        TEST_CHECK(HasMetaLeakage(L"<|im_start|>사이버", L"cyber"),
                   "WS1 needles: '<|im_start|>' fires at the head of the output");
        TEST_CHECK(HasMetaLeakage(L"이 문서는 Emebala의 출처를 기반으로 작성되었습니다.", L"cyber"),
                   "WS1 needles: the verbatim 'Emebala의 출처' hallucination is leakage");
        TEST_CHECK(HasMetaLeakage(L"이메발라의 별자리 운세는 어떨까요?", L"astrology"),
                   "WS1 needles: the verbatim '이메발라' hallucination is leakage");
        TEST_CHECK(HasMetaLeakage(L"emebala_source", L"cyber"),
                   "WS1 needles: 'emebala_source' is leakage even though 'emebala' subsumes it");
        // Source guard (repo idiom) survives for every new needle.
        TEST_CHECK(!HasMetaLeakage(L"Emebala is a company.", L"Emebala is a company."),
                   "WS1 needles: source guard - 'emebala' the SOURCE carries is content");
        TEST_CHECK(!HasMetaLeakage(L"이메발라", L"이메발라"),
                   "WS1 needles: source guard - a source-carried brand survives");
        TEST_CHECK(!HasMetaLeakage(L"assistant: 안녕", L"assistant: 안녕"),
                   "WS1 needles: source guard - a source-carried role label survives");
        // Controls: real answers do not fire.
        TEST_CHECK(!HasMetaLeakage(L"사이버", L"cyber"),
                   "WS1 needles: a clean one-word answer carries no leakage");
        TEST_CHECK(!HasMetaLeakage(L"그는 assistant: 라고 적었다.", L"hello"),
                   "WS1 needles: the role label is LINE-START only (mid-sentence prose passes)");
        TEST_CHECK(!HasMetaLeakage(L"Model: GPT-4", L"모델: GPT-4"),
                   "WS1 needles: R2-7 translated-label exemption still covers 'model:'");
    }

    // =====================================================================
    // (10) The leading-colon / emoji-shortcode decision.
    // =====================================================================
    {
        TEST_CHECK(ShouldStripLeadingColon(":사이버", "cyber"),
                   "WS1 colon: a stray leading colon on an unquoted source is dropped");
        TEST_CHECK(ShouldStripLeadingColon(": 사이버", "cyber"),
                   "WS1 colon: the colon plus its following spaces are dropped");
        TEST_CHECK(!ShouldStripLeadingColon(":laughing:", "laughing"),
                   "WS1 colon: an emoji :shortcode: is never a ChatML artefact");
        TEST_CHECK(!ShouldStripLeadingColon(":smile: 하하", "하하"),
                   "WS1 colon: a shortcode inside a real answer keeps its leading colon");
        TEST_CHECK(!ShouldStripLeadingColon(":사이버", " :원문"),
                   "WS1 colon: the source test trims leading whitespace first");
        TEST_CHECK(!ShouldStripLeadingColon(":사이버", ":원문"),
                   "WS1 colon: a source that begins with ':' keeps its answer's colon");
        TEST_CHECK(!ShouldStripLeadingColon("사이버", "cyber"),
                   "WS1 colon: no leading colon, no strip");
        TEST_CHECK(!ShouldStripLeadingColon("", "cyber"),
                   "WS1 colon: empty output is a no-op");
        TEST_CHECK(!ShouldStripLeadingColon(":+:-_x:", "cyber"),
                   "WS1 colon: the shortcode shape accepts the full allowed character set");
    }

    // =====================================================================
    // (9) The echo-probe rule: an echoed prefix aborts only when it proves the
    //     model is still copying untranslated text.
    // ADJUDICATED 261108_0002 follow-up: the abort is now TERMINAL on a
    // chat-template model (engine_failed) instead of being accepted/shipped as
    // a "translation". This suite pins the pure decision that raises the abort;
    // the shipping rule (never an accept, never a rung-2 degrade) is pinned in
    // tests/run_tests.cpp's WS1 structural block, because the runner's file
    // probe cannot be reproduced from here.
    // =====================================================================
    {
        const std::uint32_t en = TargetScriptClasses("EN");
        const std::uint32_t ko = TargetScriptClasses("KO");
        // The required live shape: a mixed-script source translated into
        // English starts with the part that is already English.
        TEST_CHECK(!IsUntranslatedEchoPrefix(L"Ah... I fell asleep... ",
                                             L"Ah... I fell asleep... :laughing: 사실 저는...",
                                             en),
                   "WS1 echo: a Latin prefix of a mixed Latin+Hangul source is not an echo "
                   "(mixed-script continuation)");
        TEST_CHECK(IsUntranslatedEchoPrefix(L"사실 저는",
                                            L"Ah... I fell asleep... 사실 저는...", en),
                   "WS1 echo: the same probe aborts once the prefix carries the source script");
        TEST_CHECK(IsUntranslatedEchoPrefix(L"사이버", L"사이버", en),
                   "WS1 echo: a script-homogeneous source aborts on ANY exact prefix");
        TEST_CHECK(!IsUntranslatedEchoPrefix(L"사이버 게임", L"사이버 게임", ko),
                   "WS1 echo: a source already in the target script has nothing untranslated");
        TEST_CHECK(!IsUntranslatedEchoPrefix(L"cyber", L"cyber space", en),
                   "WS1 echo: an English source into an English target is NOT treated as an echo "
                   "(there is nothing to translate away; identity is handled by the caller)");
        TEST_CHECK(IsUntranslatedEchoPrefix(L"사이버", L"사이버", 0),
                   "WS1 echo: an unknown/AUTO target treats every script as untranslated");
        // Both abort shapes exist and are the ONLY reason a partial prefix can
        // no longer be shipped: the homogeneous source, and a prefix that still
        // carries untranslated source-script text.
        TEST_CHECK(IsUntranslatedEchoPrefix(L"사이버", L"사이버", en) &&
                       IsUntranslatedEchoPrefix(L"사실 저는", L"Ah... 사실 저는 게임을", en),
                   "WS1 echo (adjudication): a homogeneous source AND a prefix carrying untranslated "
                   "source text both abort - the abort now answers engine_failed instead of "
                   "shipping the partial prefix");
        TEST_CHECK(!IsUntranslatedEchoPrefix(L"Ah... I fell asleep... ",
                                            L"Ah... I fell asleep... :laughing: 사실 저는...", en),
                   "WS1 echo (adjudication): the mixed-script continuation stays spared, so the "
                   "new terminal rule does not turn it into a failure");
    }

    // =====================================================================
    // (5) The single-word generation cap (128 tokens vs the 256-token RC-4 cap).
    //     v0.10.9 (261121_0001 U-D, report 2B-4): 16/64 -> 128/256. The old
    //     caps truncated LEGITIMATE answers on a slow 4B user GGUF; the
    //     structural/amplification gates below are what reject a hallucinated
    //     essay, so the caps only need to bound the decode, not to be the gate.
    // =====================================================================
    {
        static_assert(kSingleWordSourceGenCapTokens == 128,
                      "WS1 / v0.10.9: the single-word generation cap is 128 tokens");
        static_assert(kShortSourceGenCapTokens == 256,
                      "WS1 / v0.10.9: the multi-word short-source cap is 256 tokens");
        TEST_CHECK(ShortSourceGenTokenCap(5, false, false) == 128,
                   "WS1 cap: 'cyber' (5 bytes, no whitespace) -> 128 tokens");
        TEST_CHECK(ShortSourceGenTokenCap(1, false, false) == 128,
                   "WS1 cap: a 1-byte word -> 128 tokens");
        TEST_CHECK(ShortSourceGenTokenCap(16, false, false) == 128,
                   "WS1 cap: the 16-byte boundary is still a single word");
        TEST_CHECK(ShortSourceGenTokenCap(5, false, true) == 256,
                   "WS1 cap: the same length WITH whitespace keeps the 256-token cap");
        TEST_CHECK(ShortSourceGenTokenCap(17, false, false) == kLlamaGenReserve,
                   "WS1 cap: over 16 bytes is not a short source (no extra cap)");
        TEST_CHECK(ShortSourceGenTokenCap(5, true, false) == kLlamaGenReserve,
                   "WS1 cap: a single-line test is required (a line break means no cap)");
        TEST_CHECK(ShortSourceGenTokenCap(20, false, true) == kLlamaGenReserve,
                   "WS1 cap: a long short-line source keeps the historical budget");
        // The caps must stay far below the reserve, or they would be the
        // binding constraint on a legitimate long answer instead of a bound.
        static_assert(kShortSourceGenCapTokens < kLlamaGenReserve &&
                          kSingleWordSourceGenCapTokens < kShortSourceGenCapTokens,
                      "WS1 / v0.10.9: the caps stay ordered and under the generation reserve");
    }

    // =====================================================================
    // (7) Amplification floors: the reported hallucinations are rejected and
    //     real one-word translations pass.
    // =====================================================================
    {
        // Rejected (the user-report strings, verbatim shapes).
        TEST_CHECK(IsAmplifiedTinySourceOutput(
                       L"이 문서는 Emebala의 출처를 기반으로 작성되었습니다.", L"cyber"),
                   "WS1 amp: 'cyber' -> the 'Emebala의 출처' sentence is an amplification");
        TEST_CHECK(IsAmplifiedTinySourceOutput(
                       L"Astrology is the study of the positions of the stars and planets and how "
                       L"they are believed to influence human affairs.",
                       L"astrology"),
                   "WS1 amp: 'astrology' -> a long 'explanation' is an amplification");
        // The user report's exact 19-code-point answer is UNDER the single-word
        // floor (20), which is why the report needed the brand needle and the
        // structural gate as well - both are pinned below / in the needle suite.
        TEST_CHECK(!IsAmplifiedTinySourceOutput(L"이메발라의 별자리 운세는 어떨까요?", L"astrology") &&
                       ClassifyTranslationOutput(L"이메발라의 별자리 운세는 어떨까요?", L"astrology",
                                                 "KO") == OutputRejectReason::MetaLeakage,
                   "WS1 amp: the 19-code-point '이메발라' question is caught by the BRAND needle "
                   "(the floor cannot reach it)");
        TEST_CHECK(IsAmplifiedTinySourceOutput(
                       L"Clue is a hint that helps you solve a mystery.", L"clue"),
                   "WS1 amp: 'clue' -> a 46-code-point explanation is an amplification");
        TEST_CHECK(IsAmplifiedTinySourceOutput(
                       L"The company is a business organization that employs people.", L"company"),
                   "WS1 amp: 'company' -> an explanation sentence is an amplification");
        TEST_CHECK(IsAmplifiedTinySourceOutput(L"The nature of the problem is complex.", L"nature"),
                   "WS1 amp: 'nature' -> a sentence is an amplification");
        TEST_CHECK(IsAmplifiedTinySourceOutput(L"Medicine is the science of healing.", L"medicine"),
                   "WS1 amp: 'medicine' -> a definition sentence is an amplification");
        // Accepted: real one-word translations (all five reported words).
        TEST_CHECK(!IsAmplifiedTinySourceOutput(L"사이버", L"cyber"),
                   "WS1 amp: 'cyber' -> '사이버' passes");
        TEST_CHECK(!IsAmplifiedTinySourceOutput(L"회사", L"company"),
                   "WS1 amp: 'company' -> '회사' passes");
        TEST_CHECK(!IsAmplifiedTinySourceOutput(L"점성학", L"astrology"),
                   "WS1 amp: 'astrology' -> '점성학' passes");
        TEST_CHECK(!IsAmplifiedTinySourceOutput(L"단서", L"clue"),
                   "WS1 amp: 'clue' -> '단서' passes (Tier 1)");
        TEST_CHECK(!IsAmplifiedTinySourceOutput(L"자연", L"nature"),
                   "WS1 amp: 'nature' -> '자연' passes");
        TEST_CHECK(!IsAmplifiedTinySourceOutput(L"의학", L"medicine"),
                   "WS1 amp: 'medicine' -> '의학' passes");
        TEST_CHECK(!IsAmplifiedTinySourceOutput(L"Unternehmen", L"company"),
                   "WS1 amp: a longer legit word answer (11 cp) passes");
        TEST_CHECK(!IsAmplifiedTinySourceOutput(L"Cyberspace", L"cyber"),
                   "WS1 amp: a 10-code-point legit answer passes");
        // Tier boundaries.
        TEST_CHECK(IsAmplifiedTinySourceOutput(std::wstring(16, L'a'), L"음") &&
                       !IsAmplifiedTinySourceOutput(std::wstring(15, L'a'), L"음"),
                   "WS1 amp: Tier 1 floor for a 1-code-point source is exactly 16");
        TEST_CHECK(IsAmplifiedTinySourceOutput(std::wstring(16, L'a'), L"OK!!") &&
                       !IsAmplifiedTinySourceOutput(std::wstring(15, L'a'), L"OK!!"),
                   "WS1 amp: Tier 1 floor for a 4-code-point source is exactly 16 (4x)");
        TEST_CHECK(IsAmplifiedTinySourceOutput(std::wstring(60, L'a'), L"internationaliz") &&
                       !IsAmplifiedTinySourceOutput(std::wstring(59, L'a'), L"internationaliz"),
                   "WS1 amp: a 15-code-point single word needs 60 (4x), not 20");
        TEST_CHECK(IsAmplifiedTinySourceOutput(std::wstring(96, L'a'), L"internationaliza") &&
                       !IsAmplifiedTinySourceOutput(std::wstring(95, L'a'), L"internationaliza"),
                   "WS1 amp: a 16-code-point single token keeps the RC-2 64-at-6x envelope");
        TEST_CHECK(IsAmplifiedTinySourceOutput(std::wstring(66, L'a'), L"hello there") &&
                       !IsAmplifiedTinySourceOutput(std::wstring(65, L'a'), L"hello there"),
                   "WS1 amp: a multi-word short phrase keeps the RC-2 64-at-6x envelope");
        TEST_CHECK(!IsAmplifiedTinySourceOutput(std::wstring(30, L'a'), L"this is a normal phrase"),
                   "WS1 amp: a source over 15 code points is never a tiny source");
        // The laughter carve-out is intact.
        TEST_CHECK(!IsAmplifiedTinySourceOutput(std::wstring(100, L'a'), L"ㅋㅋㅋㅋ") &&
                       !IsAmplifiedTinySourceOutput(std::wstring(30, L'a'), L"하하하"),
                   "WS1 amp: the pure-repetition source carve-out is untouched (under its ceiling)");
        TEST_CHECK(IsAmplifiedTinySourceOutput(std::wstring(120, L'a'), L"ㅋㅋㅋㅋ"),
                   "WS1 amp: the 120-code-point laughter ceiling is still the bound");
        // Classification: the new floors name the right reason.
        TEST_CHECK(ClassifyTranslationOutput(
                       L"이 문서는 Emebala의 출처를 기반으로 작성되었습니다.", L"cyber", "KO") ==
                       OutputRejectReason::MetaLeakage,
                   "WS1 amp: the brand sentence classifies as MetaLeakage (the brand needle is "
                   "more specific than the floor)");
        TEST_CHECK(ClassifyTranslationOutput(
                       L"The company is a business organization that employs people.", L"company",
                       "KO") == OutputRejectReason::WrongScript,
                   "WS1 amp: an English explanation for a Korean target classifies as WrongScript "
                   "(the base no-target-script rule owns it)");
        TEST_CHECK(ClassifyTranslationOutput(
                       L"회사는 사람을 고용하고 이익을 추구하는 사업 조직입니다.", L"company", "KO") ==
                       OutputRejectReason::ScriptLeak,
                   "WS1 amp: a target-script explanation classifies as ScriptLeak");
        TEST_CHECK(ClassifyTranslationOutput(L"점성학", L"astrology", "KO") ==
                       OutputRejectReason::None,
                   "WS1 amp: the real one-word translation classifies as accepted");
        TEST_CHECK(ClassifyTranslationOutput(L"회사", L"company", "KO") ==
                       OutputRejectReason::None,
                   "WS1 amp: '회사' classifies as accepted");
    }

    // =====================================================================
    // (8) The structural one-word-to-sentence gate.
    // =====================================================================
    {
        TEST_CHECK(IsSingleWordSource(L"cyber") && IsSingleWordSource(L"사이버") &&
                       IsSingleWordSource(L"  astrology  "),
                   "WS1 word: a single script-bearing token is a single-word source");
        TEST_CHECK(IsSingleWordSource(L"점점") && IsSingleWordSource(L"바이바이"),
                   "WS1 word: RC-2 AA/ABAB words are still words (not laughter)");
        TEST_CHECK(!IsSingleWordSource(L"hello world") &&
                       !IsSingleWordSource(L"사이버 게임"),
                   "WS1 word: whitespace makes it a phrase");
        TEST_CHECK(!IsSingleWordSource(L"12345") && !IsSingleWordSource(L"!!!") &&
                       !IsSingleWordSource(L"??"),
                   "WS1 word: digits/punctuation alone are not a word");
        TEST_CHECK(!IsSingleWordSource(L"ㅋㅋㅋㅋ") && !IsSingleWordSource(L"no no no"),
                   "WS1 word: a pure repetition source keeps the laughter carve-out");
        TEST_CHECK(!IsSingleWordSource(L"") && !IsSingleWordSource(L"   "),
                   "WS1 word: empty/blank sources are not words");
        TEST_CHECK(!IsSingleWordSource(std::wstring(21, L'a')),
                   "WS1 word: over 20 code points is not the single-word shape");
        TEST_CHECK(!IsSingleWordSource(L"cyber\nspace"),
                   "WS1 word: a multi-line source is never a single word");

        // v0.10.9 (261121_0001 U-D, report 2B-4): SPACELESS-SCRIPT EXEMPTION.
        // A run in a script that does not separate words with spaces is a
        // SENTENCE past the 4-code-point word ceiling, so the structural gate
        // no longer rejects its correct multi-word translation. The reported
        // source is pinned verbatim.
        TEST_CHECK(!IsSingleWordSource(L"집에빨리가서쉬고싶다"),
                   "v0.10.9 U-D: the 11-code-point Hangul sentence is NOT a single word "
                   "(the reported false hallucination-reject)");
        TEST_CHECK(!IsSingleWordSource(L"안녕하세요"),
                   "v0.10.9 U-D: a 5-code-point Hangul run is already sentence-shaped");
        TEST_CHECK(IsSingleWordSource(L"사이버") && IsSingleWordSource(L"점점") &&
                       IsSingleWordSource(L"바이바이") && IsSingleWordSource(L"妈妈"),
                   "v0.10.9 U-D: 2-4 code point space-less words stay words");
        TEST_CHECK(IsSingleWordSource(L"cyber") && IsSingleWordSource(L"internationaliz"),
                   "v0.10.9 U-D: a Latin word of any length (<= 20 cp) stays a word "
                   "(space-separated scripts are untouched)");
        TEST_CHECK(!IsSingleWordSource(L"안녕하세요 세계"),
                   "v0.10.9 U-D: whitespace still makes it a phrase");
        {
            // The pure shape decision behind the exemption.
            const auto hangul = ToMask(ScriptClass::Hangul);
            const auto latin = ToMask(ScriptClass::Latin);
            static_assert(kSingleWordSourceNoSpaceScriptMaxCodePoints == 4,
                          "v0.10.9 U-D: the space-less word ceiling is 4 code points");
            TEST_CHECK(V109IsSpacelessScriptSentence(5, hangul) &&
                           V109IsSpacelessScriptSentence(11, hangul),
                       "v0.10.9 U-D: >4 code points of Hangul is sentence-shaped");
            TEST_CHECK(!V109IsSpacelessScriptSentence(4, hangul),
                       "v0.10.9 U-D: exactly 4 code points is still a word");
            TEST_CHECK(V109IsSpacelessScriptSentence(5, ToMask(ScriptClass::Han)) &&
                           V109IsSpacelessScriptSentence(5, ToMask(ScriptClass::Kana)) &&
                           V109IsSpacelessScriptSentence(5, ToMask(ScriptClass::Thai)),
                       "v0.10.9 U-D: Han / Kana / Thai are space-less scripts too");
            TEST_CHECK(!V109IsSpacelessScriptSentence(5, latin) &&
                           !V109IsSpacelessScriptSentence(5, hangul | latin),
                       "v0.10.9 U-D: space-separated and mixed-script sources keep the old shape");
            TEST_CHECK(!V109IsSpacelessScriptSentence(5, 0),
                       "v0.10.9 U-D: a script-less run never becomes a sentence here");
        }

        // (a) numbered / bullet openers.
        TEST_CHECK(IsSingleWordToSentenceHallucination(L"1. 당신은 어떤 방식으로 번역하나요",
                                                       L"clue"),
                   "WS1 struct: a numbered quiz answer to a word is rejected");
        TEST_CHECK(IsSingleWordToSentenceHallucination(L"2) 회사란 무엇인가", L"company"),
                   "WS1 struct: a '2)' numbered answer is rejected");
        TEST_CHECK(IsSingleWordToSentenceHallucination(L"- 사이버", L"cyber"),
                   "WS1 struct: a bullet answer is rejected");
        // (b) questions.
        TEST_CHECK(IsSingleWordToSentenceHallucination(L"점성학이란 무엇인가요?", L"astrology"),
                   "WS1 struct: a question-marked answer to a word is rejected");
        TEST_CHECK(IsSingleWordToSentenceHallucination(L"뭐죠？", L"clue"),
                   "WS1 struct: the FULLWIDTH question mark counts too");
        // (c) closed sentences of >= 3 words.
        TEST_CHECK(IsSingleWordToSentenceHallucination(L"점성학은 별을 연구합니다.", L"astrology"),
                   "WS1 struct: a closed 3-word sentence about a word is rejected");
        TEST_CHECK(IsSingleWordToSentenceHallucination(L"Astrology studies the stars.", L"astrology"),
                   "WS1 struct: the English 'explanation' shape is rejected");
        // Controls: real one-word answers and phrase sources never fire.
        TEST_CHECK(!IsSingleWordToSentenceHallucination(L"점성학", L"astrology"),
                   "WS1 struct: the real one-word translation passes");
        TEST_CHECK(!IsSingleWordToSentenceHallucination(L"사이버 공간", L"cyber"),
                   "WS1 struct: a two-word compound without a full stop passes");
        TEST_CHECK(!IsSingleWordToSentenceHallucination(L"Hello.", L"hello"),
                   "WS1 struct: a one-word answer with a full stop passes (the >= 3 word clause)");
        TEST_CHECK(!IsSingleWordToSentenceHallucination(L"점성학을 연구합니다", L"astrology"),
                   "WS1 struct: a closed sentence is required for clause (c)");
        TEST_CHECK(!IsSingleWordToSentenceHallucination(L"What is your name?", L"hello there"),
                   "WS1 struct: a PHRASE source may legitimately become a question");
        TEST_CHECK(!IsSingleWordToSentenceHallucination(L"Hahaha! That is so funny!", L"ㅋㅋㅋㅋ"),
                   "WS1 struct: a laughter expansion is exempt (carve-out)");
        // Classification: the new reason value, ordered after the calibrated
        // rules.
        TEST_CHECK(ClassifyTranslationOutput(L"점성학이란 무엇인가요?", L"astrology", "KO") ==
                       OutputRejectReason::SingleWordSentence,
                   "WS1 struct: classifies as SingleWordSentence");
        TEST_CHECK(ClassifyTranslationOutput(L"Astrology studies the stars.", L"astrology", "EN") ==
                       OutputRejectReason::SingleWordSentence,
                   "WS1 struct: the English shape classifies as SingleWordSentence");
        TEST_CHECK(ClassifyTranslationOutput(L"1. 당신은 어떤 방식으로 번역하나요", L"clue", "KO") ==
                       OutputRejectReason::ScriptLeak,
                   "WS1 struct: when the Tier-1 floor already rejects the numbered answer "
                   "(20 cp >= 16), the calibrated reason keeps the name");
        TEST_CHECK(ClassifyTranslationOutput(L"1. 국제화란 무엇인가요", L"internationalization",
                                            "KO") == OutputRejectReason::SingleWordSentence,
                   "WS1 struct: the numbered answer the floors cannot reach is claimed by the "
                   "structural gate");
        TEST_CHECK(std::string(OutputRejectReasonName(OutputRejectReason::SingleWordSentence)) ==
                       "single-word-sentence",
                   "WS1 struct: the reason has a stable shape-only name");
        TEST_CHECK(OutputRejectReasonName(OutputRejectReason::None) == std::string("none") &&
                       OutputRejectReasonName(OutputRejectReason::ScriptLeak) ==
                           std::string("script-leak"),
                   "WS1 struct: the pre-existing reason names are unchanged");
    }

    // =====================================================================
    // (11) Wrong-script LEAK DOMINANCE - the exact live repro (Hangul source
    //      characters plus an embedded untranslated English word).
    //      v0.10.9 (261121_0001 U-D, report 2B-2): the bounds moved to
    //      20 source code points / 75% retention. The old 10 / 40% pair
    //      rejected a CORRECT Korean translation of an English sentence that
    //      legitimately kept its proper nouns (Hydrogen, Sonnet) in Latin, so
    //      the rows below pin the new, narrower leak envelope: only an answer
    //      that keeps three quarters of the source-script text is a leak.
    // =====================================================================
    {
        const std::uint32_t en = TargetScriptClasses("EN");
        const std::wstring ko_source(50, L'가');
        // 38 of 50 source-script code points retained = 76% -> a leak.
        const std::wstring leaked = std::wstring(38, L'가') + L" astrology";
        TEST_CHECK(IsWrongScriptOutput(leaked, ko_source, en),
                   "WS1 script: 38 Hangul + 'astrology' for an English target is wrong-script "
                   "(76% source-script retention >= the 75% bound)");
        TEST_CHECK(ClassifyTranslationOutput(leaked, ko_source, "EN") ==
                       OutputRejectReason::WrongScript,
                   "WS1 script: ... and classifies as WrongScript");
        TEST_CHECK(!IsWrongScriptOutput(std::wstring(5, L'가') + L" astrology", ko_source, en),
                   "WS1 script: retaining only 5 of 50 source-script characters stays under the "
                   "75% bound");
        TEST_CHECK(!IsWrongScriptOutput(std::wstring(30, L'가') + L" astrology", ko_source, en),
                   "v0.10.9 U-D: 60% retention is a translation that kept real terminology, "
                   "not a leak (the old 40% bound rejected exactly this)");
        TEST_CHECK(!IsWrongScriptOutput(std::wstring(37, L'가') + L" astrology", ko_source, en),
                   "WS1 script: 74% retention is still under the bound");
        TEST_CHECK(IsWrongScriptOutput(std::wstring(38, L'가') + L" astrology", ko_source, en),
                   "WS1 script: exactly 76% retention trips the bound");
        TEST_CHECK(!IsWrongScriptOutput(std::wstring(30, L'가') + L" astrology", std::wstring(19, L'가'),
                                        en),
                   "v0.10.9 U-D: a source with fewer than 20 foreign code points never uses the "
                   "dominance clause");
        TEST_CHECK(IsWrongScriptOutput(std::wstring(30, L'가') + L" astrology", std::wstring(20, L'가'),
                                       en),
                   "v0.10.9 U-D: at exactly 20 foreign source code points the clause applies");
        // The false positive the report measured: an English source answered in
        // Korean that keeps its proper nouns and technical terms in Latin. The
        // source is 60 Latin code points, the answer keeps 30 of them (50%).
        const std::wstring en_source =
            L"Hydrogen is good enough actually damn comparable to sonnet from my use";
        const std::wstring ko_answer =
            std::wstring(20, L'가') + L" Hydrogen Sonnet GPU KV cache token";
        TEST_CHECK(!IsWrongScriptOutput(ko_answer, en_source,
                                        TargetScriptClasses("KO")),
                   "v0.10.9 U-D: a Korean answer that keeps the source's proper nouns and "
                   "technical terms is NOT a wrong-script leak (report 2B-2 false positive)");
        TEST_CHECK(!IsWrongScriptOutput(L"The document was written in Korean.",
                                        ko_source, en),
                   "WS1 script: a faithful English translation is untouched");
        // The base rule is unchanged in both directions.
        TEST_CHECK(IsWrongScriptOutput(L"안녕하세요, 반갑습니다.", L"음", en),
                   "WS1 script: the pre-existing no-target-script rule still fires");
        TEST_CHECK(!IsWrongScriptOutput(L"안녕하세요, 반갑습니다.", L"hello", en),
                   "WS1 script: a source already in the target script still never trips it");
        TEST_CHECK(!IsWrongScriptOutput(L"Hello.", L"안녕", en),
                   "WS1 script: a real one-word answer passes");
        TEST_CHECK(!IsWrongScriptOutput(L"hello there", L"안녕하세요", 0),
                   "WS1 script: an unknown/AUTO target still fails open");
        static_assert(kWrongScriptLeakMinSourceCodePoints == 20 &&
                          kWrongScriptLeakRetainedPercent == 75,
                      "v0.10.9 U-D: the leak envelope is 20 code points at 75%");
    }

    // =====================================================================
    // (12) v0.10.9 (261121_0001 U-D, report 2B-3): the single-line source
    //      answer normalization that replaced the first-newline decode break.
    //      A byte-level flatten keeps the answer's UTF-8 intact (CR/LF can
    //      never sit inside a multi-byte sequence) while turning the model's
    //      own line wrapping into word separation.
    // =====================================================================
    {
        TEST_CHECK(V109NormalizeSingleLineOutput("") == "",
                   "v0.10.9 norm: empty stays empty");
        TEST_CHECK(V109NormalizeSingleLineOutput("그냥 한 줄") == "그냥 한 줄",
                   "v0.10.9 norm: a single-line answer is untouched");
        TEST_CHECK(V109NormalizeSingleLineOutput("첫 줄\n둘째 줄") == "첫 줄 둘째 줄",
                   "v0.10.9 norm: one LF becomes one space (the pre-v0.10.9 break truncated here "
                   "mid-sentence)");
        TEST_CHECK(V109NormalizeSingleLineOutput("a\r\nb") == "a b",
                   "v0.10.9 norm: a CRLF pair becomes ONE space");
        TEST_CHECK(V109NormalizeSingleLineOutput("a\n\n\nb") == "a b",
                   "v0.10.9 norm: a run of newlines becomes ONE space");
        TEST_CHECK(V109NormalizeSingleLineOutput("\n사이버\n") == "사이버",
                   "v0.10.9 norm + fix round 1 F-3: leading/trailing breaks become spaces and "
                   "the result's edges are TRIMMED inside the helper (no caller trim exists; "
                   "the old pin asserted one did)");
        // F-3 pin: the flattened output never carries leading/trailing
        // whitespace, whatever the model wrapped its answer in. Interior
        // spacing is the model's own word separation and stays untouched.
        {
            const std::string padded = V109NormalizeSingleLineOutput(" \t\n 사이버 \t\n ");
            const bool edges_clean =
                !padded.empty() && padded.front() != ' ' && padded.front() != '\t' &&
                padded.front() != '\r' && padded.front() != '\n' &&
                padded.back() != ' ' && padded.back() != '\t' &&
                padded.back() != '\r' && padded.back() != '\n';
            TEST_CHECK(padded == "사이버" && edges_clean,
                       "v0.10.9 norm + fix round 1 F-3: a padded/wrapped answer flattens to the "
                       "bare text with no leading or trailing whitespace");
            TEST_CHECK(V109NormalizeSingleLineOutput("  ") == "",
                       "v0.10.9 norm + fix round 1 F-3: a whitespace-only answer flattens to "
                       "empty (never a lone space)");
        }
        TEST_CHECK(V109NormalizeSingleLineOutput("사이버\n\nassistant: ...") ==
                       "사이버 assistant: ...",
                   "v0.10.9 norm: a leaked dialogue turn stays VISIBLE as words - the flatten "
                   "never silently swallows text the gates have not judged");
        TEST_CHECK(V109NormalizeSingleLineOutput("a \n b") == "a   b",
                   "v0.10.9 norm: surrounding spaces are NOT collapsed (only the break run is)");
        TEST_CHECK(V109NormalizeSingleLineOutput("\xF0\x9F\x98\x80\n\xF0\x9F\x98\x80") ==
                       "\xF0\x9F\x98\x80 \xF0\x9F\x98\x80",
                   "v0.10.9 norm: a surrogate-pair payload (4-byte UTF-8 emoji) survives intact");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] WS1 decode gates (261108_0002) passed." << std::endl;
    } else {
        std::cout << "[FAIL] WS1 decode gates: " << (g_failed_count - failures_before)
                  << " check(s) failed." << std::endl;
    }
}
