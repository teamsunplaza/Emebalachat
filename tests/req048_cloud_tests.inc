// ---------------------------------------------------------------------------
// REQ-048 P3: cloud (Google) failure must surface as an EMPTY result so
// engine.cpp's cloud_call records TranslationStatus::EngineFailed — never as a
// silent identity passthrough (returning the original text) that the worker
// treats as success ("translation == source" → skip paste, no failure sound,
// no modal, and the success sentinel unlatches the modal). This was the
// user-device P3 무번역 root cause: google_translate.cpp returned the original
// text on dual-endpoint network failure, and the only caller (engine.cpp
// cloud_call) mapped any non-empty result to Ok. Staged as an .inc next to
// this runner; registered near the end of main() (after
// TestReq048OpenAiTemplateSerialization).
//
// Coverage:
//   (a) runtime: the test-only cloud-block hook (SetCloudBlockedForTesting)
//       short-circuits Translate() to empty WITHOUT touching WinHTTP — pinned
//       here as part of the failure contract (the m6 T5 suites already rely on
//       this shape; see m6_engine_host_embedded_removal_tests.inc).
//   (b) structural pin: the dual-endpoint failure path converges on
//       `return {};`, and the single legitimate `return std::wstring(text);`
//       (U-G whitespace-only early passthrough) sits after chunking and
//       before any per-chunk request, gated on the empty chunk vector. No
//       passthrough exists from the fallback endpoint onward. Source is
//       located with ResolveRepoFile and read as raw bytes; all needles are
//       pure ASCII, so the UTF-8 source encoding cannot false-match/false-miss
//       (TestReq044VerbatimSync / TestReq045EngineUnavailableModal precedent).
//   (c) engine-level signaling (engine.cpp cloud_call: empty -> EngineFailed)
//       is deliberately NOT pinned here: engine.cpp is out of scope for this
//       wave and already handles the empty outcome — documented, not tested.
//
// The SUCCESS contract: any non-empty parse (primary or fallback endpoint,
// per chunk since U-G) returns the translated text. U-G (session 261121_0001,
// report §3.2 Google row, contract C1) replaced the SEC-M1 1,500-unit
// head/tail clamp with full-text chunking, so the truncation notice is gone
// from this client: nothing is ever truncated now, there is nothing to
// disclose. The failure arm is unchanged. This file also carries the U-G
// chunk-loop suite (TestV109GoogleChunkedCloudQuery below).
// ---------------------------------------------------------------------------

