// translation_common implementation — REQ-043 (plan §5.1-1). Every function in
// this file is a VERBATIM move from src/engine.cpp (session history: I5, P2,
// REQ-R01, R6 Phase 4, SEC-B2, M3, F3, REQ-R16, P7-F2, REQ-003); see
// translation_common.hpp for the extraction rationale. The only edits are the
// class rename (TranslationManager::LlamaEngine -> LocalInferenceEngine) and
// the unconditional 4-parameter Translate signature on the no-llama stub
// (the old stub omitted src_name and could not have compiled with 8-arg
// callers; behavior in HAVE_LLAMA_CPP builds is untouched).

#include "translation_common.hpp"

#include "config.hpp"        // BuildPrompt (R6 Phase 4 / Tencent SFT template)
#include "diag_logger.hpp"   // DIAG_F (shape-only diagnostics)
#include "engine.hpp"        // TruncateHeadTailWindow / ScrubControlTokenTexts /
                             // CollectControlTokenTexts / IsValidModelPath /
                             // VerifyModelSha256 / kLlama* budget constants
#include "smart_bypass.hpp"  // R1-1: the shared script predicates (Is* ranges)
#include "unicode_utils.hpp" // ToUtf8 / ToUtf16 / DecodeNextCodePoint

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <optional>
#include <thread>
#include <vector>

namespace emebalachat {

namespace {

// I5 fix (was hardcoded 64 at the sampler-chain call site): repetition-penalty
// look-back window in tokens, per Tencent Hy-MT2 lab specification. Changing
// the tuning requires no other edit; behavior is identical to the previous
// literal 64.
constexpr int kPenaltyLastN = 64;
// I5 proof: compile-time assertion pins the lab-spec value (the constant lives
// in this TU's anonymous namespace, so tests/run_tests.cpp cannot reference it
// directly; a wrong value now fails the build instead of drifting silently).
static_assert(kPenaltyLastN == 64, "Hy-MT2 lab spec: repetition penalty last-N window is 64 tokens");

// REQ-R01 proof: the llama context budget must agree with the values EnsureLoaded
// configures and the arithmetic the unit tests rely on. Wrong values fail the build.
// P2 (session 260910_0001): re-pinned to the official Hy-MT2 model card plan -
// n_ctx 4096, generation reserve 2048 (the KV-cache memory math and the
// max_tokens=4096 trade-off history now live next to the constants in
// engine_core_helpers.hpp).
// 260930_0003 (가): re-pinned per the CEO decision - n_ctx 8192, budget 6128
// (prompt >= 6000 tokens must fit; user-GGUF vocabularies tokenize CJK less
// compactly than Hy-MT2's, see engine_core_helpers.hpp for the full rationale).
static_assert(kLlamaNCtx == 8192, "260930_0003 (가)/REQ-R01: n_ctx is 8192 (EnsureLoaded must configure the same)");
static_assert(kLlamaGenReserve == 2048, "REQ-R01/P2: generation reserve equals max_gen_tokens");
static_assert(kLlamaPromptTokenBudget == kLlamaNCtx - kLlamaGenReserve - kLlamaTokenSafetyMargin,
              "REQ-R01: prompt budget = n_ctx - gen reserve - safety margin");
static_assert(kLlamaPromptTokenBudget == 6128, "260930_0003 (가): prompt token budget is 8192-2048-16 = 6128");

} // namespace

// ---------------------------------------------------------------------------
// 260930_0003 (다, CEO decision): paragraph-boundary chunking + stitching.
// WHY: the old over-budget path kept head+tail of the SOURCE and discarded the
// MIDDLE, so a user-GGUF translation of a long input silently dropped whole
// paragraphs (Hy-MT2's compact CJK vocab masked the 2032 budget; MiLM/Gemma-
// class vocabs trip it on 1-3 paragraphs). The over-budget path now splits the
// SOURCE at paragraph boundaries, translates each chunk sequentially in the
// original order, and stitches the outputs back with the ORIGINAL separators
// verbatim. A single paragraph that alone exceeds the budget still falls back
// to the head+tail shrink inside the per-chunk path (TruncateHeadTailWindow,
// kept in engine.cpp). These helpers are model-independent and compile in
// both llama and no-llama configurations.
// ---------------------------------------------------------------------------

ParagraphSplit SplitParagraphBlocks(std::wstring_view text) {
    ParagraphSplit out;
    // A blank run is a candidate paragraph separator; a LINE BREAK is \r, \n,
    // or a \r\n pair (counted once). CRLF-aware: "p1\r\n\r\np2" is ONE
    // separator run ("\r\n\r\n"), never two adjacent cuts.
    const auto is_blank = [](wchar_t c) {
        return c == L'\r' || c == L'\n' || c == L' ' || c == L'\t';
    };
    const auto count_line_breaks = [text](size_t start, size_t end) {
        int breaks = 0;
        size_t k = start;
        while (k < end) {
            if (text[k] == L'\r') {
                ++breaks;
                if (k + 1 < end && text[k + 1] == L'\n') {
                    ++k; // CRLF pair counts as ONE line break
                }
            } else if (text[k] == L'\n') {
                ++breaks;
            }
            ++k;
        }
        return breaks;
    };
    const size_t n = text.size();
    size_t pos = 0;
    while (pos < n) {
        size_t scan = pos;
        bool found = false;
        size_t sep_start = 0;
        size_t sep_end = 0;
        while (scan < n) {
            if (!is_blank(text[scan])) {
                ++scan;
                continue;
            }
            const size_t ws_start = scan;
            size_t k = scan;
            while (k < n && is_blank(text[k])) {
                ++k;
            }
            // A whitespace run is a paragraph separator only when it contains
            // a BLANK LINE, i.e. >= 2 line breaks; single newlines stay inside
            // the surrounding paragraph (indented line breaks, soft wraps).
            if (count_line_breaks(ws_start, k) >= 2) {
                sep_start = ws_start;
                sep_end = k;
                found = true;
                break;
            }
            scan = k;
        }
        if (!found) {
            out.blocks.emplace_back(text.substr(pos));
            out.separators.emplace_back(); // no trailing separator
            break;
        }
        out.blocks.emplace_back(text.substr(pos, sep_start - pos));
        out.separators.emplace_back(text.substr(sep_start, sep_end - sep_start));
        pos = sep_end;
    }
    return out;
}

std::wstring JoinParagraphBlocks(const ParagraphSplit& split, size_t first, size_t last) {
    std::wstring out;
    for (size_t i = first; i <= last && i < split.blocks.size(); ++i) {
        if (i > first && i - 1 < split.separators.size()) {
            out += split.separators[i - 1]; // internal separator between blocks
        }
        out += split.blocks[i];
    }
    return out;
}

// ---------------------------------------------------------------------------
// R1-1 + R1-2 (session 261029_0001, v0.10.4 sweep): pure output-quality gate
// helpers. Both the gate and the sampler-shape decision are model-agnostic and
// llama-independent, so they live OUTSIDE the HAVE_LLAMA_CPP block: the
// no-llama build compiles them too and the unit suite pins every predicate
// without loading a GGUF. See translation_common.hpp for the WHY (the live
// "assist" symptom and the dropped repetition penalty).
// ---------------------------------------------------------------------------
namespace {

// ASCII-only fold, mirroring unicode_utils' EqualsIgnoreCaseAscii idiom: the
// CRT is never setlocale'd in this project, so an explicit A-Z fold keeps the
// meta-leakage verdict deterministic and leaves Korean/Han untouched.
wchar_t FoldAsciiCaseWide(wchar_t c) {
    return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c - L'A' + L'a') : c;
}

std::wstring FoldAsciiCaseWide(std::wstring_view text) {
    std::wstring out;
    out.reserve(text.size());
    for (const wchar_t c : text) {
        out.push_back(FoldAsciiCaseWide(c));
    }
    return out;
}

bool IsOutputSpace(wchar_t c) {
    return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == L'\v' || c == L'\f';
}

std::wstring_view TrimOutputSpace(std::wstring_view text) {
    size_t begin = 0;
    while (begin < text.size() && IsOutputSpace(text[begin])) {
        ++begin;
    }
    size_t end = text.size();
    while (end > begin && IsOutputSpace(text[end - 1])) {
        --end;
    }
    return text.substr(begin, end - begin);
}

// Whitespace-delimited tokens (words). CJK text without spaces stays ONE token
// — that is why the line rule matters for CJK loops and why the suffix rule
// also works on the same token stream.
std::vector<std::wstring_view> SplitOutputTokens(std::wstring_view text) {
    std::vector<std::wstring_view> tokens;
    size_t pos = 0;
    while (pos < text.size()) {
        while (pos < text.size() && IsOutputSpace(text[pos])) {
            ++pos;
        }
        const size_t begin = pos;
        while (pos < text.size() && !IsOutputSpace(text[pos])) {
            ++pos;
        }
        if (pos > begin) {
            tokens.push_back(text.substr(begin, pos - begin));
        }
    }
    return tokens;
}

// WS1 (261108_0002): defined in the gate-helper block further down (same
// anonymous namespace). IsWrongScriptOutput sits between the two blocks and
// uses it for the leak-dominance count.
std::size_t CountUnexpectedScriptCodePoints(std::wstring_view text,
                                            std::uint32_t allowed_mask);

// R1-1 meta-leakage table. Line-start needles are the shapes that only appear
// as an ANSWER PREFIX (label lines, assistant openers); anywhere needles are
// refusal phrasing that cannot sit inside a real translation without the source
// meaning it (and the source guard then skips them). ASCII needles are
// pre-folded; non-ASCII needles (Korean/Chinese label words) have no case.
struct MetaLeakageNeedle {
    const wchar_t* needle;
    bool line_start_only;
    // BUG-10 (v0.10.5 handoff section 2.11): conversational PREFIX needles
    // fire only when content follows the opener, so a legitimate ONE-WORD
    // translation ("sorry" -> "죄송합니다") is never mistaken for a refusal
    // preamble. Label-style needles keep the historical line-start shape.
    bool require_continuation = false;
    // F-04 (v0.10.5 review round 1): conversational openers additionally fire
    // only for a short source (<= kConversationalNeedleMaxSourceCodePoints):
    // "Sorry, but I cannot" legitimately translates into an apology-opener
    // output, while the model only talks BACK to a word or a short phrase.
    // Label-style prefixes (colon-terminated) stay unconditional.
    bool short_source_only = false;
};

constexpr MetaLeakageNeedle kMetaLeakageNeedles[] = {
    // Refusal openers — the CEO's "assist" repro verbatim
    // ("Translation is not possible… Please contact us for further assistance.").
    {L"translation is not possible", false},
    {L"translation is not available", false},
    {L"i can't translate", false},
    {L"i cannot translate", false},
    {L"i can not translate", false},
    {L"i'm unable to translate", false},
    {L"i am unable to translate", false},
    {L"can't translate this text", false},
    {L"cannot translate this text", false},
    {L"please contact us for", false},
    // Assistant-style answer openers (live essays/label loops).
    {L"as an ai", true},
    {L"as a language model", true},
    {L"sure, here", true},
    {L"sure! here", true},
    {L"here is a translation", true},
    {L"here's a translation", true},
    {L"here are some tips", true},
    // "<language>:" label lines — the bilingual/listing loops.
    {L"english:", true},   {L"korean:", true},  {L"chinese:", true},
    {L"japanese:", true},  {L"spanish:", true}, {L"french:", true},
    {L"german:", true},    {L"russian:", true}, {L"vietnamese:", true},
    {L"thai:", true},      {L"arabic:", true},
    {L"source:", true},    {L"target:", true},  {L"translation:", true},
    // Native-script label words (Korean/Chinese label loops).
    {L"영어:", true},     {L"한국어:", true},  {L"중국어:", true},
    {L"일본어:", true},   {L"스페인어:", true}, {L"프랑스어:", true},
    {L"독일어:", true},   {L"러시아어:", true},
    {L"中文:", true},     {L"日本語:", true},   {L"英語:", true},
    // BUG-10 (v0.10.5 handoff section 2.11): localized conversational /
    // refusal openers the shipped table missed entirely. Every needle keeps
    // the source guard below (a needle the SOURCE text itself carries is
    // legitimate content and is skipped). F-04 (review round 1) narrows the
    // conversational ones two ways: require_continuation prefixes fire only
    // when real content follows the opener (a legitimate one-word translation
    // passes), and short_source_only limits them to a short source (a real
    // sentence's translation may itself open with an apology). Colon-
    // terminated label prefixes stay unconditional, plus their fullwidth-
    // colon spellings.
    {L"번역 결과:", true, false, false},
    {L"번역문:", true, false, false},
    {L"翻译：", true, false, false},
    {L"翻译结果：", true, false, false},
    {L"翻訳結果：", true, false, false},
    {L"번역 결과", true, true, true},
    {L"다음은", true, true, true},
    {L"다음과 같습니다", true, true, true},
    {L"죄송합니다", true, true, true},
    {L"죄송하지만", true, true, true},
    {L"알겠습니다", true, true, true},
    {L"무엇을 도와드릴까요", true, true, true},
    {L"도움이 필요하신가요", true, true, true},
    {L"申し訳", true, true, true},
    {L"翻訳結果", true, true, true},
    {L"以下は", true, true, true},
    {L"お手伝い", true, true, true},
    {L"翻译结果", true, true, true},
    {L"以下是", true, true, true},
    {L"对不起", true, true, true},
    {L"抱歉", true, true, true},
    {L"有什么可以帮您", true, true, true},
    // WS1 (261108_0002, WS1-gates): DIALOGUE-ROLE and BRAND leakage - the
    // single-word hallucination chain the user report nailed down.
    //   * "<role>:" follows the colon-LABEL semantics of the language-label
    //     entries above: line-start only (after leading spaces/tabs), and
    //     exempted by the R2-7 translated-label rule, so "모델: GPT-4" ->
    //     "Model: GPT-4" stays content. "user:" / "system:" are reject-only
    //     values on purpose: StripLeadingRoleHeader does NOT salvage them - a
    //     turn from a role other than the assistant means the model continued
    //     a conversation the app never started, and only "assistant"/"model"/
    //     "ai"/"bot" can be a leaked header in front of a real answer.
    //   * "<|im_start|>" / "<|im_end|>" are ANYWHERE needles: a turn marker is
    //     never content, and it follows an answer as often as it leads one.
    //   * "emebala" / "이메발라" are ANYWHERE needles because the invented
    //     provenance sits mid-sentence ("이 문서는 Emebala의 출처를 기반으로
    //     작성되었습니다"). "emebala_source" is already subsumed by "emebala"
    //     and stays explicit so the fence token the prompt used to expose
    //     remains greppable. The source guard below still exempts a user who
    //     literally typed any of these.
    {L"assistant:", true, false, false},
    {L"user:", true, false, false},
    {L"system:", true, false, false},
    {L"model:", true, false, false},
    {L"<|im_start|>", false, false, false},
    {L"<|im_end|>", false, false, false},
    {L"emebala", false, false, false},
    {L"emebala_source", false, false, false},
    {L"이메발라", false, false, false},
};

} // namespace

