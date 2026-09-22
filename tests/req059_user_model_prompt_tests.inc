// ---------------------------------------------------------------------------
// REQ-059: user GGUF models must actually translate.
// Live evidence (tools/fidelity_probe.exe, greedy, MiLMMT-46-4B Q5_K_M — a
// Gemma3 MT model whose GGUF ships the TRIVIAL chat template {% for message
// in messages %}{{ message.content }}{% endfor %}):
//   - bare fixed Hy-MT2 instruction through the trivial template -> EMPTY
//     decode (immediate EOS) -> engine_failed;
//   - ChatML-wrapped fixed instruction: EN->KO works (stray leading ":"),
//     KO->EN ECHOES the source;
//   - the model family's official completion form ("Translate this from
//     <src_en> to <tgt_en>:\n<src_en>: <text>\n<tgt_en>:") -> PERFECT both
//     directions;
//   - Hy-MT2 (real non-trivial template) control: perfect, unchanged.
// Fix: a fixed two-rung ladder in translation_common.cpp. Rung 1: trivial
// template detected (formatting == plain content) or absent -> wrap with
// llama's BUILT-IN default template (ChatML); non-trivial templates keep the
// existing path byte-identically. Rung 2: if rung 1 decodes EMPTY or ECHOES
// the source, retry ONCE with BuildCompletionPrompt (config.cpp, this suite
// pins its exact forms) under a fresh decode wall-clock budget. Plus a
// leading-colon cleanup in the existing post-processing.
//   (a) pure — BuildCompletionPrompt's four pinned forms;
//   (b) source pins — translation_common.cpp rung-1/rung-2 structure and the
//       colon strip; config.hpp declares the helper.
// ResolveRepoFile source pins (L97 of run_tests.cpp); unresolvable file ->
// the section is skipped, exactly like the req048_r2 pattern.
// ---------------------------------------------------------------------------

