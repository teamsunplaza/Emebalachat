// 260930_0003 (가+다, CEO decision session 260930_0003): paragraph-boundary
// chunking + stitching pins for the over-budget translation path
// (src/engine_core/translation_common.cpp). Headless: the split/join helpers
// and the greedy packer (PackParagraphChunks, injectable measure) are
// model-independent, so no GGUF is needed. Covers the three decision
// points plus the review-260930 remediation:
//   1. multi-paragraph over-budget input splits at paragraph boundaries and
//      stitches back with the ORIGINAL separators verbatim (round-trip);
//   2. a single huge paragraph yields exactly ONE chunk, i.e. the plan keeps
//      the historical head+tail shrink for that chunk (TruncateHeadTailWindow
//      fallback, source-pinned);
//   3. the new budget boundary: 6128 fits unchunked, 6129 chunks
//      (PromptNeedsChunking + the REQ-R01 arithmetic static asserts);
//   P1. leading blank lines + first paragraph at/over budget never dispatch
//      a zero-length chunk (force-extend), reconstruction byte-faithful;
//   P2. packer pins behind a fake measure: chunk order, maximal packing,
//      measure-failure honesty, single-huge-paragraph single chunk.
// Defines TestS260930ChunkStitch; included by run_tests.cpp after
// ResolveRepoFile (source pins read src/engine_core/translation_common.cpp).