std::uint32_t TargetScriptClasses(std::string_view tgt_code) {
    for (const LanguageScriptEntry& entry : kLanguageScriptTable) {
        if (EqualsIgnoreCaseAscii(tgt_code, std::string_view(entry.code))) {
            return entry.script_mask;
        }
    }
    return 0; // AUTO / unknown / empty -> the wrong-script gate fails open
}

std::uint32_t TextScriptClasses(std::wstring_view text) {
    std::uint32_t mask = 0;
    if (ContainsLatin(text)) mask |= ToMask(ScriptClass::Latin);
    if (ContainsKorean(text)) mask |= ToMask(ScriptClass::Hangul);
    if (ContainsHanzi(text)) mask |= ToMask(ScriptClass::Han);
    if (ContainsKana(text)) mask |= ToMask(ScriptClass::Kana);
    if (ContainsCyrillic(text)) mask |= ToMask(ScriptClass::Cyrillic);
    if (ContainsThai(text)) mask |= ToMask(ScriptClass::Thai);
    if (ContainsArabic(text)) mask |= ToMask(ScriptClass::Arabic);
    if (ContainsDevanagari(text)) mask |= ToMask(ScriptClass::Devanagari);
    if (ContainsBengali(text)) mask |= ToMask(ScriptClass::Bengali);
    if (ContainsKhmer(text)) mask |= ToMask(ScriptClass::Khmer);
    if (ContainsLao(text)) mask |= ToMask(ScriptClass::Lao);
    if (ContainsHebrew(text)) mask |= ToMask(ScriptClass::Hebrew);
    if (ContainsGreek(text)) mask |= ToMask(ScriptClass::Greek);
    if (ContainsMyanmar(text)) mask |= ToMask(ScriptClass::Myanmar);
    return mask;
}

std::size_t CountCodePoints(std::wstring_view text) {
    std::size_t count = 0;
    std::size_t idx = 0;
    while (idx < text.size()) {
        (void)DecodeNextCodePoint(text, idx);
        ++count;
    }
    return count;
}

bool IsWrongScriptOutput(std::wstring_view output, std::wstring_view source,
                         std::uint32_t tgt_script_classes) {
    if (tgt_script_classes == 0) {
        return false; // unknown/AUTO target: never guess
    }
    const std::size_t output_cp = CountCodePoints(output);
    if (output_cp <= kWrongScriptMinOutputCodePoints) {
        return false; // a two-character answer is below the gate's floor
    }
    // The gate only fires when the SOURCE carried a script the target does not
    // use. Pure digits/symbols ("12345") therefore pass this rule: there is no
    // language signal to be wrong about (their loops are caught by repetition).
    const std::uint32_t source_classes = TextScriptClasses(source);
    const std::uint32_t foreign_classes = source_classes & ~tgt_script_classes;
    if ((TextScriptClasses(output) & tgt_script_classes) != 0) {
        // R1-1: target script present -> answered in the right language.
        // WS1 (261108_0002, WS1-gates): EXCEPT the leak-dominance shape. One
        // embedded target-script word is not proof of a translation: the live
        // repro is a Korean source answered with 58 Hangul code points plus the
        // untranslated English word "astrology" for an English target, which
        // shipped as status=ok because this early exit saw "Latin present".
        // Reject when the source carried a real word's worth of the other
        // script (>= kWrongScriptLeakMinSourceCodePoints) and the output still
        // retains at least kWrongScriptLeakRetainedPercent of it.
        if (foreign_classes == 0) {
            return false; // nothing was supposed to be translated away
        }
        const std::size_t foreign_source_cp =
            CountUnexpectedScriptCodePoints(source, tgt_script_classes);
        if (foreign_source_cp < kWrongScriptLeakMinSourceCodePoints) {
            return false; // too little foreign text to establish dominance
        }
        const std::size_t retained_cp =
            CountUnexpectedScriptCodePoints(output, tgt_script_classes);
        return static_cast<long long>(retained_cp) * 100 >=
               static_cast<long long>(foreign_source_cp) * kWrongScriptLeakRetainedPercent;
    }
    return foreign_classes != 0;
}

namespace {

// R2-6 (session 261029_0001, code-reviewer-mimo F-01): the longest run of
// consecutive identical whitespace tokens. Rule B's span floor reads this.
int LongestIdenticalTokenRun(const std::vector<std::wstring_view>& tokens) {
    if (tokens.empty()) {
        return 0;
    }
    int best = 1;
    int run = 1;
    for (size_t i = 1; i < tokens.size(); ++i) {
        if (tokens[i] == tokens[i - 1]) {
            ++run;
        } else {
            run = 1;
        }
        if (run > best) {
            best = run;
        }
    }
    return best;
}

// R2-6: does this text itself carry a run of `min_run` identical whitespace
// tokens? The Rule-B source guard. Language-agnostic by construction: a
// token-identity match across a language pair is impossible, so the SHAPE (a
// reduplicated token run) is what is compared.
bool HasIdenticalTokenRun(std::wstring_view text, int min_run) {
    return LongestIdenticalTokenRun(SplitOutputTokens(text)) >= min_run;
}

// R2-7 (code-reviewer-gemini F-05): a line that starts (after leading spaces or
// tabs) with a colon-delimited LABEL - at most kColonLabelMaxCodeUnits
// characters, no sentence-ending punctuation before the colon. This is the
// source-side evidence that makes a translated label ("Source: Reuters" for
// "출처: 로이터") legitimate content instead of meta leakage.
// HONEST ENVELOPE (R3, round-3 F-04): the predicate is a SHAPE test, not a
// prose test - a short sentence that reaches its colon before any . ! or ?
// qualifies too ("The answer: 42" passes, 10 units), and that line would then
// exempt a same-index output needle. The bound is the shape itself (short
// pre-colon run, no sentence punctuation); do not read it as "prose can never
// widen the guard".
bool LineStartsWithColonLabel(std::wstring_view line) {
    size_t pos = 0;
    while (pos < line.size() && (line[pos] == L' ' || line[pos] == L'\t')) {
        ++pos;
    }
    const size_t label_begin = pos;
    while (pos < line.size() && line[pos] != L':') {
        const wchar_t c = line[pos];
        if (c == L'.' || c == L'!' || c == L'?' || c == L'\n') {
            return false; // sentence punctuation before the colon: prose, not a label
        }
        if (pos - label_begin >= kColonLabelMaxCodeUnits) {
            return false; // too long to be a label
        }
        ++pos;
    }
    return pos < line.size() && pos > label_begin;
}

// R2-7: split on '\n' WITHOUT dropping empty lines, so an output line index
// maps 1:1 onto the source line index (a translated label sits on the line its
// source label sat on).
std::vector<std::wstring_view> SplitLinesKeepingEmpties(std::wstring_view text) {
    std::vector<std::wstring_view> lines;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find(L'\n', start);
        if (end == std::wstring_view::npos) {
            end = text.size();
        }
        lines.push_back(text.substr(start, end - start));
        if (end >= text.size()) {
            break;
        }
        start = end + 1;
    }
    return lines;
}

// R2-16: code points of `text` whose script class is not part of
// `allowed_mask`. Script-less code points (digits, punctuation, whitespace,
// emoji) never count: they carry no language signal.
std::size_t CountUnexpectedScriptCodePoints(std::wstring_view text,
                                            std::uint32_t allowed_mask) {
    std::size_t count = 0;
    size_t idx = 0;
    while (idx < text.size()) {
        const size_t begin = idx;
        (void)DecodeNextCodePoint(text, idx);
        wchar_t units[2] = {0, 0};
        const size_t n = idx - begin;
        for (size_t k = 0; k < n && k < 2; ++k) {
            units[k] = text[begin + k];
        }
        const std::uint32_t mask = TextScriptClasses(std::wstring_view(units, n));
        if (mask != 0 && (mask & ~allowed_mask) != 0) {
            ++count;
        }
    }
    return count;
}

} // namespace

// ---------------------------------------------------------------------------
// WS1 (261108_0002, WS1-gates): DECODE-SIDE normalisation. The user report's
// decode cause #1 lives here (the prompt/detection causes were fixed in
// config.cpp / smart_bypass.cpp by another unit): a leaked assistant/dialogue
// header wrapped around a real answer ("assistant: 사이버",
// "<|assistant|>\n사이버"). The header is garbage, the answer may not be:
// stripping it BEFORE the rung acceptance compares lets the remainder be judged
// on its own merits instead of being rejected as meta leakage.
// Pure (no llama types), so the no-llama build keeps it and the unit suite pins
// it headlessly.
// ---------------------------------------------------------------------------

namespace {

// ASCII whitespace trim over a UTF-8 view. Trimming only ASCII bytes is safe in
// UTF-8: every continuation byte is >= 0x80, so a multi-byte sequence can never
// be cut in half.
std::string_view TrimAsciiWhitespaceView(std::string_view text) {
    size_t begin = 0;
    while (begin < text.size() && (text[begin] == ' ' || text[begin] == '\t' ||
                                   text[begin] == '\r' || text[begin] == '\n')) {
        ++begin;
    }
    size_t end = text.size();
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' ||
                           text[end - 1] == '\r' || text[end - 1] == '\n')) {
        --end;
    }
    return text.substr(begin, end - begin);
}

std::string_view TrimAsciiLeadingWhitespaceView(std::string_view text) {
    size_t begin = 0;
    while (begin < text.size() && (text[begin] == ' ' || text[begin] == '\t' ||
                                   text[begin] == '\r' || text[begin] == '\n')) {
        ++begin;
    }
    return text.substr(begin);
}

char FoldAsciiCaseByte(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

// Case-insensitive (ASCII) prefix test over UTF-8 bytes. Folding an ASCII byte
// can never make it equal a byte >= 0x80, so non-ASCII text never matches.
bool StartsWithAsciiNoCase(std::string_view text, std::string_view prefix) {
    if (text.size() < prefix.size()) {
        return false;
    }
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (FoldAsciiCaseByte(text[i]) != FoldAsciiCaseByte(prefix[i])) {
            return false;
        }
    }
    return true;
}

// WS1: emoji :shortcode: shape (^:[A-Za-z0-9_+-]+:). The leading-colon stripper
// must never eat one - ":laughing: 하하" is an emoji answer, not a stray ChatML
// ':' token.
bool StartsWithEmojiShortcode(std::string_view text) {
    if (text.size() < 3 || text.front() != ':') {
        return false;
    }
    size_t i = 1;
    while (i < text.size()) {
        const char c = text[i];
        const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                             (c >= '0' && c <= '9') || c == '_' || c == '+' || c == '-';
        if (!allowed) {
            break;
        }
        ++i;
    }
    return i > 1 && i < text.size() && text[i] == ':';
}

// WS1: code points that make a source "not a word". ASCII punctuation plus the
// CJK/fullwidth punctuation a Korean/Chinese/Japanese input actually carries. A
// false "not a word" only makes the structural gate fail OPEN (no rejection),
// so a cheap range set is enough - no Unicode category scan.
bool IsWordPunctuation(wchar_t c) {
    if (c >= 0x21 && c <= 0x2F) return true; // ! " # $ % & ' ( ) * + , - . /
    if (c >= 0x3A && c <= 0x40) return true; // : ; < = > ? @
    if (c >= 0x5B && c <= 0x60) return true; // [ \ ] ^ _ `
    if (c >= 0x7B && c <= 0x7E) return true; // { | } ~
    if (c >= 0x3000 && c <= 0x303F) return true; // CJK symbols/punctuation (+ ideographic space)
    if (c >= 0xFF00 && c <= 0xFF65) return true; // fullwidth ASCII forms + halfwidth CJK punctuation
    switch (c) {
        case 0x00AB: case 0x00BB:              // « »
        case 0x2013: case 0x2014:              // - (en/em dash forms)
        case 0x2018: case 0x2019:              // curly single quotes
        case 0x201C: case 0x201D:              // curly double quotes
        case 0x2026: case 0x00B7: case 0x30FB: // ellipsis, middle dot, katakana middle dot
            return true;
        default:
            return false;
    }
}

// WS1: the numbered / bullet opener a one-word answer must never have
// ("1.", "1)", "1、", "- ...", "* ..."). A digit run followed by a list
// terminator, or a bullet character followed by whitespace/digit.
bool StartsWithListMarker(std::wstring_view text) {
    if (text.empty()) {
        return false;
    }
    const wchar_t first = text.front();
    if (first == L'-' || first == L'*' || first == L'\u2022' || first == L'\u00B7' ||
        first == L'\u30FB') {
        return text.size() == 1 || text[1] == L' ' || text[1] == L'\t' ||
               (text[1] >= L'0' && text[1] <= L'9');
    }
    if (first < L'0' || first > L'9') {
        return false;
    }
    size_t i = 0;
    while (i < text.size() && text[i] >= L'0' && text[i] <= L'9') {
        ++i;
    }
    if (i >= text.size()) {
        return false;
    }
    const wchar_t terminator = text[i];
    return terminator == L'.' || terminator == L')' || terminator == L'\uFF09' ||
           terminator == L'\u3001' || terminator == L'\uFF0E';
}

// WS1: the sentence terminator that closes a one-word "explanation" (ASCII
// period/exclamation plus the CJK full stop and fullwidth exclamation),
// ignoring trailing whitespace and closing quotes/brackets.
bool EndsWithSentenceTerminator(std::wstring_view text) {
    const std::wstring_view trimmed = TrimOutputSpace(text);
    size_t end = trimmed.size();
    while (end > 0) {
        const wchar_t c = trimmed[end - 1];
        if (c != L'"' && c != L'\'' && c != L'\u201D' && c != L'\u2019' &&
            c != L'\u300D' && c != L'\u300F' && c != L')' && c != L'\uFF09') {
            break;
        }
        --end;
    }
    if (end == 0) {
        return false;
    }
    const wchar_t last = trimmed[end - 1];
    return last == L'.' || last == L'!' || last == L'\u3002' || last == L'\uFF01';
}

// WS1: whitespace-token count of an output (the ">= 3 words" clause of the
// structural gate). Reuses the gate's own tokenizer so the two rules can never
// disagree about what a word is.
int CountWhitespaceTokens(std::wstring_view text) {
    return static_cast<int>(SplitOutputTokens(text).size());
}

} // namespace

