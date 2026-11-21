// 260930_0003 (가+다, CEO decision session 260930_0003) + v0.10.9
// (261121_0001 U-D): chunking + stitching pins for the over-budget translation
// path (src/engine_core/translation_common.cpp). Headless: the v0.10.9
// chunk-plan converter (V109BuildChunkSlices) and the shared 4-tier splitter are
// model-independent, so no GGUF is needed.
//
// v0.10.9 (261121_0001 U-D, report §3.2) moved the PRODUCTION chunk source from
// the blank-line-only paragraph splitter (a long single paragraph was ONE block,
// so it never chunked) to the shared 4-tier splitter
// (translation_chunker.hpp) at kV109LocalChunkMaxUtf16Units.
// 261122_0001 (A1): that splitter's family (SplitParagraphBlocks /
// JoinParagraphBlocks / ParagraphSplit / PackParagraphChunks) had no production
// caller left in any repo and was DELETED, so the pins that exercised it are
// gone too; section (2) below pins its ABSENCE instead.
//
// Covers:
//   1. the v0.10.9 chunk plan: kV109LocalChunkMaxUtf16Units == 1200, the
//      shared splitter's chunks converted by V109BuildChunkSlices, and the
//      DELIMITER-PRESERVATION invariant (chunk texts + their trailing
//      delimiters reproduce the input byte-exactly);
//   2. the budget boundary: 6128 fits unchunked, 6129 chunks
//      (PromptNeedsChunking + the REQ-R01 arithmetic static asserts);
//   3. the dead-code pin: the removed paragraph family never returns to the
//      translation core, and the per-chunk head+tail fallback wiring stays;
//   P1. a chunk with no text never reaches the decode ladder (delimiter
//      FOLDING), reconstruction byte-faithful.
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

    // ---- (2) the removed paragraph family + the fallback wiring pins ----
    {
        // 261122_0001 (A1): the blank-line-only paragraph splitter that v0.10.9
        // (U-D) stopped calling had no production caller left in any repo, so it
        // was deleted with its greedy packer. The history still matters to the
        // pins: that splitter was BY DESIGN unable to split one paragraph (a
        // 50k-unit run with no blank line was ONE block), which is why the
        // production chunk source moved to the shared 4-tier splitter - pinned
        // in section (2b) below, where the same input DOES split.
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
            // 261122_0001 (A1): the whole family is gone from the TU (no
            // resurrection), not merely uncalled by the chunk path. The CALL
            // forms are the needles on purpose - the history comments above may
            // still NAME the removed splitter, but nothing may call it.
            TEST_CHECK(src.find("SplitParagraphBlocks(") == std::string::npos &&
                           src.find("JoinParagraphBlocks(") == std::string::npos &&
                           src.find("PackParagraphChunks") == std::string::npos,
                       "261122_0001 A1: the removed blank-line paragraph family never returns to the "
                       "translation core");
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

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] 260930_0003 chunking + stitching tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] 260930_0003 chunking + stitching tests: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}
