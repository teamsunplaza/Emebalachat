// ---------------------------------------------------------------------------
// 261124_0002 (session plamo-jinja): user GGUF models whose embedded chat
// template the LEGACY catalog probe (llama_chat_apply_template) cannot name
// used to degrade to the Hy-MT2 completion form and answer engine_failed
// (live evidence: plamo3 PFML3 -> "rung 1 uses the completion form (WS1)" ->
// repetition abort). The rung-1 ladder now renders such templates through
// llama.cpp's OWN vendored jinja engine (src/engine_core/jinja_chat_render.*,
// Emebala_llama_jinja target). Checks:
//   (a) pure — NormalizeJinjaTemplateLangCode pins (incl. COD-05 shape bar);
//   (b) behavioral — JinjaChatTemplateRender on fixture templates (unknown-
//       jinja render, kwargs/bos/eos wiring, no-kwargs retry shape,
//       raise_exception degradation, COD-01 empty/verbatim render bar),
//       guarded by EMEBALA_HAVE_LLAMA_JINJA;
//   (c) source pins — the translation_common.cpp integration (catalog-failed
//       gate, kwargs verdict skip, strips with provenance guards, COD-07
//       non-degradation, SEC-01 cap/watchdog/catch, DIAG renumber 061/062);
//   (d) SEC-01/SEC-02 facade checks — template cap, kwargs-verdict cache +
//       per-model reset, render watchdog containment with an infinite-loop
//       jinja fixture (bounded join, negative cache, process-lifetime latch);
//   (e) COD-02+VFY-01 / COD-08 — the pure BOS/EOS strip-guard helpers.
// Staged as an .inc next to this runner; registered right after
// TestReq059UserModelPrompt() (same translation-core cluster).
// ---------------------------------------------------------------------------