bool StripLeadingRoleHeader(std::string& text, std::string_view source) {
    // The prefixes are the roles a translator turn can be attributed to. "user"
    // and "system" are deliberately ABSENT: those are rejected as meta leakage
    // (kMetaLeakageNeedles) instead of salvaged - a turn from another role is a
    // conversation continuation, not a header in front of a real answer.
    static constexpr std::string_view kRolePrefixes[] = {
        "assistant:", "model:", "ai:", "bot:", "<|assistant|>",
    };
    const std::string_view view = TrimAsciiLeadingWhitespaceView(text);
    if (view.empty()) {
        return false;
    }
    const std::string_view src_view = TrimAsciiLeadingWhitespaceView(source);
    for (const std::string_view prefix : kRolePrefixes) {
        if (!StartsWithAsciiNoCase(view, prefix)) {
            continue;
        }
        // Source guard (repo idiom, same rule as StripEndOfConversationMarkers
        // and HasMetaLeakage): text the SOURCE itself begins with is content,
        // never a leaked header.
        if (!src_view.empty() && StartsWithAsciiNoCase(src_view, prefix)) {
            return false;
        }
        // R2-7 label guard: when the source's first line carries its own colon
        // label, a leading target-language label is that label TRANSLATED
        // ("모델: GPT-4" -> "Model: GPT-4") and must survive verbatim.
        if (!src_view.empty()) {
            const std::wstring src_w = ToUtf16(std::string(src_view));
            const std::wstring_view src_first_line =
                std::wstring_view(src_w).substr(0, src_w.find(L'\n'));
            if (LineStartsWithColonLabel(src_first_line)) {
                return false;
            }
        }
        // The header's own trailing separator goes with it: horizontal
        // whitespace always, and the line break of the "<|assistant|>\n<answer>"
        // marker form (the ladder already trimmed the string, so any leading
        // whitespace left here belongs to the header).
        size_t cut = prefix.size();
        while (cut < view.size() && (view[cut] == ' ' || view[cut] == '\t' ||
                                     view[cut] == '\r' || view[cut] == '\n')) {
            ++cut;
        }
        const size_t leading = static_cast<size_t>(view.data() - text.data());
        text.erase(0, leading + cut);
        return true;
    }
    return false;
}

bool IsUntranslatedEchoPrefix(std::wstring_view prefix, std::wstring_view source,
                              std::uint32_t tgt_script_classes) {
    const std::uint32_t source_scripts = TextScriptClasses(source);
    const std::uint32_t must_translate = source_scripts & ~tgt_script_classes;
    if (must_translate != 0 && must_translate == source_scripts) {
        return true; // script-homogeneous source: exact tracking can only be copying
    }
    const std::size_t untranslated_prefix_cp =
        CountUnexpectedScriptCodePoints(prefix, tgt_script_classes);
    return untranslated_prefix_cp != 0;
}

bool ShouldStripLeadingColon(std::string_view output, std::string_view source) {
    if (output.empty() || output.front() != ':') {
        return false;
    }
    const std::string_view src_lead = TrimAsciiLeadingWhitespaceView(source);
    if (!src_lead.empty() && src_lead.front() == ':') {
        return false; // the user's own leading colon is content
    }
    return !StartsWithEmojiShortcode(output);
}

bool HasRepeatedLineOrTokenRun(std::wstring_view text, std::wstring_view source) {
    // Rule A (R2-6): a non-empty trimmed line repeated kRepeatedLineMinCount
    // times CONSECUTIVELY. Pre-R2-6 the rule counted a repeat anywhere after the
    // first occurrence, so a transcript/form translation saying "Yes." three
    // times across a dialogue was rejected as a loop; a loop is by definition
    // consecutive. Only the trailing kRepetitionMaxScannedLines lines are
    // compared - a loop lives at the tail, and the bound keeps the scan cheap
    // on long multi-line outputs.
    std::vector<std::wstring_view> lines;
    size_t line_start = 0;
    while (line_start <= text.size()) {
        size_t line_end = text.find(L'\n', line_start);
        if (line_end == std::wstring_view::npos) {
            line_end = text.size();
        }
        const std::wstring_view line =
            TrimOutputSpace(text.substr(line_start, line_end - line_start));
        if (!line.empty()) {
            lines.push_back(line);
        }
        if (line_end >= text.size()) {
            break;
        }
        line_start = line_end + 1;
    }
    if (static_cast<int>(lines.size()) >= kRepeatedLineMinCount) {
        const size_t scanned = static_cast<size_t>(kRepetitionMaxScannedLines);
        const size_t first = lines.size() > scanned ? lines.size() - scanned : 0;
        int consecutive = 1;
        for (size_t i = first + 1; i < lines.size(); ++i) {
            if (lines[i] == lines[i - 1]) {
                if (++consecutive >= kRepeatedLineMinCount) {
                    return true;
                }
            } else {
                consecutive = 1;
            }
        }
    }

    // Rule B (R2-6): one whitespace token repeated kRepeatedTokenRunMinCount
    // times in a row, with TWO guards so ordinary human emphasis survives
    // ("no no no", "Help! Help! Help!", "ha ha ha"):
    //   (a) span floor - the run must cover at least
    //       kRepeatedTokenRunMinCoveragePercent of the output's tokens (a real
    //       one-token loop runs to the token cap: coverage ~100%; an emphasis
    //       inside a sentence covers a small fraction);
    //   (b) source guard - a reduplication the SOURCE itself carries is the
    //       user's content, not a loop (repo idiom, same rule as
    //       HasMetaLeakage below).
    const std::vector<std::wstring_view> tokens = SplitOutputTokens(text);
    if (tokens.empty()) {
        return false;
    }
    const int longest_run = LongestIdenticalTokenRun(tokens);
    if (longest_run < kRepeatedTokenRunMinCount) {
        return false;
    }
    // Integer form of `longest_run >= tokens.size() * percent / 100`. Truncation
    // makes the floor at most one token LOOSER than the exact product (11
    // tokens -> floor 5, so a 5-token run = 45% clears it); the unit suite pins
    // both sides of the boundary.
    const int coverage_floor = static_cast<int>(
        (static_cast<long long>(tokens.size()) * kRepeatedTokenRunMinCoveragePercent) / 100);
    if (longest_run < coverage_floor) {
        return false;
    }
    return !HasIdenticalTokenRun(source, kRepeatedTokenRunMinCount);
}

bool HasSuffixRepetition(std::wstring_view text) {
    const std::vector<std::wstring_view> tokens = SplitOutputTokens(text);
    const int n = static_cast<int>(tokens.size());
    if (n < kSuffixRepetitionMinSpanTokens) {
        return false;
    }
    // Rule C: the trailing tokens are >= kSuffixRepetitionMinRepeats identical
    // consecutive blocks of period p, and the repeated span covers at least
    // kSuffixRepetitionMinSpanTokens tokens (so "no no no" — 3 tokens — is not
    // a loop, while "no no no no no no no no" is).
    // (No std::min here: this TU includes windows.h through the project
    // headers, where the min/max macros would mangle `std::min(`.)
    int max_period = n / kSuffixRepetitionMinRepeats;
    if (max_period > kSuffixRepetitionMaxPeriodTokens) {
        max_period = kSuffixRepetitionMaxPeriodTokens;
    }
    for (int period = 1; period <= max_period; ++period) {
        int repeats = 1;
        while ((repeats + 1) * period <= n) {
            const int base = n - (repeats + 1) * period;
            bool equal = true;
            for (int t = 0; t < period; ++t) {
                if (tokens[base + t] != tokens[base + period + t]) {
                    equal = false;
                    break;
                }
            }
            if (!equal) {
                break;
            }
            ++repeats;
        }
        if (repeats >= kSuffixRepetitionMinRepeats &&
            repeats * period >= kSuffixRepetitionMinSpanTokens) {
            return true;
        }
    }
    return false;
}

bool OutputHasRepetition(std::wstring_view text, std::wstring_view source) {
    return HasRepeatedLineOrTokenRun(text, source) || HasSuffixRepetition(text);
}

bool HasMetaLeakage(std::wstring_view text, std::wstring_view source) {
    if (text.empty()) {
        return false;
    }
    const std::wstring folded = FoldAsciiCaseWide(text);
    const std::wstring folded_source = FoldAsciiCaseWide(source);
    // R2-7: line indices of the source, so a translated label can be matched to
    // the source line it came from.
    const std::vector<std::wstring_view> output_lines = SplitLinesKeepingEmpties(folded);
    const std::vector<std::wstring_view> source_lines =
        SplitLinesKeepingEmpties(folded_source);
    for (const MetaLeakageNeedle& needle : kMetaLeakageNeedles) {
        const std::wstring_view pattern(needle.needle);
        if (pattern.empty()) {
            continue;
        }
        // Source guard (repo idiom, same rule as StripEndOfConversationMarkers):
        // phrasing the SOURCE itself carries is legitimate content, never
        // leakage — a user who typed it must get it translated, not rejected.
        if (!folded_source.empty() && folded_source.find(pattern) != std::wstring::npos) {
            continue;
        }
        // F-04 (v0.10.5 review round 1): conversational openers fire only for a
        // short source. The model talks back to a word or a short phrase; an
        // opener-leading output for a longer source is that source's
        // translation ("Sorry, but I cannot" -> "죄송하지만 ...") and must
        // never be rejected as meta leakage.
        if (needle.short_source_only &&
            CountCodePoints(source) > kConversationalNeedleMaxSourceCodePoints) {
            continue;
        }
        if (!needle.line_start_only) {
            if (folded.find(pattern) != std::wstring::npos) {
                return true;
            }
            continue;
        }
        // R2-7 (code-reviewer-gemini F-05): a colon-label needle is legitimate
        // when the source line at the SAME index starts with its own colon
        // label — the model translated a label into a label ("출처: 로이터" ->
        // "Source: Reuters", "English: Fluent" -> "영어: 유창함"). The
        // pre-R2-7 guard could not see this: it only skipped a needle whose
        // exact TARGET-language spelling appeared in the source, which a real
        // language pair never contains.
        const bool colon_label = pattern.back() == L':';
        for (size_t i = 0; i < output_lines.size(); ++i) {
            const std::wstring_view line = output_lines[i];
            size_t content_start = 0;
            while (content_start < line.size() &&
                   (line[content_start] == L' ' || line[content_start] == L'\t')) {
                ++content_start;
            }
            if (line.compare(content_start, pattern.size(), pattern) != 0) {
                continue;
            }
            // BUG-10 (v0.10.5): a conversational PREFIX needle only fires when
            // real content follows the opener (punctuation alone does not
            // count) - "죄송합니다, 저는 ..." is a refusal preamble, while the
            // legitimate one-word translation "죄송합니다" must pass.
            if (needle.require_continuation) {
                bool has_more = false;
                for (size_t k = content_start + pattern.size(); k < line.size(); ++k) {
                    const wchar_t c = line[k];
                    if (c != L' ' && c != L'\t' && c != L'.' && c != L',' && c != L'!' &&
                        c != L'?' && c != L':' && c != L';' &&
                        c != L'，' && c != L'。' && c != L'、' && c != L'！' &&
                        c != L'？' && c != L'：' && c != L'；') {
                        has_more = true;
                        break;
                    }
                }
                if (!has_more) {
                    continue;
                }
            }
            if (colon_label && i < source_lines.size() &&
                LineStartsWithColonLabel(source_lines[i])) {
                continue;
            }
            return true;
        }
    }
    return false;
}

bool IsUnexpectedScriptHeavyOutput(std::wstring_view output, std::wstring_view source,
                                   std::uint32_t tgt_script_classes) {
    if (tgt_script_classes == 0) {
        return false; // AUTO / unknown target: nothing can be "unexpected"
    }
    const std::size_t output_cp = CountCodePoints(output);
    const std::size_t source_cp = CountCodePoints(source);
    if (output_cp == 0 || source_cp == 0) {
        return false;
    }
    // Both halves are required: a quoted foreign term inside a faithful
    // translation is neither a third of the text nor a >3x expansion.
    if (static_cast<long long>(output_cp) * 100 <=
        static_cast<long long>(source_cp) * kUnexpectedScriptLengthExpansionPercent) {
        return false;
    }
    const std::uint32_t allowed = tgt_script_classes | TextScriptClasses(source);
    const std::size_t unexpected = CountUnexpectedScriptCodePoints(output, allowed);
    return static_cast<long long>(unexpected) * 100 >=
           static_cast<long long>(output_cp) * kUnexpectedScriptSharePercent;
}

bool IsPureRepetitionSource(std::wstring_view source) {
    // The code-point sequence with whitespace dropped: the shape is a
    // repetition whether or not the user separated the repeats ("ㅋㅋㅋㅋ" vs
    // "k k k k"). Surrogate pairs stay one unit.
    std::vector<std::wstring> units;
    size_t idx = 0;
    while (idx < source.size()) {
        const size_t begin = idx;
        (void)DecodeNextCodePoint(source, idx);
        if (idx - begin == 1) {
            const wchar_t c = source[begin];
            if (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n') {
                continue;
            }
        }
        units.emplace_back(source.substr(begin, idx - begin));
    }
    const size_t n = units.size();
    if (n < 3) {
        return false; // fewer than three units is never a repetition
    }
    // A repetition needs a period that divides the WHOLE sequence into at
    // least THREE identical blocks ("ㅋㅋㅋㅋ", "hahaha", "no no no").
    // RC-2 (v0.10.5 handoff section 2.3): the pre-fix floor of two blocks
    // classified everyday reduplicated WORDS as laugh-track repetition -
    // 2-char AA ("점점", "妈妈", "谢谢") and 4-char ABAB ("바이바이", "haha")
    // - and handed them the 120-code-point laughter ceiling. ("음" stays
    // non-repetition: n < 3 covers the single-unit residual.)
    for (size_t period = 1; period * 3 <= n; ++period) {
        if (n % period != 0) {
            continue;
        }
        bool all_same = true;
        for (size_t i = period; i < n && all_same; ++i) {
            all_same = (units[i] == units[i % period]);
        }
        if (all_same) {
            return true;
        }
    }
    return false;
}

bool IsAmplifiedTinySourceOutput(std::wstring_view output, std::wstring_view source) {
    const std::size_t source_cp = CountCodePoints(source);
    if (source_cp == 0) {
        return false;
    }
    const std::size_t output_cp = CountCodePoints(output);
    // R3 source-shape carve-out (round-3 measured corpus ruling), BOUNDED by
    // 261029_0001 FIX-D2: a source that is nothing but one repeated unit is
    // laughter / emphasis, and a chatty model's laughter expansion of it is a
    // legitimate answer - but only under the laughter-legitimate ceiling. The
    // R3 draft made that exemption unbounded, and the rebuilt worker then
    // shipped 980 / 998 / 1110-code-point repeated-paragraph essays (958 / 1008
    // / 1122 UTF-8 bytes) for "kkkk" / "ㅎㅎㅎㅎ" / "haha" as status=ok; that is
    // nonsense output, not laughter (the longest legitimate reduplication
    // answer measures 19 code points). At or above
    // kAmplifiedRepetitionOutputMinCodePoints rule (ii) applies again. The
    // single-code-point residual ("음") is never a repetition, so it keeps the
    // tier-1 floor below.
    if (IsPureRepetitionSource(source)) {
        return output_cp >= kAmplifiedRepetitionOutputMinCodePoints;
    }
    // WS1 (261108_0002, WS1-gates) RE-CALIBRATED tiers (see the constant block
    // in translation_common.hpp for the measurement rationale). Tier 1 (1-4
    // code points): output >= max(16, 4x source) - the RC-2 32-at-10x floor
    // sat above the reported 14-32 code-point hallucinations, so every one of
    // them passed. Tier 2 (5..15 code points, or ANY single whitespace-free
    // token): a single WORD of <= 15 code points gets floor 20 at 4x (the
    // user-report window), a multi-word phrase of 5..15 code points and a
    // 16+ code point single token keep the RC-2 64-at-6x envelope (a phrase
    // legitimately expands more than a word).
    if (source_cp <= kAmplifiedSourceMaxCodePoints) {
        return output_cp >= kAmplifiedTinyTier1MinOutputCodePoints &&
               output_cp >= source_cp * kAmplifiedTinyTier1Ratio;
    }
    const bool single_token_source =
        source.find_first_of(L" \t\r\n") == std::wstring_view::npos;
    const bool tier2_source =
        source_cp <= kAmplifiedTinyTier2SourceMaxCodePoints || single_token_source;
    if (!tier2_source) {
        return false;
    }
    if (single_token_source && source_cp <= kAmplifiedTinyTier2SourceMaxCodePoints) {
        // The user-report window: ONE word, 5..15 code points, answered with a
        // sentence ("astrology" -> "이메발라의 별자리 운세는 어떨까요?" = 19).
        return output_cp >= kAmplifiedSingleWordOutputMinCodePoints &&
               output_cp >= source_cp * kAmplifiedSingleWordRatio;
    }
    return output_cp >= kAmplifiedOutputMinCodePoints &&
           output_cp >= source_cp * kAmplifiedTinyTier2Ratio;
}

