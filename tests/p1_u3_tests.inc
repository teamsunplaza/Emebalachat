// ===========================================================================
// 261122_0002 P1-U3 (v0.10.10 phased fixes, master plan Phase 1 Wave 1):
// the OpenAI settings dialog — BUG-03..BUG-06.
//
// Covered:
//   (1) BUG-05 pure seam: OpenAiHttpConsentScope — the exact http origin
//       ("host:port" lowercase, port default-filled, IPv6 in [brackets],
//       userinfo stripped) a cleartext-HTTP consent is scoped to; https /
//       invalid / unparseable URLs scope to "" (never match, always re-ask).
//   (2) BUG-05 pure seam: OpenAiHttpConsentCarries — the consent-on-record
//       carry decision (same-origin only, https never carries).
//   (3) BUG-03/04/06 source pins: the dialog wiring the fixes install —
//       the CBN_SELCHANGE CB_GETCURSEL+CB_GETLBTEXT read, the profile-switch
//       list/catalog/generation/button reset, the shared ResolveHttpConsent
//       gate on all three call sites, and the stale-generation completion
//       branch that must NOT touch IDC_FETCH_BTN.
//
// Zero network, zero UI: pure helpers + static source pins only (the house
// ResolveRepoFile seam reads src/ui/openai_settings_window.cpp verbatim).
// ===========================================================================
namespace p1_u3_tests {

// ---- BUG-05: the origin scope parser --------------------------------------
void TestP1U3HttpConsentScope() {
    std::cout << "[RUN] Testing P1-U3 BUG-05 OpenAiHttpConsentScope..." << std::endl;
    const int failures_before = g_failed_count;

    // Plain http origins, default port filled.
    TEST_CHECK(OpenAiHttpConsentScope("http://example.com") == "example.com:80",
               "host without port scopes to :80");
    TEST_CHECK(OpenAiHttpConsentScope("http://example.com/") == "example.com:80",
               "trailing slash does not change the scope");
    TEST_CHECK(OpenAiHttpConsentScope("http://example.com/v1") == "example.com:80",
               "path is not part of the scope");
    TEST_CHECK(OpenAiHttpConsentScope("http://example.com?x=1") == "example.com:80",
               "query is not part of the scope");
    TEST_CHECK(OpenAiHttpConsentScope("http://example.com#frag") == "example.com:80",
               "fragment is not part of the scope");

    // Explicit ports.
    TEST_CHECK(OpenAiHttpConsentScope("http://example.com:8080/v1/chat") == "example.com:8080",
               "explicit port wins over the default");
    TEST_CHECK(OpenAiHttpConsentScope("http://example.com:80") == "example.com:80",
               "the explicit default port equals the filled one (same origin)");

    // Case-insensitivity (scheme AND host).
    TEST_CHECK(OpenAiHttpConsentScope("HTTP://Example.COM:8080") == "example.com:8080",
               "scheme and host compare case-insensitively");

    // Userinfo is stripped: an origin never includes it.
    TEST_CHECK(OpenAiHttpConsentScope("http://user:pass@example.com:8080") == "example.com:8080",
               "userinfo is stripped from the scope");

    // IPv6 literals stay bracketed; their port defaults.
    TEST_CHECK(OpenAiHttpConsentScope("http://[2001:db8::1]:11434") == "[2001:db8::1]:11434",
               "bracketed IPv6 with port");
    TEST_CHECK(OpenAiHttpConsentScope("http://[2001:DB8::1]") == "[2001:db8::1]:80",
               "bracketed IPv6 default-fills the port and lowercases");

    // https / invalid / unparseable URLs scope to "" (they never carry http
    // consent, and "" matches no real origin, keeping the gate re-asking).
    TEST_CHECK(OpenAiHttpConsentScope("https://example.com").empty(),
               "https never carries an http consent scope");
    TEST_CHECK(OpenAiHttpConsentScope("https://example.com:8443").empty(),
               "https with a port still scopes to nothing");
    TEST_CHECK(OpenAiHttpConsentScope("ftp://example.com").empty(),
               "a non-http scheme scopes to nothing");
    TEST_CHECK(OpenAiHttpConsentScope("example.com").empty(),
               "a scheme-less URL scopes to nothing");
    TEST_CHECK(OpenAiHttpConsentScope("http://example.com:abc").empty(),
               "a non-numeric port makes the scope unmatchable");
    TEST_CHECK(OpenAiHttpConsentScope("http:///v1").empty(),
               "an empty host scopes to nothing");
    TEST_CHECK(OpenAiHttpConsentScope("http://:8080").empty(),
               "a port with an empty host scopes to nothing");
    TEST_CHECK(OpenAiHttpConsentScope("").empty(),
               "an empty URL scopes to nothing");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] P1-U3 BUG-05 consent scope parser passed." << std::endl;
    } else {
        std::cout << "[FAIL] P1-U3 BUG-05 consent scope parser: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- BUG-05: the consent carry decision -----------------------------------
void TestP1U3HttpConsentCarries() {
    std::cout << "[RUN] Testing P1-U3 BUG-05 OpenAiHttpConsentCarries..." << std::endl;
    const int failures_before = g_failed_count;

    // Same origin carries without a prompt (path/case/port-spelling tolerant
    // through the scope parser).
    TEST_CHECK(OpenAiHttpConsentCarries("http://example.com", true, "example.com:80"),
               "a YES recorded for the origin carries to the same origin");
    TEST_CHECK(OpenAiHttpConsentCarries("http://example.com/v1", true, "example.com:80"),
               "the recorded origin carries across paths");
    TEST_CHECK(OpenAiHttpConsentCarries("HTTP://example.com", true, "example.com:80"),
               "the recorded origin carries across spellings");
    TEST_CHECK(OpenAiHttpConsentCarries("http://example.com:80", true, "example.com:80"),
               "the explicit default port is the same origin");

    // A different origin never inherits the YES.
    TEST_CHECK(!OpenAiHttpConsentCarries("http://example.com:8080", true, "example.com:80"),
               "a different port re-asks");
    TEST_CHECK(!OpenAiHttpConsentCarries("http://other.com", true, "example.com:80"),
               "a different host re-asks");
    TEST_CHECK(!OpenAiHttpConsentCarries("http://[2001:db8::1]:11435", true,
                                         "[2001:db8::1]:11434"),
               "a different IPv6 port re-asks");

    // https never carries http consent, even with a recorded YES.
    TEST_CHECK(!OpenAiHttpConsentCarries("https://example.com", true, "example.com:80"),
               "https never carries http consent");

    // No YES on record -> never carries.
    TEST_CHECK(!OpenAiHttpConsentCarries("http://example.com", false, "example.com:80"),
               "no consent on record re-asks");
    TEST_CHECK(!OpenAiHttpConsentCarries("http://example.com", true, ""),
               "a YES without a recorded scope re-asks (legacy/https-seeded flag)");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] P1-U3 BUG-05 consent carry decision passed." << std::endl;
    } else {
        std::cout << "[FAIL] P1-U3 BUG-05 consent carry decision: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- BUG-03/04/06: the dialog wiring pins ---------------------------------
void TestP1U3DialogSeams() {
    std::cout << "[RUN] Testing P1-U3 BUG-03/04/06 dialog wiring pins..." << std::endl;
    const int failures_before = g_failed_count;

    const std::filesystem::path src = ResolveRepoFile(L"src\\ui\\openai_settings_window.cpp");
    std::ifstream in(src, std::ios::binary);
    std::string raw;
    if (in) {
        std::ostringstream ss;
        ss << in.rdbuf();
        raw = ss.str();
    }
    TEST_CHECK(!raw.empty(), "P1-U3: src/ui/openai_settings_window.cpp resolves for the pins");
    if (raw.empty()) {
        std::cout << "[FAIL] P1-U3 dialog wiring pins: source unreadable, checks skipped."
                  << std::endl;
        return;
    }
    // Normalize CRLF so the multi-line anchors below see plain \n.
    std::string src_norm;
    src_norm.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\r' && i + 1 < raw.size() && raw[i + 1] == '\n') continue;
        src_norm.push_back(raw[i]);
    }

    // BUG-03: CBN_SELCHANGE names the just-selected item straight off the
    // listbox (the CBS_DROPDOWN edit text can still hold the previous model).
    {
        const std::size_t at = src_norm.find("HIWORD(wp) == CBN_SELCHANGE");
        TEST_CHECK(at != std::string::npos &&
                       src_norm.find("CB_GETCURSEL", at) != std::string::npos &&
                       src_norm.find("CB_GETLBTEXT", at) != std::string::npos &&
                       src_norm.find("RefreshReasoningCombo(dlg, st, &model)") !=
                           std::string::npos,
                   "BUG-03: the model-combo CBN_SELCHANGE reads CB_GETCURSEL+CB_GETLBTEXT "
                   "and passes the override");
    }

    // BUG-04: a profile switch retires the previous endpoint's list, catalog,
    // in-flight fetch and button state BEFORE the new text lands.
    {
        const std::size_t at = src_norm.find("void ApplyOpenAiProfileSelection(");
        TEST_CHECK(at != std::string::npos,
                   "BUG-04: ApplyOpenAiProfileSelection is locatable");
        if (at != std::string::npos) {
            const std::size_t end = src_norm.find("\n}\n", at);
            const std::string body = src_norm.substr(
                at, end == std::string::npos ? std::string::npos : end - at);
            TEST_CHECK(body.find("CB_RESETCONTENT") != std::string::npos,
                       "BUG-04: the model combo list is reset on the switch");
            TEST_CHECK(body.find("st->catalog = OpenAiModelCatalog{};") != std::string::npos,
                       "BUG-04: the previous endpoint's catalog is dropped");
            TEST_CHECK(body.find("++st->fetch_generation;") != std::string::npos,
                       "BUG-04: the in-flight fetch is retired via the generation bump");
            TEST_CHECK(body.find("st->fetch_button_busy = false;") != std::string::npos &&
                           body.find("EnableWindow(::GetDlgItem(dlg, IDC_FETCH_BTN), TRUE)") !=
                               std::string::npos,
                       "BUG-04: the fetch button is restored by the switch itself");
            TEST_CHECK(body.find("SetCtrlText(dlg, IDC_MODEL_COMBO") != std::string::npos,
                       "BUG-04: the reset still precedes the new model text");
            TEST_CHECK(body.find("CB_RESETCONTENT") < body.find("SetCtrlText(dlg, IDC_MODEL_COMBO"),
                       "BUG-04: the list reset precedes the model SetCtrlText");
        }
    }

    // BUG-05: one shared gate serves all three call sites (fetch / [저장] / OK),
    // and the gate itself clears the record for https.
    {
        const std::size_t gate_at = src_norm.find("bool ResolveHttpConsent(");
        TEST_CHECK(gate_at != std::string::npos, "BUG-05: the shared gate is defined");
        if (gate_at != std::string::npos) {
            const std::size_t gate_end = src_norm.find("\n}\n", gate_at);
            const std::string gate = src_norm.substr(
                gate_at, gate_end == std::string::npos ? std::string::npos
                                                       : gate_end - gate_at);
            TEST_CHECK(gate.find("OpenAiHttpConsentCarries(") != std::string::npos,
                       "BUG-05: the gate decides through the pure carry rule");
            TEST_CHECK(gate.find("st->cfg->http_consent_given = false;") != std::string::npos,
                       "BUG-05: https clears the record (https never stores http consent)");
            TEST_CHECK(gate.find("OpenAiHttpConsentScope(base_url)") != std::string::npos,
                       "BUG-05: a YES records the exact origin scope");
        }
        // Three call sites share the gate: DoFetchModels, [저장], IDOK.
        int call_sites = 0;
        for (std::size_t pos = src_norm.find("ResolveHttpConsent(dlg, st,");
             pos != std::string::npos;
             pos = src_norm.find("ResolveHttpConsent(dlg, st,", pos + 1)) {
            ++call_sites;
        }
        TEST_CHECK(call_sites == 3,
                   "BUG-05: exactly the fetch / save / OK paths call the shared gate");
        // The silent [저장] copy is gone: inside the [저장] case the shared gate
        // runs BEFORE the consent value is read.
        {
            const std::size_t save_at = src_norm.find("case IDC_PROFILE_SAVE_BTN: {");
            const std::size_t del_at = src_norm.find("case IDC_PROFILE_DEL_BTN: {");
            TEST_CHECK(save_at != std::string::npos && del_at != std::string::npos &&
                           save_at < del_at,
                       "BUG-05: the [저장] case is locatable");
            if (save_at != std::string::npos && del_at != std::string::npos &&
                save_at < del_at) {
                const std::string save_src =
                    src_norm.substr(save_at, del_at - save_at);
                const std::size_t gate = save_src.find("ResolveHttpConsent(dlg, st,");
                const std::size_t copy = save_src.find(
                    "candidate.http_consent_given = st->cfg->http_consent_given;");
                TEST_CHECK(gate != std::string::npos && copy != std::string::npos &&
                               gate < copy,
                           "BUG-05: [저장] runs the consent gate before reading the flag");
            }
        }
    }

    // BUG-06: the stale-generation completion branch discards ONLY — the
    // button state belongs to the live generation (and the profile switch).
    {
        const std::size_t at = src_norm.find("result.generation != st->fetch_generation");
        TEST_CHECK(at != std::string::npos, "BUG-06: the stale branch is locatable");
        if (at != std::string::npos) {
            const std::size_t cont = src_norm.find("continue;", at);
            TEST_CHECK(cont != std::string::npos,
                       "BUG-06: the stale branch still discards via continue");
            if (cont != std::string::npos) {
                const std::string branch = src_norm.substr(at, cont - at);
                TEST_CHECK(branch.find("EnableWindow") == std::string::npos &&
                               branch.find("fetch_button_busy") == std::string::npos,
                           "BUG-06: the stale branch no longer touches the fetch button");
            }
        }
        // The LIVE branch still owns the restore (before the refill).
        const std::size_t refill_at = src_norm.find("RefillOpenAiModelCombo(dlg, result.catalog.models)");
        TEST_CHECK(refill_at != std::string::npos &&
                       src_norm.rfind("EnableWindow(::GetDlgItem(dlg, IDC_FETCH_BTN), TRUE)",
                                      refill_at) != std::string::npos,
                   "BUG-06: the live completion still restores the button before the refill");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] P1-U3 BUG-03/04/06 dialog wiring pins passed." << std::endl;
    } else {
        std::cout << "[FAIL] P1-U3 BUG-03/04/06 dialog wiring pins: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// 261122_0002 P1-U3: single registration entry the integrator calls from
// main(). (This harness has no TestRunner type; the house .inc idiom is plain
// functions behind one Register entry — same shape as the openai_profile suite.)
void P1U3RegisterTests() {
    TestP1U3HttpConsentScope();
    TestP1U3HttpConsentCarries();
    TestP1U3DialogSeams();
}

} // namespace p1_u3_tests
