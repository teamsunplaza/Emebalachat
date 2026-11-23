// ---------------------------------------------------------------------------
// P1-U4 (261122_0002 v0.10.10 phased fixes, wave 1): engine/client/config/probe
// backend hotfixes — behavioral suites + structural source pins.
//   * BUG-07: DecomposeBaseUrl bracketed-IPv6 / empty-port / stray-bracket /
//     query-fragment grammar, exported as a pure seam (the behavioral proof
//     the req045 suite deferred to source differentials while the function
//     lived in an anonymous namespace).
//   * BUG-08: AppConfig::SetEngineHostConfig / GetEngineHostConfig locked
//     round-trip + write-surface pins (mirror of the fix-E openai surface).
//   * E1: TranslationManager::IsAnyCancelRequested truth table (shutdown
//     latch OR per-request token) + engine.cpp gate pins proving the helper
//     guards the two attempt discards and the pre-recorder convergence, and
//     that the post-retry cloud-entry gate carries BOTH operands (the pin-
//     compatible spelling of the helper).
//   * E3: BundledModelIdCached caches only a non-empty id (the absent-
//     registry first call no longer latches "" forever).
//   * BUG-09: the probe transport's per-service cancel token — wired into the
//     production ctor, handed to Translate as ctx.cancel, tripped by
//     BeginShutdown().
// Registered from main() next to the other wave-1 suites.
// ---------------------------------------------------------------------------