bool IsSingleWordSource(std::wstring_view source) {
    // The laughter/emphasis carve-out applies here too (WS1 keeps the RC-2
    // ruling intact): "ㅋㅋㅋㅋ" is a reduplication, not a word whose answer must
    // be one word, and its expansion has its own bounded ceiling above.
    if (IsPureRepetitionSource(source)) {
        return false;
    }
    const std::wstring_view trimmed = TrimOutputSpace(source);
    if (trimmed.empty() || CountCodePoints(trimmed) > kSingleWordSourceMaxCodePoints) {
        return false;
    }
    bool has_letter = false;
    size_t idx = 0;
    while (idx < trimmed.size()) {
        const size_t begin = idx;
        (void)DecodeNextCodePoint(trimmed, idx);
        const size_t n = idx - begin;
        if (n == 1) {
            const wchar_t c = trimmed[begin];
            if (IsOutputSpace(c) || c == L'\u3000' || IsWordPunctuation(c)) {
                return false;
            }
        }
        wchar_t units[2] = {0, 0};
        for (size_t k = 0; k < n && k < 2; ++k) {
            units[k] = trimmed[begin + k];
        }
        if (TextScriptClasses(std::wstring_view(units, n)) != 0) {
            has_letter = true;
        }
    }
    return has_letter;
}

bool IsSingleWordToSentenceHallucination(std::wstring_view output,
                                         std::wstring_view source) {
    if (output.empty() || !IsSingleWordSource(source)) {
        return false;
    }
    // (a) the numbered / bullet quiz answer.
    if (StartsWithListMarker(TrimOutputSpace(output))) {
        return true;
    }
    // (b) an answer to a word is never a question.
    if (output.find(L'?') != std::wstring_view::npos ||
        output.find(L'\uFF1F') != std::wstring_view::npos) {
        return true;
    }
    // (c) the "explanation" shape: a closed sentence with real word count.
    return EndsWithSentenceTerminator(output) && CountWhitespaceTokens(output) >= 3;
}

const char* OutputRejectReasonName(OutputRejectReason reason) {
    switch (reason) {
        case OutputRejectReason::Repetition:  return "repetition";
        case OutputRejectReason::WrongScript: return "wrong-script";
        case OutputRejectReason::MetaLeakage: return "meta-leakage";
        case OutputRejectReason::ScriptLeak:  return "script-leak";
        case OutputRejectReason::SingleWordSentence: return "single-word-sentence";
        case OutputRejectReason::None:        break;
    }
    return "none";
}

OutputRejectReason ClassifyTranslationOutput(std::wstring_view output,
                                             std::wstring_view source,
                                             std::string_view tgt_code) {
    if (output.empty()) {
        return OutputRejectReason::None; // empty is the ladder's own concern
    }
    if (OutputHasRepetition(output, source)) {
        return OutputRejectReason::Repetition;
    }
    // WS1 (261108_0002, WS1-gates): META LEAKAGE now precedes WRONG SCRIPT.
    // The new leak-dominance clause in IsWrongScriptOutput fires on "foreign
    // text retained in the answer", which is exactly what a source echo inside a
    // refusal preamble looks like (the verbatim 'assist' repro: the English
    // source echoed back plus "죄송합니다"); the refusal diagnosis is strictly
    // more informative than "wrong script" and both reject, so the more specific
    // name wins. A plain wrong-language essay carries no needle and still
    // classifies as WrongScript.
    if (HasMetaLeakage(output, source)) {
        return OutputRejectReason::MetaLeakage;
    }
    if (IsWrongScriptOutput(output, source, TargetScriptClasses(tgt_code))) {
        return OutputRejectReason::WrongScript;
    }
    // R2-16 (CEO decision "언제나 잘 번역", calibrated thresholds above): the
    // answer is in the target script but is mostly a script neither side uses,
    // or a four-character source was answered with an essay. Runs LAST so a
    // more specific reason keeps its own diagnostic name.
    if (IsUnexpectedScriptHeavyOutput(output, source, TargetScriptClasses(tgt_code)) ||
        IsAmplifiedTinySourceOutput(output, source)) {
        return OutputRejectReason::ScriptLeak;
    }
    // WS1 (261108_0002, WS1-gates): the STRUCTURAL one-word-to-sentence shape
    // ("clue" -> "1. 당신은 어떤 방식으로..." / "astrology" -> "이메발라의 별자리
    // 운세는 어떨까요?"). Runs after every calibrated rule so their reason names
    // stay specific; it claims exactly the answers those floors still miss (the
    // window between the new single-word floor and the old 64-code-point one,
    // where the reported 19-code-point question sits).
    if (IsSingleWordToSentenceHallucination(output, source)) {
        return OutputRejectReason::SingleWordSentence;
    }
    return OutputRejectReason::None;
}

#ifdef HAVE_LLAMA_CPP

namespace {

// REQ-059: the fixed end-of-conversation marker table, shared by the
// mid-output strip (StripEndOfConversationMarkers) and the bare-special
// acceptance predicate. Session 260922_0002 live evidence: llama's BUILT-IN
// default template (ChatML) spells its turn ends as <|im_end|>/<|im_start|>,
// which the Gemma-family native specials do NOT cover (the MiLM vocab
// declares eot id 106 whose text is the NATIVE form, while the model spelled
// the CHATML form out of NORMAL tokens). Neither source alone catches both
// shapes, so the table stays fixed alongside the declared-special texts.
constexpr const char* kEndOfConversationMarkers[] = {
    "<|im_end|>", "<|im_start|>", "<end_of_turn>", "<start_of_turn>",
};

// REQ-059 perf: rung-1 early-echo-abort cadence — every N generated tokens
// the accumulated output is compared against the source prefix (exact
// compare). 16 tokens of exact source tracking is already conclusive (a
// real translation never tracks the source for that long), and the probe
// stays negligible against a 4B decode.
constexpr int kEchoPrefixProbeTokenCadence = 16;

// WS1 (261108_0002, WS1-gates) item 3: does the loaded vocab carry the ChatML
// turn markers as REAL CONTROL tokens? The REQ-059 rung-1 fallback wraps the
// fixed instruction in llama's BUILT-IN default template (ChatML) for a trivial
// or absent GGUF template. On a vocab that has no <|im_start|> control token,
// llama_chat_apply_template still emits the marker TEXT, which the tokenizer
// then breaks into NORMAL tokens - the model sees a literal "<|im_start|>user"
// string, attends to it, and the decode drifts into a literal role header
// ("assistant: ...") that IS the user report's hallucination shape. Tokenize
// the marker exactly the way tokenize_prompt does (parse_special=true, no BOS)
// and require: ONE token, CONTROL attribute. Any other outcome means the wrap
// must not be used for this model.
//
// API note (llama.cpp pinned tag b6099, build/_deps/llama_cpp-src/include/
// llama.h): the historical llama_token_get_attr is DEPRECATED in this tag in
// favour of llama_vocab_get_attr (L1003), and LLAMA_TOKEN_ATTR_CONTROL is
// L106 (1 << 3). Verified against the vendored header; the integration build
// re-checks it at compile time through the ordinary declaration use below.
bool VocabCarriesChatmlControlToken(const llama_vocab* vocab, const char* marker) {
    if (!vocab || !marker || !*marker) {
        return false;
    }
    const std::string_view marker_view(marker);
    const int32_t marker_len = static_cast<int32_t>(marker_view.size());
    if (marker_len <= 0) {
        return false;
    }
    llama_token tokens[8] = {};
    const int32_t n =
        llama_tokenize(vocab, marker, marker_len, tokens, 8, /*add_special=*/false,
                       /*parse_special=*/true);
    if (n != 1) {
        return false; // spelled out of NORMAL tokens (or rejected): not a control token
    }
    return (llama_vocab_get_attr(vocab, tokens[0]) & LLAMA_TOKEN_ATTR_CONTROL) != 0;
}

// REQ-R16 (audit §5 latent item 4): llama.cpp abort callback. ggml calls this
// between tensor-evaluation chunks of an in-flight llama_decode(); returning
// true aborts the compute. user_data is the caller's cancellation atomic
// (registered once at engine creation; TranslationManager passes its shutdown
// latch, the engine host passes its per-request flag — the address stays
// stable for the engine's lifetime). Documented llama.cpp limitation: CPU
// execution only - the per-token stop check in the decode loop below covers
// the GPU path (one forward pass per token, milliseconds each).
bool LlamaAbortIfCanceled(void* user_data) {
    if (!user_data) {
        return false;
    }
    const auto* flag = static_cast<const std::atomic<bool>*>(user_data);
    return flag->load(std::memory_order_acquire);
}

// REQ-R16: model-load cancellation. llama_progress_callback returns TRUE to
// CONTINUE loading; returning false aborts llama_model_load_from_file(). This
// unwinds the startup warmup thread (PreloadLocalModel) during app exit
// instead of the shutdown path waiting out a multi-second VRAM load - the
// other half of the "no zombie thread at exit" requirement.
bool LlamaLoadProgress(float /*progress*/, void* user_data) {
    return !LlamaAbortIfCanceled(user_data);
}

// P7-F2 compile-time pin: the device-pinning logic in EnsureLoaded relies on
// the exact b6099 split_mode enum layout (llama.h L184-187: NONE=0, LAYER=1).
// If a llama.cpp upgrade renumbers the enum, the build fails here instead of
// silently mis-pinning model devices.
static_assert(LLAMA_SPLIT_MODE_NONE == 0 && LLAMA_SPLIT_MODE_LAYER == 1,
              "P7-F2: llama.cpp split_mode enum layout changed; re-verify the "
              "NONE+main_gpu device pin and the CPU-fallback restore in EnsureLoaded");

// R6 Phase 4 (B2, architect plan §4.1 item 2): opt-in local-prompt observability.
// The user-machine JA->ZH confirmation needs proof of the EXACT prompt sent to
// Hy-MT2 (the 040 line only names the routed target). Launch Emebala_chat.exe
// with EMEBALA_DEBUG_PROMPT=1 in the environment to log every built local
// prompt (first 240 bytes) to stderr. Off by default: the prompt embeds user
// text, so capture is strictly user-initiated and stays on local stderr.
// Cached in a function-local static (thread-safe init since C++11): read once,
// never races with SetEnvironmentVariable mid-run.
bool DebugPromptEnabled() {
    static const bool enabled = [] {
        wchar_t buf[8] = {0};
        const DWORD n = ::GetEnvironmentVariableW(L"EMEBALA_DEBUG_PROMPT", buf, 8);
        return n > 0 && n < 8;
    }();
    return enabled;
}

// RT-C (260926_0003, task RT-C "D안-개선"): the translation-worker VRAM auto
// gate decision, evaluated ONCE per process (worker start = first EnsureLoaded)
// and cached in the function-local static (thread-safe init since C++11 —
// same discipline as DebugPromptEnabled above). Reads the manual override
// EMEBALA_MT_GPU (0/1), probes the free LOCAL VRAM through the DXGI query in
// engine_core_helpers.cpp, and applies the pure rule pinned by
// TestRtCMtGpuVramGate. The decision is logged shape-only (byte counts, no
// user content). Forcing GPU on a driverless machine is safe: the CUDA load
// leg then fails into the pre-existing CPU retry below.
bool MtGpuOffloadGateDecision() {
    static const bool decision = [] {
        wchar_t env_buf[16] = {0};
        const DWORD env_n = ::GetEnvironmentVariableW(L"EMEBALA_MT_GPU", env_buf, 16);
        const int override_value = (env_n > 0 && env_n < 16)
            ? ParseMtGpuOverride(std::wstring_view(env_buf, env_n))
            : ParseMtGpuOverride({});
        unsigned long long free_vram_bytes = 0;
        const bool query_ok = QueryMtAdapterFreeVramBytes(free_vram_bytes);
        const bool gpu = DecideMtGpuOffload(override_value, query_ok, free_vram_bytes);
        DIAG_F("ENGINE/EnsureLoaded/040: RT-C VRAM gate -> %s (query_ok=%d free_bytes=%llu threshold_bytes=%llu override=%d)\n",
               gpu ? "gpu" : "cpu", query_ok ? 1 : 0, free_vram_bytes,
               kMtGpuFreeVramThresholdBytes, override_value);
        return gpu;
    }();
    return decision;
}

} // namespace

// P7-F2: the SetGpuOffloadParams DEFINITION moved to
// engine_core_helpers.cpp (REQ-043 M6 T1 — design §4.2 lists it among the
// llama-independent seams of engine_core; keeping it here too produced
// LNK2005 within the same library). The declaration is re-exported via
// engine.hpp -> engine_core_helpers.hpp; EnsureLoaded below still calls it
// by its unqualified name.

LocalInferenceEngine::LocalInferenceEngine() {
    llama_log_set([](ggml_log_level level, const char* text, void* /*user_data*/) {
        if (level >= GGML_LOG_LEVEL_WARN) {
            DIAG_F("%s", text);
        }
    }, nullptr);
    llama_backend_init();
}

LocalInferenceEngine::~LocalInferenceEngine() {
    Unload();
    llama_backend_free();
}

void LocalInferenceEngine::Unload() {
    if (ctx) {
        llama_free(ctx);
        ctx = nullptr;
    }
    if (model) {
        llama_model_free(model);
        model = nullptr;
    }
    vocab = nullptr;
    loaded_path.clear();
    // SEC-B2: the scrub set belongs to the unloaded vocab - force a rebuild
    // for whatever model loads next (CollectControlTokenTexts is vocab-
    // derived, never hardcoded, so an alternative user GGUF is covered by
    // its own vocabulary).
    control_token_texts.clear();
    control_texts_built = false;
}