void TestS260930ChunkStitch() {
    std::cout << "[RUN] Testing 260930_0003 Paragraph Chunking + Stitching..." << std::endl;
    const int failures_before = g_failed_count;

    // ---- (3) budget boundary pins (260930_0003 (가)) ----
    static_assert(emebalachat::kLlamaNCtx == 8192,
                  "260930_0003 (가): n_ctx raised 4096 -> 8192");
    static_assert(emebalachat::kLlamaGenReserve == 2048,
                  "260930_0003 (가): generation reserve stays 2048");
    static_assert(emebalachat::kLlamaPromptTokenBudget == 6128,
                  "260930_0003 (가): budget = 8192 - 2048 - 16 = 6128");
    static_assert(emebalachat::kLlamaPromptTokenBudget >= 6000,
                  "260930_0003 (가): prompt >= 6000 tokens fit per the CEO decision");
    using emebalachat::PromptNeedsChunking;
    TEST_CHECK(!PromptNeedsChunking(0), "260930_0003: zero tokens never chunks");
    TEST_CHECK(!PromptNeedsChunking(1), "260930_0003: minimal input never chunks");
    TEST_CHECK(!PromptNeedsChunking(6128),
               "260930_0003 (3): budget boundary - exactly 6128 fits UNCHUNKED");
    TEST_CHECK(PromptNeedsChunking(6129),
               "260930_0003 (3): budget boundary - 6129 selects the chunking path");
    TEST_CHECK(PromptNeedsChunking(100000),
               "260930_0003 (3): far-over-budget input chunks");

    // ---- (1) split + stitch round-trips: separators preserved verbatim ----
    // Helper: full reconstruction is Join(0, last) + separators.back() - the
    // same shape Translate() uses when stitching fully-translated chunks.
    auto roundtrip = [](const std::wstring& src) {
        const emebalachat::ParagraphSplit split = emebalachat::SplitParagraphBlocks(src);
        if (split.blocks.empty()) {
            return std::wstring(); // empty input -> empty reconstruction
        }
        std::wstring out = emebalachat::JoinParagraphBlocks(split, 0, split.blocks.size() - 1);
        if (!split.separators.empty()) {
            out += split.separators.back();
        }
        return out;
    };

    TEST_CHECK(roundtrip(L"").empty(), "260930_0003: empty input splits into zero blocks");
    TEST_CHECK(roundtrip(L"hello") == L"hello",
               "260930_0003: single line without separator is one block, unchanged");
    TEST_CHECK(roundtrip(L"p1\n\np2") == L"p1\n\np2",
               "260930_0003 (1): LF blank-line separator preserved verbatim");
    TEST_CHECK(roundtrip(L"p1\r\n\r\np2") == L"p1\r\n\r\np2",
               "260930_0003 (1): CRLF blank-line separator preserved verbatim (one run, not two cuts)");
    TEST_CHECK(roundtrip(L"p1\n\n\n\np2") == L"p1\n\n\n\np2",
               "260930_0003 (1): CONSECUTIVE blank lines preserved as one verbatim separator run");
    TEST_CHECK(roundtrip(L"p1\n \n\t\r\np2") == L"p1\n \n\t\r\np2",
               "260930_0003 (1): mixed-whitespace blank line (spaces/tabs between newlines) preserved verbatim");
    TEST_CHECK(roundtrip(L"p1\np2") == L"p1\np2",
               "260930_0003: a SINGLE newline does NOT split a paragraph (soft wrap stays in-block)");
    TEST_CHECK(roundtrip(L"p1\r\np2") == L"p1\r\np2",
               "260930_0003: a single CRLF does NOT split a paragraph");
    TEST_CHECK(roundtrip(L"\n\np1") == L"\n\np1",
               "260930_0003: leading blank line preserved verbatim");
    TEST_CHECK(roundtrip(L"p1\n\n") == L"p1\n\n",
               "260930_0003: trailing blank line preserved verbatim");
    TEST_CHECK(roundtrip(L"p1\n") == L"p1\n",
               "260930_0003: trailing single newline stays inside the block");
    TEST_CHECK(roundtrip(L"p1\n\np2\n\np3") == L"p1\n\np2\n\np3",
               "260930_0003 (1): three-paragraph input round-trips exactly");
    // Emoji (UTF-16 surrogate pair) must never be split: cut points are
    // whitespace-run boundaries, which a surrogate cannot be part of.
    TEST_CHECK(roundtrip(L"p1 \xD83D\xDE00\n\np2") == L"p1 \xD83D\xDE00\n\np2",
               "260930_0003 (1): surrogate-pair content round-trips unbroken");

    // Explicit block/separator expectations (not only round-trips).
    {
        const emebalachat::ParagraphSplit split = emebalachat::SplitParagraphBlocks(L"aaa\r\n\r\nbbb\n\nccc");
        TEST_CHECK(split.blocks.size() == 3, "260930_0003 (1): three blocks detected");
        TEST_CHECK(split.separators.size() == 3, "260930_0003: one separator slot per block");
        if (split.blocks.size() == 3 && split.separators.size() == 3) {
            TEST_CHECK(split.blocks[0] == L"aaa" && split.blocks[1] == L"bbb" && split.blocks[2] == L"ccc",
                       "260930_0003 (1): block texts in original order");
            TEST_CHECK(split.separators[0] == L"\r\n\r\n" && split.separators[1] == L"\n\n",
                       "260930_0003 (1): separators verbatim, CRLF vs LF kept distinct");
            TEST_CHECK(split.separators[2].empty(), "260930_0003: last separator empty when text ends mid-block");
        }
        // Join ranges used by the greedy packer: internal separators inside a
        // chunk, boundary separator appended only at stitch time.
        TEST_CHECK(emebalachat::JoinParagraphBlocks(split, 0, 1) == L"aaa\r\n\r\nbbb",
                   "260930_0003 (1): join [0,1] keeps the internal separator inside the chunk");
        TEST_CHECK(emebalachat::JoinParagraphBlocks(split, 1, 2) == L"bbb\n\nccc",
                   "260930_0003 (1): join [1,2] keeps the internal separator inside the chunk");
        TEST_CHECK(emebalachat::JoinParagraphBlocks(split, 1, 1) == L"bbb",
                   "260930_0003 (1): single-block join is the block itself");
        TEST_CHECK(emebalachat::JoinParagraphBlocks(split, 0, 1) + split.separators[1] +
                       emebalachat::JoinParagraphBlocks(split, 2, 2) == L"aaa\r\n\r\nbbb\n\nccc",
                   "260930_0003 (1): two-chunk stitch (join[0,1] + boundary sep + join[2,2]) reproduces the input");
    }

    // ---- (2) single-huge-paragraph fallback structure ----
    {
        // One paragraph, no blank line anywhere: 50k code units must NOT be
        // split - the plan is exactly one chunk, and that chunk alone over the
        // budget falls back to the REQ-R01 head+tail shrink inside
        // translate_once (TruncateHeadTailWindow, still defined in engine.cpp).
        std::wstring huge(50000, L'x');
        const emebalachat::ParagraphSplit split = emebalachat::SplitParagraphBlocks(huge);
        TEST_CHECK(split.blocks.size() == 1, "260930_0003 (2): single huge paragraph stays ONE block");
        if (split.blocks.size() == 1) {
            TEST_CHECK(split.blocks[0].size() == 50000,
                       "260930_0003 (2): the single block is the whole input (nothing discarded at split time)");
        }
        // The head+tail helper itself is still pinned by TestTokenTruncation;
        // here we pin the fallback WIRING in the translation core.
        const std::filesystem::path tc_path =
            ResolveRepoFile(L"src\\engine_core\\translation_common.cpp");
        TEST_CHECK(!tc_path.empty(), "260930_0003 (2): translation_common.cpp resolvable for the fallback source pin");
        if (!tc_path.empty()) {
            std::ifstream in(tc_path, std::ios::binary);
            std::stringstream ss;
            ss << in.rdbuf();
            const std::string src = ss.str();
            TEST_CHECK(src.find("TruncateHeadTailWindow(cur,") != std::string::npos,
                       "260930_0003 (2): per-chunk head+tail fallback call present in the shrink loop");
            TEST_CHECK(src.find("input chunked into %zu parts") != std::string::npos,
                       "260930_0003 (2): the 010 diag line reports chunking");
            TEST_CHECK(src.find("SplitParagraphBlocks(src_w)") != std::string::npos,
                       "260930_0003 (2): the over-budget path splits the scrubbed source");
            TEST_CHECK(src.find("stitched += chunk_trailing_separators[k]") != std::string::npos,
                       "260930_0003 (2): chunk outputs are stitched with the ORIGINAL separators");
        }
    }

    // ---- P2 (review 260930): greedy packer pins behind an injectable measure ----
    // Fake measure: token cost = fixed prompt overhead + per-block weights +
    // per-internal-separator cost. Deterministic, so packing outcomes are
    // exact. The packer is llama-independent; these pins are the headless
    // coverage the review found missing.
    {
        // Chunk order + full coverage: four equal blocks, budget takes two.
        const emebalachat::ParagraphSplit split =
            emebalachat::SplitParagraphBlocks(L"aa\n\nbb\n\ncc\n\ndd");
        std::vector<std::pair<size_t, size_t>> calls;
        const auto measure = [&](size_t first, size_t last) -> int {
            calls.emplace_back(first, last);
            return 5 + 10 * static_cast<int>(last - first + 1) +
                   1 * static_cast<int>(last - first); // overhead + blocks + internal seps
        };
        const auto ranges = emebalachat::PackParagraphChunks(split, 26, measure);
        TEST_CHECK(ranges.has_value(), "260930_0003 P2: packer succeeds on a clean split");
        if (ranges) {
            TEST_CHECK(ranges->size() == 2, "260930_0003 P2: four 10-token blocks at budget 26 pack into two chunks");
            if (ranges->size() == 2) {
                TEST_CHECK((*ranges)[0] == std::make_pair(size_t{0}, size_t{1}) &&
                           (*ranges)[1] == std::make_pair(size_t{2}, size_t{3}),
                           "260930_0003 P2: chunk order preserved, contiguous, every block covered once");
            }
            // Maximal packing proof: the break decision measured (0,2) and
            // rejected it - chunk 0 left no room it could still have used.
            TEST_CHECK(std::find(calls.begin(), calls.end(), std::make_pair(size_t{0}, size_t{2})) != calls.end(),
                       "260930_0003 P2: maximal packing - the packer measured (0,2) and rejected it (no under-filled earlier chunk)");
        }
    }
    {
        // Single-huge-paragraph -> single chunk (head+tail fallback wiring is
        // source-pinned in section (2) above).
        const emebalachat::ParagraphSplit split =
            emebalachat::SplitParagraphBlocks(std::wstring(5000, L'x'));
        const auto measure = [](size_t, size_t) { return 99999; }; // everything over budget
        const auto ranges = emebalachat::PackParagraphChunks(split, 6128, measure);
        TEST_CHECK(ranges.has_value() && ranges->size() == 1 && (*ranges)[0] == std::make_pair(size_t{0}, size_t{0}),
                   "260930_0003 P2: a single over-budget paragraph yields exactly one chunk (per-chunk head+tail fallback)");
    }
    {
        // Measure failure propagates as nullopt (-> Translate returns {}).
        const emebalachat::ParagraphSplit split =
            emebalachat::SplitParagraphBlocks(L"aa\n\nbb");
        const auto measure = [](size_t, size_t) { return -1; };
        const auto ranges = emebalachat::PackParagraphChunks(split, 6128, measure);
        TEST_CHECK(!ranges.has_value(),
                   "260930_0003 P2: a tokenizer failure inside the measure aborts packing honestly (nullopt)");
    }

    // ---- P1 (review 260930): leading blank lines + first paragraph at/over
    // budget must NOT dispatch an empty chunk (the exact long-paste
    // regression). The leading empty block is force-extended with the next
    // block; separator reconstruction stays byte-faithful. ----
    {
        const std::wstring big(3000, L'y'); // first REAL paragraph
        const std::wstring src = L"\n\n" + big + L"\r\n\r\n" + L"tail";
        const emebalachat::ParagraphSplit split = emebalachat::SplitParagraphBlocks(src);
        TEST_CHECK(split.blocks.size() == 3 && split.blocks[0].empty(),
                   "260930_0003 P1: fixture splits into a leading EMPTY block + two real paragraphs");
        bool measured_empty_prefix = false;
        // Cost model: overhead 5, per-block weight, separator 1. big=3000
        // makes (0,1) = 5+0+1+3000 = 3006 > 50: pre-fix code closed the
        // first chunk at the empty block and dispatched translate_once("").
        const auto measure = [&](size_t first, size_t last) -> int {
            if (first == 0 && last == 1) {
                measured_empty_prefix = true;
            }
            int total = 5;
            for (size_t i = first; i <= last; ++i) {
                total += (i == 1) ? 3000 : (i == 2 ? 10 : 0);
            }
            total += static_cast<int>(last - first); // internal separators
            return total;
        };
        const auto ranges = emebalachat::PackParagraphChunks(split, 50, measure);
        TEST_CHECK(ranges.has_value(), "260930_0003 P1: packing succeeds for a leading-blank over-budget input");
        if (ranges) {
            TEST_CHECK(ranges->size() == 2 && (*ranges)[0] == std::make_pair(size_t{0}, size_t{1}) &&
                       (*ranges)[1] == std::make_pair(size_t{2}, size_t{2}),
                       "260930_0003 P1: first chunk force-extends past the empty block, tail becomes chunk 2");
            bool any_empty = false;
            for (const auto& range : *ranges) {
                if (emebalachat::JoinParagraphBlocks(split, range.first, range.second).empty()) {
                    any_empty = true;
                }
            }
            TEST_CHECK(!any_empty,
                       "260930_0003 P1: NO zero-length chunk is dispatched (every joined chunk has real text)");
        }
        TEST_CHECK(!measured_empty_prefix,
                   "260930_0003 P1: the empty-prefix candidate is force-extended WITHOUT measuring (pre-fix close point never evaluated)");
        // Byte-faithful reconstruction at the stitch structure level:
        // join(each chunk) + its trailing separator reproduces the input.
        if (ranges && ranges->size() == 2) {
            std::wstring reconstructed =
                emebalachat::JoinParagraphBlocks(split, (*ranges)[0].first, (*ranges)[0].second) +
                split.separators[(*ranges)[0].second] +
                emebalachat::JoinParagraphBlocks(split, (*ranges)[1].first, (*ranges)[1].second) +
                split.separators[(*ranges)[1].second];
            TEST_CHECK(reconstructed == src,
                       "260930_0003 P1: stitched reconstruction preserves the leading separator run byte-for-byte");
        }
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] 260930_0003 chunking + stitching tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] 260930_0003 chunking + stitching tests: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}
