// ---------------------------------------------------------------------------
// 261122_0002 P2-A (v0.10.10 phased fixes, wave 2 unit P2-A: cloud/OpenAI
// clients + download transport).
//
//   Fix 1 / RISK-02   cancel latency: the mutex-guarded single-owner HINTERNET
//                     slot (gguf_model_manager_window.cpp HfClearSlot pattern)
//                     in both cloud clients plus the traversal of the caller's
//                     abort token between reads.
//   Fix 2 / RISK-03   the per-request out_error_detail carrier (identical to
//                     the process-global LastChatErrorDetail text, which stays).
//   Fix 3 / L3-B      OpenAI SSE: "stream":true body, frame/delta parsing,
//                     cumulative preview, [DONE] truncation guard.
//   Fix 4 / BUG-10    DownloadUpdate abort slot (updatelogic::DownloadAbortSlot)
//                     so a cancelled download's socket closes immediately.
//   Fix 5 / V10F-R1   Round-1 remediation (fix-list F-01/F-10/F-16): the SSE
//                     terminator matrix (ClassifyOpenAiSseTermination) plus
//                     structural pins for the per-purpose abort slots with
//                     token keying (F-01), the no-[DONE] classifier wiring
//                     (F-10), and the update checker's fail-closed redirect
//                     pin + launch-time re-hash (F-16).
//
// Coverage:
//   (a) TestP2ASseFrameGrammar        - ParseOpenAiSseFrameData (pure).
//   (b) TestP2ASseDeltaParse          - ParseOpenAiSseDeltaJson (pure).
//   (c) TestP2AStreamRequestBody      - BuildChatBody stream switch; the
//                                       non-streaming body stays byte-identical.
//   (d) TestP2ADownloadAbortSlot      - the slot state machine on REAL (network
//                                       free) WinHTTP session handles.
//   (e) TestP2AStructuralPins         - source pins on the four TUs.
//
// Staged as an .inc next to this runner; registered from main() per the
// end-of-file pattern. The structural pins read the production sources with
// ResolveRepoFile + a CRLF-normalized raw read (the P1-U1 / Req052ReadSrc
// precedent); every needle is ASCII-only. NOTE: this file needs
// `update_checker.hpp` / `model_downloader.hpp` already included in the runner
// (both are, above the wave-1 suite cluster) for HINTERNET + the slot type.
// ---------------------------------------------------------------------------

namespace {

// Resolve + read a production source for the P2-A structural pins.
void P2AReadSrc(const wchar_t* rel, std::string& out) {
    out.clear();
    const std::filesystem::path p = ResolveRepoFile(rel);
    if (p.empty()) return;
    std::ifstream in(p, std::ios::binary);
    if (!in) return;
    out.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string norm;
    norm.reserve(out.size());
    for (size_t i = 0; i < out.size(); ++i) {
        if (out[i] == '\r' && i + 1 < out.size() && out[i + 1] == '\n') continue;
        norm.push_back(out[i]);
    }
    out.swap(norm);
}

size_t P2ACountOf(const std::string& hay, const std::string& needle) {
    size_t n = 0;
    for (size_t p = hay.find(needle); p != std::string::npos;
         p = hay.find(needle, p + needle.size())) {
        ++n;
    }
    return n;
}

} // namespace

// ---- (a) the SSE frame grammar (pure) -------------------------------------