void TestReq048CloudFailureSignaling() {
    std::cout << "[RUN] Testing REQ-048 P3 cloud failure signaling..." << std::endl;
    const int failures_before = g_failed_count;
    using emebalachat::GoogleTranslate;

    // ---- (a) runtime: the blocked-cloud path returns EMPTY ----------------
    // Same engage/restore discipline as the m6 T5 suites: engage the hook,
    // exercise the seam, restore unconditionally (no early returns between
    // engage and restore, so the process-wide flag cannot leak to later
    // suites).
    TEST_CHECK(!GoogleTranslate::IsCloudBlockedForTesting(),
               "REQ-048 P3: cloud-block hook starts disengaged");
    GoogleTranslate::SetCloudBlockedForTesting(true);
    TEST_CHECK(GoogleTranslate::IsCloudBlockedForTesting(),
               "REQ-048 P3: cloud-block hook engaged");
    // REQ-043 (M6 T5 debug) contract: blocked Translate() returns empty (the
    // historical no-network outcome) without touching WinHTTP — this IS the
    // failure shape P3 requires from the real network-failure arm too.
    const std::wstring blocked = GoogleTranslate::Translate(L"hello", "English", "Korean");
    TEST_CHECK(blocked.empty(),
               "REQ-048 P3: blocked cloud returns EMPTY (failure contract: no "
               "identity passthrough, no WinHTTP touched)");
    GoogleTranslate::SetCloudBlockedForTesting(false);
    TEST_CHECK(!GoogleTranslate::IsCloudBlockedForTesting(),
               "REQ-048 P3: cloud-block hook restored (disengaged)");

    // ---- (b) structural pins: google_translate.cpp failure shape ----------
    // The real dual-endpoint failure arm cannot be exercised hermetically
    // (it requires both live endpoints to fail), so it is pinned by source
    // inspection — same technique as TestReq044VerbatimSync (ResolveRepoFile
    // + raw-byte read) and TestReq045EngineUnavailableModal (structural pins
    // for a TU whose behavior cannot be linked here). The source is UTF-8;
    // every needle below is pure ASCII, so byte-level find() is exact.
    const std::filesystem::path gt_path = ResolveRepoFile(L"src\\google_translate.cpp");
    TEST_CHECK(!gt_path.empty(),
               "REQ-048 P3: src/google_translate.cpp located via ResolveRepoFile");
    if (gt_path.empty()) {
        std::cout << "[FAIL] REQ-048 P3: google_translate.cpp not resolvable "
                     "from the test CWD; structural pins failed."
                  << std::endl;
        return;
    }
    std::ifstream gt_in(gt_path, std::ios::binary);
    TEST_CHECK(static_cast<bool>(gt_in),
               "REQ-048 P3: google_translate.cpp opened for reading");
    if (!gt_in) {
        std::cout << "[FAIL] REQ-048 P3: google_translate.cpp unreadable; structural pins failed."
                  << std::endl;
        return;
    }
    const std::string gt_src((std::istreambuf_iterator<char>(gt_in)),
                             std::istreambuf_iterator<char>());
    TEST_CHECK(!gt_src.empty(), "REQ-048 P3: google_translate.cpp non-empty after read");

    // The P3 root cause, pinned dead on the FAILURE arm: the identity
    // passthrough must never be the dual-endpoint failure outcome. U-G
    // (261121_0001) introduced exactly ONE legitimate passthrough — a
    // whitespace-only input short-circuits to `return std::wstring(text);`
    // BEFORE any network I/O (identity is the correct translation of pure
    // delimiters; BuildCloudChunks returns an empty vector there by the C1
    // contract). Pin it positionally: it must sit after the chunk build and
    // before the first per-chunk request call, be gated on the empty chunk
    // vector, and no passthrough may exist anywhere from the fallback
    // endpoint onward (that stays the exclusive failure-signal `return {};`,
    // pinned below).
    const std::size_t ws_pass_pos = gt_src.find("return std::wstring(text);");
    TEST_CHECK(ws_pass_pos != std::string::npos,
               "REQ-048 P3/U-G pin: the whitespace-only early passthrough "
               "exists (gated on BuildCloudChunks returning no chunks)");
    const std::size_t primary_pos = gt_src.find("clients5.google.com");
    TEST_CHECK(primary_pos != std::string::npos,
               "REQ-048 P3/U-G pin: primary endpoint (clients5.google.com) present");
    if (ws_pass_pos != std::string::npos && primary_pos != std::string::npos) {
        // Textual order in the TU: the passthrough sits inside Translate(),
        // AFTER the chunk build and BEFORE the first per-chunk request call
        // (the endpoint strings live in the TranslateChunkUtf8 helper, which
        // precedes Translate — so the anchor is the request call, not the
        // endpoint literal).
        const std::size_t build_pos =
            gt_src.find("const std::vector<TranslationChunk> chunks = BuildCloudChunks(text);");
        const std::size_t chunk_call_pos = gt_src.find("TranslateChunkUtf8(chunk.text");
        TEST_CHECK(build_pos != std::string::npos && chunk_call_pos != std::string::npos &&
                       build_pos < ws_pass_pos && ws_pass_pos < chunk_call_pos,
                   "REQ-048 P3/U-G pin: the passthrough runs AFTER chunking and "
                   "BEFORE any per-chunk network I/O");
        TEST_CHECK(gt_src.rfind("chunks.empty()", ws_pass_pos) != std::string::npos,
                   "REQ-048 P3/U-G pin: the passthrough is gated on the empty chunk vector");
    }

    // The dual-endpoint failure arm converges on `return {};`. Pin it
    // positionally: the empty return must appear AFTER the fallback endpoint
    // marker (translate.googleapis.com), i.e. at the tail of Translate()
    // where both endpoints have been tried — not at one of the earlier
    // legitimate `return {};` sites (empty input, test-block hook).
    const std::size_t fallback_pos = gt_src.find("translate.googleapis.com");
    TEST_CHECK(fallback_pos != std::string::npos,
               "REQ-048 P3 pin: fallback endpoint (translate.googleapis.com) present");
    if (fallback_pos != std::string::npos) {
        const std::size_t fail_ret = gt_src.find("return {};", fallback_pos);
        TEST_CHECK(fail_ret != std::string::npos,
                   "REQ-048 P3 pin: dual-endpoint failure path returns `{}` "
                   "(empty = failure signal for engine.cpp cloud_call)");
        // And the passthrough must not sit between the fallback endpoint and
        // that failure return either (a half-refactored arm).
        if (fail_ret != std::string::npos) {
            TEST_CHECK(gt_src.find("return std::wstring(text);", fallback_pos) == std::string::npos ||
                           gt_src.find("return std::wstring(text);", fallback_pos) > fail_ret,
                       "REQ-048 P3 pin: no identity passthrough after the "
                       "fallback endpoint before the empty failure return");
        }
    }

    // ---- (b2) fix round 1: no-content fold + total deadline ----------------
    // F-1: the empty-only fold became the shared no-content predicate, so the
    // whitespace-only chunk shape (chunker: "\n\n\n" + a full-budget run) can
    // never spend a GET either, and its bytes still ride into the result.
    TEST_CHECK(gt_src.find("if (chunk.text.empty())") == std::string::npos,
               "F-1 pin: the empty-only fold is gone (replaced by the no-content predicate)");
    TEST_CHECK(gt_src.find("if (!ChunkNeedsRequest(chunk.text))") != std::string::npos &&
                   gt_src.find("result += ToUtf16(chunk.text);") != std::string::npos,
               "F-1 pin: a no-content chunk is re-appended verbatim without a GET");
    // F-2: the elapsed budget is tested before every per-chunk request and an
    // exhausted budget takes the SAME whole-request failure return as a chunk
    // failure (the localized notice is the message).
    TEST_CHECK(gt_src.find("ChunkLoopBudgetExhausted(::GetTickCount64() - loop_started_ms)") !=
                   std::string::npos,
               "F-2 pin: the loop tests the elapsed budget before every chunk request");
    TEST_CHECK(gt_src.find("GOOGLE_T/Translate/006") != std::string::npos,
               "F-2 pin: an exhausted budget is logged shape-only");

    // ---- (c) engine-level signaling: documented, not pinned ---------------
    // The consumer contract this empty result feeds is: engine.cpp cloud_call
    // maps empty -> TranslationStatus::EngineFailed (failure sound + modal,
    // success sentinel NOT cleared). That mapping lives in engine.cpp, which
    // is explicitly out of scope for the REQ-048 P3 wave (already handles
    // empty -> EngineFailed), so it is recorded here as the rationale for the
    // empty-failure contract rather than pinned as a source assertion.

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-048 P3 cloud failure signaling tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-048 P3 cloud failure signaling tests: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}