bool LocalInferenceEngine::EnsureLoaded(const std::string& path) {
    if (model && ctx && loaded_path == path) {
        return true;
    }

    Unload();

    // M3 (security): validate the path (non-empty, regular file, .gguf
    // extension, no '..' traversal for relative paths) BEFORE passing it to
    // the GGUF loader. Fail-closed with an ENGINE/IsValidModelPath/NNN code
    // on stderr; the worker treats a false return like any load failure.
    if (!IsValidModelPath(path)) {
        return false;
    }

    // F3 (security, session 260909_0002, audit §F3): content-hash pin at
    // runtime. The installer verifies the SHA-256 only at download time,
    // so a GGUF tampered AFTER installation (or restored from backup)
    // used to reach the llama.cpp parser unchecked. VerifyModelSha256()
    // streams the file through Windows CNG once and caches the result in
    // a "<model>.sha256ok" marker keyed on mtime+size, so the 1.91 GB
    // hash costs at most one full pass per changed file. Fail-closed for
    // the pinned filename; user-configured alternative names proceed on
    // explicit-config consent basis (see engine.hpp contract). A false
    // return here behaves like any load failure: Auto falls back to the
    // cloud, strict-local surfaces EngineFailed/LocalModelMissing to the
    // worker, and every rejection carries an ENGINE/VerifyModelSha256/NNN
    // diagnostic on stderr (DIAG_F mirrors to the diagnostic log).
    // This also completes the F2 story: an installer run where the user
    // answered "No" to the mismatch dialog leaves the unverified file in
    // place, but the app refuses to load it now.
    if (!VerifyModelSha256(std::filesystem::path{path}, {})) {
        return false;
    }

    // REQ-R16 + REQ-051 U-1 FIX 2: if a load-cancellation was requested while
    // we were queued behind the caller's lock, do not even start a model load.
    // The LOAD leg reads LoadCancelRequested() (load_cancel_flag, defaulting
    // to the cancel_flag alias) — a decode-time abort never reaches it.
    if (LoadCancelRequested()) {
        DIAG_F("ENGINE/EnsureLoaded/030: load skipped, shutdown cancellation pending\n");
        return false;
    }

    llama_model_params mparams = llama_model_default_params();
    // P7-F2 (universal GPU, session 260909_0004): GPU-offload leg pins the
    // WHOLE model to device 0 (= CUDA on NVIDIA dual-backend boxes) to stop
    // the CUDA+Vulkan layer split of one physical card. Full contract +
    // evidence: SetGpuOffloadParams in src/engine.hpp. Seam-tested
    // headlessly by TestP7F2GpuOffloadParams (tests/run_tests.cpp).
    // RT-C (260926_0003, task RT-C "D안-개선"): the FIRST leg is no longer
    // hardcoded true — the VRAM auto gate decides it from the free local
    // VRAM at worker start (protects the Listener ASR worker's VRAM share
    // on 8 GB-class cards). CPU-gated machines run the CPU leg directly:
    // no CUDA context is initialized, no GPU load attempt is wasted.
    const bool gpu_offload = MtGpuOffloadGateDecision();
    SetGpuOffloadParams(mparams, gpu_offload);
    // REQ-R16: abort an in-progress model load when shutdown is requested.
    // REQ-051 U-1 FIX 2: the progress callback reads the LOAD flag (never the
    // decode flag), so a per-job abort mid-load cannot discard the load — the
    // default load_cancel_flag alias keeps the legacy behavior byte-identical.
    mparams.progress_callback = LlamaLoadProgress;
    mparams.progress_callback_user_data =
        const_cast<void*>(static_cast<const void*>(EffectiveLoadCancelFlag()));

    model = llama_model_load_from_file(path.c_str(), mparams);
    if (!model) {
        // REQ-R16: a cancel-aborted load is not a CUDA failure; do not
        // spend another full load attempt on the CPU path afterwards.
        // REQ-051 U-1 FIX 2: same load-flag split as the pre-load check.
        if (LoadCancelRequested()) {
            DIAG_F("ENGINE/EnsureLoaded/031: model load aborted by shutdown cancellation\n");
            return false;
        }
        // RT-C (260926_0003): when the VRAM gate already routed this load
        // onto the CPU leg, the failure is final — retrying the IDENTICAL
        // CPU-shaped load would only burn another multi-second pass before
        // answering the same false (model_missing).
        if (!gpu_offload) {
            DIAG_F("ENGINE/EnsureLoaded/041: CPU-gated model load failed (RT-C gate decision)\n");
            return false;
        }
        // Fallback to CPU-only load if CUDA load encounters an issue
        // P7-F2: this leg MUST un-pin (n_gpu_layers=0 + default
        // LLAMA_SPLIT_MODE_LAYER, per the seam contract): llama.cpp
        // validates split_mode/main_gpu against the GPU-device list even
        // at n_gpu_layers=0 (src/llama.cpp L200-213), so NONE +
        // main_gpu=0 would fail this retry too on machines with zero
        // enumerable GPU devices and hard-break the CPU fallback.
        SetGpuOffloadParams(mparams, /*gpu_offload=*/false);
        model = llama_model_load_from_file(path.c_str(), mparams);
        if (!model) {
            return false;
        }
    }

    vocab = llama_model_get_vocab(model);

    llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx = kLlamaNCtx;             // REQ-R01: budget constants shared with Translate()
    cparams.n_batch = kLlamaNCtx;
    cparams.n_ubatch = 512;
    unsigned int hw_threads = std::thread::hardware_concurrency();
    cparams.n_threads = hw_threads > 0 ? static_cast<int32_t>(hw_threads) : 4;
    cparams.n_threads_batch = cparams.n_threads;
    cparams.flash_attn = true;
    // REQ-R16: per-chunk abort inside llama_decode (CPU execution path).
    cparams.abort_callback = LlamaAbortIfCanceled;
    cparams.abort_callback_data =
        const_cast<void*>(static_cast<const void*>(cancel_flag));

    ctx = llama_init_from_model(model, cparams);
    if (!ctx && cparams.flash_attn) {
        cparams.flash_attn = false;
        ctx = llama_init_from_model(model, cparams);
    }
    if (!ctx) {
        llama_model_free(model);
        model = nullptr;
        vocab = nullptr;
        return false;
    }

    loaded_path = path;
    return true;
}