void TestP2ASseFrameGrammar() {
    std::cout << "[RUN] Testing P2-A L3-B SSE frame grammar..." << std::endl;
    const int failures_before = g_failed_count;

    // One data line: the ordinary OpenAI frame.
    TEST_CHECK(ParseOpenAiSseFrameData("data: {\"a\":1}") == "{\"a\":1}",
               "L3-B frame: a single data field yields its payload verbatim");
    // No space after the colon is valid SSE.
    TEST_CHECK(ParseOpenAiSseFrameData("data:{\"a\":1}") == "{\"a\":1}",
               "L3-B frame: 'data:' without a space parses (one optional space stripped)");
    // Exactly ONE leading space is stripped (a second one is payload).
    TEST_CHECK(ParseOpenAiSseFrameData("data:  x") == " x",
               "L3-B frame: only ONE optional space after the colon is removed");
    // Multi-line data joins with LF (this is what keeps pretty-printed JSON
    // payloads parseable).
    TEST_CHECK(ParseOpenAiSseFrameData("data: {\"a\":\ndata: 1}") == "{\"a\":\n1}",
               "L3-B frame: consecutive data lines join with LF (multi-line payload)");
    // CRLF transport: the trailing CR never reaches the payload.
    TEST_CHECK(ParseOpenAiSseFrameData("data: {\"a\":1}\r\ndata: {\"b\":2}\r") ==
                   "{\"a\":1}\n{\"b\":2}",
               "L3-B frame: CRLF lines lose their CR (CRLF transport)");
    // Comments (keep-alive frames) and non-data fields are ignored.
    TEST_CHECK(ParseOpenAiSseFrameData(": ping") == "" &&
                   ParseOpenAiSseFrameData("event: message\nid: 7\ndata: ok") == "ok",
               "L3-B frame: comment lines are ignored and non-data fields are skipped");
    // No data field at all.
    TEST_CHECK(ParseOpenAiSseFrameData("") == "" &&
                   ParseOpenAiSseFrameData("event: done") == "",
               "L3-B frame: a frame without a data field yields no payload");
    // The terminator rides through as data (the caller compares it).
    TEST_CHECK(ParseOpenAiSseFrameData("data: [DONE]") == "[DONE]",
               "L3-B frame: the [DONE] terminator is returned as the payload");
    // An empty data payload is indistinguishable from 'no data' (documented).
    TEST_CHECK(ParseOpenAiSseFrameData("data:") == "",
               "L3-B frame: an empty data payload reads as no payload (documented no-op)");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] P2-A SSE frame grammar passed." << std::endl;
    } else {
        std::cout << "[FAIL] P2-A SSE frame grammar: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- (b) the streaming delta parser (pure) --------------------------------

void TestP2ASseDeltaParse() {
    std::cout << "[RUN] Testing P2-A L3-B SSE delta parsing..." << std::endl;
    const int failures_before = g_failed_count;

    {
        const OpenAiSseDelta d = ParseOpenAiSseDeltaJson(
            "{\"id\":\"c1\",\"object\":\"chat.completion.chunk\",\"choices\":[{\"index\":0,"
            "\"delta\":{\"content\":\"\\uc548\"},\"finish_reason\":null}]}");
        TEST_CHECK(d.content == "\xEC\x95\x88" && d.finish_reason.empty(),
                   "L3-B delta: choices[0].delta.content decodes the \\u escape; a null "
                   "finish_reason stays empty");
    }
    {
        // The opening role prelude carries no text.
        const OpenAiSseDelta d = ParseOpenAiSseDeltaJson(
            "{\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\",\"content\":\"\"},"
            "\"finish_reason\":null}]}");
        TEST_CHECK(d.content.empty() && d.finish_reason.empty(),
                   "L3-B delta: the role-only prelude frame yields no content");
    }
    {
        // The final frame: empty delta + a finish_reason token.
        const OpenAiSseDelta d = ParseOpenAiSseDeltaJson(
            "{\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"length\"}]}");
        TEST_CHECK(d.content.empty() && d.finish_reason == "length",
                   "L3-B delta: a finish_reason-only frame reports the reason (\"length\")");
    }
    {
        // Later choices are ignored (the first choice is the answer).
        const OpenAiSseDelta d = ParseOpenAiSseDeltaJson(
            "{\"choices\":[{\"delta\":{\"content\":\"a\"}},{\"delta\":{\"content\":\"b\"}}]}");
        TEST_CHECK(d.content == "a", "L3-B delta: only choices[0] is read");
    }
    {
        // G-4 bound applies at parse time (a hostile reason cannot bloat the
        // failure detail) and the cut stays on a UTF-8 boundary.
        const std::string long_reason =
            "{\"choices\":[{\"delta\":{},\"finish_reason\":\"0123456789012345678901234567890123456789\"}]}";
        const OpenAiSseDelta d = ParseOpenAiSseDeltaJson(long_reason);
        TEST_CHECK(!d.finish_reason.empty() && d.finish_reason.size() <= 32,
                   "L3-B delta: a long finish_reason is bounded at parse time (<= 32 chars)");
        const OpenAiSseDelta multi = ParseOpenAiSseDeltaJson(
            "{\"choices\":[{\"delta\":{},\"finish_reason\":\"\xEC\x95\x88\xEC\x95\x88\xEC\x95\x88"
            "\xEC\x95\x88\xEC\x95\x88\xEC\x95\x88\xEC\x95\x88\xEC\x95\x88\xEC\x95\x88"
            "\xEC\x95\x88\xEC\x95\x88\xEC\x95\x88\"}]}");
        TEST_CHECK(!multi.finish_reason.empty() && multi.finish_reason.size() <= 32 &&
                       multi.finish_reason.size() % 3 == 0,
                   "L3-B delta: the bounded token never ends mid-sequence (3-byte cuts retreat "
                   "to a whole character)");
    }
    {
        // Malformed / hostile bodies must terminate and yield nothing (the F-1
        // no-spin contract applied to a per-token payload).
        TEST_CHECK(ParseOpenAiSseDeltaJson("").content.empty() &&
                       ParseOpenAiSseDeltaJson("{").content.empty() &&
                       ParseOpenAiSseDeltaJson("{\"choices\":[}").content.empty() &&
                       ParseOpenAiSseDeltaJson("{\"choices\":[{\"delta\":").content.empty() &&
                       ParseOpenAiSseDeltaJson("{\"choices\":[123]}").content.empty() &&
                       ParseOpenAiSseDeltaJson("{\"other\":[1,2,3]}").content.empty(),
                   "L3-B delta: malformed / non-chat payloads terminate with no content "
                   "(progress-guarded walk)");
    }
    {
        // Content survives a nested tool_calls object (skipped, not mis-parsed).
        const OpenAiSseDelta d = ParseOpenAiSseDeltaJson(
            "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"id\":\"x\",\"content\":\"NO\"}],"
            "\"content\":\"yes\"}}]}");
        TEST_CHECK(d.content == "yes",
                   "L3-B delta: a nested tool_calls object is skipped, the real content is read");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] P2-A SSE delta parsing passed." << std::endl;
    } else {
        std::cout << "[FAIL] P2-A SSE delta parsing: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- (c) the stream request body (pure) -----------------------------------

void TestP2AStreamRequestBody() {
    std::cout << "[RUN] Testing P2-A L3-B stream request body..." << std::endl;
    const int failures_before = g_failed_count;

    const std::string body_plain = BuildChatBody("m", "s", "u", "", false);
    TEST_CHECK(body_plain ==
                   "{\"model\":\"m\",\"messages\":[{\"role\":\"system\",\"content\":\"s\"},"
                   "{\"role\":\"user\",\"content\":\"u\"}]}",
               "L3-B body: the explicit stream=false default is byte-identical to the "
               "5-argument call (zero feature loss)");

    const std::string body_stream = BuildChatBody("m", "s", "u", "", false, true);
    TEST_CHECK(body_stream ==
                   "{\"model\":\"m\",\"messages\":[{\"role\":\"system\",\"content\":\"s\"},"
                   "{\"role\":\"user\",\"content\":\"u\"}],\"stream\":true}",
               "L3-B body: stream=true appends exactly ,\"stream\":true");
    TEST_CHECK(body_stream.find("temperature") == std::string::npos &&
                   body_stream.find("max_tokens") == std::string::npos,
               "L3-B body: the streaming body adds no other optional parameter");

    // Ordering: reasoning_effort, then stream, then the (test-only) legacy pair
    // so that pair stays the body tail its pin expects.
    const std::string body_effort_stream = BuildChatBody("m", "s", "u", "low", false, true);
    TEST_CHECK(body_effort_stream.find(",\"reasoning_effort\":\"low\",\"stream\":true}") !=
                   std::string::npos,
               "L3-B body: reasoning_effort precedes the stream key");
    const std::string body_legacy_stream = BuildChatBody("m", "s", "u", "", true, true);
    TEST_CHECK(body_legacy_stream.find(",\"stream\":true,\"temperature\":0,\"max_tokens\":8192}") !=
                   std::string::npos,
               "L3-B body: the legacy temperature/max_tokens pair still terminates the body "
               "(stream sits before it)");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] P2-A stream request body passed." << std::endl;
    } else {
        std::cout << "[FAIL] P2-A stream request body: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- (d) the download abort slot (real handles, no network) ---------------

void TestP2ADownloadAbortSlot() {
    std::cout << "[RUN] Testing P2-A BUG-10/RISK-02 download abort slot..." << std::endl;
    const int failures_before = g_failed_count;

    // DownloadOptions keeps its historical aggregate shape: the new pointer is
    // trailing and defaulted, so every existing call site still compiles.
    {
        const updatelogic::DownloadOptions defaults{};
        TEST_CHECK(defaults.abort == nullptr &&
                       defaults.absolute_cap == updatelogic::kMaxDownloadBytes &&
                       defaults.host_allowed == nullptr,
                   "BUG-10: DownloadOptions{} keeps its pre-fix defaults (abort is null)");
    }

    const auto open_session = []() {
        return ::WinHttpOpen(L"EmebalaChat/1.0 (p2-a unit test)",
                             WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
                             WINHTTP_NO_PROXY_BYPASS, 0);
    };

    // The HfClearSlot protocol, state machine only (no socket involved):
    // publish -> abort takes the handle, closes it, sets the sticky flag, and
    // a second abort (or the worker's ClearAndClose) is a silent no-op.
    HINTERNET h1 = open_session();
    HINTERNET h2 = open_session();
    TEST_CHECK(h1 != nullptr && h2 != nullptr,
               "BUG-10: WinHttpOpen session handles available for the slot harness");
    if (h1 != nullptr && h2 != nullptr) {
        updatelogic::DownloadAbortSlot slot;
        TEST_CHECK(!slot.WasAborted() && !slot.AbortCurrent(),
                   "BUG-10: an idle slot reports no abort and AbortCurrent is a no-op");
        TEST_CHECK(slot.Publish(updatelogic::DownloadAbortSlot::Kind::Connect, h1),
                   "BUG-10: Publish takes an empty slot");
        TEST_CHECK(!slot.Publish(updatelogic::DownloadAbortSlot::Kind::Connect, h2),
                   "BUG-10: Publish refuses an already occupied slot (single owner)");
        TEST_CHECK(slot.Publish(updatelogic::DownloadAbortSlot::Kind::Request, h2),
                   "BUG-10: the connect and request slots are independent");
        TEST_CHECK(slot.AbortCurrent(),
                   "BUG-10: AbortCurrent closes the published handles");
        TEST_CHECK(slot.WasAborted(),
                   "BUG-10: the abort leaves a sticky flag for the worker's failure mapping");
        TEST_CHECK(!slot.AbortCurrent(),
                   "BUG-10: a second AbortCurrent finds nothing to close (idempotent)");
        TEST_CHECK(!slot.ClearAndClose(updatelogic::DownloadAbortSlot::Kind::Request, h2),
                   "BUG-10: the worker's ClearAndClose stays silent after the abort path "
                   "took the handle (exactly-once close, no double close)");
    } else {
        if (h1 != nullptr) ::WinHttpCloseHandle(h1);
        if (h2 != nullptr) ::WinHttpCloseHandle(h2);
    }

    // The non-aborted direction: the WORKER owns and performs the close, and no
    // abort flag is set (a normal completion must never read as cancelled).
    HINTERNET h3 = open_session();
    HINTERNET h4 = open_session();
    TEST_CHECK(h3 != nullptr && h4 != nullptr,
               "BUG-10: second slot harness sessions available");
    if (h3 != nullptr && h4 != nullptr) {
        updatelogic::DownloadAbortSlot slot;
        TEST_CHECK(slot.Publish(updatelogic::DownloadAbortSlot::Kind::Request, h3),
                   "BUG-10: Publish (worker-owned close direction)");
        TEST_CHECK(!slot.ClearAndClose(updatelogic::DownloadAbortSlot::Kind::Connect, h3),
                   "BUG-10: ClearAndClose with the WRONG kind does not close a request handle");
        TEST_CHECK(slot.ClearAndClose(updatelogic::DownloadAbortSlot::Kind::Request, h3),
                   "BUG-10: the worker's ClearAndClose owns the close when no abort landed");
        TEST_CHECK(!slot.WasAborted(),
                   "BUG-10: a worker-owned close never sets the cancelled flag");
        // h4 stays worker-local in this scenario; close it here.
        ::WinHttpCloseHandle(h4);
    } else {
        if (h3 != nullptr) ::WinHttpCloseHandle(h3);
        if (h4 != nullptr) ::WinHttpCloseHandle(h4);
    }

    // ResetAborted scopes the sticky flag to one transfer.
    {
        updatelogic::DownloadAbortSlot slot;
        HINTERNET h5 = open_session();
        TEST_CHECK(h5 != nullptr, "BUG-10: third harness session available");
        if (h5 != nullptr) {
            slot.Publish(updatelogic::DownloadAbortSlot::Kind::Request, h5);
            TEST_CHECK(slot.AbortCurrent() && slot.WasAborted(),
                       "BUG-10: abort sets the sticky flag");
            slot.ResetAborted();
            TEST_CHECK(!slot.WasAborted(),
                       "BUG-10: ResetAborted scopes the flag to one transfer");
        }
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] P2-A download abort slot passed." << std::endl;
    } else {
        std::cout << "[FAIL] P2-A download abort slot: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- (e) production-source pins -------------------------------------------

void TestP2AStructuralPins() {
    std::cout << "[RUN] Testing P2-A structural pins..." << std::endl;
    const int failures_before = g_failed_count;

    // ---- OpenAI client header -------------------------------------------
    {
        std::string hpp;
        P2AReadSrc(L"src\\openai_compatible_client.hpp", hpp);
        TEST_CHECK(!hpp.empty(),
                   "P2-A pin: openai_compatible_client.hpp resolvable");
        if (!hpp.empty()) {
            TEST_CHECK(hpp.find("static bool RequestAbortCurrent();") != std::string::npos,
                       "RISK-02 pin: the client declares the thread-safe RequestAbortCurrent");
            TEST_CHECK(hpp.find("bool include_optional_params, bool stream = false);") !=
                               std::string::npos,
                       "L3-B pin: BuildChatBody carries the additive, defaulted stream switch");
            TEST_CHECK(hpp.find("std::string ParseOpenAiSseFrameData(std::string_view frame);") !=
                               std::string::npos &&
                           hpp.find("OpenAiSseDelta ParseOpenAiSseDeltaJson(std::string_view json);") !=
                               std::string::npos,
                       "L3-B pin: both pure SSE seams are exported (headless-testable)");
        }
    }

    // ---- OpenAI client TU -----------------------------------------------
    {
        std::string cpp;
        P2AReadSrc(L"src\\openai_compatible_client.cpp", cpp);
        TEST_CHECK(!cpp.empty(),
                   "P2-A pin: openai_compatible_client.cpp resolvable");
        if (!cpp.empty()) {
            // The slot protocol (the HfClearSlot shape: clear under the mutex,
            // close OUTSIDE it, exactly-once in both directions).
            TEST_CHECK(cpp.find("struct RequestAbortSlot {") != std::string::npos &&
                           cpp.find("std::lock_guard<std::mutex> lk(g_request_slot.mu);") !=
                               std::string::npos,
                       "RISK-02 pin: the request slot is mutex-guarded (single owner)");
            TEST_CHECK(cpp.find("if (mine) ::WinHttpCloseHandle(h);") != std::string::npos,
                       "RISK-02 pin: the close happens only for the side that cleared the slot");
            TEST_CHECK(cpp.find("ScopedAbortableRequest hRequest(::WinHttpOpenRequest(") !=
                               std::string::npos,
                       "RISK-02 pin: the request handle publishes into the abort slot");
            TEST_CHECK(cpp.find("bool OpenAiCompatibleClient::RequestAbortCurrent() {") !=
                               std::string::npos,
                       "RISK-02 pin: the exported abort is defined");
            TEST_CHECK(cpp.find("if (ChunkLoopAbortRequested(abort) || RequestSlotAborted()) {") !=
                               std::string::npos,
                       "RISK-02 pin: the body read loop fails closed on the caller token OR a "
                       "cross-thread close");
            // RISK-03: the per-request carrier mirrors the global record.
            TEST_CHECK(cpp.find("*ctx.out_error_detail = detail;") != std::string::npos &&
                           cpp.find("RecordChatErrorDetail(detail);") != std::string::npos,
                       "RISK-03 pin: the failure recorder fills the per-request slot AND keeps "
                       "the process-global record");
            // L3-B: the streaming path.
            TEST_CHECK(cpp.find("const bool streaming = static_cast<bool>(ctx.on_chunk_text);") !=
                               std::string::npos,
                       "L3-B pin: the preview sink's presence is the streaming switch");
            TEST_CHECK(cpp.find("if (stream) {") != std::string::npos &&
                           cpp.find("b << \",\\\"stream\\\":true\";") != std::string::npos,
                       "L3-B pin: the request body carries the stream flag only when asked");
            TEST_CHECK(cpp.find("ParseOpenAiSseFrameData(sse_frame)") != std::string::npos &&
                           cpp.find("dispatch_sse_frame();") != std::string::npos,
                       "L3-B pin: the byte loop dispatches pure-parsed frames");
            TEST_CHECK(cpp.find("if (ctx.on_chunk_text && preview_prefix.empty()) {") !=
                               std::string::npos,
                       "L3-B pin: per-delta previews are cumulative (prefix + current chunk)");
            TEST_CHECK(cpp.find("else if (!sse_done) {") != std::string::npos,
                       "L3-B pin: a stream without the [DONE] terminator fails closed");
            TEST_CHECK(cpp.find("if (!sse_frames_seen) {") != std::string::npos,
                       "L3-B pin: a non-SSE 200 body degrades to the ordinary JSON parse");
            TEST_CHECK(cpp.find("OPENAI/HttpRequest/006") != std::string::npos &&
                           cpp.find("OPENAI/ChatCompletion/011") != std::string::npos,
                       "RISK-02 pin: both abort arms log shape-only");
        }
    }

    // ---- Google client --------------------------------------------------
    {
        std::string hpp;
        std::string cpp;
        P2AReadSrc(L"src\\google_translate.hpp", hpp);
        P2AReadSrc(L"src\\google_translate.cpp", cpp);
        TEST_CHECK(!hpp.empty() && !cpp.empty(),
                   "P2-A pin: google_translate.hpp/.cpp resolvable");
        if (!hpp.empty()) {
            TEST_CHECK(hpp.find("static bool RequestAbortCurrent();") != std::string::npos,
                       "RISK-02 pin: the Google client declares RequestAbortCurrent");
        }
        if (!cpp.empty()) {
            TEST_CHECK(cpp.find("bool GoogleTranslate::RequestAbortCurrent() {") !=
                               std::string::npos,
                       "RISK-02 pin: the Google abort is defined");
            TEST_CHECK(cpp.find("ScopedAbortableRequest hRequest(::WinHttpOpenRequest(") !=
                               std::string::npos,
                       "RISK-02 pin: the GET handle publishes into the abort slot");
            TEST_CHECK(cpp.find("if (ChunkLoopAbortRequested(abort) || RequestSlotAborted()) {") !=
                               std::string::npos,
                       "RISK-02 pin: the GET read loop fails closed on the token OR a close");
            TEST_CHECK(cpp.find("TranslateChunkUtf8(chunk.text, sl, tl, ctx.abort)") !=
                               std::string::npos,
                       "RISK-02 pin: the caller's abort token reaches the per-chunk request");
            TEST_CHECK(cpp.find("fallback GET suppressed") != std::string::npos,
                       "RISK-02 pin: an aborted primary attempt never launches the gtx fallback");
            TEST_CHECK(cpp.find("if (ChunkLoopCancelRequested(ctx.cancel) || "
                                "ChunkLoopAbortRequested(ctx.abort)) {") != std::string::npos,
                       "RISK-02 pin: the chunk loop gate folds the shutdown latch in");
            TEST_CHECK(cpp.find("ctx.out_error_detail->clear();") != std::string::npos,
                       "RISK-03 pin: the Google leg clears the per-request detail (it publishes "
                       "no cause of its own)");
        }
    }

    // ---- update checker (download transport) ----------------------------
    {
        std::string hpp;
        P2AReadSrc(L"src\\update_checker.hpp", hpp);
        TEST_CHECK(!hpp.empty(),
                   "P2-A pin: update_checker.hpp resolvable");
        if (!hpp.empty()) {
            TEST_CHECK(hpp.find("class DownloadAbortSlot {") != std::string::npos &&
                           hpp.find("bool AbortCurrent() {") != std::string::npos &&
                           hpp.find("bool ClearAndClose(Kind kind, HINTERNET h) {") !=
                               std::string::npos,
                       "BUG-10 pin: the abort slot exposes the single-owner close protocol");
            TEST_CHECK(hpp.find("DownloadAbortSlot* abort = nullptr;") != std::string::npos,
                       "BUG-10 pin: DownloadOptions carries the additive abort slot pointer");
            TEST_CHECK(hpp.find("options.abort->Publish(") != std::string::npos &&
                           hpp.find("slot->ClearAndClose(kind, h)") != std::string::npos,
                       "BUG-10 pin: DownloadUpdate publishes its connect/request handles and "
                       "closes them through the slot");
            TEST_CHECK(P2ACountOf(hpp, "return fail_status();") >= 4,
                       "BUG-10 pin: every post-close call failure maps to Cancelled, not Failed");
            TEST_CHECK(hpp.find("options.abort->ResetAborted();") != std::string::npos,
                       "BUG-10 pin: a transfer resets the sticky flag on entry");
            // The pre-fix path stays intact when no slot is wired.
            TEST_CHECK(hpp.find("if (slot == nullptr) {") != std::string::npos,
                       "BUG-10 pin: a null slot keeps the historical close behaviour");
        }
    }

    // ---- model downloader ------------------------------------------------
    {
        std::string hpp;
        std::string cpp;
        P2AReadSrc(L"src\\model_downloader.hpp", hpp);
        P2AReadSrc(L"src\\model_downloader.cpp", cpp);
        TEST_CHECK(!hpp.empty() && !cpp.empty(),
                   "P2-A pin: model_downloader.hpp/.cpp resolvable");
        if (!hpp.empty()) {
            TEST_CHECK(hpp.find("bool AbortActiveModelDownload();") != std::string::npos,
                       "BUG-10 pin: the dialog-facing abort entry point is declared");
        }
        if (!cpp.empty()) {
            TEST_CHECK(cpp.find("opts.abort = &ActiveModelDownloadSlot();") != std::string::npos,
                       "BUG-10 pin: FetchAndInstallModel publishes its slot");
            TEST_CHECK(cpp.find("bool AbortActiveModelDownload() {") != std::string::npos,
                       "BUG-10 pin: the abort entry point is defined");
            // The cancel -> close bridge: a cancel-flag-only caller (the model
            // dialog) must still get the prompt socket close, so the pairing
            // lives in this TU, not at the dialog.
            TEST_CHECK(cpp.find("abort_slot.AbortCurrent(); // no-op when nothing is in flight") !=
                               std::string::npos &&
                           cpp.find("struct CancelWatcherJoin {") != std::string::npos &&
                           cpp.find("if (thread.joinable()) thread.join();") != std::string::npos,
                       "BUG-10 pin: the in-TU watcher bridges the cancel flag to the slot close "
                       "and is joined on every exit path");
            // R1-24 (N4) pin: the truthful bound note survives the fix (the
            // receive timeout is still the outer bound when nobody cancels).
            TEST_CHECK(cpp.find("30 s receive timeout") != std::string::npos,
                       "BUG-10 pin: the cancel-bound note stays truthful (30 s receive timeout "
                       "is the outer bound when no cancel lands)");
            TEST_CHECK(cpp.find("if (cancel.load(std::memory_order_relaxed)) return "
                                "FetchOutcome::Cancelled;") != std::string::npos,
                       "BUG-10 pin: the pre-transfer cancel check is untouched");
        }
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] P2-A structural pins passed." << std::endl;
    } else {
        std::cout << "[FAIL] P2-A structural pins: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- (f) V10F-R1 (fix-list round 1): the SSE terminator matrix (pure) ------
//
// F-10: a stream WITHOUT the "data: [DONE]" terminator is COMPLETE when the
// final frame named finish_reason "stop" (several compatible servers omit
// [DONE]); anything else without [DONE] stays a truncation and fails closed.

void TestP2AR1SseTerminatorMatrix() {
    std::cout << "[RUN] Testing V10F-R1 F-10 SSE terminator matrix..." << std::endl;
    const int failures_before = g_failed_count;

    // [DONE] arrived: the ordinary close, whatever the finish_reason said.
    TEST_CHECK(ClassifyOpenAiSseTermination(true, "") == OpenAiSseTermination::Done &&
                   ClassifyOpenAiSseTermination(true, "stop") == OpenAiSseTermination::Done &&
                   ClassifyOpenAiSseTermination(true, "length") == OpenAiSseTermination::Done,
               "F-10 terminator: [DONE] is always the Done close (finish_reason ignored)");
    // F-10: [DONE] absent + finish_reason "stop" -> the answer is complete.
    TEST_CHECK(ClassifyOpenAiSseTermination(false, "stop") ==
                   OpenAiSseTermination::StopWithoutDone,
               "F-10 terminator: finish_reason \"stop\" without [DONE] is a complete answer");
    // Fail-closed half: no [DONE] and no "stop" (empty reason, truncation
    // tokens, blocked tokens, hostile garbage) all stay Truncated.
    TEST_CHECK(ClassifyOpenAiSseTermination(false, "") == OpenAiSseTermination::Truncated &&
                   ClassifyOpenAiSseTermination(false, "length") ==
                       OpenAiSseTermination::Truncated &&
                   ClassifyOpenAiSseTermination(false, "content_filter") ==
                       OpenAiSseTermination::Truncated &&
                   ClassifyOpenAiSseTermination(false, "STOP") ==
                       OpenAiSseTermination::Truncated &&
                   ClassifyOpenAiSseTermination(false, "stop ") ==
                       OpenAiSseTermination::Truncated &&
                   ClassifyOpenAiSseTermination(false, "stopped") ==
                       OpenAiSseTermination::Truncated,
               "F-10 terminator: no [DONE] without an exact \"stop\" stays Truncated "
               "(fail-closed, case/whitespace-sensitive)");
    // The classifier is a constant expression (pure seam, no clock/no I/O).
    static_assert(ClassifyOpenAiSseTermination(true, "") == OpenAiSseTermination::Done,
                  "F-10: the classifier is constexpr");
    static_assert(ClassifyOpenAiSseTermination(false, "stop") ==
                      OpenAiSseTermination::StopWithoutDone,
                  "F-10: stop-without-[DONE] is the accepted terminator");
    static_assert(ClassifyOpenAiSseTermination(false, "length") ==
                      OpenAiSseTermination::Truncated,
                  "F-10: truncation tokens stay fail-closed");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] V10F-R1 F-10 SSE terminator matrix passed." << std::endl;
    } else {
        std::cout << "[FAIL] V10F-R1 F-10 SSE terminator matrix: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- (g) V10F-R1 (fix-list round 1) structural pins ------------------------
//
// F-01 (per-purpose abort slots + token keying + mid-read fail-closed),
// F-10 (the classifier wired into the streaming assembly), F-16 (update
// checker: fail-closed redirect pin + launch-time re-hash). The OpenAI slot
// machinery lives in the TU's anonymous namespace (the header stays
// Win32-free), so the token-keying protocol is pinned as source exactly like
// the P2-A RISK-02 pins this suite already carries.

void TestP2AR1FixStructuralPins() {
    std::cout << "[RUN] Testing V10F-R1 F-01/F-10/F-16 structural pins..." << std::endl;
    const int failures_before = g_failed_count;

    // ---- OpenAI client header: the F-10 pure seam is exported -------------
    {
        std::string hpp;
        P2AReadSrc(L"src\\openai_compatible_client.hpp", hpp);
        TEST_CHECK(!hpp.empty(), "V10F-R1 pin: openai_compatible_client.hpp resolvable");
        if (!hpp.empty()) {
            TEST_CHECK(hpp.find("enum class OpenAiSseTermination { Done, StopWithoutDone, "
                                "Truncated };") != std::string::npos,
                       "F-10 pin: the terminator decision enum is exported");
            TEST_CHECK(hpp.find("constexpr OpenAiSseTermination ClassifyOpenAiSseTermination("
                                "bool saw_done,") != std::string::npos,
                       "F-10 pin: the pure terminator classifier is exported (headless-testable)");
            TEST_CHECK(hpp.find("V10F-R1") != std::string::npos,
                       "F-10 pin: the hunk carries its V10F-R1 tag");
        }
    }

    // ---- OpenAI client TU: F-01 slot split + token keying ------------------
    {
        std::string cpp;
        P2AReadSrc(L"src\\openai_compatible_client.cpp", cpp);
        TEST_CHECK(!cpp.empty(), "V10F-R1 pin: openai_compatible_client.cpp resolvable");
        if (!cpp.empty()) {
            // F-01: per-purpose slots (chat vs model-list fetch).
            TEST_CHECK(cpp.find("enum class OpenAiRequestSlotPurpose { Chat, Models };") !=
                               std::string::npos &&
                           cpp.find("RequestAbortSlot g_request_slot_chat;") !=
                               std::string::npos &&
                           cpp.find("RequestAbortSlot g_request_slot_models;") !=
                               std::string::npos,
                       "F-01 pin: the process-wide slot is split into chat and models slots");
            // F-01: the exported abort names the CHAT slot only.
            TEST_CHECK(cpp.find("return AbortRequestSlot(g_request_slot_chat);") !=
                               std::string::npos,
                       "F-01 pin: a chat cancel closes the chat slot, never the models fetch");
            // F-01: the model-list GET publishes into the MODELS slot.
            TEST_CHECK(cpp.find("OpenAiRequestSlotPurpose::Models") != std::string::npos,
                       "F-01 pin: FetchModels publishes into the models purpose slot");
            // F-01: token keying — the clear matches (handle AND token), never
            // the raw recycled HINTERNET value alone.
            TEST_CHECK(cpp.find("g_request_slot.token == token") != std::string::npos,
                       "F-01 pin: ClearRequestSlot keys on the publish token, not the raw "
                       "HINTERNET value");
            TEST_CHECK(cpp.find("g_request_slot.token = g_request_slot.next_token++;") !=
                               std::string::npos,
                       "F-01 pin: every publish draws a fresh monotonic token");
            // F-01: the sticky verdict lives in the guard's own flag (a later
            // publish cannot reset a reader's abort mid-read).
            TEST_CHECK(cpp.find("std::atomic<bool> aborted_flag_{false};") !=
                               std::string::npos &&
                           cpp.find("live_flag->store(true, std::memory_order_release);") !=
                               std::string::npos,
                       "F-01 pin: the abort verdict is guard-owned (survives concurrent "
                       "publishes)");
            // F-01: a read failure (or a QueryDataAvailable failure) after a
            // cross-thread close re-polls the verdict so a partial 200 body is
            // discarded, never parsed.
            TEST_CHECK(cpp.find("if (!aborted && (ChunkLoopAbortRequested(abort) || "
                                "RequestSlotAborted())) {") != std::string::npos,
                       "F-01 pin: the reader re-polls after the read loop so an aborted "
                       "mid-read body fails closed (no 200+partial overclaim)");
            // F-10: the classifier decides the no-[DONE] arm, and the arm's
            // legacy shape (else if (!sse_done)) survives.
            TEST_CHECK(cpp.find("ClassifyOpenAiSseTermination(sse_done, sse_finish)") !=
                               std::string::npos &&
                           cpp.find("OpenAiSseTermination::Truncated") != std::string::npos,
                       "F-10 pin: the no-[DONE] arm classifies the close (stop = complete, "
                       "else fail closed)");
            TEST_CHECK(cpp.find("else if (!sse_done) {") != std::string::npos,
                       "F-10 pin: the no-terminator arm keeps its single guarded site");
            TEST_CHECK(cpp.find("V10F-R1") != std::string::npos,
                       "F-01/F-10 pin: the hunks carry their V10F-R1 tag");
        }
    }

    // ---- update checker: F-16 ----------------------------------------------
    {
        std::string hpp;
        P2AReadSrc(L"src\\update_checker.hpp", hpp);
        TEST_CHECK(!hpp.empty(), "V10F-R1 pin: update_checker.hpp resolvable");
        if (!hpp.empty()) {
            // F-16: the pin reports its result...
            TEST_CHECK(hpp.find("inline bool PinRedirectPolicyNever(HINTERNET session) {") !=
                               std::string::npos,
                       "F-16 pin: PinRedirectPolicyNever returns the SetOption result");
            // ...and BOTH callers fail closed on it.
            TEST_CHECK(hpp.find("if (!PinRedirectPolicyNever(hSession)) {") !=
                               std::string::npos &&
                           hpp.find("if (!update_detail::PinRedirectPolicyNever(hSession)) {") !=
                               std::string::npos,
                       "F-16 pin: both the update check and the download abandon a session "
                       "that cannot be pinned to never-follow");
            // F-16: the launch-time re-hash exists and fails closed (no hash ->
            // no launch; mismatch -> no launch + the swapped file deleted).
            TEST_CHECK(hpp.find("inline bool LaunchVerifiedInstaller(const std::wstring& path,") !=
                               std::string::npos,
                       "F-16 pin: the launch-time re-hash entry point is exported");
            TEST_CHECK(hpp.find("ComputeFileSha256Hex(path, actual)") != std::string::npos &&
                           hpp.find("if (!ComputeFileSha256Hex(path, actual) || "
                                    "actual != *expected_sha256) {") != std::string::npos,
                       "F-16 pin: the file is re-hashed immediately before launch");
            TEST_CHECK(hpp.find("::DeleteFileW(path.c_str());") != std::string::npos,
                       "F-16 pin: a mismatched/swapped installer is deleted, never launched");
            TEST_CHECK(hpp.find("V10F-R1") != std::string::npos,
                       "F-16 pin: the hunks carry their V10F-R1 tag");
            // The BUG-10 post-close failure mapping only gained arms.
            TEST_CHECK(P2ACountOf(hpp, "return fail_status();") >= 5,
                       "F-16 pin: the new fail-closed arm keeps the BUG-10 Cancelled/Failed "
                       "mapping (five or more fail_status sites)");
        }
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] V10F-R1 structural pins passed." << std::endl;
    } else {
        std::cout << "[FAIL] V10F-R1 structural pins: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- registration ---------------------------------------------------------

namespace p2_a_tests {

void P2ARegisterTests() {
    TestP2ASseFrameGrammar();
    TestP2ASseDeltaParse();
    TestP2AStreamRequestBody();
    TestP2ADownloadAbortSlot();
    TestP2AStructuralPins();
    TestP2AR1SseTerminatorMatrix();
    TestP2AR1FixStructuralPins();
}

} // namespace p2_a_tests