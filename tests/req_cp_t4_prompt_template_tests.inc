// ---------------------------------------------------------------------------
// REQ-CP T4 (session 260928_0001, design §8 + §9 row 6 + tech-gate A2):
// the worker-side PROMPT TEMPLATE REGISTRY.
//
// BuildPrompt gained a trailing `template_ref` (default "" = hymt2-official).
// The registry is a CLOSED enum-like set of compiled-in bodies; an unknown ref
// or the empty ref falls back FAIL-CLOSED to hymt2-official, so a bogus/absent
// ref can never reach a non-sanctioned prompt body. The three refs:
//   * hymt2-official        — EXISTING Tencent 100% Official Full SFT form,
//                             byte-identical incl. AUTO/unresolvable degenerate
//                             forms; ALSO the universal fallback. (The
//                             pre-existing R6/REQ-059 golden pins in run_tests.cpp
//                             already cover this body with the DEFAULT ref; here
//                             we pin explicit-ref == default-ref byte-identity.)
//   * subtitle-realtime-v1  — design §8.2 (EmebalaListener), copied verbatim.
//   * literary-flow-v1      — design §8.3 (EmebalaReader slot), copied verbatim.
//
// Per tech-gate amendment A2 the ref steers the rung-1 BuildPrompt ONLY; the
// rung-2 BuildCompletionPrompt retry is template-independent and keeps its
// single pinned form for ALL templates (REQ-059 pins cover that form).
//
//   (a) pure — the two NEW templates' English-source + Chinese-target forms,
//       {source}/{target}/{text} substitution, and the AUTO/unresolvable
//       "no hint token" degenerate path (mirrors hymt2-official's src_en-empty
//       path) stay byte-stable;
//   (b) fail-closed — unknown ref -> hymt2-official; "" ref -> hymt2-official;
//       explicit "hymt2-official" ref -> byte-identical to the default call;
//   (c) source pins — config.cpp/translation_common.cpp carry the registry +
//       the A2 threading (BuildPrompt template_ref param, Translate tail param,
//       worker pass-through point).
// BuildPrompt/BuildCompletionPrompt come from ../src/config.hpp (this TU
// includes it at the top); ResolveRepoFile source pins (run_tests.cpp L97).
// ---------------------------------------------------------------------------

