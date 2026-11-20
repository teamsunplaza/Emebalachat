// ---------------------------------------------------------------------------
// v0.10.9 (session 261121_0001, U-A) contracts C1 + C3: the shared 4-tier
// hierarchical translation chunker (engine repo translation_chunker.hpp)
// and the chunked-request IPC budget formulas (engine repo
// engine_host_budget.hpp) — report §3.2/§3.3 option 2 (the shared chunker
// foundation every engine path consumes). Staged as an .inc next to this
// runner.
//
// Register at the end of main() (the end-of-file pattern):
//     int translation_chunker_failures = 0;
//     RunTranslationChunkerTests(translation_chunker_failures);
// The entry also feeds the shared TEST_CHECK counters (g_failed_count /
// g_test_count), so a plain `RunTranslationChunkerTests(some_int)` wiring
// keeps both books consistent; the return value is the number of failed
// checks in THIS suite.
//
// Headers: relies on run_tests.cpp's existing includes PLUS the two
// v0.10.9 U-A engine headers the orchestrator adds to the top include
// block (they resolve through the engine libs' PUBLIC include dirs, like
// the other bare-name engine headers):
//     #include "translation_chunker.hpp"  // v0.10.9 U-A (C1)
//     #include "engine_host_budget.hpp"   // v0.10.9 U-A (C3)
//
// Coverage:
//   (a) degenerate inputs: empty / whitespace-only -> empty vector, and
//       max_units == 0 (unlimited) -> one chunk, byte-exact.
//   (b) tier 1 (blank line) fires first: a fitting paragraph keeps its
//       sentences whole; cross-tier packing (paragraph + newline lines);
//       consecutive blank lines stay byte-exact.
//   (c) tier 2 (single newline) fires when there is no blank line.
//   (d) tier 3 (". " / "! " / "? " + full-width "。！？") and tier 4
//       (spaces), with exact greedy packing boundaries.
//   (e) the tier-4 hard-split fallback: an indivisible ASCII run, a CJK
//       run and an emoji (surrogate pair) run are cut at max_units, never
//       inside a UTF-8 sequence.
//   (f) mixed-script sweep: the byte-exact join invariant holds for every
//       budget, and every chunk respects the unit bound.
//   (g) the C3 budget formulas: clamp boundaries and the
//       client >= worker + 30 s margin invariant across the sweep.
//   (h) source pins on the two engine headers (pure-include set, tier
//       table, protocol-cap citation, compile-time ordering proofs).
// ---------------------------------------------------------------------------

namespace {

// Byte-exact join: text + trailing_delimiter over all chunks, in order.
std::string JoinChunksUATest(const std::vector<TranslationChunk>& chunks) {
    std::string joined;
    for (const TranslationChunk& chunk : chunks) {
        joined += chunk.text;
        joined += chunk.trailing_delimiter;
    }
    return joined;
}

// Independent UTF-8 integrity check: every byte sequence is complete, so a
// chunk never ends mid-sequence (the never-split-inside-UTF-8 invariant).
bool IsWholeUtf8UATest(const std::string& s) {
    std::size_t i = 0;
    while (i < s.size()) {
        const auto b = static_cast<unsigned char>(s[i]);
        if (b < 0x80) {
            ++i;
            continue;
        }
        std::size_t want;
        if ((b & 0xE0) == 0xC0) {
            want = 2;
        } else if ((b & 0xF0) == 0xE0) {
            want = 3;
        } else if ((b & 0xF8) == 0xF0) {
            want = 4;
        } else {
            return false;
        }
        if (i + want > s.size()) return false;
        for (std::size_t k = 1; k < want; ++k) {
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
        }
        i += want;
    }
    return true;
}

std::string RepeatUtf8UATest(const char* unit, int n) {
    std::string out;
    for (int i = 0; i < n; ++i) out += unit;
    return out;
}

// Multi-byte fixtures as explicit UTF-8 byte escapes (encoding-proof):
// 한 (U+D55C, 3 bytes, 1 UTF-16 unit), 글 (U+AE00, 3 bytes, 1 unit),
// 。 (U+3002, 3 bytes), and the 4-byte emoji (2 UTF-16 units each).
const std::string kHanU8 = "\xED\x95\x9C";
const std::string kGeulU8 = "\xEA\xB8\x80";
const std::string kStopU8 = "\xE3\x80\x82";
const std::string kTadaU8 = "\xF0\x9F\x8E\x89";
const std::string kConfettiU8 = "\xF0\x9F\x8E\x8A";
const std::string kBalloonU8 = "\xF0\x9F\x8E\x88";

} // namespace