void TestReq059JinjaChatTemplate() {
    std::cout << "[RUN] Testing 261124_0002 vendored-jinja chat-template path..." << std::endl;
    const int failures_before = g_failed_count;

    // ---- (a) pure: NormalizeJinjaTemplateLangCode ---------------------------
    TEST_CHECK(NormalizeJinjaTemplateLangCode("ZH-CN") == "zh-hans",
               "261124_0002 (a): ZH-CN folds to zh-hans");
    TEST_CHECK(NormalizeJinjaTemplateLangCode("ZH-TW") == "zh-hant",
               "261124_0002 (a): ZH-TW folds to zh-hant");
    TEST_CHECK(NormalizeJinjaTemplateLangCode("zh") == "zh-hans",
               "261124_0002 (a): bare zh folds to zh-hans");
    TEST_CHECK(NormalizeJinjaTemplateLangCode("zh-hant") == "zh-hant",
               "261124_0002 (a): zh-hant is stable");
    TEST_CHECK(NormalizeJinjaTemplateLangCode("zh-hk") == "zh-hant",
               "261124_0002 (a): zh-hk folds to zh-hant");
    TEST_CHECK(NormalizeJinjaTemplateLangCode("KO") == "ko",
               "261124_0002 (a): KO lowercases");
    TEST_CHECK(NormalizeJinjaTemplateLangCode("JA") == "ja",
               "261124_0002 (a): JA lowercases");
    TEST_CHECK(NormalizeJinjaTemplateLangCode("EN") == "en",
               "261124_0002 (a): EN lowercases");
    TEST_CHECK(NormalizeJinjaTemplateLangCode("YUE") == "yue",
               "261124_0002 (a): YUE passes through lowercased (3-letter code)");
    TEST_CHECK(NormalizeJinjaTemplateLangCode("AUTO").empty(),
               "261124_0002 (a): AUTO maps to no kwarg");
    TEST_CHECK(NormalizeJinjaTemplateLangCode("").empty(),
               "261124_0002 (a): empty maps to no kwarg");
    // COD-05 (fix round 1): the strict [a-z-]{2,12} shape bar — a bad token
    // falls back to no-kwargs instead of passing through to the template.
    TEST_CHECK(NormalizeJinjaTemplateLangCode("K0").empty(),
               "261124_0002 (a) COD-05: a digit inside the code maps to no kwarg");
    TEST_CHECK(NormalizeJinjaTemplateLangCode("ko_kr").empty(),
               "261124_0002 (a) COD-05: an underscore maps to no kwarg");
    TEST_CHECK(NormalizeJinjaTemplateLangCode("k").empty(),
               "261124_0002 (a) COD-05: a 1-char token maps to no kwarg");
    TEST_CHECK(NormalizeJinjaTemplateLangCode("abcdefghijklm").empty(),
               "261124_0002 (a) COD-05: a 13-char token maps to no kwarg");
    TEST_CHECK(NormalizeJinjaTemplateLangCode("pt-br") == "pt-br",
               "261124_0002 (a) COD-05: a hyphenated 5-char region code passes");
    // R2-COD-05 (fix round 2): the char set alone admitted letter-less /
    // one-letter shapes ("--", "-", "a-", "-a"). The bar now requires at least
    // two ASCII letters, so none of them can reach a strict template validator.
    TEST_CHECK(NormalizeJinjaTemplateLangCode("--").empty(),
               "261124_0002 (a) R2-COD-05: an all-hyphen token maps to no kwarg");
    TEST_CHECK(NormalizeJinjaTemplateLangCode("-").empty(),
               "261124_0002 (a) R2-COD-05: a hyphen-only token maps to no kwarg");
    TEST_CHECK(NormalizeJinjaTemplateLangCode("a-").empty(),
               "261124_0002 (a) R2-COD-05: a one-letter + hyphen token maps to no kwarg");
    TEST_CHECK(NormalizeJinjaTemplateLangCode("-a").empty(),
               "261124_0002 (a) R2-COD-05: a hyphen + one-letter token maps to no kwarg");
    TEST_CHECK(NormalizeJinjaTemplateLangCode("a-b") == "a-b",
               "261124_0002 (a) R2-COD-05: a two-letter hyphenated shape still passes");
    TEST_CHECK(NormalizeJinjaTemplateLangCode("zh-hans") == "zh-hans",
               "261124_0002 (a) R2-COD-05: the 7-char script-subtag form still passes");

    // ---- (b) behavioral: the renderer itself --------------------------------
#ifdef EMEBALA_HAVE_LLAMA_JINJA
    {
        // (b1) an unknown-jinja template (the legacy catalog would return -1
        // for these markers) renders through the vendored engine.
        JinjaChatTemplateRenderArgs args{};
        args.template_source =
            "PRE|{% for m in messages %}U:{{ m['role'] }}|{{ m['content'] }};{% endfor %}"
            "{% if add_generation_prompt %}GEN{% endif %}";
        args.user_content = "hello";
        std::string out;
        TEST_CHECK(JinjaChatTemplateRender(args, out) &&
                       out == "PRE|U:user|hello;GEN",
                   "261124_0002 (b1): unknown-jinja template renders through the "
                   "vendored engine");
    }
    {
        // (b2) bos/eos + kwargs reach the template as top-level globals (the
        // common/chat.cpp direct_apply contract); omitted kwargs stay
        // undefined and render empty.
        JinjaChatTemplateRenderArgs args{};
        args.template_source =
            "{{ bos_token }}|{% for m in messages %}{{ m['content'] }}{% endfor %}"
            "|{{ target_lang }}|{{ source_lang }}|{{ eos_token }}";
        args.user_content = "hello";
        args.bos_token = "<s>";
        args.eos_token = "</s>";
        args.target_lang = "en";
        args.source_lang = "ja";
        std::string out;
        TEST_CHECK(JinjaChatTemplateRender(args, out) &&
                       out == "<s>|hello|en|ja|</s>",
                   "261124_0002 (b2): bos/eos + kwargs reach the template");
        args.target_lang = {};
        args.source_lang = {};
        out.clear();
        TEST_CHECK(JinjaChatTemplateRender(args, out) &&
                       out == "<s>|hello|||</s>",
                   "261124_0002 (b2): omitted kwargs stay undefined (the "
                   "retry-without-kwargs shape)");
    }
    {
        // (b3) a template-side raise_exception degrades safely: false, output
        // untouched, no throw.
        JinjaChatTemplateRenderArgs args{};
        args.template_source = "{% if true %}{{ raise_exception('boom') }}{% endif %}x";
        args.user_content = "hello";
        std::string out = "SENTINEL";
        TEST_CHECK(!JinjaChatTemplateRender(args, out) && out == "SENTINEL",
                   "261124_0002 (b3): raise_exception degrades safely (false, "
                   "output untouched)");
        // a lexically broken template fails the same way
        args.template_source = "{% for m in messages %}";
        out = "SENTINEL";
        TEST_CHECK(!JinjaChatTemplateRender(args, out) && out == "SENTINEL",
                   "261124_0002 (b3): unparseable template degrades safely");
    }
    {
        // (b4) PFML3-shaped validator: target_lang defaults to 'auto' when
        // undefined; a code failing check_lang raises (the caller's
        // retry-without-kwargs then rescues the request).
        JinjaChatTemplateRenderArgs args{};
        args.template_source =
            "{%- if target_lang is not defined -%}{%- set target_lang = 'auto' -%}{%- endif -%}"
            "{%- macro check_lang(v) -%}"
            "{%- if v != 'en' and v != 'ja' -%}{{ raise_exception('bad lang') }}{%- endif -%}"
            "{%- endmacro -%}"
            "{%- if target_lang != 'auto' -%}{{ check_lang(target_lang) }}{%- endif -%}"
            "T={{ target_lang }};{% for m in messages %}{{ m['content'] }}{% endfor %}";
        args.user_content = "hello";
        std::string out;
        args.target_lang = "en";
        TEST_CHECK(JinjaChatTemplateRender(args, out) && out == "T=en;hello",
                   "261124_0002 (b4): accepted target_lang renders");
        args.target_lang = "ko";
        out = "SENTINEL";
        TEST_CHECK(!JinjaChatTemplateRender(args, out) && out == "SENTINEL",
                   "261124_0002 (b4): rejected target_lang raises -> false");
        args.target_lang = {};
        out.clear();
        TEST_CHECK(JinjaChatTemplateRender(args, out) && out == "T=auto;hello",
                   "261124_0002 (b4): undefined target_lang defaults to auto "
                   "(the no-kwargs retry outcome)");
    }
    {
        // (b5) COD-01 (fix round 1): the probe's empty/verbatim bar on the RAW
        // render — a template that renders nothing, or that renders exactly
        // the user content (a concat template), adds no structure and is a
        // render failure. (The caller applies the same bar AGAIN after its
        // BOS/EOS strips; a bos-only template is emptied by the strip.)
        JinjaChatTemplateRenderArgs args{};
        args.template_source = "{% if false %}x{% endif %}";
        args.user_content = "hello";
        std::string out = "SENTINEL";
        TEST_CHECK(JinjaChatTemplateRenderEx(args, out) == JinjaRenderOutcome::Failed &&
                       out == "SENTINEL",
                   "261124_0002 (b5) COD-01: an empty render is a render failure");
        args.template_source = "{% for m in messages %}{{ m['content'] }}{% endfor %}";
        out = "SENTINEL";
        TEST_CHECK(JinjaChatTemplateRenderEx(args, out) == JinjaRenderOutcome::Failed &&
                       out == "SENTINEL",
                   "261124_0002 (b5) COD-01: a verbatim (concat) render is a render failure");
        args.template_source = "WRAP[{{ bos_token }}]{% for m in messages %}{{ m['content'] }}{% endfor %}";
        args.bos_token = "<s>";
        out.clear();
        TEST_CHECK(JinjaChatTemplateRenderEx(args, out) == JinjaRenderOutcome::Rendered &&
                       out == "WRAP[<s>]hello",
                   "261124_0002 (b5): a structured render still passes (regression guard)");
    }
#else
    std::cout << "[SKIP] EMEBALA_HAVE_LLAMA_JINJA undefined; 261124_0002 (b) "
                 "renderer checks skipped." << std::endl;
#endif

    // ---- (e) COD-02+VFY-01 / COD-08: the pure strip-guard helpers -----------
    // Strip ONE leading/trailing special spelling only when the render
    // starts/ends with the exact vocab piece AND the FULL pre-render input
    // (prev_chunk_context + request_context + user text — VFY-01: the context
    // prefixes are NOT SEC-B2-scrubbed) does not.
    TEST_CHECK(JinjaShouldStripLeadingBos("<s>hello", "<s>", "hello"),
               "261124_0002 (e): leading BOS strips when the input does not begin with it");
    TEST_CHECK(!JinjaShouldStripLeadingBos("<s>hello", "<s>", "<s>hello"),
               "261124_0002 (e) VFY-01: leading BOS is kept when the pre-render input itself begins with it");
    TEST_CHECK(!JinjaShouldStripLeadingBos("hello<s>", "<s>", "hello"),
               "261124_0002 (e): no leading BOS -> no strip");
    TEST_CHECK(!JinjaShouldStripLeadingBos("hello", "", "hello"),
               "261124_0002 (e): empty BOS spelling -> no strip");
    TEST_CHECK(JinjaShouldStripLeadingBos("<s>", "<s>", "ab"),
               "261124_0002 (e): a bos-only render passes the guard (the caller's empty bar then catches it)");
    TEST_CHECK(JinjaShouldStripTrailingEos("hello</s>", "</s>", "hello", true),
               "261124_0002 (e) COD-08: trailing EOS strips when the input does not end with it");
    TEST_CHECK(!JinjaShouldStripTrailingEos("hello</s>", "</s>", "hello</s>", true),
               "261124_0002 (e) COD-08+VFY-01: trailing EOS is kept when the pre-render input itself ends with it");
    TEST_CHECK(!JinjaShouldStripTrailingEos("</s>hello", "</s>", "hello", true),
               "261124_0002 (e) COD-08: no trailing EOS -> no strip");
    TEST_CHECK(!JinjaShouldStripTrailingEos("he", "</s>", "he", true),
               "261124_0002 (e) COD-08: a render shorter than the EOS spelling -> no strip");
    TEST_CHECK(!JinjaShouldStripTrailingEos("hello", "", "hello", true),
               "261124_0002 (e) COD-08: empty EOS spelling -> no strip");
    // R2-COD-04 (fix round 2): the reference's add_eos gate
    // (common/chat.cpp:1141-1147) rides INSIDE the pure helper, so the
    // add_eos=false leg is pinned here instead of at the call site: a vocab
    // that does not ask for an EOS keeps the template's own trailing EOS,
    // mirroring the add_bos=false BOS leg.
    TEST_CHECK(!JinjaShouldStripTrailingEos("hello</s>", "</s>", "hello", false),
               "261124_0002 (e) R2-COD-04: an add_eos=false vocab keeps the template's trailing EOS");
    TEST_CHECK(!JinjaShouldStripTrailingEos("hello</s>", "</s>", "hello</s>", false),
               "261124_0002 (e) R2-COD-04: add_eos=false also keeps an input-terminated EOS");

    // ---- (d) SEC-01/SEC-02 facade checks ------------------------------------
#ifdef EMEBALA_HAVE_LLAMA_JINJA
    TEST_CHECK(kJinjaTemplateSourceCapBytes == 64 * 1024 &&
                   kJinjaRenderWatchdogMs == 30000,
               "261124_0002 (d): the named SEC-01 constants are 64 KiB and 30 s");
    {
        // SEC-01 (1): the template-source cap. At-cap renders; one byte over
        // fails before the vendored engine ever sees the source.
        const std::string at_cap(kJinjaTemplateSourceCapBytes, 'a');
        JinjaChatTemplateRenderArgs args{};
        args.template_source = at_cap;
        args.user_content = "hello";
        std::string out;
        TEST_CHECK(JinjaChatTemplateRenderEx(args, out) == JinjaRenderOutcome::Rendered &&
                       out == at_cap,
                   "261124_0002 (d) SEC-01: an exactly-64-KiB template still renders");
        const std::string over_cap = at_cap + "b";
        args.template_source = over_cap;
        out = "SENTINEL";
        TEST_CHECK(JinjaChatTemplateRenderEx(args, out) == JinjaRenderOutcome::Failed &&
                       out == "SENTINEL",
                   "261124_0002 (d) SEC-01: a 64-KiB+1 template is refused");
    }
    {
        // R2-COD-02 (fix round 2): the renderer's no-throw contract on the
        // guarded entry/spawn leg. A portable thread-creation/OOM injection
        // does not exist in this harness, so the observable half is pinned here
        // (every entry-leg refusal answers an outcome instead of throwing) and
        // the structure half is pinned by the (c) source pins.
        JinjaChatTemplateRenderArgs args{};
        std::string out = "SENTINEL";
        bool threw = false;
        try {
            threw = JinjaChatTemplateRenderEx(args, out) != JinjaRenderOutcome::Failed;
        } catch (...) {
            threw = true;
        }
        TEST_CHECK(!threw && out == "SENTINEL",
                   "261124_0002 (d) R2-COD-02: an empty template answers Failed without throwing "
                   "(the pre-spawn leg is caught)");
        args.template_source = std::string(kJinjaTemplateSourceCapBytes + 1, 'x');
        out = "SENTINEL";
        threw = false;
        try {
            threw = JinjaChatTemplateRenderEx(args, out) != JinjaRenderOutcome::Failed;
        } catch (...) {
            threw = true;
        }
        TEST_CHECK(!threw && out == "SENTINEL",
                   "261124_0002 (d) R2-COD-02: an over-cap template answers Failed without throwing");
    }
    {
        // SEC-02: the kwargs verdict cache. A kwargs render that fails once is
        // remembered for the template; JinjaChatTemplateResetForModelChange()
        // (Unload() calls it) scopes the cache to one loaded model.
        const std::string tmpl =
            "{%- if target_lang is not defined -%}{%- set target_lang = 'auto' -%}{%- endif -%}"
            "{%- macro check_lang(v) -%}"
            "{%- if v != 'en' -%}{{ raise_exception('bad lang') }}{%- endif -%}"
            "{%- endmacro -%}"
            "{%- if target_lang != 'auto' -%}{{ check_lang(target_lang) }}{%- endif -%}"
            "T={{ target_lang }};{% for m in messages %}{{ m['content'] }}{% endfor %}";
        JinjaChatTemplateRenderArgs args{};
        args.template_source = tmpl;
        args.user_content = "hello";
        args.target_lang = "ko";
        std::string out = "SENTINEL";
        TEST_CHECK(JinjaChatTemplateRenderEx(args, out) == JinjaRenderOutcome::Failed,
                   "261124_0002 (d) SEC-02: a rejected kwargs render fails");
        TEST_CHECK(JinjaChatTemplateKwargsFailed(tmpl, "ko", ""),
                   "261124_0002 (d) SEC-02: the failing kwargs verdict is cached");
        TEST_CHECK(!JinjaChatTemplateKwargsFailed(tmpl, "en", ""),
                   "261124_0002 (d) SEC-02: a different kwargs pair is not cached");
        JinjaChatTemplateResetForModelChange();
        TEST_CHECK(!JinjaChatTemplateKwargsFailed(tmpl, "ko", ""),
                   "261124_0002 (d) SEC-02: the per-model reset drops the verdict cache");
    }
    {
        // SEC-01 (2): render-watchdog containment at the facade level. An
        // infinite-loop template (bounded-memory CPU burn: per iteration one
        // 1 MiB string is built and dropped) outruns a 300 ms watchdog budget.
        // The render answers TimedOut WITHOUT the caller ever blocking past
        // the budget, the template is negative-cached (a second request fails
        // instantly instead of hanging again), and the process-lifetime latch
        // fires. The unit runner never links the worker, so the latch has no
        // self-termination side effect here; the production worker checks it
        // after the engine_failed frame is on the wire.
        const std::string hang_tmpl =
            "{% for i in range(1500) %}{% set d = \"x\" * 1048576 %}{% endfor %}{{ \"done\" }}";
        JinjaChatTemplateRenderArgs args{};
        args.template_source = hang_tmpl;
        args.user_content = "hello";
        args.watchdog_ms = 300; // test override; production uses kJinjaRenderWatchdogMs
        std::string out = "SENTINEL";
        const auto t0 = std::chrono::steady_clock::now();
        const JinjaRenderOutcome outcome = JinjaChatTemplateRenderEx(args, out);
        const auto elapsed_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0)
                .count();
        TEST_CHECK(outcome == JinjaRenderOutcome::TimedOut && out == "SENTINEL",
                   "261124_0002 (d) SEC-01: an infinite-loop render answers TimedOut");
        TEST_CHECK(elapsed_ms < 10000,
                   "261124_0002 (d) SEC-01: the bounded join never blocks past the watchdog budget");
        TEST_CHECK(JinjaChatTemplateWatchdogTriggered(),
                   "261124_0002 (d) SEC-01: the process-lifetime watchdog latch fired");
        // The negative cache: the SAME template now fails instantly (the
        // helper thread never spawns again for it).
        out = "SENTINEL";
        TEST_CHECK(JinjaChatTemplateRenderEx(args, out) == JinjaRenderOutcome::TimedOut &&
                       out == "SENTINEL",
                   "261124_0002 (d) SEC-01: a negative-cached template answers TimedOut instantly");
        // The per-model reset drops the negative cache (a fresh model would
        // re-probe) but NOT the latch (the worker still self-terminates).
        JinjaChatTemplateResetForModelChange();
        TEST_CHECK(JinjaChatTemplateWatchdogTriggered(),
                   "261124_0002 (d) SEC-01: the watchdog latch survives the per-model reset");
    }
