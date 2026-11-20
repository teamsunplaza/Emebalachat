// 260930_0003 (가+다, CEO decision session 260930_0003) + v0.10.9
// (261121_0001 U-D): chunking + stitching pins for the over-budget translation
// path (src/engine_core/translation_common.cpp). Headless: the split/join
// helpers, the greedy packer (PackParagraphChunks, injectable measure) and the
// v0.10.9 chunk-plan converter (V109BuildChunkSlices) are model-independent, so
// no GGUF is needed.
//
// v0.10.9 (261121_0001 U-D, report §3.2) moved the PRODUCTION chunk source from
// SplitParagraphBlocks (blank lines only - a long single paragraph was ONE
// block, so it never chunked) to the shared 4-tier splitter
// (translation_chunker.hpp) at kV109LocalChunkMaxUtf16Units. The paragraph
// helpers below stay exported and pinned as the "blank-line paragraphs only"
// text API; the local decode path no longer calls them.
//
// Covers:
//   1. the blank-line splitter + join contract (paragraph blocks/separators
//      verbatim) - the pure API, unchanged and still pinned;
//   2. the v0.10.9 chunk plan: kV109LocalChunkMaxUtf16Units == 1200, the
//      shared splitter's chunks converted by V109BuildChunkSlices, and the
//      DELIMITER-PRESERVATION invariant (chunk texts + their trailing
//      delimiters reproduce the input byte-exactly);
//   3. the budget boundary: 6128 fits unchunked, 6129 chunks
//      (PromptNeedsChunking + the REQ-R01 arithmetic static asserts);
//   P1. leading blank lines + first paragraph at/over budget never dispatch
//      a zero-length chunk (force-extend for the packer, delimiter FOLDING for
//      the v0.10.9 converter), reconstruction byte-faithful;
//   P2. packer pins behind a fake measure: chunk order, maximal packing,
//      measure-failure honesty, single-huge-paragraph single chunk.
// Defines TestS260930ChunkStitch; included by run_tests.cpp after
// ResolveRepoFile (source pins read src/engine_core/translation_common.cpp).
//
// v0.10.9 U-D: includes the chunker + budget headers so the pins below can call
// the SHARED contract (header-only, pure; resolved through the engine libs'
// PUBLIC include dirs).

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

    // ---- (1) the blank-line splitter + join contract (pure API) ----
    // Unchanged v0.10.9: SplitParagraphBlocks/JoinParagraphBlocks stay exported
    // and are pinned as the "blank lines only" text API. The local decode path
    // no longer calls them (section 2b), but a regression in the API - or a
    // silent widening of what counts as a paragraph separator - still fails
    // here.
    // Helper: full reconstruction is Join(0, last) + separators.back().
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

    // ---- (2) single long paragraph: why the production splitter changed ----
    {
        // The blank-line splitter is BY DESIGN unable to split one paragraph: a
        // 50k-unit run with no blank line is ONE block, and a chunk that alone
        // exceeds the prompt budget falls back to the REQ-R01 head+tail shrink
        // inside translate_once (TruncateHeadTailWindow). That is exactly the
        // reported long-paste failure, and exactly why v0.10.9 (U-D) moved the
        // production chunk source to the shared 4-tier splitter - pinned in
        // section (2b) below, where the same input DOES split.
        std::wstring huge(50000, L'x');
        const emebalachat::ParagraphSplit split = emebalachat::SplitParagraphBlocks(huge);
        TEST_CHECK(split.blocks.size() == 1,
                   "260930_0003 (2): the blank-line API cannot split one paragraph (1 block)");
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
            TEST_CHECK(src.find("SplitParagraphBlocks(src_w)") == std::string::npos,
                       "v0.10.9 U-D: the over-budget path no longer splits with the blank-line API");
            TEST_CHECK(src.find("SplitTextIntoTranslationChunks(ToUtf8(src_w), kV109LocalChunkMaxUtf16Units)") !=
                           std::string::npos,
                       "v0.10.9 U-D: the over-budget path splits the scrubbed source with the shared "
                       "4-tier splitter at the pinned unit cap");
            TEST_CHECK(src.find("stitched += chunk_trailing_separators[k]") != std::string::npos,
                       "260930_0003 (2): chunk outputs are stitched with the ORIGINAL delimiters");
        }
    }

    // ---- (2b) v0.10.9 (261121_0001 U-D): the chunk plan + delimiter invariant ----
    // THE pin the report's §3.2 row needs: the splitter's chunks are converted
    // to (text, trailing delimiter) pairs whose concatenation reproduces the
    // input BYTE-EXACTLY, and the local chunk cap is the pinned 1200 UTF-16
    // units. A regression in either half (a dropped delimiter, a lost
    // separator, a chunker that silently truncates) fails here.
    {
        static_assert(emebalachat::kV109LocalChunkMaxUtf16Units == 1200,
                      "v0.10.9 U-D: the local chunk cap is 1200 UTF-16 units");

        auto join_plan = [](const std::vector<std::wstring>& texts,
                            const std::vector<std::wstring>& delimiters,
                            const std::wstring& leading) {
            // Mirrors the production loop (stitched += text; stitched += delim).
            std::wstring out = leading;
            for (size_t i = 0; i < texts.size(); ++i) {
                out += texts[i];
                if (i < delimiters.size()) {
                    out += delimiters[i];
                }
            }
            for (size_t i = texts.size(); i < delimiters.size(); ++i) {
                out += delimiters[i]; // no text: never lost
            }
            return out;
        };
        auto convert = [](const std::string& utf8, std::vector<std::wstring>& texts,
                          std::vector<std::wstring>& delimiters, std::wstring& leading) {
            const std::vector<emebalachat::TranslationChunk> chunks =
                emebalachat::SplitTextIntoTranslationChunks(
                    utf8, emebalachat::kV109LocalChunkMaxUtf16Units);
            return emebalachat::V109BuildChunkSlices(chunks, texts, delimiters, leading);
        };

        // 1. A multi-tier document: blank line (tier 1), single newline (tier 2),
        //    sentence boundaries (tier 3) and a long run (tier 4).
        {
            std::string doc =
                "First paragraph sentence one. Sentence two!\r\n\r\n"
                "Second paragraph, a single line.\n"
                "Third line with a question? Yes.\n\n"
                "Last paragraph with a very long run of words that keeps going and going so that the "
                "splitter must fall through to its clause tier instead of cutting a UTF-8 sequence.";
            std::vector<std::wstring> texts;
            std::vector<std::wstring> delimiters;
            std::wstring leading;
            TEST_CHECK(convert(doc, texts, delimiters, leading),
                       "v0.10.9 U-D: the 4-tier splitter yields a non-empty chunk plan");
            TEST_CHECK(texts.size() == delimiters.size(),
                       "v0.10.9 U-D: every chunk carries exactly one trailing delimiter slot");
            const std::wstring reconstructed = join_plan(texts, delimiters, leading);
            TEST_CHECK(reconstructed == ToUtf16(doc),
                       "v0.10.9 U-D: chunk texts + their ORIGINAL delimiters reproduce the input "
                       "byte-exactly (delimiter-preservation invariant)");
        }
        // 2. A single long paragraph DOES split now (the reported failure).
        {
            std::string one_paragraph;
            for (int i = 0; i < 400; ++i) {
                one_paragraph += "This is sentence number " + std::to_string(i) + ". ";
            }
            std::vector<std::wstring> texts;
            std::vector<std::wstring> delimiters;
            std::wstring leading;
            TEST_CHECK(convert(one_paragraph, texts, delimiters, leading),
                       "v0.10.9 U-D: one long paragraph produces a chunk plan");
            TEST_CHECK(texts.size() > 1,
                       "v0.10.9 U-D: one long paragraph splits into MORE than one chunk "
                       "(the blank-line API produced exactly one)");
            for (const std::wstring& t : texts) {
                TEST_CHECK(t.size() <= emebalachat::kV109LocalChunkMaxUtf16Units,
                           "v0.10.9 U-D: no chunk exceeds the 1200 UTF-16 unit cap");
            }
            TEST_CHECK(join_plan(texts, delimiters, leading) == ToUtf16(one_paragraph),
                       "v0.10.9 U-D: the long-paragraph plan still reproduces the input exactly");
        }
        // 3. Surrogate pairs never split, and the UTF-8 round trip is lossless.
        {
            std::string emoji;
            for (int i = 0; i < 200; ++i) {
                emoji += "ok \xF0\x9F\x98\x80 word. "; // U+1F600 between words
            }
            std::vector<std::wstring> texts;
            std::vector<std::wstring> delimiters;
            std::wstring leading;
            TEST_CHECK(convert(emoji, texts, delimiters, leading),
                       "v0.10.9 U-D: a surrogate-pair payload produces a chunk plan");
            TEST_CHECK(join_plan(texts, delimiters, leading) == ToUtf16(emoji),
                       "v0.10.9 U-D: surrogate-pair content round-trips unbroken");
        }
        // 4. Empty / whitespace-only input: no plan (never a zero-length decode).
        {
            std::vector<std::wstring> texts;
            std::vector<std::wstring> delimiters;
            std::wstring leading;
            TEST_CHECK(!convert("", texts, delimiters, leading),
                       "v0.10.9 U-D: empty input yields no chunk plan");
            TEST_CHECK(!convert("   \r\n\t ", texts, delimiters, leading),
                       "v0.10.9 U-D: whitespace-only input yields no chunk plan");
        }
        // 5. P1 defence: a chunk with no text NEVER reaches the decode ladder, and
        //    its delimiter is folded into the neighbour instead of being dropped.
        {
            std::vector<emebalachat::TranslationChunk> hand_built = {
                {"", "\n\n"},
                {"aaa", "\n"},
                {"", "  "},
                {"bbb", ""},
            };
            std::vector<std::wstring> texts;
            std::vector<std::wstring> delimiters;
            std::wstring leading;
            TEST_CHECK(emebalachat::V109BuildChunkSlices(hand_built, texts, delimiters, leading),
                       "v0.10.9 U-D: a hand-built plan with empty-text chunks still converts");
            TEST_CHECK(texts.size() == 2 && texts[0] == L"aaa" && texts[1] == L"bbb",
                       "v0.10.9 U-D: empty-text chunks are dropped (never a zero-length decode)");
            TEST_CHECK(leading == L"\n\n",
                       "v0.10.9 U-D: a leading empty chunk folds its delimiter into the leading stub");
            TEST_CHECK(delimiters.size() == 2 && delimiters[0] == L"\n  " && delimiters[1].empty(),
                       "v0.10.9 U-D: a middle empty chunk folds its delimiter into the previous slot");
            TEST_CHECK(join_plan(texts, delimiters, leading) == L"\n\naaa\n  bbb",
                       "v0.10.9 U-D: the folded plan still reproduces the input exactly");
        }
    }

    // ---- P2 (review 260930): greedy packer pins behind an injectable measure ----
    // v0.10.9 (U-D): the packer is no longer the local chunk source (the shared
    // 4-tier splitter is), but it stays EXPORTED as the "paragraph blocks ->
    // token-budget ranges" utility and its contract is still pinned here, so a
    // sibling consumer cannot get a silently broken packer.
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

    // ---- P1 (review 260930 + v0.10.9 U-D): leading blank lines must NOT
    // dispatch an empty chunk (the exact long-paste regression). The packer
    // force-extends the leading empty block with the next one; the v0.10.9
    // chunk-plan converter (section 2b case 5) folds a leading delimiter into
    // the leading stub. Separator reconstruction stays byte-faithful either way.
    // ----
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