// ---------------------------------------------------------------------------
// U-G (session 261121_0001, report §3.2 Google row, vp-plan contract C1):
// the Google client must translate long inputs IN FULL. v0.10.8 and earlier
// head/tail-truncated any query over kMaxCloudQueryUnits (1,500 UTF-16
// units), silently deleting the middle — the CEO-reported defect. That clamp
// (SEC-M1 ClampCloudQuery) is deleted; BuildCloudChunks is the pure,
// no-network seam for the exact chunk list Translate() sends: one GET per
// chunk on the existing request path, reassembled with the verbatim
// trailing_delimiter. The join invariant below IS the no-middle-deletion
// guarantee. Registration in tests/run_tests.cpp is the orchestrator's
// (contract C6); this suite supersedes the deleted TestSecM1CloudQueryClamp
// coverage at the chunk-seam level.
// ---------------------------------------------------------------------------
void TestV109GoogleChunkedCloudQuery() {
    std::cout << "[RUN] Testing U-G v0.10.9 Google chunked cloud query..." << std::endl;
    const int failures_before = g_failed_count;
    using emebalachat::GoogleTranslate;

    // 0. Compile-time pins on the new chunk size (vp-plan §3.2 Google row:
    //    1,200 units) and the SEC-M1 transport evidence, re-pinned per chunk.
    static_assert(GoogleTranslate::kCloudChunkUnits == 1200,
                  "U-G: cloud chunk size is exactly 1200 UTF-16 units");
    static_assert(GoogleTranslate::kCloudChunkUnits * 9 < 16200,
                  "U-G: worst-case Korean chunk percent-encodes under the 16,200-B OK boundary");

    const auto join_chunks = [](const std::vector<emebalachat::TranslationChunk>& chunks) {
        std::string joined;
        for (const auto& c : chunks) {
            joined += c.text;
            joined += c.trailing_delimiter;
        }
        return joined;
    };
    const auto chunk_units = [](const emebalachat::TranslationChunk& c) {
        return ToUtf16(c.text).size(); // UTF-16 code units, the budget unit
    };

    // 1. The 5,000-unit CEO case: multiple chunks, every chunk inside the
    //    transport budget, every non-last chunk closing on a C1 tier
    //    delimiter, and the join byte-exact — no content dropped from
    //    anywhere, the middle included.
    {
        std::wstring input;
        // W3 (261121_0001): 12 sentences per paragraph puts the input at
        // ~5,215 UTF-16 units, over the 5,000-unit CEO-class bar the U-G
        // report documents for this case (~5,140 estimated); the landed
        // 10-sentence fixture measured only ~4,415 units and tripped the
        // input guard below. The chunker still yields 5 two-paragraph chunks.
        for (int p = 0; p < 10; ++p) {
            if (p > 0) input += L"\n\n";
            for (int s = 0; s < 12; ++s) {
                if (s > 0) input += L" ";
                input += (p == 5 && s == 5)
                             ? L"Sentence with MIDDLEMARKER42 inside."
                             : L"Plain filler sentence for chunk sizing.";
            }
            input += L"\nTail line with 한국어 text and a full stop。";
        }
        TEST_CHECK(input.size() >= 5000,
                   "U-G: CEO case input is at least 5,000 UTF-16 units");
        const std::string utf8_input = ToUtf8(input);
        const std::vector<emebalachat::TranslationChunk> chunks =
            GoogleTranslate::BuildCloudChunks(input);
        TEST_CHECK(chunks.size() >= 2,
                   "U-G: 5,000-unit input becomes multiple chunks (never one clamped query)");
        bool within_budget = true;
        for (const auto& c : chunks) {
            if (chunk_units(c) > GoogleTranslate::kCloudChunkUnits) within_budget = false;
        }
        TEST_CHECK(within_budget,
                   "U-G: every chunk text fits the 1,200-unit transport budget");
        const std::string joined = join_chunks(chunks);
        TEST_CHECK(joined == utf8_input,
                   "U-G: join reproduces the 5,000-unit input byte-exactly (no middle deletion)");
        TEST_CHECK(joined.find("MIDDLEMARKER42") != std::string::npos,
                   "U-G: mid-input marker survives — the middle is translated, not dropped");
        // Every non-last chunk must close on a C1 tier token: that is what
        // "every original delimiter survives at a chunk boundary" means.
        const char* kTierTokens[] = {"\n\n", "\n", ". ", "! ", "? ", " ",
                                     "\xE3\x80\x82", "\xEF\xBC\x81", "\xEF\xBC\x9F"};
        bool boundary_ok = true;
        for (size_t i = 0; i + 1 < chunks.size(); ++i) {
            bool known = false;
            for (const char* tok : kTierTokens) {
                if (chunks[i].trailing_delimiter == tok) known = true;
            }
            if (!known) boundary_ok = false;
        }
        TEST_CHECK(boundary_ok,
                   "U-G: every non-last chunk closes on a C1 tier delimiter");
    }

    // 2. Delimiter-free 5,000-unit run: the chunker hard-splits (its tier-4
    //    fallback) — exactly 5 chunks, every unit preserved, no truncation
    //    path involved.
    {
        const std::wstring run(5000, L'x');
        const std::vector<emebalachat::TranslationChunk> chunks =
            GoogleTranslate::BuildCloudChunks(run);
        TEST_CHECK(chunks.size() == 5,
                   "U-G: 5,000-unit run hard-splits into exactly 5 chunks");
        TEST_CHECK(join_chunks(chunks) == ToUtf8(run),
                   "U-G: hard-split join is byte-exact (nothing dropped by the split)");
    }

    // 3. Under-limit input stays a single chunk with a byte-exact join.
    {
        const std::wstring short_input = L"Hello world. 안녕하세요.";
        const std::vector<emebalachat::TranslationChunk> chunks =
            GoogleTranslate::BuildCloudChunks(short_input);
        TEST_CHECK(chunks.size() == 1, "U-G: short input stays one chunk");
        TEST_CHECK(join_chunks(chunks) == ToUtf8(short_input),
                   "U-G: short input join byte-exact");
    }

    // 4. Supplementary-plane safety: the split never lands inside a surrogate
    //    pair (emoji straddling the 1,200-unit neighborhood of a long run).
    {
        std::wstring input(1199, L'a');
        input += L"\U0001F680"; // rocket, 2 UTF-16 units
        input.append(3000, L'b');
        const std::vector<emebalachat::TranslationChunk> chunks =
            GoogleTranslate::BuildCloudChunks(input);
        TEST_CHECK(chunks.size() >= 2, "U-G: emoji run splits into multiple chunks");
        const auto has_lone_surrogate = [](const std::wstring& w) {
            for (size_t i = 0; i < w.size(); ++i) {
                const bool hi = w[i] >= 0xD800 && w[i] <= 0xDBFF;
                const bool lo = w[i] >= 0xDC00 && w[i] <= 0xDFFF;
                if (hi && (i + 1 >= w.size() || w[i + 1] < 0xDC00 || w[i + 1] > 0xDFFF)) {
                    return true;
                }
                if (lo) return true;
                if (hi) ++i; // valid pair: skip the low partner
            }
            return false;
        };
        bool clean = true;
        for (const auto& c : chunks) {
            if (has_lone_surrogate(ToUtf16(c.text))) clean = false;
        }
        TEST_CHECK(clean, "U-G: no chunk text carries a lone surrogate");
        const std::string joined = join_chunks(chunks);
        TEST_CHECK(joined.find("\xF0\x9F\x9A\x80") != std::string::npos,
                   "U-G: the emoji survives the split (UTF-8 bytes intact)");
        TEST_CHECK(joined == ToUtf8(input), "U-G: emoji run join is byte-exact");
    }

    // 5. Whitespace-only input: the C1 contract yields no chunks; Translate()
    //    short-circuits to identity BEFORE any network I/O (structurally
    //    pinned in TestReq048CloudFailureSignaling).
    {
        const std::vector<emebalachat::TranslationChunk> chunks =
            GoogleTranslate::BuildCloudChunks(L"  \n\t  ");
        TEST_CHECK(chunks.empty(),
                   "U-G: whitespace-only input yields no chunks (identity short-circuit)");
    }

    // 6. Per-chunk transport bound through the REAL encoding chain:
    //    worst-case Korean, one chunk at a time, under the measured 16,200-B
    //    OK boundary — the SEC-M1 evidence, re-pinned for the chunk size.
    {
        const std::wstring korean(5000, L'각');
        const std::vector<emebalachat::TranslationChunk> chunks =
            GoogleTranslate::BuildCloudChunks(korean);
        TEST_CHECK(chunks.size() == 5, "U-G: 5,000-unit Korean run yields 5 chunks");
        bool under = true;
        for (const auto& c : chunks) {
            if (GoogleTranslate::UrlEncode(c.text).size() >= 16200) under = false;
        }
        TEST_CHECK(under,
                   "U-G: every Korean chunk percent-encodes under the 16,200-byte OK boundary");
    }

    // 7. Fix round 1 (F-1/F-2): the no-content predicate and the loop deadline.
    //    The chunker emits BOTH shapes at the budget boundary — the empty
    //    delimiter-only chunk and the whitespace-only chunk — and neither may
    //    become a GET; the deadline predicate flips exactly at 300,000 ms.
    {
        static_assert(GoogleTranslate::kChunkLoopTotalBudgetMs == 300000,
                      "F-2: the Google chunk-loop budget is exactly 300,000 ms");
        volatile unsigned long long under =
            GoogleTranslate::kChunkLoopTotalBudgetMs - 1;
        volatile unsigned long long at = GoogleTranslate::kChunkLoopTotalBudgetMs;
        TEST_CHECK(!GoogleTranslate::ChunkLoopBudgetExhausted(under) &&
                       GoogleTranslate::ChunkLoopBudgetExhausted(at),
                   "F-2: the Google loop gives up at exactly 300,000 ms (299,999 keeps going)");
        TEST_CHECK(!GoogleTranslate::ChunkNeedsRequest("") &&
                       !GoogleTranslate::ChunkNeedsRequest("  \n\t ") &&
                       GoogleTranslate::ChunkNeedsRequest(" a "),
                   "F-1: the no-content predicate folds empty and whitespace-only chunk text "
                   "and keeps every content-bearing chunk");
        std::wstring ws_input = L"\n\n\n";
        ws_input.append(GoogleTranslate::kCloudChunkUnits, L'x');
        const std::vector<emebalachat::TranslationChunk> ws_chunks =
            GoogleTranslate::BuildCloudChunks(ws_input);
        TEST_CHECK(ws_chunks.size() == 2 && !ws_chunks[0].text.empty() &&
                       !GoogleTranslate::ChunkNeedsRequest(ws_chunks[0].text),
                   "F-1: the reachable whitespace-only chunk (blank-line run) is folded, not sent");
        TEST_CHECK(join_chunks(ws_chunks) == ToUtf8(ws_input),
                   "F-1: folding costs no bytes - the whitespace-shape join stays byte-exact");
    }

    // 8. Fix round 2 (G-1, SEC-F01 bytes half): the aggregate response-byte
    //    cap. Typed pin on the constant plus structural pins on the real loop:
    //    every chunk response's parsed bytes accumulate and the whole request
    //    returns {} (the existing REQ-048 P3 failure arm) at 4 MiB.
    {
        volatile std::size_t total_cap = GoogleTranslate::kCloudMaxTotalResponseBytes;
        TEST_CHECK(total_cap == 4194304 && total_cap == 4u * 1024u * 1024u,
                   "G-1: the aggregate cloud response-byte cap is exactly 4 MiB "
                   "(4194304 = 4*1024*1024, >400x any legitimate per-chunk translation)");

        const std::filesystem::path gt_cpp = ResolveRepoFile(L"src\\google_translate.cpp");
        if (gt_cpp.empty()) {
            std::cout << "[SKIP] src/google_translate.cpp not resolvable; G-1/G-3 structural "
                         "pins skipped." << std::endl;
        } else {
            std::ifstream in(gt_cpp, std::ios::binary);
            const std::string src((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
            TEST_CHECK(src.find("total_response_bytes += piece.size() * sizeof(wchar_t);") !=
                           std::string::npos,
                       "G-1: the chunk loop accumulates every chunk response's parsed bytes");
            TEST_CHECK(src.find("total_response_bytes > kCloudMaxTotalResponseBytes") !=
                           std::string::npos,
                       "G-1: the accumulated sum is tested against the cap inside Translate's loop");
            TEST_CHECK(src.find("GOOGLE_T/Translate/007") != std::string::npos,
                       "G-1: exceeding the cap fails closed through the existing {} arm "
                       "(shape-only DIAG 007)");
            // G-3 (SEC-F03): the Format-2 segment loop carries the F-1
            // non-progress guard, exactly like the repo's other JSON walkers.
            TEST_CHECK(src.find("const size_t iter_start = pos;") != std::string::npos &&
                           src.find("if (pos == iter_start) ++pos; // non-progress guard (never stall)") !=
                               std::string::npos,
                       "G-3: the Format-2 segment loop has the iter_start non-progress guard");
        }
    }

    // 9. Fix round 2 (G-3, SEC-F03): hostile Format-2 bodies. A bare string at
    //    segment-array level ([[["a"],"b"]] — the reviewer's PoC) used to
    //    advance NO cursor and wedge the pipeline worker at 100% CPU forever
    //    (CWE-835); the parse must terminate and keep the segments already
    //    collected. A revert hangs this runner, which IS the pin.
    {
        TEST_CHECK(GoogleTranslate::ParseResponseJson("[[[\"a\"],\"b\"]]") == L"a",
                   "G-3: the hostile 'bare string at segment level' body terminates and keeps "
                   "the parsed segment (was a permanent 100% CPU wedge)");
        TEST_CHECK(GoogleTranslate::ParseResponseJson("[[[\"a\"],42],[\"c\"]]") == L"a",
                   "G-3: a numeric element at segment level terminates too "
                   "(trailing garbage after it is not parsed as a segment)");
        TEST_CHECK(GoogleTranslate::ParseResponseJson("[[[\"a\"],\"x\"],[\"b\"]]") == L"a",
                   "G-3: junk at segment level no longer stalls the walk; the segment after it "
                   "is not reached (the parse ends at the segment-array close)");
        // Sanity: well-formed bodies still parse fully (no regression).
        TEST_CHECK(GoogleTranslate::ParseResponseJson("[[[\"ab\",\"cd\"]]]") == L"ab",
                   "G-3: the well-formed Format-2 body still parses its segment "
                   "(metadata element skipped by skip_to_segment_close)");
        TEST_CHECK(GoogleTranslate::ParseResponseJson("[[[\"a\"],[\"b\"]],[[\"c\"],[\"d\"]]]") == L"ab",
                   "G-3: multi-segment Format-2 still concatenates both segments");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] U-G Google chunked cloud query tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] U-G Google chunked cloud query tests: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}