#else
    std::cout << "[SKIP] EMEBALA_HAVE_LLAMA_JINJA undefined; 261124_0002 (d) "
                 "SEC-01/SEC-02 facade checks skipped." << std::endl;
#endif

    // ---- (c) source pins: the translation_common.cpp integration ------------
    const std::filesystem::path tc_cpp = ResolveRepoFile(L"src\\engine_core\\translation_common.cpp");
    if (tc_cpp.empty()) {
        std::cout << "[SKIP] src/engine_core/translation_common.cpp not resolvable; "
                     "261124_0002 (c) pins skipped." << std::endl;
    } else {
        std::ifstream in(tc_cpp, std::ios::binary);
        const std::string src((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
        TEST_CHECK(src.find("chat_tmpl_catalog_failed") != std::string::npos,
                   "261124_0002 (c): the catalog-failed gate exists");
        TEST_CHECK(src.find("has_nontrivial_jinja_template") != std::string::npos,
                   "261124_0002 (c): the jinja usability probe exists");
        TEST_CHECK(src.find("JinjaChatTemplateRenderEx") != std::string::npos,
                   "261124_0002 (c): the vendored-jinja renderer is called");
        TEST_CHECK(src.find("NormalizeJinjaTemplateLangCode") != std::string::npos,
                   "261124_0002 (c): the kwargs language-code normalizer is used");
        // SEC-02 (fix round 1): the no-kwargs retry is the render_jinja_once
        // second attempt, skipped when the kwargs verdict is already cached.
        TEST_CHECK(src.find("render_jinja_once(/*with_kwargs=*/false)") != std::string::npos &&
                       src.find("JinjaChatTemplateKwargsFailed") != std::string::npos,
                   "261124_0002 (c): the retry-without-kwargs + the kwargs-verdict skip exist");
        TEST_CHECK(src.find("jinja_probe_cached_") != std::string::npos,
                   "261124_0002 (c) SEC-02: the probe verdict is cached per loaded model");
        TEST_CHECK(src.find("JinjaChatTemplateResetForModelChange") != std::string::npos,
                   "261124_0002 (c) SEC-02: Unload() drops the per-model facade caches");
        // SEC-01 (fix round 1): the cap at the gate, the watchdog handling and
        // the catch(...) around the prompt-build/tokenize leg.
        TEST_CHECK(src.find("kJinjaTemplateSourceCapBytes") != std::string::npos &&
                       src.find("ENGINE/Translate/059") != std::string::npos,
                   "261124_0002 (c) SEC-01: the 64 KiB template cap gates the probe with a shape-only diagnostic");
        TEST_CHECK(src.find("JinjaRenderOutcome::TimedOut") != std::string::npos &&
                       src.find("ENGINE/Translate/060") != std::string::npos,
                   "261124_0002 (c) SEC-01: a watchdog timeout answers engine_failed (060)");
        TEST_CHECK(src.find("catch (...)") != std::string::npos &&
                       src.find("ENGINE/Translate/042") != std::string::npos,
                   "261124_0002 (c) SEC-01: the prompt-build/tokenize leg is wrapped in catch(...)");
        // COD-03 (fix round 1): the jinja DIAG codes moved off the colliding
        // 053/054 (the WS1 messages keep those numbers).
        TEST_CHECK(src.find("ENGINE/Translate/061: legacy catalog rejected") != std::string::npos &&
                       src.find("ENGINE/Translate/062: embedded chat template is not renderable") != std::string::npos,
                   "261124_0002 (c) COD-03: the jinja DIAG codes are renumbered 061/062");
        TEST_CHECK(src.find("ENGINE/Translate/053: legacy catalog rejected") == std::string::npos,
                   "261124_0002 (c) COD-03: no jinja message keeps the colliding 053");
        // COD-02+VFY-01 / COD-08: the strips run through the pure guards with
        // the FULL pre-render input, and the BOS strip keeps its add_bos gate.
        TEST_CHECK(src.find("JinjaShouldStripLeadingBos(rendered, jinja_bos_token, u8)") != std::string::npos,
                   "261124_0002 (c) COD-02: the BOS strip uses the provenance guard on the full pre-render input");
        // R2-COD-04 (fix round 2): the tail strip carries the vocab's add_eos
        // flag into the pure helper (the reference's gate), so the call site
        // cannot lose it without failing this pin.
        TEST_CHECK(src.find("const bool jinja_add_eos = llama_vocab_get_add_eos(vocab);") != std::string::npos &&
                       src.find("JinjaShouldStripTrailingEos(rendered, jinja_eos_token, u8, jinja_add_eos)") != std::string::npos,
                   "261124_0002 (c) R2-COD-04: the tail EOS strip is gated on the vocab's add_eos, like the BOS add_bos gate");
        {
            const size_t add_bos_pos = src.find("if (llama_vocab_get_add_bos(vocab) &&");
            const size_t strip_pos = src.find("JinjaShouldStripLeadingBos(rendered, jinja_bos_token, u8)");
            TEST_CHECK(add_bos_pos != std::string::npos && strip_pos != std::string::npos &&
                           add_bos_pos < strip_pos,
                       "261124_0002 (c) COD-02: the BOS strip keeps the add_bos gate ahead of the guard");
        }
        TEST_CHECK(src.find("rendered.empty() || rendered == u8") != std::string::npos,
                   "261124_0002 (c) COD-01: the empty/verbatim bar runs on the post-strip request render");
        // COD-07 (fix round 1) + R2-COD-05 (fix round 2): a jinja-served rung 1
        // never falls to the untemplated rung 2. The pins are EXACT per-gate
        // conjunction strings: the shared predicate definition plus each gate's
        // full condition, so the RC-5 echo conjunction and the 052/054
        // `!rung1_bare_special` guards can never be dropped while the pin stays
        // green (the previous bare-substring pin could not tell them apart).
        TEST_CHECK(src.find("const bool rung1_chat_templated = has_nontrivial_chat_template || served_by_jinja;") != std::string::npos,
                   "261124_0002 (c) R2-COD-05: the three gates share one pinned chat-templated predicate");
        TEST_CHECK(src.find("if (rung1_echo && !rung1_bare_special && rung1_chat_templated) {") != std::string::npos,
                   "261124_0002 (c) R2-COD-05: the RC-5 full-echo conjunction is pinned exactly");
        TEST_CHECK(src.find("if (rung1_aborted_echo && rung1_chat_templated && !rung1_bare_special) {") != std::string::npos,
                   "261124_0002 (c) R2-COD-05: the aborted-echo gate conjunction is pinned exactly");
        TEST_CHECK(src.find("if (rung1_chat_templated && !rung1_empty && !rung1_bare_special) {") != std::string::npos,
                   "261124_0002 (c) R2-COD-05: the rejected-output (052) conjunction is pinned exactly");
        {
            const size_t aborted_pos = src.find(
                "if (rung1_aborted_echo && rung1_chat_templated && !rung1_bare_special) {");
            const size_t rejected_pos = src.find(
                "if (rung1_chat_templated && !rung1_empty && !rung1_bare_special) {");
            TEST_CHECK(aborted_pos != std::string::npos && rejected_pos != std::string::npos &&
                           aborted_pos < rejected_pos,
                       "261124_0002 (c) R2-COD-05: the aborted-echo arm still runs BEFORE the generic rejected-output arm");
        }
        TEST_CHECK(src.find("ENGINE/Translate/058") != std::string::npos &&
                       src.find("if (served_by_jinja)") != std::string::npos,
                   "261124_0002 (c) COD-07: the empty/bare-special jinja fall-through answers engine_failed");
        // R2-COD-03 (fix round 2): the out-param is reset at the TOP of the
        // lambda (a jinja-served build followed by a legacy shrink rebuild must
        // not leave the caller's flag stale).
        {
            const size_t lambda_pos = src.find("auto build_final_prompt = [&]");
            const size_t reset_pos = src.find("*out_jinja_served = false;");
            const size_t jinja_branch_pos =
                src.find("if (!completion_form && has_nontrivial_jinja_template)", lambda_pos);
            TEST_CHECK(lambda_pos != std::string::npos && reset_pos != std::string::npos &&
                           jinja_branch_pos != std::string::npos &&
                           lambda_pos < reset_pos && reset_pos < jinja_branch_pos,
                       "261124_0002 (c) R2-COD-03: the served_by_jinja out-param is reset at the top of build_final_prompt");
        }
        // R2-COD-02 (fix round 2): the two RenderEx caller-side legs that used
        // to sit outside every try/catch (the probe prelude and the chunking
        // pre-check) answer through their own catch arms.
        TEST_CHECK(src.find("ENGINE/Translate/043: vendored-jinja probe prelude threw") != std::string::npos &&
                       src.find("ENGINE/Translate/044: vendored-jinja kwargs normalizer threw") != std::string::npos &&
                       src.find("ENGINE/Translate/045: chunking pre-check prompt build threw") != std::string::npos,
                   "261124_0002 (c) R2-COD-02: the probe/normalizer prelude and the chunking pre-check are caught (043/044/045)");
        // Ordering inside build_final_prompt: the jinja branch runs BEFORE the
        // legacy chain, which stays wrapped in !jinja_applied (byte-identical
        // for every non-jinja model).
        const size_t lambda_pos = src.find("auto build_final_prompt = [&]");
        const size_t jinja_pos = src.find("if (!completion_form && has_nontrivial_jinja_template)", lambda_pos);
        const size_t guard_pos = src.find("if (!jinja_applied) {", lambda_pos);
        const size_t completion_pos = src.find("BuildCompletionPrompt(u8, tgt_name, src_name)", lambda_pos);
        TEST_CHECK(lambda_pos != std::string::npos &&
                       jinja_pos != std::string::npos &&
                       guard_pos != std::string::npos &&
                       completion_pos != std::string::npos &&
                       jinja_pos < guard_pos && guard_pos < completion_pos,
                   "261124_0002 (c): rung-1 order is jinja -> !jinja_applied "
                   "guard -> the legacy completion/chat chain");
        // The legacy catalog/chatml branches are still present verbatim (the
        // REQ-059/WS1 pins in the sibling suites cover their semantics).
        TEST_CHECK(src.find("llama_chat_apply_template(chat_tmpl") != std::string::npos &&
                       src.find("else if (builtin_chatml_wrap_available)") != std::string::npos &&
                       src.find("rung1_without_chatml") != std::string::npos,
                   "261124_0002 (c): the legacy catalog + ChatML-wrap branches "
                   "are preserved");
    }

    // The pure strip-guard helpers live in translation_common.hpp (same
    // header the unit suite binds to for the budget helpers).
    const std::filesystem::path tc_hpp = ResolveRepoFile(L"src\\engine_core\\translation_common.hpp");
    if (tc_hpp.empty()) {
        std::cout << "[SKIP] src/engine_core/translation_common.hpp not resolvable; "
                     "261124_0002 strip-guard header pin skipped." << std::endl;
    } else {
        std::ifstream in(tc_hpp, std::ios::binary);
        const std::string src((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
        TEST_CHECK(src.find("JinjaShouldStripLeadingBos") != std::string::npos &&
                       src.find("JinjaShouldStripTrailingEos") != std::string::npos,
                   "261124_0002 (e): translation_common.hpp declares the pure strip guards");
    }

    // The renderer pair exists in the engine repo, with the fix-round-1
    // contract surface and the KNOWN RESIDUAL lines (SEC-03 containment,
    // SEC-04 deferred).
    const std::filesystem::path jr_hpp = ResolveRepoFile(L"src\\engine_core\\jinja_chat_render.hpp");
    if (jr_hpp.empty()) {
        std::cout << "[SKIP] src/engine_core/jinja_chat_render.hpp not resolvable; "
                     "261124_0002 header pin skipped." << std::endl;
    } else {
        std::ifstream in(jr_hpp, std::ios::binary);
        const std::string src((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
        TEST_CHECK(src.find("JinjaChatTemplateRender") != std::string::npos &&
                       src.find("NormalizeJinjaTemplateLangCode") != std::string::npos,
                   "261124_0002 (c): jinja_chat_render.hpp declares the renderer "
                   "and the normalizer");
        TEST_CHECK(src.find("kJinjaTemplateSourceCapBytes") != std::string::npos &&
                       src.find("kJinjaRenderWatchdogMs") != std::string::npos &&
                       src.find("JinjaRenderOutcome") != std::string::npos,
                   "261124_0002 (c): the header exposes the SEC-01 cap/watchdog constants and the outcome enum");
        TEST_CHECK(src.find("KNOWN RESIDUAL (SEC-03") != std::string::npos &&
                       src.find("KNOWN RESIDUAL (SEC-04") != std::string::npos,
                   "261124_0002 (c): the KNOWN RESIDUAL lines (SEC-03 containment, SEC-04 deferred) are documented");
        // R2-SEC-01 (fix round 2): the SEC-03 wording states the PER-EVENT
        // containment (worker crash BEFORE the negative cache is written, so the
        // cache does not survive it) instead of claiming the template never
        // renders again for the loaded model.
        TEST_CHECK(src.find("does not survive the crash") != std::string::npos ||
                       src.find("NOT survive the crash") != std::string::npos,
                   "261124_0002 (c) R2-SEC-01: the SEC-03 containment is documented per event (the negative cache does not survive a crash)");
        TEST_CHECK(src.find("128-depth caps") != std::string::npos,
                   "261124_0002 (c) R2-SEC-01: the vendored depth caps that bound the crash surface are named");
    }

    // R2-COD-01 / R2-COD-02 (fix round 2): the facade STRUCTURE pins. Both are
    // ordering pins over jinja_chat_render.cpp, so a refactor that puts the
    // lex+parse back under g_cache_mu (R2-COD-01) or drops the spawn-leg
    // catch(...) (R2-COD-02) fails here.
    const std::filesystem::path jr_cpp = ResolveRepoFile(L"src\\engine_core\\jinja_chat_render.cpp");
    if (jr_cpp.empty()) {
        std::cout << "[SKIP] src/engine_core/jinja_chat_render.cpp not resolvable; "
                     "261124_0002 facade structure pins skipped." << std::endl;
    } else {
        std::ifstream in(jr_cpp, std::ios::binary);
        const std::string src((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
        const size_t prog_fn = src.find("std::shared_ptr<const jinja::program> JinjaProgramFor");
        const size_t probe_lock = src.find("std::lock_guard<std::mutex> lock(g_cache_mu);", prog_fn);
        const size_t lex_pos = src.find("lexer.tokenize(template_source)", prog_fn);
        const size_t parse_pos = src.find("jinja::parse_from_tokens", prog_fn);
        const size_t insert_lock = src.find("std::lock_guard<std::mutex> lock(g_cache_mu);", lex_pos);
        const size_t insert_pos = src.find("g_template_cache[template_source]", prog_fn);
        TEST_CHECK(prog_fn != std::string::npos && probe_lock != std::string::npos &&
                       lex_pos != std::string::npos && parse_pos != std::string::npos &&
                       insert_lock != std::string::npos && insert_pos != std::string::npos &&
                       probe_lock < lex_pos && lex_pos < parse_pos && parse_pos < insert_lock &&
                       insert_lock < insert_pos,
                   "261124_0002 (c) R2-COD-01: JinjaProgramFor is a double-checked lock — "
                   "probe lookup under the mutex, lex+parse OUTSIDE it, insert under it");
        TEST_CHECK(src.find("if (slot.program) {") != std::string::npos,
                   "261124_0002 (c) R2-COD-01: the insert is first-writer-wins (a concurrent parse cannot replace the cached AST)");
        TEST_CHECK(src.find("auto& slot = g_template_cache[template_source];") != std::string::npos,
                   "261124_0002 (c) R2-COD-01: the insert reuses the entry slot, so a cached failed_kwargs set is preserved");
        const size_t spawn_try = src.find("try {", src.find("R2-COD-02 (fix round 2)"));
        const size_t spawn_alloc = src.find("job = std::make_shared<JinjaRenderJob>();");
        const size_t spawn_thread = src.find("helper = std::thread(JinjaRenderJobRun, job);");
        const size_t spawn_catch = src.find("} catch (...)", spawn_thread);
        TEST_CHECK(spawn_try != std::string::npos && spawn_alloc != std::string::npos &&
                       spawn_thread != std::string::npos && spawn_catch != std::string::npos &&
                       spawn_try < spawn_alloc && spawn_alloc < spawn_thread &&
                       spawn_thread < spawn_catch,
                   "261124_0002 (c) R2-COD-02: the RenderEx spawn leg (job block, input copies, "
                   "thread creation) is wrapped in catch(...)");
        // R2-SEC-04 (fix round 2): the DEFERRED slow-template re-render
        // amplification note lives next to the SEC-02 cache comment.
        TEST_CHECK(src.find("R2-SEC-04 (fix round 2) KNOWN RESIDUAL") != std::string::npos,
                   "261124_0002 (c) R2-SEC-04: the deferred slow-template amplification note sits at the SEC-02 cache comment");
    }

    // SEC-01 worker seam: the ggml-translate worker self-terminates (the
    // existing crash-isolation respawn path) once the engine_failed frame is
    // on the wire and the watchdog latch has fired.
    const std::filesystem::path worker_cpp = ResolveRepoFile(L"src\\ggml_translate_worker.cpp");
    if (worker_cpp.empty()) {
        std::cout << "[SKIP] src/ggml_translate_worker.cpp not resolvable; "
                     "261124_0002 worker pin skipped." << std::endl;
    } else {
        std::ifstream in(worker_cpp, std::ios::binary);
        const std::string src((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
        TEST_CHECK(src.find("JinjaChatTemplateWatchdogTriggered()") != std::string::npos &&
                       src.find("ENGINEHOST/worker/032") != std::string::npos,
                   "261124_0002 (c) SEC-01: the worker checks the latch after the job answer and self-terminates");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] 261124_0002 vendored-jinja chat-template checks passed." << std::endl;
    } else {
        std::cout << "[FAIL] 261124_0002 vendored-jinja chat-template: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}