std::wstring LocalInferenceEngine::Translate(
    std::wstring_view text,
    std::string_view tgt_name,
    std::string_view src_name,
    const std::string& path,
    float temperature,
    float top_p,
    int top_k,
    float rep_pen,
    std::string_view template_ref
) {
    // REQ-R16: a canceled engine short-circuits before touching llama.
    // REQ-051 U-1 FIX 1: every Translate() entry clears the wall-clock
    // exhaustion latch so a previous timeout can never leak into this result.
    decode_wall_clock_exhausted_ = false;
    if (CancelRequested()) {
        return {};
    }
    if (!EnsureLoaded(path)) {
        return {};
    }

    // SEC-B2: build the scrub set from the now-loaded vocab (once per model
    // load). The only way the security guard could silently disappear is an
    // EMPTY set on a non-null vocab (degenerate/corrupt vocab) - surface
    // that loudly instead of pretending the scrub is active.
    if (!control_texts_built) {
        control_token_texts = CollectControlTokenTexts(vocab);
        control_texts_built = true;
        if (vocab && control_token_texts.empty()) {
            DIAG_F("ENGINE/ScrubControlTokens/001: vocab exposes no control-class tokens; scrub set is empty\n");
        }
    }

    // Tencent Hy-MT2 instruction format + optional GGUF chat template. Both are
    // rebuilt inside the REQ-R01 shrink loop, so they live in one lambda.
    // REQ-059 rung 1: the template is probed ONCE per call — if applying it to
    // a single user message returns the content VERBATIM (adds no structure;
    // MiLM ships the trivial {% for message in messages %}{{ message.content
    // %}{% endfor %} concat template) the bare fixed instruction is out-of-
    // distribution (live evidence: immediate-EOS empty decode). A trivial (or
    // absent) template therefore falls back to llama's BUILT-IN default
    // template (ChatML) instead; non-trivial templates (Hy-MT2) keep the
    // existing path byte-identically.
    const char* chat_tmpl = llama_model_chat_template(model, nullptr);
    bool chat_tmpl_is_trivial = true;
    if (chat_tmpl) {
        static constexpr const char* kTmplProbe = "REQ059_TEMPLATE_PROBE";
        llama_chat_message probe{"user", kTmplProbe};
        const int32_t probe_needed =
            llama_chat_apply_template(chat_tmpl, &probe, 1, true, nullptr, 0);
        if (probe_needed > 0) {
            std::vector<char> formatted(static_cast<size_t>(probe_needed) + 1);
            const int32_t probe_written = llama_chat_apply_template(
                chat_tmpl, &probe, 1, true, formatted.data(),
                static_cast<int32_t>(formatted.size()));
            chat_tmpl_is_trivial =
                probe_written > 0 &&
                std::string(formatted.data(), static_cast<size_t>(probe_written)) == kTmplProbe;
        }
    }
    // REQ-059: two fixed prompt forms, selected per attempt (rung 1 = chat,
    // rung 2 = completion). RC-5 (v0.10.5 handoff section 2.6) corrects the
    // historical claim that "the completion form is never applied to the
    // Hy-MT2 path's non-trivial template": the code below DOES reach rung 2
    // for any model after an empty/echo/garbage rung 1, and raw completion on
    // a chat-template model is exactly the single-word hallucination chain
    // the handoff documents. Rung 2 therefore stays for trivial/absent-
    // template user models, while a rung-1 ECHO on a NON-trivial chat
    // template (Hy-MT2) is accepted as-is instead of degrading (see the
    // acceptance test below).
    const bool has_nontrivial_chat_template = chat_tmpl && !chat_tmpl_is_trivial;
    // WS1 (261108_0002, WS1-gates) item 3: the built-in ChatML wrap is only
    // usable when the vocab declares <|im_start|> as a CONTROL token. Probing
    // costs one tokenize per Translate() and only when the rung-1 fallback
    // branch would actually run (trivial/absent template), so Hy-MT2 and any
    // user model with a real template pay nothing.
    const bool builtin_chatml_wrap_available =
        !has_nontrivial_chat_template &&
        VocabCarriesChatmlControlToken(vocab, "<|im_start|>");
    if (!has_nontrivial_chat_template && !builtin_chatml_wrap_available) {
        DIAG_F("ENGINE/Translate/051: vocab has no ChatML CONTROL token; rung 1 uses the completion form instead of a literal-marker wrap (WS1)\n");
    }
    auto build_final_prompt = [&](std::wstring_view s, bool completion_form,
                                  bool suppress_debug_log = false) -> std::string {
        std::string u8 = ToUtf8(s);
        if (u8.empty()) {
            return {};
        }
        // REQ-CP T4 (tech-gate A2): the template_ref steers the rung-1 chat
        // form ONLY. The rung-2 completion retry keeps its single pinned,
        // template-independent form for ALL templates, preserving the REQ-059
        // byte-identity golden pins. template_ref defaults to "" (hymt2-
        // official) so the pre-T5 default path is byte-identical.
        //
        // WS1 (261108_0002, WS1-gates) item 3: a trivial/absent-template model
        // whose vocab has no ChatML control token gets the completion form on
        // rung 1 as well. Wrapping it would inject the markers as literal NORMAL
        // text, which is the in-distribution-for-nothing prompt that produces
        // the "assistant: ..." dialogue hallucination; the completion form is the
        // untemplated shape such a model was actually trained on. Byte-identical
        // for every other configuration (non-trivial template / usable ChatML /
        // rung 2).
        const bool rung1_without_chatml =
            !completion_form && !has_nontrivial_chat_template && !builtin_chatml_wrap_available;
        std::string p = (completion_form || rung1_without_chatml)
            ? BuildCompletionPrompt(u8, tgt_name, src_name)
            : BuildPrompt(u8, tgt_name, src_name, template_ref);
        if (!completion_form) {
            if (chat_tmpl && !chat_tmpl_is_trivial) {
                llama_chat_message msg{"user", p.c_str()};
                int32_t needed = llama_chat_apply_template(chat_tmpl, &msg, 1, true, nullptr, 0);
                if (needed > 0) {
                    std::vector<char> formatted(needed + 1);
                    int32_t written = llama_chat_apply_template(chat_tmpl, &msg, 1, true, formatted.data(), static_cast<int32_t>(formatted.size()));
                    if (written > 0) {
                        p.assign(formatted.data(), written);
                    }
                }
            } else if (builtin_chatml_wrap_available) {
                // REQ-059 rung 1: trivial (concat-only) or absent template —
                // wrap with llama's BUILT-IN default template (ChatML) so the
                // fixed instruction is in-distribution for Gemma-style user
                // models. (Hy-MT2 never reaches this branch: its real
                // template is non-trivial and stays byte-identical.)
                llama_chat_message msg{"user", p.c_str()};
                int32_t needed = llama_chat_apply_template(nullptr, &msg, 1, true, nullptr, 0);
                if (needed > 0) {
                    std::vector<char> formatted(needed + 1);
                    int32_t written = llama_chat_apply_template(nullptr, &msg, 1, true, formatted.data(), static_cast<int32_t>(formatted.size()));
                    if (written > 0) {
                        p.assign(formatted.data(), written);
                    }
                }
            }
        }
        // REQ-003 (session 260909) exhaustive-content audit: the built
        // prompt embeds the user's source text (first 240 bytes printed
        // below), so it needs BOTH gates - the pre-existing opt-in
        // EMEBALA_DEBUG_PROMPT env var AND diag_log_content (default off).
        // With either off, only the byte count is recorded.
        // 260930_0003 (다): suppress_debug_log is set ONLY by the chunking
        // pre-check probe below, so a within-budget request emits the exact
        // same diagnostic lines as before the chunking change.
        if (DebugPromptEnabled() && !suppress_debug_log) {
            if (diag::ContentLoggingEnabled()) {
                DIAG_F(
                        "ENGINE/BuildPrompt/050: local prompt target=\"%.*s\" source=\"%.*s\" bytes=%zu:\n%.240s\n---\n",
                        static_cast<int>(tgt_name.size()), tgt_name.data(),
                        static_cast<int>(src_name.size()), src_name.data(),
                        p.size(), p.c_str());
            } else {
                DIAG_F(
                        "ENGINE/BuildPrompt/050: local prompt target=\"%.*s\" source=\"%.*s\" bytes=%zu (content logging disabled)\n",
                        static_cast<int>(tgt_name.size()), tgt_name.data(),
                        static_cast<int>(src_name.size()), src_name.data(),
                        p.size());
            }
        }
        return p;
    };

    // Tokenize helper (REQ-R01): fills `out` and returns the token count, or -1
    // on tokenizer failure. Allocation mirrors the original probe-then-size.
    // SEC-B2 (session 260911_0002, verify 233020 §5): parse_special=true is
    // load-bearing and MUST stay - it is how the chat-template markers the
    // app itself spliced in above (begin_of_sentence / hy_User / hy_Assistant)
    // tokenize as genuine control tokens. The injection channel it opens for
    // USER text is closed upstream by ScrubControlTokenTexts, not here.
    auto tokenize_prompt = [&](const std::string& p, std::vector<llama_token>& out) -> int32_t {
        if (p.empty()) {
            return -1;
        }
        int32_t n_alloc = -llama_tokenize(vocab, p.c_str(), static_cast<int32_t>(p.size()), nullptr, 0, true, true);
        if (n_alloc <= 0) {
            n_alloc = static_cast<int32_t>(p.size()) + kLlamaTokenSafetyMargin;
        }
        out.resize(static_cast<size_t>(n_alloc) + static_cast<size_t>(kLlamaTokenSafetyMargin));
        int32_t n = llama_tokenize(
            vocab,
            p.c_str(),
            static_cast<int32_t>(p.size()),
            out.data(),
            static_cast<int32_t>(out.size()),
            true,
            true
        );
        if (n <= 0) {
            return -1;
        }
        out.resize(static_cast<size_t>(n));
        return n;
    };

    std::wstring src_w(text);
    // SEC-B2 (OWASP LLM01, verify 233020 §6): with parse_special=true the
    // tokenizer substring-matches EVERY CONTROL/USER_DEFINED/UNKNOWN vocab
    // text (llama-vocab.cpp L2396-2402, L2603-2635), so a user text
    // containing "<｜hy_User｜>" (id 120006), "<｜hy_Assistant｜>"
    // (120007) or the metadata-EOS "<｜hy_place▁holder▁no▁2｜>" (120020)
    // would be emitted as a GENUINE role-boundary control token -> prompt
    // injection. Scrub the user-controlled text here - after capture,
    // BEFORE BuildPrompt's verbatim append and the chat-template wrap, so
    // the app's own template markers (added inside build_final_prompt
    // after this point) stay intact. The shrink loop below only re-slices
    // this already-scrubbed copy, so every rebuild inherits the scrub.
    src_w = ScrubControlTokenTexts(src_w, control_token_texts);
    // R1-1 (261029_0001): the target SCRIPT family is resolved ONCE per call
    // from the i18n language-registry code. tgt_name is the registry's name_en
    // ("Korean"/"English"/..., the worker parity contract in
    // ggml_translate_worker.cpp) and NormalizeLanguageCode accepts either form;
    // an unknown/AUTO target resolves to mask 0 and the wrong-script gate then
    // fails open. Model-agnostic by construction: the script table is keyed by
    // language code, never by model or template.
    const std::string tgt_code = NormalizeLanguageCode(tgt_name);
    // REQ-059: the prompt build + tokenize + decode body runs per attempt
    // (attempt 0 = rung-1 chat form; attempt 1 = rung-2 completion form,
    // only after an empty/echo rung 1). The shrink loop, hard cap, sampler,
    // cancel/wall-clock checks and the REQ-051 U-1 latch semantics are
    // identical per attempt; the decode wall-clock budget is FRESH per
    // attempt. Hy-MT2 never degrades: its rung-1 output is never empty/echo.
    // REQ-059 live-verified: a rung-1 output can also be a BARE SPECIAL
    // MARKER — the live MiLM host spelled "<|im_end|>" out of SIX NORMAL
    // tokens (the eot id 106 was never sampled, so the stop disjuncts and
    // the control-piece scrub — NORMAL pieces by definition — could not
    // catch it). Such an output is rejected exactly like empty/echo.
    // REQ-059: the full end-of-conversation marker text set, built ONCE per
    // call: the fixed ChatML/native table above PLUS the texts of every
    // special id the vocab declares (eos/eot/bos/sep/pad; same no-op-safety
    // argument as the eos disjunct for undeclared ids (-1)). Shared by the
    // mid-output strip and the bare-special acceptance predicate.
    std::vector<std::string> eoc_markers;
    for (const char* marker : kEndOfConversationMarkers) {
        eoc_markers.emplace_back(marker);
    }
    {
        const llama_token specials[] = {
            llama_vocab_eos(vocab), llama_vocab_eot(vocab), llama_vocab_bos(vocab),
            llama_vocab_sep(vocab), llama_vocab_pad(vocab),
        };
        for (const llama_token id : specials) {
            if (id < 0) {
                continue;
            }
            const char* special_text = llama_vocab_get_text(vocab, id);
            if (special_text && *special_text) {
                eoc_markers.emplace_back(special_text);
            }
        }
    }
    auto output_is_bare_special = [&](const std::string& out) {
        if (out.empty()) {
            return false;
        }
        return std::find(eoc_markers.begin(), eoc_markers.end(), out) != eoc_markers.end();
    };
    // REQ-059: strip end-of-conversation markers embedded in an otherwise
    // real output (live user case: rung-1 answered with the source ECHO +
    // a spelled-out "<|im_end|>" tail, which the EOG break and the
    // control-piece scrub both miss because the marker is spelled from
    // NORMAL tokens). Source-guard: a marker the SOURCE text itself
    // contains is NEVER stripped, so legitimate user text about chat
    // formats survives untouched.
    auto StripEndOfConversationMarkers = [&](std::string& out, std::string_view source_u8) {
        for (const std::string& marker : eoc_markers) {
            if (source_u8.find(marker) != std::string_view::npos) {
                continue; // source-guard (see above)
            }
            size_t pos = 0;
            while ((pos = out.find(marker, pos)) != std::string::npos) {
                out.erase(pos, marker.size());
            }
        }
    };
    // 260930_0003 (다): the per-chunk translation body — the former inline
    // REQ-059 attempt loop (rung 1 chat -> rung 2 completion), unchanged in
    // behavior, wrapped in a lambda so an over-budget input can be translated
    // chunk by chunk. `chunk_text` is the source slice for ONE chunk; the
    // working copy `cur` is what the head+tail shrink mutates (each chunk
    // shrinks independently). std::nullopt maps the historical in-body
    // `return {}` failures; a completed attempt ladder returns the trimmed
    // UTF-8 output even when empty (the caller treats empty as engine_failed,
    // exactly like the pre-chunking code).
    auto translate_once = [&](std::wstring_view chunk_text,
                              const std::optional<std::chrono::steady_clock::time_point>&
                                  whole_request_deadline) -> std::optional<std::string> {
    std::wstring cur(chunk_text);
    std::string trimmed_u8;
    bool degraded_to_completion = false;
    // REQ-059 perf: set when the rung-1 early-echo probe aborts the decode —
    // the acceptance test below treats it exactly like an echo (the output
    // is only a source PREFIX at that point, so the full-equality echo
    // compare alone could not catch it).
    bool rung1_aborted_echo = false;
    // R1-1 (261029_0001): set when the periodic repetition probe aborts a
    // rung's decode. Rung 1 -> the acceptance test degrades to the completion
    // form; rung 2 -> the ladder has no rung left, so the attempt answers
    // std::nullopt instead of returning the partial loop.
    bool rung1_aborted_repetition = false;
    bool rung2_repetition_aborted = false;
    for (int attempt = 0; attempt < 2; ++attempt) {
    const bool completion_form = (attempt == 1);
    std::string prompt = build_final_prompt(cur, completion_form);
    std::vector<llama_token> prompt_tokens;
    int32_t n_prompt_tokens = tokenize_prompt(prompt, prompt_tokens);
    if (n_prompt_tokens < 0) {
        return std::nullopt;
    }

    // REQ-R01 (audit §2.1): count prompt tokens BEFORE llama_decode. 260930_0003
    // (다): this shrink is now the PER-CHUNK fallback — reached only when a
    // single paragraph alone exceeds the budget (multi-paragraph over-budget
    // input is chunked before translate_once is ever called). When the tokens
    // exceed the budget (n_ctx - generation reserve - safety margin), shrink
    // the chunk's SOURCE text with a head+tail sliding window and re-tokenize.
    // Each iteration targets a proportional size minus 25% headroom, so the
    // loop makes geometric progress and terminates quickly even when the
    // character -> token compression ratio differs between iterations.
    if (n_prompt_tokens > kLlamaPromptTokenBudget) {
        const int32_t overflow_n = n_prompt_tokens;
        int shrink_iters = 0;
        while (n_prompt_tokens > kLlamaPromptTokenBudget && shrink_iters < 16 && cur.size() > 64) {
            const double ratio = static_cast<double>(kLlamaPromptTokenBudget) / static_cast<double>(n_prompt_tokens);
            size_t target_len = static_cast<size_t>(static_cast<double>(cur.size()) * ratio * 0.75);
            if (target_len < 64) {
                target_len = 64;
            }
            if (target_len >= cur.size()) {
                target_len = cur.size() - 1; // shrink at least one unit per iteration
            }
            cur = TruncateHeadTailWindow(cur, target_len / 2);
            prompt = build_final_prompt(cur, completion_form);
            const int32_t n2 = tokenize_prompt(prompt, prompt_tokens);
            if (n2 < 0) {
                DIAG_F("ENGINE/Translate/013: tokenizer rejected the truncated prompt\n");
                return std::nullopt;
            }
            n_prompt_tokens = n2;
            ++shrink_iters;
        }
        // 260930_0003 (다): same 010 over-budget family as the chunking line,
        // message now names the per-chunk head+tail fallback explicitly.
        DIAG_F("ENGINE/Translate/010: chunk prompt tokens %d exceeded budget %d; chunk head+tail-shrunk to %zu UTF-16 units -> %d tokens after %d shrink iterations\n",
                overflow_n, kLlamaPromptTokenBudget, cur.size(), n_prompt_tokens, shrink_iters);
    }

    // Last-resort hard cap: if the shrink loop still could not reach the budget
    // (pathological template overhead or iteration cap), clip the TOKEN vector
    // head+tail so llama_decode can never fail on length. Preserves the BOS +
    // instruction prefix (head) and the sentence-final tokens (tail).
    if (n_prompt_tokens > kLlamaPromptTokenBudget) {
        const size_t head_n = static_cast<size_t>(kLlamaPromptTokenBudget) / 2;
        const size_t tail_n = static_cast<size_t>(kLlamaPromptTokenBudget) - head_n;
        std::vector<llama_token> kept;
        kept.reserve(prompt_tokens.size());
        kept.insert(kept.end(), prompt_tokens.begin(), prompt_tokens.begin() + head_n);
        kept.insert(kept.end(), prompt_tokens.end() - tail_n, prompt_tokens.end());
        prompt_tokens.swap(kept);
        n_prompt_tokens = static_cast<int32_t>(prompt_tokens.size());
        DIAG_F("ENGINE/Translate/012: hard token-window cap applied, prompt clipped to %d tokens\n", n_prompt_tokens);
    }

    // The post-generation quote-strip heuristic below compares against the
    // ORIGINAL source quoting; recompute UTF-8 from the (possibly shrunk)
    // working copy so the comparison reflects what was actually sent.
    std::string src_u8 = ToUtf8(cur);
    // RC-5 (v0.10.5 handoff section 2.6): a source with no line break can
    // only be answered with a single line; the decode stops at the first
    // generated newline on BOTH rungs (see the token loop - WS1
    // 261108_0002 extended it from the completion form to rung 1, because a
    // single word answered with "사이버\n\nassistant: ..." leaked the trailing
    // dialogue turn through rung 1).
    const bool single_line_source =
        cur.find_first_of(L"\r\n") == std::wstring::npos;
    // WS1 (261108_0002, WS1-gates) item 9: the echo probe's mixed-script rule
    // is the pure IsUntranslatedEchoPrefix decision (pinned headlessly); the
    // only thing it needs from here is the target script family.
    const std::uint32_t echo_target_scripts = TargetScriptClasses(tgt_code);
    // Trimmed source for the prefix compare: a leading separator space or a
    // trailing one must not defeat the probe (WS1 item 9a).
    const std::string_view src_probe_view = TrimAsciiWhitespaceView(src_u8);

    // Clear KV memory for clean inference sequence
    llama_memory_clear(llama_get_memory(ctx), true);

    // Process prompt tokens
    llama_batch batch = llama_batch_get_one(prompt_tokens.data(), static_cast<int32_t>(prompt_tokens.size()));
    if (llama_decode(ctx, batch) != 0) {
        DIAG_F("ENGINE/Translate/011: llama_decode failed (%d prompt tokens, budget %d)\n",
                n_prompt_tokens, kLlamaPromptTokenBudget);
        return std::nullopt;
    }

    // Initialize sampler according to Tencent Hy-MT2 official specifications
    // R1-2 (261029_0001): the temperature<=0.001 leg is chosen by the pure
    // DecideSamplerShape decision (pinned headlessly). With a configured
    // repetition penalty the chain now APPLIES it (penalties -> greedy); the
    // pre-R1-2 code built a bare greedy sampler and silently dropped the
    // shipped rep_pen=1.05, which is what let greedy decodes loop. rep_pen
    // <= 1.0 keeps the historical bare-greedy allocation byte-identically.
    llama_sampler* smpl = nullptr;
    switch (DecideSamplerShape(temperature, rep_pen)) {
        case SamplerShape::BareGreedy:
            smpl = llama_sampler_init_greedy();
            break;
        case SamplerShape::PenalizedGreedy: {
            llama_sampler_chain_params sparams = llama_sampler_chain_default_params();
            smpl = llama_sampler_chain_init(sparams);
            // Penalty FIRST, greedy LAST: the chain's final sampler owns the
            // selection (llama_sampling.cpp llama_sampler_sample reads the
            // `selected` index), so greedy is an argmax over the PENALIZED,
            // optionally filtered logits. top_k/top_p are deliberately absent
            // (see DecideSamplerShape: argmax-invariant at temperature 0).
            llama_sampler_chain_add(smpl, llama_sampler_init_penalties(kPenaltyLastN, rep_pen, 0.0f, 0.0f));
            llama_sampler_chain_add(smpl, llama_sampler_init_greedy());
            break;
        }
        case SamplerShape::Dist: {
            llama_sampler_chain_params sparams = llama_sampler_chain_default_params();
            smpl = llama_sampler_chain_init(sparams);
            if (rep_pen > 1.0f) {
                llama_sampler_chain_add(smpl, llama_sampler_init_penalties(kPenaltyLastN, rep_pen, 0.0f, 0.0f));
            }
            if (top_k > 0) {
                llama_sampler_chain_add(smpl, llama_sampler_init_top_k(top_k));
            }
            if (top_p > 0.0f && top_p < 1.0f) {
                llama_sampler_chain_add(smpl, llama_sampler_init_top_p(top_p, 1));
            }
            llama_sampler_chain_add(smpl, llama_sampler_init_temp(temperature));
            llama_sampler_chain_add(smpl, llama_sampler_init_dist(LLAMA_DEFAULT_SEED));
            break;
        }
    }
    if (!smpl) {
        return std::nullopt;
    }

    std::string output_u8;
    // REQ-051 U-1 FIX 1: input-scaled generation cap (min(kLlamaGenReserve,
    // max(256, 4n+128))) replaces the flat 2048 mirror — kLlamaGenReserve stays
    // the named ceiling constant (untouched REQ-R01 pin), but a degenerate
    // non-Hy-MT2 loop now deterministically dies at 4x the prompt size + 128
    // tokens instead of pasting 2048 tokens of garbage. The wall-clock budget
    // below is the second, VRAM-contention-proof backstop: whichever trips
    // first wins.
    const int max_gen_tokens = ScaledMaxGenTokens(n_prompt_tokens);
    // RC-4 (v0.10.5 handoff section 2.5): short single-line source cap. The
    // historical 256-token floor scaled off the WHOLE formatted prompt, so a
    // one-word translation carried a 256-token runway for hallucinated
    // essays. A <= 16 UTF-8 byte single-line source can only produce a word
    // or one sentence: cap its generation budget to 64 tokens, and to
    // kSingleWordSourceGenCapTokens (16) when the source carries no whitespace
    // at all (WS1 261108_0002: ONE word cannot legitimately need more, and the
    // 64-token runway is what let a 1.8B model continue the word into a
    // paragraph or a dialogue turn). The wall-clock budget below stays scaled
    // off the historical max_gen_tokens - whenever this cap fires both budgets
    // land on the 12 s floor, so the budget is unchanged in practice.
    const int short_source_gen_cap = ShortSourceGenTokenCap(
        src_u8.size(), src_u8.find_first_of("\r\n") != std::string::npos,
        src_u8.find_first_of(" \t\v\f") != std::string::npos);
    const int effective_max_gen_tokens =
        max_gen_tokens < short_source_gen_cap ? max_gen_tokens : short_source_gen_cap;
    // REQ-051 U-1 FIX 1: the decode wall-clock budget starts HERE (after the
    // prompt decode + sampler setup) and covers token GENERATION only — a
    // model load is governed by the REQ-051 U-1 FIX 2 finish-once-started
    // rule, not by this clock.
    // REQ-059 perf: the budget is input-scaled
    // (ScaledDecodeWallClockBudgetMs(max_gen_tokens)): the 12 s REQ-051
    // floor for typical inputs, scaling to 27 s for long translations on
    // slow 4B user models. REQ-051 U-1 latch semantics unchanged.
    const auto decode_start = std::chrono::steady_clock::now();
    auto decode_deadline = decode_start +
        std::chrono::milliseconds(ScaledDecodeWallClockBudgetMs(max_gen_tokens));
    // Backend P1-3 (v0.10.5 handoff section 4.4 item 3): a chunked whole-
    // request deadline clamps this chunk's budget so the per-chunk decode
    // scale (up to 27 s) cannot accumulate past the frozen 30 s host/client
    // IPC timeout across N chunks. std::nullopt (the single-shot path) keeps
    // the historical deadline byte-identically.
    if (whole_request_deadline.has_value() && *whole_request_deadline < decode_deadline) {
        decode_deadline = *whole_request_deadline;
    }

    for (int i = 0; i < effective_max_gen_tokens; ++i) {
        // REQ-R16: cancellation check BETWEEN DECODE STEPS - the token
        // loop is the app-level seam: shutdown posts the flag, and at
        // most one more sampled token (+ its batched decode, the
        // abort_callback unwinds that from inside) runs before we exit
        // the loop with the partial output discarded as empty.
        if (CancelRequested()) {
            DIAG_F("ENGINE/Translate/032: local decode canceled at token %d (shutdown)\n", i);
            llama_sampler_free(smpl);
            return std::nullopt;
        }

        // REQ-051 U-1 FIX 1: wall-clock budget check BETWEEN DECODE STEPS
        // (same seam as the cancellation check — at most one more sampled
        // token runs before we exit). On expiry the partial output is
        // discarded as empty and the exhaustion latch is set; the worker
        // maps it to the frozen "timeout" wire code (transient -> the
        // client's one-shot retry). Shape-only line: ms + token index, no
        // content.
        const auto now = std::chrono::steady_clock::now();
        if (now >= decode_deadline) {
            const long long elapsed_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - decode_start).count();
            decode_wall_clock_exhausted_ = true;
            DIAG_F("ENGINE/Translate/033: decode wall-clock budget exhausted at token %d (%lld ms); answering timeout\n",
                   i, static_cast<long long>(elapsed_ms));
            llama_sampler_free(smpl);
            return std::nullopt;
        }

        // Guard against context overflow (REQ-R01: constant now shared with the
        // budget computed before decode, instead of a second hardcoded 2048).
        if (static_cast<int>(prompt_tokens.size()) + i + 1 >= kLlamaNCtx) {
            break;
        }

        llama_token token = llama_sampler_sample(smpl, ctx, -1);
        llama_sampler_accept(smpl, token);

        // REQ-006: EOS double-check. Hy-MT2 GGUF metadata ships with
        // special_eos_id missing from the eog set, so llama_vocab_is_eog
        // alone can let generation run past the model's stop token. The
        // is_eog test stays FIRST (fast path unchanged for well-formed
        // vocabs); the eos disjunct only catches the malformed-metadata
        // case. llama_vocab_eos returns -1 for EOS-less vocabs, which
        // never equals a valid sampled token, so this is a safe no-op
        // there.
        // REQ-059: third disjunct — llama_vocab_eot (end-of-turn). Gemma-
        // family GGUFs (user models) declare an eot id that is NOT eos and
        // NOT in the eog set consulted above, so a model emitting its eot
        // directly would leak the piece into the output (live MiLM host,
        // session 260922_0002). NOTE (live-traced): this vocab's actual
        // failure spelled "<|im_end|>" out of SIX NORMAL tokens instead —
        // caught by the bare-special acceptance test below, not this
        // disjunct; both guards stay (a direct eot sample remains possible
        // on other GGUFs). Mirroring the eos no-op-safety argument:
        // llama_vocab_eot returns -1 for eot-less vocabs, never equal to a
        // valid sampled token.
        if (llama_vocab_is_eog(vocab, token) || token == llama_vocab_eos(vocab) ||
            token == llama_vocab_eot(vocab)) {
            break;
        }

        char piece[256] = {};
        int n_piece = llama_token_to_piece(vocab, token, piece, sizeof(piece), 0, false);
        if (n_piece > 0) {
            // REQ-059: output-side control-piece scrub (defense-in-depth).
            // The eot break above handles termination; this stops any other
            // stray control-class piece (mid-output specials) from reaching
            // the caller's text. The scrub set is the same per-load
            // control_token_texts built for the SEC-B2 input scrub — cheap:
            // it holds only control-class token texts.
            const std::wstring piece_w =
                ToUtf16(std::string_view(piece, static_cast<size_t>(n_piece)));
            if (std::find(control_token_texts.begin(), control_token_texts.end(), piece_w) ==
                control_token_texts.end()) {
                output_u8.append(piece, n_piece);
            }
        } else if (n_piece < 0) {
            int needed = -n_piece;
            std::vector<char> big_piece(needed);
            int written = llama_token_to_piece(vocab, token, big_piece.data(), needed, 0, false);
            if (written > 0) {
                // REQ-059: same control-piece scrub on the oversized-piece path.
                const std::wstring piece_w =
                    ToUtf16(std::string_view(big_piece.data(), static_cast<size_t>(written)));
                if (std::find(control_token_texts.begin(), control_token_texts.end(), piece_w) ==
                    control_token_texts.end()) {
                    output_u8.append(big_piece.data(), written);
                }
            }
        }

        // RC-5 (v0.10.5 handoff section 2.6): the rung-2 raw completion form
        // invites few-shot continuations ("Korean: ...\nEnglish: ...") after
        // the answer. When the source itself is a single line, the first
        // generated newline ends the answer - everything after it is the
        // model continuing the pattern, not translation.
        // WS1 (261108_0002, WS1-gates) item 6: BOTH rungs, not only the
        // completion form. A single-line source can never legitimately be
        // answered with two lines, and the live single-word leak was exactly a
        // rung-1 answer followed by a second line ("사이버\n\nassistant: ..."),
        // which the post-decode gate only sees AFTER the model has spent the
        // tokens. Multi-line sources are untouched (they legitimately produce
        // line breaks and keep the old behavior).
        if (single_line_source) {
            const size_t newline_pos = output_u8.find_first_of("\r\n");
            if (newline_pos != std::string::npos) {
                output_u8.resize(newline_pos);
                break;
            }
        }

        // REQ-059 perf: early echo abort (RUNG 1 ONLY). A rung-1 echo on a
        // long paragraph otherwise generates the ENTIRE source (hundreds of
        // tokens, seconds on a 4B model) before the end-of-generation echo
        // check can reject it — roughly doubling the bill before rung 2
        // re-decodes from scratch. Every kEchoPrefixProbeTokenCadence
        // tokens, compare the accumulated output against the source prefix:
        // an EXACT-prefix match means the model is echoing — bail out now
        // (shape-only 037 line) and let the attempt loop degrade to the
        // completion form. Exact compare only, no fuzzy heuristic: Hy-MT2
        // translations never track the source for 16+ tokens, and even a
        // false fire still answers correctly via rung 2.
        //
        // R1-1 (261029_0001): the same cadence now also runs the repetition
        // predicate on BOTH rungs. A loop that starts mid-decode otherwise
        // burns the entire generation budget (the live "np" -> "npi" x64 run
        // took 8.2 s for nothing) before the post-decode gate could reject it.
        // Same predicate as the post-decode gate (OutputHasRepetition), so the
        // two sites can never disagree; the per-rung FRESH decode budget and
        // the input-scaled cap are untouched. Shape-only diagnostic (token
        // index only, no content). R2-6: the source rides along so the Rule-B
        // reduplication guard sees the same evidence at both sites.
        if ((i + 1) % kEchoPrefixProbeTokenCadence == 0 && !output_u8.empty()) {
            if (OutputHasRepetition(ToUtf16(output_u8), cur)) {
                if (completion_form) {
                    rung2_repetition_aborted = true;
                } else {
                    rung1_aborted_repetition = true;
                }
                DIAG_F("ENGINE/Translate/039: %s output is repeating at token %d; aborting the decode (R1-1)\n",
                       completion_form ? "completion-form" : "rung-1", i + 1);
                break;
            }
            if (!completion_form) {
                // WS1 (261108_0002, WS1-gates) item 9a: compare a
                // LEADING-WHITESPACE-TRIMMED output prefix against a TRIMMED
                // source. The pre-WS1 compare used the raw byte prefix, so the
                // separator space the chat form leaves in front of the answer
                // ("...:\n\n cyber") - or a leading space in the user's own
                // source - made every prefix compare fail and the echo probe
                // never fired.
                const std::string_view out_probe_view =
                    TrimAsciiLeadingWhitespaceView(output_u8);
                // Two legs, both required to be cheap:
                //   * the RAW prefix compare is the pre-WS1 fast path, kept
                //     byte-identical for sources that carry no surrounding
                //     whitespace (and the REQ-059 (b) source pin keys on it);
                //   * the TRIMMED compare is the WS1 fix - it also fires when
                //     the chat form left a separator space in front of the
                //     answer or the source itself begins with one.
                const bool raw_prefix_match =
                    output_u8.size() <= src_u8.size() &&
                    output_u8.compare(0, output_u8.size(), src_u8, 0, output_u8.size()) == 0;
                const bool trimmed_prefix_match =
                    !out_probe_view.empty() && out_probe_view.size() <= src_probe_view.size() &&
                    out_probe_view.compare(0, out_probe_view.size(), src_probe_view, 0,
                                           out_probe_view.size()) == 0;
                if (raw_prefix_match || trimmed_prefix_match) {
                    // WS1 item 9b: MIXED-SCRIPT CONTINUATION must not abort. The
                    // decision is the pure IsUntranslatedEchoPrefix predicate:
                    // abort only when the matched prefix itself carries a code
                    // point of a script the target does not use (proof the model
                    // is still copying untranslated text) OR the source is
                    // script-homogeneous (no part of it can be "already in the
                    // target", so any exact tracking is copying).
                    const std::wstring prefix_w = ToUtf16(std::string(out_probe_view));
                    if (IsUntranslatedEchoPrefix(prefix_w, cur, echo_target_scripts)) {
                        DIAG_F("ENGINE/Translate/037: rung-1 output tracks the source prefix at token %d (out=%zu src=%zu); aborting to the completion form\n",
                               i + 1, output_u8.size(), src_u8.size());
                        rung1_aborted_echo = true;
                        break;
                    }
                    DIAG_F("ENGINE/Translate/053: rung-1 prefix match at token %d is already in the target script (mixed-script source); continuing (WS1)\n",
                           i + 1);
                }
            }
        }

        batch = llama_batch_get_one(&token, 1);
        if (llama_decode(ctx, batch) != 0) {
            // REQ-049: a mid-generation decode failure used to fall through
            // with the partial output masquerading as a successful
            // translation; answer honestly (empty -> engine_failed) like the
            // prompt-decode failure above.
            DIAG_F("ENGINE/Translate/014: llama_decode failed mid-generation at token %d; answering engine_failed\n", i);
            llama_sampler_free(smpl);
            return std::nullopt;
        }
    }

    llama_sampler_free(smpl);

    // Trim leading and trailing whitespace / newlines
    size_t start = 0;
    while (start < output_u8.size() && (output_u8[start] == ' ' || output_u8[start] == '\n' || output_u8[start] == '\r' || output_u8[start] == '\t')) {
        start++;
    }
    size_t end = output_u8.size();
    while (end > start && (output_u8[end - 1] == ' ' || output_u8[end - 1] == '\n' || output_u8[end - 1] == '\r' || output_u8[end - 1] == '\t')) {
        end--;
    }

    trimmed_u8 = output_u8.substr(start, end - start);

    // REQ-059: strip embedded end-of-conversation markers FIRST — the
    // ladder's acceptance checks (echo compare + bare-special predicate)
    // and the colon/quote cleanup all operate on the STRIPPED string, so
    // "source + marker" reduces to exactly the source and is correctly
    // judged an ECHO (rung 2).
    StripEndOfConversationMarkers(trimmed_u8, src_u8);

    // WS1 (261108_0002, WS1-gates) item 1: strip a leaked assistant/role header
    // BEFORE the acceptance compares, so the answer behind it is judged on its
    // own merits ("assistant: 사이버" -> "사이버" is a correct translation;
    // "<|assistant|>\n사이버" likewise). Source-guarded (a prefix the SOURCE
    // begins with is content) and label-guarded (a translated colon label the
    // source itself labels is content) - see StripLeadingRoleHeader.
    if (StripLeadingRoleHeader(trimmed_u8, src_u8)) {
        DIAG_F("ENGINE/Translate/050: stripped a leading assistant/role header from the %s output (WS1; shape-only)\n",
               completion_form ? "completion-form" : "rung-1");
    }

    // REQ-059: leading-colon cleanup — Gemma-style models answering through
    // the ChatML wrap start with a stray ':' token; drop the colon (and any
    // spaces after it) when the source itself did not start with one.
    // WS1 (261108_0002, WS1-gates) item 10 closes two pre-existing misses and
    // moves the decision into the pure ShouldStripLeadingColon:
    //   (a) the source test now looks at the source's LEADING-WHITESPACE-TRIMMED
    //       start, so a source that begins with a separator space does not make
    //       the stripper eat a legitimate answer's colon (" : 사과");
    //   (b) an emoji :shortcode: is never a stray ChatML ':' - ":laughing: 하하"
    //       keeps its first colon (the pre-fix code turned it into "laughing:
    //       하하", a mangle the user sees verbatim).
    // The leading ':' test stays at the call site (the REQ-059 (b) source pin
    // keys on it); the erase below is the unchanged cleanup shape.
    if (!trimmed_u8.empty() && trimmed_u8.front() == ':' &&
        ShouldStripLeadingColon(trimmed_u8, src_u8)) {
        size_t colon_drop = 1;
        while (colon_drop < trimmed_u8.size() && trimmed_u8[colon_drop] == ' ') {
            ++colon_drop;
        }
        trimmed_u8.erase(0, colon_drop);
    }

    // Strip matching outer quotes if model wrapped translation in quotes but source text was not quoted
    if (trimmed_u8.size() >= 2 && trimmed_u8.front() == '\"' && trimmed_u8.back() == '\"') {
        if (src_u8.empty() || (src_u8.front() != '\"' && src_u8.back() != '\"')) {
            trimmed_u8 = trimmed_u8.substr(1, trimmed_u8.size() - 2);
        }
    }

    // REQ-059 rung acceptance: an EMPTY rung-1 decode (immediate EOS, or a
    // bare marker stripped to empty just above), an ECHO (trimmed output
    // equals the trimmed source — the live MiLM KO->EN behavior), or a
    // bare special marker degrades to the completion form for ONE more
    // decode with a fresh wall-clock budget. RC-5 (v0.10.5) amendment: the
    // ECHO case degrades only for trivial/absent-template user models; on a
    // real chat template (Hy-MT2) the echo is accepted as-is right below, so
    // a chat-tuned model never sees the raw completion form.
    //
    // R1-1 (261029_0001): a rung-1 output that is non-empty but GARBAGE
    // (repetition loop / wrong script / meta-assistant leakage) degrades the
    // same way — one completion-form retry instead of shipping the garbage.
    // The gate runs on the TRIMMED rung-1 output; a mid-decode repetition
    // abort is classified as Repetition without re-scanning (it already fired
    // on the partial text).
    if (attempt == 0) {
        std::string src_cmp = src_u8;
        size_t cs = 0;
        while (cs < src_cmp.size() &&
               (src_cmp[cs] == ' ' || src_cmp[cs] == '\n' || src_cmp[cs] == '\r' || src_cmp[cs] == '\t')) {
            ++cs;
        }
        size_t ce = src_cmp.size();
        while (ce > cs &&
               (src_cmp[ce - 1] == ' ' || src_cmp[ce - 1] == '\n' ||
                src_cmp[ce - 1] == '\r' || src_cmp[ce - 1] == '\t')) {
            --ce;
        }
        src_cmp = src_cmp.substr(cs, ce - cs);
        const bool rung1_empty = trimmed_u8.empty();
        const bool rung1_echo = !rung1_empty && !src_cmp.empty() && trimmed_u8 == src_cmp;
        const bool rung1_bare_special = output_is_bare_special(trimmed_u8);
        const OutputRejectReason rung1_reject =
            rung1_aborted_repetition
                ? OutputRejectReason::Repetition
                : (rung1_empty ? OutputRejectReason::None
                               : ClassifyTranslationOutput(ToUtf16(trimmed_u8), cur, tgt_code));
        if (!rung1_empty && !rung1_echo && !rung1_bare_special && !rung1_aborted_echo &&
            rung1_reject == OutputRejectReason::None) {
            break; // acceptable rung-1 output — no completion-form retry
        }
        // RC-5 (v0.10.5 handoff section 2.6): a rung-1 ECHO on a model with a
        // real chat template (Hy-MT2) must NOT degrade to the raw completion
        // form - BuildCompletionPrompt has no template, and a chat-tuned
        // model continues it into hallucinated dialogue/definitions (the
        // RC-1 chain). Accept the echo: identity text is a legitimate answer
        // for loanwords and already-target words, and the caller's exact-echo
        // pass-through (EqualsSourceNeedsSendThrough) then forwards Enter
        // cleanly. This arm covers the FULL echo ONLY (unchanged by the WS1
        // adjudication below).
        if (rung1_echo && !rung1_bare_special && has_nontrivial_chat_template) {
            DIAG_F("ENGINE/Translate/048: rung-1 echo on a chat-template model accepted as-is (no raw-completion fallback)\n");
            break;
        }
        // WS1 (261108_0002, WS1-gates) ADJUDICATED 2026-11 follow-up: an
        // EARLY-ABORTED echo is NOT identity text - it is a PARTIAL SOURCE
        // PREFIX, and after the item-9b mixed-script fix the probe only aborts
        // when that prefix carries untranslated source-script characters (or
        // the source is script-homogeneous), i.e. exactly the case where the
        // model is copying rather than translating. Shipping the prefix would
        // overwrite the user's text with a truncated copy of itself; degrading
        // to the raw completion form on a chat-tuned model is the documented
        // hallucination chain (RC-5). The honest answer is engine_failed - the
        // app's Auto fallback / strict-local notice already covers it. The
        // diagnostic is separate (054) from the generic rejected-output arm so
        // the two shapes never get confused in a live log.
        if (rung1_aborted_echo && has_nontrivial_chat_template && !rung1_bare_special) {
            DIAG_F("ENGINE/Translate/054: rung-1 early-aborted source echo on a chat-template model; answering engine_failed (a partial source prefix is never shipped, WS1)\n");
            return std::nullopt;
        }
        // WS1 (261108_0002, WS1-gates) item 4b: a chat-template model never
        // runs the UNTEMPLATED rung 2. Rung 1 produced a real answer and the
        // output gate rejected it (or the repetition probe aborted it): the
        // raw completion form is the exact prompt that makes a chat-tuned model
        // continue into hallucinated dialogue, and the sampler is greedy at
        // temperature 0, so a re-decode of the same content cannot rescue it.
        // An honest engine_failed is strictly better than shipping garbage or
        // burning a second multi-second decode for a known-bad result. The
        // EMPTY decode and the bare special marker keep the historical retry
        // (rung 1 produced no answer at all, so there is nothing to preserve -
        // those arms are unchanged).
        if (has_nontrivial_chat_template && !rung1_empty && !rung1_bare_special) {
            DIAG_F("ENGINE/Translate/052: rung-1 output rejected on a chat-template model (%s); answering engine_failed instead of the untemplated completion form (WS1)\n",
                   rung1_aborted_repetition ? "repetition loop (early abort)"
                                            : OutputRejectReasonName(rung1_reject));
            return std::nullopt;
        }
        degraded_to_completion = true;
        DIAG_F("ENGINE/Translate/034: rung-1 chat form produced %s; retrying with the completion form (shape-only)\n",
               rung1_aborted_echo ? "a source-prefix echo (early abort)"
                                  : rung1_aborted_repetition
                                        ? "a repetition loop (R1-1 early abort)"
                                        : (rung1_empty ? "an empty decode"
                                                       : (rung1_echo ? "an echo of the source"
                                                                     : (rung1_bare_special
                                                                            ? "a bare special marker"
                                                                            : OutputRejectReasonName(rung1_reject)))));
    }
    } // REQ-059 attempt loop (rung 1 chat -> rung 2 completion)

    // REQ-059: the completion form can emit the same bare marker (defense:
    // the rung-1 check above only guards the retry decision) — answer
    // honestly with empty (-> engine_failed) instead of handing the caller
    // a control-token string.
    if (output_is_bare_special(trimmed_u8)) {
        DIAG_F("ENGINE/Translate/036: output is a bare special-token marker; answering engine_failed\n");
        return std::nullopt;
    }

    if (degraded_to_completion) {
        // R1-1 (261029_0001): the SAME gate runs on the completion-form result
        // (both rungs, model-agnostic). This rung is the last one, so a
        // rejection here is an HONEST failure: std::nullopt -> the worker maps
        // it to the frozen engine_failed (transient-retry / fallback UX is the
        // app's existing behavior). Garbage is never handed to the user.
        const OutputRejectReason rung2_reject =
            rung2_repetition_aborted
                ? OutputRejectReason::Repetition
                : ClassifyTranslationOutput(ToUtf16(trimmed_u8), cur, tgt_code);
        if (rung2_reject != OutputRejectReason::None) {
            DIAG_F("ENGINE/Translate/038: completion-form output rejected by the R1-1 gate (%s); answering engine_failed\n",
                   OutputRejectReasonName(rung2_reject));
            return std::nullopt;
        }
        DIAG_F("ENGINE/Translate/035: returning the completion-form result (shape-only)\n");
    }
    return trimmed_u8;
    }; // 260930_0003 (다): translate_once (per-chunk attempt ladder)

    // 260930_0003 (다): over-budget pre-check. Tokenize the FULL prompt once
    // (probe; debug-prompt log suppressed so a within-budget request emits
    // exactly the same diagnostics as before chunking). Within budget ->
    // the historical single-shot path, byte-identical. Over budget ->
    // paragraph-boundary chunking + stitching below.
    {
        std::vector<llama_token> probe_tokens;
        const int32_t n_full_prompt = tokenize_prompt(
            build_final_prompt(src_w, /*completion_form=*/false, /*suppress_debug_log=*/true),
            probe_tokens);
        if (n_full_prompt < 0) {
            return {};
        }
        if (!PromptNeedsChunking(n_full_prompt)) {
            const std::optional<std::string> single = translate_once(src_w, std::nullopt);
            if (!single) {
                return {};
            }
            return ToUtf16(*single);
        }

        // ---- 260930_0003 (다): over-budget paragraph chunking + stitching ----
        const ParagraphSplit split = SplitParagraphBlocks(src_w);
        if (split.blocks.empty()) {
            return {}; // impossible for a tokenized non-empty prompt; fail honestly
        }
        // Greedy measure-and-pack behind the injectable seam
        // (PackParagraphChunks in translation_common.hpp): every candidate is
        // measured through the REAL prompt builder (chat-template overhead
        // included), so a packed chunk can never drift over the budget,
        // whatever the model's vocabulary. The packer never emits a
        // zero-length chunk (P1: a leading blank-only range is force-extended
        // with the next block), so translate_once() always sees real text.
        auto measure_chunk = [&](size_t first, size_t last) -> int32_t {
            const std::wstring t = JoinParagraphBlocks(split, first, last);
            std::vector<llama_token> tmp;
            return tokenize_prompt(
                build_final_prompt(t, /*completion_form=*/false, /*suppress_debug_log=*/true),
                tmp);
        };
        const std::optional<std::vector<std::pair<size_t, size_t>>> ranges =
            PackParagraphChunks(split, kLlamaPromptTokenBudget, measure_chunk);
        if (!ranges) {
            return {}; // tokenizer rejected a mid-input candidate
        }
        std::vector<std::wstring> chunks;
        std::vector<std::wstring> chunk_trailing_separators;
        for (const std::pair<size_t, size_t>& range : *ranges) {
            std::wstring chunk_text = JoinParagraphBlocks(split, range.first, range.second);
            if (chunk_text.empty()) {
                // P1 defense-in-depth: the packer guarantees non-empty ranges,
                // but a blank-only chunk must never reach the decode ladder.
                DIAG_F("ENGINE/Translate/010: skipping zero-length chunk (blank-only block)\n");
                continue;
            }
            chunks.emplace_back(std::move(chunk_text));
            chunk_trailing_separators.emplace_back(
                range.second < split.separators.size()
                    ? split.separators[range.second]
                    : std::wstring());
        }
        if (chunks.empty()) {
            return {}; // blank-only input can never be over budget; fail honestly
        }
        DIAG_F("ENGINE/Translate/010: prompt tokens %d exceeded budget %d; input chunked into %zu parts at paragraph boundaries\n",
               n_full_prompt, kLlamaPromptTokenBudget, chunks.size());

        // Sequential in-order translation, then stitch with the ORIGINAL
        // separators verbatim. Each chunk runs the full attempt ladder with a
        // FRESH decode wall-clock budget and its own input-scaled generation
        // cap (ScaledMaxGenTokens / ScaledDecodeWallClockBudgetMs are computed
        // per chunk inside translate_once), so every chunk request stays
        // inside the wall-clock budget even though the whole input exceeds
        // the prompt budget. One failed chunk fails the whole request
        // honestly - never a partial stitch. A single chunk that alone
        // exceeds the budget keeps the historical head+tail fallback through
        // the REQ-R01 shrink loop inside translate_once.
        // Backend P1-3 (v0.10.5 handoff section 4.4 item 3): ONE accumulated
        // whole-request deadline (from here, the request start) bounds the
        // sequential chunk loop. Each chunk's decode budget is additionally
        // clamped to the remaining time inside translate_once; the loop
        // refuses to START a chunk past the deadline. Without this, N chunks
        // each burning up to 27 s blew the frozen 30 s host/client IPC
        // timeout cumulatively.
        const auto whole_request_deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(kChunkedRequestBudgetMs);
        std::wstring stitched;
        for (size_t k = 0; k < chunks.size(); ++k) {
            if (std::chrono::steady_clock::now() >= whole_request_deadline) {
                DIAG_F("ENGINE/Translate/049: chunked whole-request budget (%d ms) exhausted before chunk %zu/%zu; answering engine_failed\n",
                       kChunkedRequestBudgetMs, k + 1, chunks.size());
                return {};
            }
            const std::optional<std::string> part = translate_once(chunks[k], whole_request_deadline);
            if (!part) {
                return {};
            }
            stitched += ToUtf16(*part);
            stitched += chunk_trailing_separators[k];
        }
        // R2-8 (session 261029_0001, security-reviewer-mimo L-2): every chunk
        // passed the output gate on its OWN trimmed text, so a shape that only
        // exists after stitching (the live case: >= 3 chunks each emitting the
        // same non-needle line -> Rule A reassembles across the separators)
        // would ship un-gated. Re-run the CHEAP, model-agnostic repetition
        // predicate over the stitched whole: same predicate as the per-chunk
        // gate, no second decode, no second tokenize. The per-chunk gates stay
        // (cost boundary: a chunk must fail before the next one is decoded);
        // meta-leakage needles cannot reassemble across a chunk separator
        // (separators carry >= 2 line breaks and a needle is line-start only),
        // so repetition is the one predicate stitching can create.
        if (OutputHasRepetition(stitched, src_w)) {
            DIAG_F("ENGINE/Translate/041: stitched output is repeating (R2-8 whole-output re-gate); answering engine_failed\n");
            return {};
        }
        return stitched;
    }
}

#else // !HAVE_LLAMA_CPP

LocalInferenceEngine::LocalInferenceEngine() = default;
LocalInferenceEngine::~LocalInferenceEngine() = default;
void LocalInferenceEngine::Unload() {}
bool LocalInferenceEngine::EnsureLoaded(const std::string&) { return false; }
std::wstring LocalInferenceEngine::Translate(std::wstring_view, std::string_view,
                                             std::string_view, const std::string&,
                                             float, float, int, float,
                                             std::string_view) {
    // REQ-051 U-1 FIX 1: parity with the llama build — the exhaustion latch
    // is cleared at every Translate() entry even though the stub can never
    // exhaust the budget.
    decode_wall_clock_exhausted_ = false;
    return {};
}

#endif // HAVE_LLAMA_CPP

} // namespace emebalachat