int RunTranslationChunkerTests(int& failures) {
    std::cout << "[TEST] v0.10.9 (261121_0001, U-A) shared 4-tier translation chunker + IPC budget (C1/C3)" << std::endl;
    const int failures_before = g_failed_count;

    // ---- (a) degenerate inputs -------------------------------------------
    TEST_CHECK(SplitTextIntoTranslationChunks("", 1200).empty(),
               "empty input -> empty vector (bounded budget)");
    TEST_CHECK(SplitTextIntoTranslationChunks("", 0).empty(),
               "empty input -> empty vector (unlimited budget)");
    TEST_CHECK(SplitTextIntoTranslationChunks("   ", 100).empty(),
               "whitespace-only input -> empty vector");
    TEST_CHECK(SplitTextIntoTranslationChunks("\t\r\n \n", 0).empty(),
               "whitespace-only input -> empty vector even when unlimited");
    {
        // max_units == 0: no size limit, one chunk, byte-exact.
        const std::string input = "Hello world.";
        const auto chunks = SplitTextIntoTranslationChunks(input, 0);
        TEST_CHECK(chunks.size() == 1 && chunks[0].text == input &&
                       chunks[0].trailing_delimiter.empty(),
                   "unlimited budget: a short input is one verbatim chunk");
        const std::string paragraphs = "para one.\n\npara two.\n\npara three.";
        const auto wide = SplitTextIntoTranslationChunks(paragraphs, 0);
        TEST_CHECK(wide.size() == 1 && JoinChunksUATest(wide) == paragraphs,
                   "unlimited budget: multi-paragraph input stays one chunk");
        // A trailing delimiter with no successor text rides in the chunk
        // text; the join stays byte-exact.
        const auto trailing = SplitTextIntoTranslationChunks("aaa\n\n", 0);
        TEST_CHECK(trailing.size() == 1 && JoinChunksUATest(trailing) == "aaa\n\n",
                   "unlimited budget: a trailing blank line is preserved verbatim");
    }

    // ---- (b) tier 1 (blank line) fires first, tier order ------------------
    {
        // 714-unit paragraph that CONTAINS sentence boundaries; it fits, so
        // tier 1 must keep it whole (tier 3 never gets to cut it).
        std::string para_with_sentences;
        for (int i = 0; i < 7; ++i) {
            para_with_sentences += "Sentence " + std::to_string(i) + ". "; // 12 units
            para_with_sentences += std::string(90, 'a');                    // 102 per round
        }
        TEST_CHECK(para_with_sentences.size() == 714,
                   "tier-1 fixture: the paragraph is 714 units with 7 sentence stops");
        const std::string p2(700, 'b');
        const std::string input = para_with_sentences + "\n\n" + p2; // 1416 units
        const auto chunks = SplitTextIntoTranslationChunks(input, 1200);
        TEST_CHECK(chunks.size() == 2, "tier 1: two fitting paragraphs -> two chunks");
        TEST_CHECK(chunks[0].text == para_with_sentences,
                   "tier order: a fitting paragraph keeps its sentences whole (no tier-3 cut)");
        TEST_CHECK(chunks[0].trailing_delimiter == "\n\n",
                   "tier 1: the blank line is the chunk's verbatim trailing delimiter");
        TEST_CHECK(chunks[1].text == p2 && chunks[1].trailing_delimiter.empty(),
                   "tier 1: the final paragraph has no trailing delimiter");
        TEST_CHECK(JoinChunksUATest(chunks) == input, "tier 1: byte-exact join");
    }
    {
        // Cross-tier packing: a fitting paragraph packs with the first line
        // of the next (paragraph split at tier 1, line split at tier 2).
        const std::string p1(600, 'a');
        const std::string l0(400, 'x');
        const std::string l1(400, 'y');
        const std::string l2(400, 'z');
        const std::string input = p1 + "\n\n" + l0 + "\n" + l1 + "\n" + l2;
        const auto chunks = SplitTextIntoTranslationChunks(input, 1200);
        TEST_CHECK(chunks.size() == 2, "cross-tier: paragraph + lines pack into two chunks");
        TEST_CHECK(chunks[0].text == p1 + "\n\n" + l0 && chunks[0].trailing_delimiter == "\n",
                   "cross-tier: chunk 0 is the paragraph plus the first line (single newline kept)");
        TEST_CHECK(chunks[1].text == l1 + "\n" + l2 && chunks[1].trailing_delimiter.empty(),
                   "cross-tier: chunk 1 is the remaining two lines");
        TEST_CHECK(JoinChunksUATest(chunks) == input, "cross-tier: byte-exact join");
    }
    {
        // Consecutive blank lines: the empty middle segment is preserved.
        const std::string input = "aaa\n\n\n\nbbb";
        const auto chunks = SplitTextIntoTranslationChunks(input, 1000);
        TEST_CHECK(chunks.size() == 1 && JoinChunksUATest(chunks) == input,
                   "consecutive blank lines: one chunk, byte-exact join");
    }

    // ---- (c) tier 2 (single newline) --------------------------------------
    {
        const std::string l0(500, 'x');
        const std::string l1(500, 'y');
        const std::string l2(500, 'z');
        const std::string input = l0 + "\n" + l1 + "\n" + l2; // 1502 units, no blank line
        const auto chunks = SplitTextIntoTranslationChunks(input, 1200);
        TEST_CHECK(chunks.size() == 2, "tier 2: three over-budget lines -> two chunks");
        TEST_CHECK(chunks[0].text == l0 + "\n" + l1 && chunks[0].trailing_delimiter == "\n",
                   "tier 2: lines pack greedily across single newlines");
        TEST_CHECK(chunks[1].text == l2 && chunks[1].trailing_delimiter.empty(),
                   "tier 2: the last line closes the request");
        TEST_CHECK(JoinChunksUATest(chunks) == input, "tier 2: byte-exact join");
    }

    // ---- (d) tier 3 (sentences), tier 4 (spaces), packing boundaries ------
    {
        const std::string s0(300, 'a');
        const std::string s1(300, 'b');
        const std::string s2(300, 'c');
        const std::string s3(300, 'd');
        const std::string s4(300, 'e');
        const std::string input = s0 + ". " + s1 + ". " + s2 + ". " + s3 + ". " + s4 + ".";
        const auto chunks = SplitTextIntoTranslationChunks(input, 700);
        TEST_CHECK(chunks.size() == 3, "tier 3: five over-budget sentences -> three chunks");
        TEST_CHECK(chunks[0].text == s0 + ". " + s1 && chunks[0].trailing_delimiter == ". ",
                   "tier 3: two sentences pack per chunk, \". \" preserved verbatim");
        TEST_CHECK(chunks[1].text == s2 + ". " + s3 && chunks[1].trailing_delimiter == ". ",
                   "tier 3: the middle pair packs the same way");
        TEST_CHECK(chunks[2].text == s4 + "." && chunks[2].trailing_delimiter.empty(),
                   "tier 3: a terminal \".\" without a following space is NOT a boundary token "
                   "(it rides in the text, per the C1 \". \" / \"! \" / \"? \" forms)");
        TEST_CHECK(JoinChunksUATest(chunks) == input, "tier 3: byte-exact join");
    }
    {
        // Full-width sentence stops are tier-3 boundaries too (CJK input).
        const std::string a = RepeatUtf8UATest(kHanU8.c_str(), 5);              // 5 units
        const std::string b = RepeatUtf8UATest(kGeulU8.c_str(), 5);             // 5 units
        const std::string c = RepeatUtf8UATest(kHanU8.c_str(), 3) +
                              RepeatUtf8UATest(kGeulU8.c_str(), 2);             // 5 units
        const std::string input = a + kStopU8 + b + kStopU8 + c;                // 17 units
        const auto chunks = SplitTextIntoTranslationChunks(input, 12);
        TEST_CHECK(chunks.size() == 2, "tier 3 (full-width): 17 units at budget 12 -> two chunks");
        TEST_CHECK(chunks[0].text == a + kStopU8 + b && chunks[0].trailing_delimiter == kStopU8,
                   "tier 3 (full-width): \xE3\x80\x82 is the verbatim trailing delimiter");
        TEST_CHECK(chunks[1].text == c && chunks[1].trailing_delimiter.empty(),
                   "tier 3 (full-width): the tail sentence closes the chunk");
        TEST_CHECK(JoinChunksUATest(chunks) == input, "tier 3 (full-width): byte-exact join");
    }
    {
        const std::string w0(100, 'a');
        const std::string w1(100, 'b');
        const std::string w2(100, 'c');
        const std::string w3(100, 'd');
        const std::string w4(100, 'e');
        const std::string w5(100, 'f');
        const std::string w6(100, 'g');
        const std::string w7(100, 'h');
        const std::string w8(100, 'i');
        const std::string w9(100, 'j');
        const std::string input = w0 + " " + w1 + " " + w2 + " " + w3 + " " + w4 + " " + w5 +
                                  " " + w6 + " " + w7 + " " + w8 + " " + w9; // 1009 units
        const auto chunks = SplitTextIntoTranslationChunks(input, 450);
        TEST_CHECK(chunks.size() == 3, "tier 4: ten over-budget words -> three chunks");
        TEST_CHECK(chunks[0].text == w0 + " " + w1 + " " + w2 + " " + w3 &&
                       chunks[0].trailing_delimiter == " ",
                   "tier 4: four words pack per chunk (403 units), space preserved");
        TEST_CHECK(chunks[1].text == w4 + " " + w5 + " " + w6 + " " + w7,
                   "tier 4: the middle four words pack the same way");
        TEST_CHECK(chunks[2].text == w8 + " " + w9 && chunks[2].trailing_delimiter.empty(),
                   "tier 4: the last two words close the chunk");
        TEST_CHECK(JoinChunksUATest(chunks) == input, "tier 4: byte-exact join");
    }
    {
        // Greedy boundary: four words fill a chunk exactly. The packing
        // budget counts the closing delimiter too (403 text units + 1
        // space = 404), so budget 404 packs four words and the fifth word
        // starts a new chunk.
        const std::string w0(100, 'a');
        const std::string w1(100, 'b');
        const std::string w2(100, 'c');
        const std::string w3(100, 'd');
        const std::string w4(100, 'e');
        const std::string w5(100, 'f');
        const std::string input = w0 + " " + w1 + " " + w2 + " " + w3 + " " + w4 + " " + w5;
        const auto chunks = SplitTextIntoTranslationChunks(input, 404);
        TEST_CHECK(chunks.size() == 2, "packing boundary: a full chunk packs, the next word overflows");
        TEST_CHECK(chunks[0].text == w0 + " " + w1 + " " + w2 + " " + w3 &&
                       chunks[0].trailing_delimiter == " ",
                   "packing boundary: 403 text units + closing space = 404 budget packs");
        TEST_CHECK(chunks[1].text == w4 + " " + w5 && chunks[1].trailing_delimiter.empty(),
                   "packing boundary: the fifth word starts a new chunk");
        TEST_CHECK(JoinChunksUATest(chunks) == input, "packing boundary: byte-exact join");
    }

    // ---- (e) hard-split fallback (tier 4 cannot break the run) ------------
    {
        const std::string input(50, 'a'); // no delimiter at any tier
        const auto chunks = SplitTextIntoTranslationChunks(input, 20);
        TEST_CHECK(chunks.size() == 3, "hard split: a 50-unit run at budget 20 -> three chunks");
        TEST_CHECK(chunks[0].text.size() == 20 && chunks[1].text.size() == 20 &&
                       chunks[2].text.size() == 10,
                   "hard split: 20 + 20 + 10 units, none over budget");
        TEST_CHECK(chunks[0].trailing_delimiter.empty() && chunks[2].trailing_delimiter.empty(),
                   "hard split: pieces carry no trailing delimiter");
        TEST_CHECK(JoinChunksUATest(chunks) == input, "hard split (ASCII): byte-exact join");
    }
    {
        // CJK run: 30 x 3-byte characters (30 UTF-16 units), budget 10.
        const std::string input = RepeatUtf8UATest(kHanU8.c_str(), 30);
        const auto chunks = SplitTextIntoTranslationChunks(input, 10);
        TEST_CHECK(chunks.size() == 3, "hard split (CJK): 30 units at budget 10 -> three chunks");
        bool whole_and_bounded = true;
        for (const TranslationChunk& chunk : chunks) {
            whole_and_bounded = whole_and_bounded && IsWholeUtf8UATest(chunk.text) &&
                                chunk.text.size() == 30; // 10 chars x 3 bytes
        }
        TEST_CHECK(whole_and_bounded,
                   "hard split (CJK): every piece is 10 whole characters, never a split sequence");
        TEST_CHECK(JoinChunksUATest(chunks) == input, "hard split (CJK): byte-exact join");
    }
    {
        // Emoji run with spaces: 4-byte characters (2 UTF-16 units each).
        const std::string e1 = kTadaU8;
        const std::string e2 = kConfettiU8;
        const std::string e3 = kBalloonU8;
        const std::string e4 = kTadaU8;
        const std::string e5 = kConfettiU8;
        const std::string input = e1 + " " + e2 + " " + e3 + " " + e4 + " " + e5; // 14 units
        const auto chunks = SplitTextIntoTranslationChunks(input, 9);
        TEST_CHECK(chunks.size() == 2, "emoji + spaces: 14 units at budget 9 -> two chunks");
        TEST_CHECK(chunks[0].text == e1 + " " + e2 + " " + e3,
                   "emoji + spaces: three emoji pack per chunk (8 units)");
        TEST_CHECK(chunks[1].text == e4 + " " + e5, "emoji + spaces: the tail pair closes");
        TEST_CHECK(JoinChunksUATest(chunks) == input && IsWholeUtf8UATest(chunks[0].text) &&
                       IsWholeUtf8UATest(chunks[1].text),
                   "emoji + spaces: byte-exact join, whole UTF-8 sequences only");
    }
    {
        // Surrogate-pair hard split: 7 emoji (14 units), budget 6 -> pieces
        // of 3/3/1 whole emoji (6/6/2 units). A split inside a 4-byte
        // sequence would break IsWholeUtf8 and the byte-exact join.
        const std::string input = kTadaU8 + kConfettiU8 + kBalloonU8 + kTadaU8 +
                                  kConfettiU8 + kBalloonU8 + kTadaU8;
        const auto chunks = SplitTextIntoTranslationChunks(input, 6);
        TEST_CHECK(chunks.size() == 3, "hard split (emoji): 14 units at budget 6 -> three chunks");
        bool whole = true;
        for (const TranslationChunk& chunk : chunks) {
            whole = whole && IsWholeUtf8UATest(chunk.text) && chunk.text.size() % 4 == 0;
        }
        TEST_CHECK(whole, "hard split (emoji): no chunk splits a surrogate pair");
        TEST_CHECK(chunks[0].text.size() == 12 && chunks[1].text.size() == 12 &&
                       chunks[2].text.size() == 4,
                   "hard split (emoji): 3 + 3 + 1 whole emoji (6/6/2 units, all <= 6)");
        TEST_CHECK(JoinChunksUATest(chunks) == input, "hard split (emoji): byte-exact join");
    }
    {
        // Greedy maximal pieces: 5 emoji (10 units) at budget 7 -> 3 + 2
        // emoji (the second piece takes both remaining whole characters).
        const std::string input = kTadaU8 + kConfettiU8 + kBalloonU8 + kTadaU8 + kConfettiU8;
        const auto chunks = SplitTextIntoTranslationChunks(input, 7);
        TEST_CHECK(chunks.size() == 2 && chunks[0].text.size() == 12 &&
                       chunks[1].text.size() == 8,
                   "hard split (emoji): budget 7 -> 3 + 2 whole emoji (6/4 units)");
        TEST_CHECK(JoinChunksUATest(chunks) == input,
                   "hard split (emoji, budget 7): byte-exact join");
    }

    // ---- (f) mixed-script sweep: the byte-exact join invariant ------------
    {
        std::string mixed = "Alpha beta gamma. Delta epsilon. ";
        mixed += RepeatUtf8UATest(kHanU8.c_str(), 20); // delimiter-free CJK run
        mixed += "\n\n";
        mixed += RepeatUtf8UATest(kGeulU8.c_str(), 8) + kStopU8 +
                 RepeatUtf8UATest(kHanU8.c_str(), 6);
        mixed += "\n";
        mixed += kTadaU8 + " " + kConfettiU8 + " " + kBalloonU8;
        mixed += "\n\n";
        mixed += "Zeta eta theta. Iota kappa.";
        const std::size_t budgets[] = {2, 3, 7, 13, 64, 1200, 0};
        for (const std::size_t max_units : budgets) {
            const auto chunks = SplitTextIntoTranslationChunks(mixed, max_units);
            TEST_CHECK(!chunks.empty(), "sweep: a non-empty input always yields chunks");
            TEST_CHECK(JoinChunksUATest(chunks) == mixed,
                       "sweep: byte-exact join at every budget");
            bool bounded_and_whole = true;
            for (const TranslationChunk& chunk : chunks) {
                bounded_and_whole = bounded_and_whole && IsWholeUtf8UATest(chunk.text) &&
                                    IsWholeUtf8UATest(chunk.trailing_delimiter) &&
                                    (max_units == 0 ||
                                     detail::Utf16UnitsOfUtf8(chunk.text) <= max_units);
            }
            TEST_CHECK(bounded_and_whole,
                       "sweep: every chunk respects the unit bound and whole-sequence integrity");
        }
    }

    // ---- (g) C3 budget formulas -------------------------------------------
    // The header additionally proves client >= worker at COMPILE time via
    // static_assert (0 / 1200 / 100000 and the protocol cap); these checks
    // pin the same formulas at runtime.
    TEST_CHECK(kV107ChunkedDeadlineBaseMs == 30000 && kV107ChunkedPerUnitMs == 12,
               "C3: the deadline formula constants are the contract values");
    TEST_CHECK(kV107ChunkedDeadlineMaxMs == 570000 && kV107ClientTimeoutMaxMs == 600000,
               "C3: the clamp ceilings are 570 s (worker) and 600 s (client)");
    TEST_CHECK(V107ChunkedDeadlineMs(0) == 30000, "C3: the deadline floor is 30 s");
    TEST_CHECK(V107ChunkedDeadlineMs(1200) == 44400,
               "C3: 1200 units scale the deadline by 12 ms each (44400 ms)");
    TEST_CHECK(V107ChunkedDeadlineMs(45000) == 570000,
               "C3: the deadline saturates at 570 s from 45k units on");
    TEST_CHECK(V107ChunkedDeadlineMs(1000000) == 570000, "C3: the deadline ceiling holds");
    TEST_CHECK(V107ClientTimeoutMs(0) == 60000, "C3: the client floor is 30 s + 30 s = 60 s");
    TEST_CHECK(V107ClientTimeoutMs(1200) == 74400,
               "C3: the client timeout is the worker deadline + 30 s");
    TEST_CHECK(V107ClientTimeoutMs(45000) == 600000,
               "C3: the saturated client timeout lands exactly on the protocol cap");
    TEST_CHECK(V107ClientTimeoutMs(1000000) == 600000, "C3: the client ceiling holds");
    {
        const std::size_t sweep[] = {0, 1, 1200, 44999, 45000, 45001, 100000, 1000000};
        for (const std::size_t units : sweep) {
            const int worker = V107ChunkedDeadlineMs(units);
            const int client = V107ClientTimeoutMs(units);
            TEST_CHECK(client >= worker && worker >= kV107ChunkedDeadlineMinMs &&
                           worker <= kV107ChunkedDeadlineMaxMs && client >= 60000 &&
                           client <= 600000,
                       "C3 ordering: client >= worker, both inside their clamps and the protocol cap");
        }
    }

    // ---- (h) source pins on the engine headers ----------------------------
    {
        const std::filesystem::path chunker_path = ResolveRepoFile(L"src\\translation_chunker.hpp");
        const std::filesystem::path budget_path = ResolveRepoFile(L"src\\engine_host_budget.hpp");
        if (chunker_path.empty() || budget_path.empty()) {
            std::cout << "[SKIP] v0.10.9 U-A header pins skipped (engine headers not resolvable "
                         "from the test CWD)." << std::endl;
        } else {
            std::ifstream chunker_in(chunker_path, std::ios::binary);
            const std::string chunker_src((std::istreambuf_iterator<char>(chunker_in)),
                                          std::istreambuf_iterator<char>());
            std::ifstream budget_in(budget_path, std::ios::binary);
            const std::string budget_src((std::istreambuf_iterator<char>(budget_in)),
                                         std::istreambuf_iterator<char>());
            TEST_CHECK(!chunker_src.empty() && !budget_src.empty(),
                       "U-A pins: both engine headers are readable and non-empty");
            // C1 surface + the pure-include contract.
            TEST_CHECK(chunker_src.find("struct TranslationChunk") != std::string::npos &&
                           chunker_src.find("std::vector<TranslationChunk> "
                                            "SplitTextIntoTranslationChunks") != std::string::npos,
                       "C1: the chunker exposes the contract struct and splitter signature");
            TEST_CHECK(chunker_src.find("trailing_delimiter") != std::string::npos,
                       "C1: delimiters are preserved verbatim as trailing_delimiter");
            TEST_CHECK(chunker_src.find("\"\\n\\n\"") != std::string::npos &&
                           chunker_src.find("\"\\n\"") != std::string::npos,
                       "C1: tiers 1 and 2 are the blank-line and single-newline tokens");
            TEST_CHECK(chunker_src.find("\\xE3\\x80\\x82") != std::string::npos &&
                           chunker_src.find("\\xEF\\xBC\\x81") != std::string::npos &&
                           chunker_src.find("\\xEF\\xBC\\x9F") != std::string::npos,
                       "C1: tier 3 carries the full-width sentence stops (U+3002/U+FF01/U+FF1F)");
            TEST_CHECK(chunker_src.find("kChunkTierClause") != std::string::npos,
                       "C1: tier 4 (clause/whitespace) exists");
            TEST_CHECK(chunker_src.find("windows.h") == std::string::npos &&
                           chunker_src.find("llama.h") == std::string::npos,
                       "C1 purity: no Win32 / llama includes");
            // C3 surface + the protocol-cap citation + the compile-time proof.
            TEST_CHECK(budget_src.find("V107ChunkedDeadlineMs") != std::string::npos &&
                           budget_src.find("V107ClientTimeoutMs") != std::string::npos,
                       "C3: both budget formulas exist");
            TEST_CHECK(budget_src.find("kMaxTranslateTimeoutMs") != std::string::npos,
                       "C3: the 600 s ceiling cites the frozen protocol maximum");
            TEST_CHECK(budget_src.find("kV107ClientTimeoutMaxMs = 600000") != std::string::npos,
                       "C3: the client ceiling is pinned to the 600000 ms protocol cap");
            TEST_CHECK(budget_src.find("static_assert(V107ClientTimeoutMs(1200) >=") !=
                           std::string::npos,
                       "C3: the client >= worker ordering is proven by static_assert");
        }
    }

    const int delta = g_failed_count - failures_before;
    failures += delta;
    if (delta == 0) {
        std::cout << "[PASS] v0.10.9 translation chunker / IPC budget tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] v0.10.9 translation chunker / IPC budget tests: " << delta
                  << " check(s) failed." << std::endl;
    }
    return delta;
}