void TestP1U4BackendFixes() {
    std::cout << "[RUN] Testing P1-U4 v0.10.10 backend fixes (BUG-07/08, E1, E3, BUG-09)..."
              << std::endl;
    const int failures_before = g_failed_count;
    auto count_of = [](const std::string& hay, const std::string& needle) {
        size_t n = 0;
        for (size_t p = hay.find(needle); p != std::string::npos;
             p = hay.find(needle, p + needle.size())) {
            ++n;
        }
        return n;
    };

    // ---- BUG-07 (behavioral): the exported base-URL decomposition ----------
    {
        std::wstring host, prefix;
        unsigned short port = 0;
        bool https = false;

        // Plain host, scheme defaults.
        TEST_CHECK(DecomposeBaseUrl("https://api.openai.com", host, port, https, prefix) &&
                       host == L"api.openai.com" && port == 443 && https && prefix.empty(),
                   "BUG-07: https host with no path resolves to the 443 default");
        TEST_CHECK(DecomposeBaseUrl("http://localhost:11434/v1", host, port, https, prefix) &&
                       host == L"localhost" && port == 11434 && !https && prefix == L"/v1",
                   "BUG-07: http host:port + /v1 prefix");
        // The BUG-07 core case: bracketed IPv6 WITHOUT a port must resolve to
        // the scheme default (the pre-fix rfind(':') grammar rejected it).
        TEST_CHECK(DecomposeBaseUrl("http://[::1]/v1", host, port, https, prefix) &&
                       host == L"[::1]" && port == 80 && !https && prefix == L"/v1",
                   "BUG-07: bracketed IPv6 without a port is VALID (http default 80)");
        TEST_CHECK(DecomposeBaseUrl("http://[::1]", host, port, https, prefix) &&
                       host == L"[::1]" && port == 80 && prefix.empty(),
                   "BUG-07: bare bracketed IPv6 keeps the default port");
        TEST_CHECK(DecomposeBaseUrl("http://[::1]:8080/v1", host, port, https, prefix) &&
                       host == L"[::1]" && port == 8080 && prefix == L"/v1",
                   "BUG-07: bracketed IPv6 with an explicit port");
        TEST_CHECK(DecomposeBaseUrl("https://[2001:db8::1]", host, port, https, prefix) &&
                       host == L"[2001:db8::1]" && port == 443 && https,
                   "BUG-07: full-form bracketed IPv6 over https");
        // Rejections.
        TEST_CHECK(!DecomposeBaseUrl("http://[::1]:", host, port, https, prefix),
                   "BUG-07: an explicitly empty bracketed port is rejected");
        TEST_CHECK(!DecomposeBaseUrl("http://host:", host, port, https, prefix),
                   "BUG-07: an explicitly empty unbracketed port is rejected");
        TEST_CHECK(!DecomposeBaseUrl("http://ho]st", host, port, https, prefix),
                   "BUG-07: a stray ']' in an unbracketed host is rejected");
        TEST_CHECK(!DecomposeBaseUrl("http://ho]st:80", host, port, https, prefix),
                   "BUG-07: a stray ']' with a port is rejected (the old grammar accepted it)");
        TEST_CHECK(!DecomposeBaseUrl("http://[::1", host, port, https, prefix),
                   "BUG-07: an unterminated bracket literal is rejected");
        TEST_CHECK(!DecomposeBaseUrl("http://[::1]x", host, port, https, prefix),
                   "BUG-07: garbage after the closing bracket is rejected");
        TEST_CHECK(!DecomposeBaseUrl("http://host?x=1", host, port, https, prefix) &&
                       !DecomposeBaseUrl("http://host/path?x=1", host, port, https, prefix),
                   "BUG-07: '?' in the authority or path is rejected");
        TEST_CHECK(!DecomposeBaseUrl("http://host#f", host, port, https, prefix) &&
                       !DecomposeBaseUrl("http://host/path#f", host, port, https, prefix),
                   "BUG-07: '#' in the authority or path is rejected");
        TEST_CHECK(!DecomposeBaseUrl("http://", host, port, https, prefix) &&
                       !DecomposeBaseUrl("ftp://host", host, port, https, prefix) &&
                       !DecomposeBaseUrl("http://user@host", host, port, https, prefix) &&
                       !DecomposeBaseUrl("http://host:abc", host, port, https, prefix) &&
                       !DecomposeBaseUrl("http://host:65536", host, port, https, prefix),
                   "BUG-07: empty authority / non-http(s) scheme / userinfo / bad port reject");
        // Path hygiene: trailing slashes trimmed from the prefix.
        TEST_CHECK(DecomposeBaseUrl("http://host/v1/", host, port, https, prefix) &&
                       prefix == L"/v1",
                   "BUG-07: the path prefix loses its trailing slashes");
        TEST_CHECK(DecomposeBaseUrl("HTTP://HOST", host, port, https, prefix) &&
                       host == L"HOST" && port == 80 && !https,
                   "BUG-07: the scheme stays case-insensitive");

        // The header carries the exported (Win32-free) seam declaration.
        const std::filesystem::path client_hpp = ResolveRepoFile(L"src\\openai_compatible_client.hpp");
        TEST_CHECK(!client_hpp.empty(), "BUG-07: openai_compatible_client.hpp resolvable");
        if (!client_hpp.empty()) {
            std::ifstream in(client_hpp, std::ios::binary);
            const std::string hpp_src((std::istreambuf_iterator<char>(in)),
                                      std::istreambuf_iterator<char>());
            TEST_CHECK(hpp_src.find("bool DecomposeBaseUrl(std::string_view base, std::wstring& host, "
                                    "unsigned short& port,") != std::string::npos,
                       "BUG-07: the header exports DecomposeBaseUrl as a pure seam");
        }
    }

    // ---- BUG-07 pairing: consent-free transport-security classification ----
    {
        // A portless bracketed IPv6 base is plaintext http — usable ONLY with
        // the explicit http consent (the pairing that makes the BUG-07 fix
        // reachable for local OpenAI-compatible servers).
        TEST_CHECK(ClassifyOpenAiBaseUrl("http://[::1]/v1") == OpenAiUrlSecurity::Http &&
                       ClassifyOpenAiBaseUrl("http://[::1]:11434") == OpenAiUrlSecurity::Http,
                   "BUG-07 pairing: bracketed-IPv6 http bases classify Http (consent-gated)");
        TEST_CHECK(ClassifyOpenAiBaseUrl("https://api.openai.com") == OpenAiUrlSecurity::Https &&
                       ClassifyOpenAiBaseUrl("HTTP://HOST") == OpenAiUrlSecurity::Http,
                   "BUG-07 pairing: https is unconditional, the scheme stays case-insensitive");
        TEST_CHECK(ClassifyOpenAiBaseUrl("") == OpenAiUrlSecurity::Invalid &&
                       ClassifyOpenAiBaseUrl("ftp://host") == OpenAiUrlSecurity::Invalid &&
                       ClassifyOpenAiBaseUrl("https") == OpenAiUrlSecurity::Invalid,
                   "BUG-07 pairing: empty / non-http(s) / scheme-less bases are Invalid");
        // Structural: BOTH WinHTTP entry points gate http behind the consent
        // flag (fetch-probe and chat), so no request can leave unconsented.
        const std::filesystem::path client_cpp = ResolveRepoFile(L"src\\openai_compatible_client.cpp");
        TEST_CHECK(!client_cpp.empty(), "BUG-07 pairing: openai_compatible_client.cpp resolvable");
        if (!client_cpp.empty()) {
            std::ifstream in(client_cpp, std::ios::binary);
            const std::string cpp_src((std::istreambuf_iterator<char>(in)),
                                      std::istreambuf_iterator<char>());
            TEST_CHECK(count_of(cpp_src,
                                "OpenAiUrlSecurity::Http && !cfg.http_consent_given") == 2,
                       "BUG-07 pairing: both client entry points require http consent");
        }
    }

    // ---- BUG-08 (behavioral + pins): the locked engine_host block verbs -----
    {
        AppConfig cfg;
        EngineHostConfig block;
        block.enabled = false;
        block.spawn = false;
        block.idle_exit_ms = 123456;
        block.worker_idle_ms = 54321;
        block.warmup = true;
        cfg.SetEngineHostConfig(block);
        const EngineHostConfig got = cfg.GetEngineHostConfig();
        TEST_CHECK(!got.enabled && !got.spawn && got.idle_exit_ms == 123456 &&
                       got.worker_idle_ms == 54321 && got.warmup,
                   "BUG-08: Set/GetEngineHostConfig round-trips the block verbatim");
        // An empty-vector... empty-block overwrite is a REAL edit too.
        cfg.SetEngineHostConfig(EngineHostConfig{});
        const EngineHostConfig dflt = cfg.GetEngineHostConfig();
        TEST_CHECK(dflt.enabled && dflt.spawn && dflt.idle_exit_ms == 600000 &&
                       dflt.worker_idle_ms == 90000 && !dflt.warmup,
                   "BUG-08: a default block overwrite applies verbatim (not a no-op)");

        const std::filesystem::path cfg_hpp = ResolveRepoFile(L"src\\config.hpp");
        const std::filesystem::path cfg_cpp = ResolveRepoFile(L"src\\config.cpp");
        TEST_CHECK(!cfg_hpp.empty() && !cfg_cpp.empty(),
                   "BUG-08: config.hpp/config.cpp resolvable");
        if (!cfg_hpp.empty()) {
            std::ifstream in(cfg_hpp, std::ios::binary);
            const std::string hpp_src((std::istreambuf_iterator<char>(in)),
                                      std::istreambuf_iterator<char>());
            TEST_CHECK(hpp_src.find("void SetEngineHostConfig(EngineHostConfig block);") !=
                               std::string::npos &&
                           hpp_src.find("EngineHostConfig GetEngineHostConfig() const;") !=
                               std::string::npos,
                       "BUG-08: both verbs are declared next to SetOpenAiConfig");
        }
        if (!cfg_cpp.empty()) {
            std::ifstream in(cfg_cpp, std::ios::binary);
            const std::string cpp_src((std::istreambuf_iterator<char>(in)),
                                      std::istreambuf_iterator<char>());
            const size_t set_pos = cpp_src.find("void AppConfig::SetEngineHostConfig(EngineHostConfig block) {");
            const size_t get_pos = cpp_src.find("EngineHostConfig AppConfig::GetEngineHostConfig() const {");
            TEST_CHECK(set_pos != std::string::npos && get_pos != std::string::npos,
                       "BUG-08: both verbs are defined in config.cpp");
            if (set_pos != std::string::npos) {
                const std::string set_body = cpp_src.substr(set_pos, 220);
                TEST_CHECK(set_body.find("std::lock_guard<std::mutex> lock(mutex_);") !=
                               std::string::npos,
                           "BUG-08: the write takes mutex_ (the fix-E discipline)");
            }
            if (get_pos != std::string::npos) {
                const std::string get_body = cpp_src.substr(get_pos, 200);
                TEST_CHECK(get_body.find("std::lock_guard<std::mutex> lock(mutex_);") !=
                               std::string::npos,
                           "BUG-08: the read takes mutex_");
            }
        }
    }

    // ---- E1 (behavioral): the IsAnyCancelRequested truth table -------------
    {
        TranslationManager mgr;
        std::atomic<bool> token{false};
        TranslationTaskContext ctx;
        ctx.cancel = &token;

        TEST_CHECK(!mgr.IsCancelRequested() &&
                       !mgr.IsAnyCancelRequested(TranslationTaskContext{}),
                   "E1: neither latch nor token -> no cancel (default ctx)");
        TEST_CHECK(!mgr.IsAnyCancelRequested(ctx),
                   "E1: an unset per-request token alone -> no cancel");
        token.store(true, std::memory_order_release);
        TEST_CHECK(mgr.IsAnyCancelRequested(ctx),
                   "E1: the per-request token alone -> cancel");
        mgr.RequestCancel();
        TEST_CHECK(mgr.IsAnyCancelRequested(TranslationTaskContext{}),
                   "E1: the shutdown latch alone -> cancel (null token)");
        TEST_CHECK(mgr.IsAnyCancelRequested(ctx),
                   "E1: latch OR token -> cancel when both are set");
    }

    // ---- E1 (pins): the helper guards the retry / fallback / cloud-entry ---
    {
        const std::filesystem::path eng_path = ResolveRepoFile(L"src\\engine.cpp");
        TEST_CHECK(!eng_path.empty(), "E1: src/engine.cpp resolvable");
        if (!eng_path.empty()) {
            std::ifstream in(eng_path, std::ios::binary);
            const std::string eng_src((std::istreambuf_iterator<char>(in)),
                                      std::istreambuf_iterator<char>());

            // The predicate folds BOTH operands (REQ-R16 latch OR user token).
            const size_t def_pos = eng_src.find(
                "bool TranslationManager::IsAnyCancelRequested(const TranslationTaskContext& ctx) const {");
            TEST_CHECK(def_pos != std::string::npos, "E1: the helper is defined in engine.cpp");
            if (def_pos != std::string::npos) {
                const std::string def_body = eng_src.substr(def_pos, 320);
                TEST_CHECK(def_body.find("cancel_requested_.load(std::memory_order_acquire)") !=
                               std::string::npos &&
                           def_body.find("ChunkLoopCancelRequested(ctx.cancel)") != std::string::npos,
                           "E1: the helper ORs the shutdown latch with the per-request token");
            }

            // The helper guards exactly the three discard / convergence gates
            // (attempt-1 result discard, pre-recorder convergence, retry result
            // discard); the cloud-entry gate carries the spelled-out OR.
            TEST_CHECK(count_of(eng_src, "if (IsAnyCancelRequested(ctx)) {") == 3,
                       "E1: the helper guards the three retry/fallback gates");

            // Attempt discards precede BOTH NoteHostSuccess sites.
            size_t success_pos = eng_src.find("NoteHostSuccess();");
            int success_sites = 0;
            int helper_discard_first = 0;
            while (success_pos != std::string::npos) {
                ++success_sites;
                const size_t helper_before =
                    eng_src.rfind("if (IsAnyCancelRequested(ctx)) {", success_pos);
                if (helper_before != std::string::npos &&
                    eng_src.find("set_status(TranslationStatus::Canceled);", helper_before) <
                        success_pos) {
                    ++helper_discard_first;
                }
                success_pos = eng_src.find("NoteHostSuccess();", success_pos + 1);
            }
            TEST_CHECK(success_sites == 2 && helper_discard_first == 2,
                       "E1: a helper-guarded discard precedes BOTH host-success notes");

            // The post-retry cloud-entry gate: BOTH operands, converging to
            // Canceled BEFORE the Auto/fallback cloud legs (the e4-compatible
            // spelling — the 261121_0002 pin needs the literal token check
            // inside this block).
            const size_t retry_failed = eng_src.find("ENGINE/Translate/047");
            const size_t auto_fallback =
                eng_src.find("if (preferred == EngineType::Auto)", retry_failed);
            TEST_CHECK(retry_failed != std::string::npos && auto_fallback != std::string::npos &&
                           auto_fallback > retry_failed,
                       "E1: the post-retry convergence block is locatable");
            if (retry_failed != std::string::npos && auto_fallback != std::string::npos &&
                auto_fallback > retry_failed) {
                const std::string fallback_body =
                    eng_src.substr(retry_failed, auto_fallback - retry_failed);
                TEST_CHECK(fallback_body.find("cancel_requested_.load(std::memory_order_acquire) ||") !=
                               std::string::npos &&
                           fallback_body.find("ChunkLoopCancelRequested(ctx.cancel)") !=
                               std::string::npos &&
                           fallback_body.find("set_status(TranslationStatus::Canceled);") !=
                               std::string::npos,
                           "E1: the cloud-entry gate folds the shutdown latch into the token "
                           "(Auto/consented-local can never leak to the cloud mid-exit)");
            }

            // The REQ-051 warrant guard is untouched (the latch OR the token).
            const size_t warrant = eng_src.find("const uint64_t first_attempt_ms");
            const size_t backoff = eng_src.find("::Sleep(kEngineHostTransientRetryBackoffMs)", warrant);
            TEST_CHECK(warrant != std::string::npos && backoff != std::string::npos &&
                           backoff > warrant,
                       "E1: the transient-retry block is locatable");
            if (warrant != std::string::npos && backoff != std::string::npos && backoff > warrant) {
                const std::string retry_body = eng_src.substr(warrant, backoff - warrant);
                TEST_CHECK(retry_body.find("cancel_requested_.load(std::memory_order_acquire) ||") !=
                               std::string::npos &&
                           retry_body.find("ChunkLoopCancelRequested(ctx.cancel)") != std::string::npos,
                           "E1: the retry warrant still ORs both cancel operands");
            }
        }
    }

    // ---- E3 (pin): BundledModelIdCached caches only a non-empty id ----------
    {
        const std::filesystem::path eng_path2 = ResolveRepoFile(L"src\\engine.cpp");
        TEST_CHECK(!eng_path2.empty(), "E3: src/engine.cpp resolvable");
        if (!eng_path2.empty()) {
            std::ifstream in(eng_path2, std::ios::binary);
            const std::string eng_src((std::istreambuf_iterator<char>(in)),
                                      std::istreambuf_iterator<char>());
            const size_t fn = eng_src.find("std::string BundledModelIdCached() {");
            TEST_CHECK(fn != std::string::npos, "E3: BundledModelIdCached is locatable");
            if (fn != std::string::npos) {
                const size_t fn_end = eng_src.find("\n}\n", fn);
                const std::string body =
                    fn_end == std::string::npos ? eng_src.substr(fn) : eng_src.substr(fn, fn_end - fn);
                // The session cache exists...
                TEST_CHECK(body.find("static const std::string id") != std::string::npos,
                           "E3: the session-scoped cache is still the fast path");
                // ...but only a NON-empty id is latched: the empty path re-reads
                // the registry AFTER the cache check (an absent registry.json on
                // the first call no longer pins "" for the whole session).
                const size_t cache_check = body.find("if (!id.empty())");
                TEST_CHECK(cache_check != std::string::npos,
                           "E3: a non-empty guard sits on the cached value");
                TEST_CHECK(cache_check != std::string::npos &&
                           body.rfind("LoadDefaultRegistry") != std::string::npos &&
                           body.rfind("LoadDefaultRegistry") > cache_check,
                           "E3: the empty-cache path re-reads the registry (no forever latch)");
            }
        }
    }

    // ---- BUG-09 (pins): the probe transport's per-service cancel token ------
    {
        const std::filesystem::path probe_hpp = ResolveRepoFile(L"src\\user_language_probe.hpp");
        const std::filesystem::path probe_cpp = ResolveRepoFile(L"src\\user_language_probe.cpp");
        TEST_CHECK(!probe_hpp.empty() && !probe_cpp.empty(),
                   "BUG-09: user_language_probe.hpp/.cpp resolvable");
        if (!probe_hpp.empty()) {
            std::ifstream in(probe_hpp, std::ios::binary);
            const std::string hpp_src((std::istreambuf_iterator<char>(in)),
                                      std::istreambuf_iterator<char>());
            TEST_CHECK(hpp_src.find("std::atomic<bool> cancel_token{false};") != std::string::npos,
                       "BUG-09: the service state owns the per-service cancel token");
            TEST_CHECK(hpp_src.find("MakeProductionUserLanguageProbeTransport(\n"
                                    "    TranslationManager& engine, const std::atomic<bool>* shutdown_token);") !=
                           std::string::npos,
                       "BUG-09: the token-taking factory overload is declared");
        }
        if (!probe_cpp.empty()) {
            std::ifstream in(probe_cpp, std::ios::binary);
            const std::string cpp_src((std::istreambuf_iterator<char>(in)),
                                      std::istreambuf_iterator<char>());
            TEST_CHECK(cpp_src.find("ctx.cancel = shutdown_token;") != std::string::npos,
                       "BUG-09: the transport hands the token to Translate as ctx.cancel");
            TEST_CHECK(cpp_src.find("MakeProductionUserLanguageProbeTransport(engine, &state_->cancel_token)") !=
                           std::string::npos,
                       "BUG-09: the production ctor wires the service-owned token");
            const size_t begin = cpp_src.find("void UserLanguageProbeService::BeginShutdown()");
            TEST_CHECK(begin != std::string::npos, "BUG-09: BeginShutdown is locatable");
            if (begin != std::string::npos) {
                const std::string body = cpp_src.substr(begin, 900);
                TEST_CHECK(body.find("cancel_token.store(true, std::memory_order_release);") !=
                               std::string::npos,
                           "BUG-09: BeginShutdown trips the probe cancel token");
            }
        }
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] P1-U4 v0.10.10 backend fixes passed." << std::endl;
    } else {
        std::cout << "[FAIL] P1-U4 v0.10.10 backend fixes: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}