void TestReqCpT4PromptTemplates() {
    std::cout << "[RUN] Testing REQ-CP T4 prompt template registry..." << std::endl;
    const int failures_before = g_failed_count;

    // ---- (a) subtitle-realtime-v1 (design §8.2, verbatim) -------------------
    // English-source canonical form: "Translate this subtitle line from
    // {source_en} into {target_en}. Output ONLY the translation: ... If the
    // input is not intelligible {source_en}, output the input unchanged.\n\n{text}"
    TEST_CHECK(BuildPrompt("hello world", "Korean", "English",
                           emebalachat::kPromptTemplateSubtitleRealtime) ==
                   "Translate this subtitle line from English into Korean. Output ONLY the "
                   "translation: one short line, no explanations, no quotation marks, no labels, "
                   "no added punctuation. Keep it as brief as the original; if the line is cut off "
                   "mid-sentence, translate only what is there and do not complete it. If the input "
                   "is not intelligible English, output the input unchanged.\n\nhello world",
               "REQ-CP T4 (a): subtitle-realtime-v1 English-source canonical form (verbatim)");

    // Chinese-target form (verbatim): "将以下{source_native}字幕翻译为{target_native}。只输出译文一行…"
    TEST_CHECK(BuildPrompt("hello", "ZH-CN", "Japanese",
                           emebalachat::kPromptTemplateSubtitleRealtime) ==
                   "将以下日本語字幕翻译为简体中文。只输出译文一行，不要解释，不要引号，不要标签，不要添加标点。"
                   "尽量与原文同样简短；若句子不完整，只翻译已有内容，不要补全。若内容无法辨认，原样输出原文。\n\nhello",
               "REQ-CP T4 (a): subtitle-realtime-v1 Chinese-target form (verbatim, native both sides)");

    // AUTO/unresolvable source -> NO hint token (byte-stable degenerate form,
    // mirrors hymt2-official's src_en-empty path). English branch:
    const std::string sub_base_en = BuildPrompt("hi", "Korean", "English",
                                                emebalachat::kPromptTemplateSubtitleRealtime);
    TEST_CHECK(BuildPrompt("hi", "Korean", "AUTO", emebalachat::kPromptTemplateSubtitleRealtime) ==
                   BuildPrompt("hi", "Korean", "", emebalachat::kPromptTemplateSubtitleRealtime),
               "REQ-CP T4 (a): subtitle AUTO source == empty source (English branch, no hint token)");
    TEST_CHECK(BuildPrompt("hi", "Korean", "", emebalachat::kPromptTemplateSubtitleRealtime) ==
                   "Translate this subtitle line from into Korean. Output ONLY the translation: "
                   "one short line, no explanations, no quotation marks, no labels, no added "
                   "punctuation. Keep it as brief as the original; if the line is cut off "
                   "mid-sentence, translate only what is there and do not complete it. If the input "
                   "is not intelligible , output the input unchanged.\n\nhi",
               "REQ-CP T4 (a): subtitle English-branch degenerate form drops the source token "
               "(byte-stable 'from into Korean' / 'not intelligible ,')");
    (void)sub_base_en;

    // Chinese branch AUTO source -> no {source_native} token (degenerate "将以下字幕翻译为…").
    const std::string sub_zh_base = BuildPrompt("hi", "ZH-CN", "AUTO",
                                                emebalachat::kPromptTemplateSubtitleRealtime);
    TEST_CHECK(sub_zh_base ==
                   "将以下字幕翻译为简体中文。只输出译文一行，不要解释，不要引号，不要标签，不要添加标点。"
                   "尽量与原文同样简短；若句子不完整，只翻译已有内容，不要补全。若内容无法辨认，原样输出原文。\n\nhi",
               "REQ-CP T4 (a): subtitle Chinese-branch AUTO source drops the native hint token "
               "(byte-stable degenerate '将以下字幕翻译为…')");
    TEST_CHECK(BuildPrompt("hi", "ZH-CN", "", emebalachat::kPromptTemplateSubtitleRealtime) == sub_zh_base,
               "REQ-CP T4 (a): subtitle Chinese-branch empty source == AUTO source (byte-identical)");

    // ---- (a) literary-flow-v1 (design §8.3, verbatim) -----------------------
    // English-source form: "Translate the following {source_en}text into
    // {target_en} for on-screen reading. Preserve the tone, register, and
    // paragraph structure of the original. Output only the translation, without
    // explanations or annotations.\n\n{text}"
    TEST_CHECK(BuildPrompt("paragraph one.", "Korean", "English",
                           emebalachat::kPromptTemplateLiteraryFlow) ==
                   "Translate the following English text into Korean for on-screen reading. Preserve "
                   "the tone, register, and paragraph structure of the original. Output only the "
                   "translation, without explanations or annotations.\n\nparagraph one.",
               "REQ-CP T4 (a): literary-flow-v1 English-source form (verbatim)");

    // Chinese-target form (verbatim): "将以下{source_native}文本翻译为{target_native}，供屏幕阅读使用。…"
    TEST_CHECK(BuildPrompt("hello", "ZH-TW", "Japanese",
                           emebalachat::kPromptTemplateLiteraryFlow) ==
                   "将以下日本語文本翻译为繁體中文，供屏幕阅读使用。保持原文的语气与段落结构。只输出译文，不要任何解释或注释。\n\nhello",
               "REQ-CP T4 (a): literary-flow-v1 Chinese-target form (verbatim, native both sides)");

    // AUTO/absent source -> no source token (byte-stable degenerate form).
    TEST_CHECK(BuildPrompt("hi", "Korean", "AUTO", emebalachat::kPromptTemplateLiteraryFlow) ==
                   BuildPrompt("hi", "Korean", "", emebalachat::kPromptTemplateLiteraryFlow),
               "REQ-CP T4 (a): literary AUTO source == empty source (English branch, no hint token)");
    TEST_CHECK(BuildPrompt("hi", "Korean", "", emebalachat::kPromptTemplateLiteraryFlow) ==
                   "Translate the following text into Korean for on-screen reading. Preserve the "
                   "tone, register, and paragraph structure of the original. Output only the "
                   "translation, without explanations or annotations.\n\nhi",
               "REQ-CP T4 (a): literary English-branch degenerate form drops the source token");
    const std::string lit_zh_auto = BuildPrompt("hi", "ZH-CN", "AUTO",
                                                emebalachat::kPromptTemplateLiteraryFlow);
    TEST_CHECK(lit_zh_auto ==
                   "将以下文本翻译为简体中文，供屏幕阅读使用。保持原文的语气与段落结构。只输出译文，不要任何解释或注释。\n\nhi",
               "REQ-CP T4 (a): literary Chinese-branch AUTO source drops the native hint token");
    TEST_CHECK(BuildPrompt("hi", "ZH-CN", "", emebalachat::kPromptTemplateLiteraryFlow) == lit_zh_auto,
               "REQ-CP T4 (a): literary Chinese-branch empty source == AUTO source (byte-identical)");

    // ---- (b) fail-closed registry ------------------------------------------
    // Unknown ref -> hymt2-official (byte-identical to the default no-ref call).
    const std::string official_default = BuildPrompt("今日はいい天気ですね。", "English", "Japanese");
    TEST_CHECK(BuildPrompt("今日はいい天気ですね。", "English", "Japanese", "bogus-template-v9") ==
                   official_default,
               "REQ-CP T4 (b): unknown template ref falls back fail-closed to hymt2-official");
    // "" ref -> hymt2-official (byte-identical to the default no-ref call).
    TEST_CHECK(BuildPrompt("今日はいい天気ですね。", "English", "Japanese", "") == official_default,
               "REQ-CP T4 (b): empty template ref == hymt2-official byte-identity");
    // Explicit "hymt2-official" ref -> byte-identical to the default call.
    TEST_CHECK(BuildPrompt("今日はいい天気ですね。", "English", "Japanese",
                           emebalachat::kPromptTemplateHymt2Official) == official_default,
               "REQ-CP T4 (b): explicit hymt2-official ref == default byte-identity");
    // Fail-closed also holds on the Chinese branch.
    const std::string official_zh = BuildPrompt("hello", "ZH-CN", "JA");
    TEST_CHECK(BuildPrompt("hello", "ZH-CN", "JA", "unknown") == official_zh,
               "REQ-CP T4 (b): unknown ref falls back on the Chinese branch too");
    // The new-template refs must NOT alias the official body.
    TEST_CHECK(BuildPrompt("hi", "Korean", "English", emebalachat::kPromptTemplateSubtitleRealtime) !=
                   official_default,
               "REQ-CP T4 (b): subtitle-realtime-v1 body is distinct from hymt2-official");

    // ---- (c) source pins: the registry + the A2 threading ------------------
    const std::filesystem::path cfg_cpp = ResolveRepoFile(L"src\\config.cpp");
    if (cfg_cpp.empty()) {
        std::cout << "[SKIP] src/config.cpp not resolvable; REQ-CP T4 (c) pins skipped." << std::endl;
    } else {
        std::ifstream in(cfg_cpp, std::ios::binary);
        const std::string src((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
        // The registry constants exist (design §8 closed set).
        TEST_CHECK(src.find("kPromptTemplateSubtitleRealtime") != std::string::npos &&
                       src.find("kPromptTemplateLiteraryFlow") != std::string::npos,
                   "REQ-CP T4 (c): config.cpp carries the subtitle-realtime-v1 + literary-flow-v1 refs");
        // Fail-closed fallback to hymt2-official on unknown ref.
        TEST_CHECK(src.find("Unknown ref: fall through to hymt2-official") != std::string::npos,
                   "REQ-CP T4 (c): config.cpp fails closed to hymt2-official on an unknown ref");
        // The subtitle body is present verbatim (design §8.2 anchor).
        TEST_CHECK(src.find("Translate this subtitle line from ") != std::string::npos &&
                       src.find("output the input unchanged") != std::string::npos,
                   "REQ-CP T4 (c): config.cpp carries the subtitle-realtime-v1 body (§8.2)");
        // The literary body is present verbatim (design §8.3 anchor).
        TEST_CHECK(src.find("for on-screen reading") != std::string::npos,
                   "REQ-CP T4 (c): config.cpp carries the literary-flow-v1 body (§8.3)");
    }

    const std::filesystem::path tc_cpp = ResolveRepoFile(L"src\\engine_core\\translation_common.cpp");
    if (tc_cpp.empty()) {
        std::cout << "[SKIP] src/engine_core/translation_common.cpp not resolvable; REQ-CP T4 (c) pins skipped."
                  << std::endl;
    } else {
        std::ifstream in(tc_cpp, std::ios::binary);
        const std::string src((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
        // A2: Translate carries the template_ref tail param.
        TEST_CHECK(src.find("std::string_view template_ref") != std::string::npos,
                   "REQ-CP T4 (c): translation_common.cpp Translate threads the template_ref param");
        // A2: the rung-1 BuildPrompt call passes the ref; rung-2 completion form does not.
        TEST_CHECK(src.find("BuildPrompt(u8, tgt_name, src_name, template_ref)") != std::string::npos &&
                       src.find("BuildCompletionPrompt(u8, tgt_name, src_name)") != std::string::npos,
                   "REQ-CP T4 (c): rung-1 BuildPrompt gets the ref; rung-2 completion form stays "
                   "template-independent (A2)");
        // No-llama stub parity: the stub signature also carries the tail param.
        const size_t stub_pos = src.find("#else // !HAVE_LLAMA_CPP");
        TEST_CHECK(stub_pos != std::string::npos &&
                       src.find("std::string_view)", stub_pos) != std::string::npos,
                   "REQ-CP T4 (c): the no-llama Translate stub carries the template_ref tail param (parity)");
    }

    const std::filesystem::path worker_cpp = ResolveRepoFile(L"src\\ggml_translate_worker.cpp");
    if (worker_cpp.empty()) {
        std::cout << "[SKIP] src/ggml_translate_worker.cpp not resolvable; REQ-CP T4 (c) pins skipped."
                  << std::endl;
    } else {
        std::ifstream in(worker_cpp, std::ios::binary);
        const std::string src((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
        // The pass-through point exists and forwards the (currently empty) ref,
        // ready for T5 to connect job.prompt_template.
        TEST_CHECK(src.find("template_ref=") != std::string::npos &&
                       src.find("job.prompt_template") != std::string::npos,
                   "REQ-CP T4 (c): the worker Translate pass-through forwards the template ref "
                   "(T5 connects job.prompt_template)");
    }

    std::cout << "[DONE] REQ-CP T4 prompt template registry (failures: "
              << (g_failed_count - failures_before) << ")" << std::endl;
}