void TestReq059UserModelPrompt() {
    std::cout << "[RUN] Testing REQ-059 user-model prompt ladder..." << std::endl;
    const int failures_before = g_failed_count;

    // ---- (a) pure: BuildCompletionPrompt's pinned forms --------------------
    TEST_CHECK(BuildCompletionPrompt("hello", "Korean", "English") ==
                   "Translate this from English to Korean:\nEnglish: hello\nKorean:",
               "REQ-059 (a): resolved source -> the official two-sided completion form");
    TEST_CHECK(BuildCompletionPrompt("안녕하세요", "English", "Korean") ==
                   "Translate this from Korean to English:\nKorean: 안녕하세요\nEnglish:",
               "REQ-059 (a): ko->en rides the same two-sided form");
    TEST_CHECK(BuildCompletionPrompt("hello", "Korean", "AUTO") ==
                   "Translate into Korean:\nhello\nKorean:",
               "REQ-059 (a): AUTO source -> the target-side-only form");
    TEST_CHECK(BuildCompletionPrompt("hello", "Korean") ==
                   "Translate into Korean:\nhello\nKorean:",
               "REQ-059 (a): absent source argument defaults to the same form");
    TEST_CHECK(BuildCompletionPrompt("hello", "Klingon", "English") ==
                   "Translate this from English to Klingon:\nEnglish: hello\nKlingon:",
               "REQ-059 (a): unresolvable target token is injected raw "
               "(BuildPrompt's historical fallback)");

    // ---- (b) source pins: the ladder in translation_common.cpp -------------
    const std::filesystem::path tc_cpp = ResolveRepoFile(L"src\\engine_core\\translation_common.cpp");
    if (tc_cpp.empty()) {
        std::cout << "[SKIP] src/engine_core/translation_common.cpp not resolvable; "
                     "REQ-059 (b) pins skipped." << std::endl;
    } else {
        std::ifstream in(tc_cpp, std::ios::binary);
        const std::string src((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
        // Rung 1: trivial/absent template falls back to llama's built-in
        // default template (ChatML).
        TEST_CHECK(src.find("llama_chat_apply_template(nullptr") != std::string::npos,
                   "REQ-059 (b): the trivial-template fallback wraps with the "
                   "built-in default template");
        TEST_CHECK(src.find("chat_tmpl_is_trivial") != std::string::npos,
                   "REQ-059 (b): the trivial-template probe exists");
        // Rung 2: BuildCompletionPrompt is referenced inside Translate, and
        // rung 1 (template application) is tried FIRST.
        const size_t translate_pos = src.find("LocalInferenceEngine::Translate(");
        const size_t completion_pos = src.find("BuildCompletionPrompt", translate_pos);
        TEST_CHECK(translate_pos != std::string::npos && completion_pos != std::string::npos,
                   "REQ-059 (b): LocalInferenceEngine::Translate references "
                   "BuildCompletionPrompt (the rung-2 retry)");
        const size_t rung1_pos = src.find("chat_tmpl_is_trivial");
        TEST_CHECK(rung1_pos != std::string::npos && completion_pos != std::string::npos &&
                       rung1_pos < completion_pos,
                   "REQ-059 (b): rung-1 template application precedes the "
                   "completion-form reference (Hy-MT2 tried first)");
        // The empty/echo retry condition exists.
        TEST_CHECK(src.find("rung1_echo") != std::string::npos,
                   "REQ-059 (b): the empty/echo rung-1 acceptance test exists");
        // The leading-colon cleanup sits in the post-processing.
        TEST_CHECK(src.find("trimmed_u8.front() == ':'") != std::string::npos,
                   "REQ-059 (b): the leading-colon cleanup exists");
        // REQ-059 (live-verification follow-up): the stop condition catches
        // end-of-turn tokens (Gemma-family eot is neither eos nor eog — the
        // live MiLM rung-1 decode leaked "<|im_end|>" into the output), and
        // the piece append skips control-class pieces via control_token_texts.
        const size_t stop_pos = src.find("llama_vocab_is_eog(vocab, token)");
        TEST_CHECK(stop_pos != std::string::npos &&
                       src.find("llama_vocab_eot(vocab)", stop_pos) != std::string::npos,
                   "REQ-059 (b): the EOS check carries the llama_vocab_eot disjunct");
        const size_t piece_pos = src.find("llama_token_to_piece(vocab, token");
        TEST_CHECK(piece_pos != std::string::npos &&
                       src.find("control_token_texts.end()", piece_pos) != std::string::npos,
                   "REQ-059 (b): the piece append scrubs control-class pieces "
                   "(control_token_texts)");
        // REQ-059 live-verification follow-up: the traced MiLM failure was
        // "<|im_end|>" spelled out of SIX NORMAL tokens — the bare-special
        // rejection compares the output against the vocab's declared special
        // token texts (eos/eot/bos/sep/pad) and degrades rung 1 exactly like
        // empty/echo; the final guard answers engine_failed if rung 2 emits
        // the same marker.
        TEST_CHECK(src.find("output_is_bare_special") != std::string::npos &&
                       src.find("llama_vocab_get_text(vocab, id)") != std::string::npos,
                   "REQ-059 (b): the bare-special rejection exists and compares "
                   "against the declared special token texts");
        TEST_CHECK(src.find("\"<|im_end|>\"") != std::string::npos,
                   "REQ-059 (b): the bare-special rejection covers the fixed ChatML "
                   "turn markers (live MiLM spelled <|im_end|> from NORMAL tokens)");
        {
            const size_t bare_pos = src.find("output_is_bare_special");
            TEST_CHECK(src.find("rung1_bare_special") != std::string::npos &&
                           bare_pos != std::string::npos && bare_pos < src.find("rung1_bare_special"),
                       "REQ-059 (b): the bare-special predicate is defined before "
                       "the rung-1 acceptance test");
            TEST_CHECK(src.find("Translate/036") != std::string::npos,
                       "REQ-059 (b): the final bare-special guard answers engine_failed");
        }
    }

    // config.hpp declares the helper.
    const std::filesystem::path cfg_hpp = ResolveRepoFile(L"src\\config.hpp");
    if (cfg_hpp.empty()) {
        std::cout << "[SKIP] src/config.hpp not resolvable; REQ-059 header pin skipped."
                  << std::endl;
    } else {
        std::ifstream in(cfg_hpp, std::ios::binary);
        const std::string src((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
        TEST_CHECK(src.find("BuildCompletionPrompt") != std::string::npos,
                   "REQ-059 (b): config.hpp declares BuildCompletionPrompt");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-059 user-model prompt ladder checks passed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-059 user-model prompt ladder: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}
