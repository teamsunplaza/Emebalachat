// ---------------------------------------------------------------------------
// 261119 E1-A (CEO decision 5.a): wire-regression corpus for the comment-aware
// JSON scanning helpers. The engine repo's frozen enginehost::JsonParseObject
// (engine_host_protocol.hpp) was comment-blind: the engine parses the Chat
// app's config.json through it (engine_host_config_reader.cpp — user_model_id,
// diag_log_enabled, the nested engine_host block), so a commented-out
// user_model_id / engine_host line was silently mis-parsed engine-side. The
// 261119 E1-A fix makes ScanBalanced (line/block comment states, own-group
// stack, escape bounds), JsonReader::SkipWs, and the JsonParseStringArray
// element trivia comment-aware, mirroring the Chat app config reader's
// adopted PR11-01 fix (src/config.cpp CaptureRawValue/SkipWhitespace).
//
// This corpus proves, against the REAL compiled helpers (the header is
// compiled into this runner through the engine repo's Emebala_engine_host):
//   (a) TestEngineHostWireCommentValidCorpus — valid host wire messages that
//       carry '//' line comments and '/* */' block comments (inside objects,
//       inside arrays, and as separators around keys/values) are ACCEPTED;
//       the pass metric printed to the log is "valid=N rejected=0".
//   (b) TestEngineHostWireCommentInvalidCorpus — the malformed messages are
//       STILL rejected with unchanged counts (no acceptance widening); the
//       corpus is comment-free-structural plus comment-involving malformations
//       whose verdict is identical under the old and the new scanner. It is
//       therefore a VERDICT-STABILITY CONTROL, not evidence for the fix: rows
//       that cannot discriminate the change are labelled NON-LOAD-BEARING in
//       place (261119 R17-04c), so no reader mistakes this corpus for proof.
//       Printed metric: "invalid=N rejected=N accepted=0".
//   (c) TestEngineHostWireCommentDifferential — the same logical message in
//       three spellings (no comment / line comment / block comment) parses to
//       IDENTICAL field values, plus the end-to-end CEO legs through the real
//       engine boot readers (LoadUserModelIdFromConfig / LoadHostBootConfig
//       over a temp %LOCALAPPDATA% root): a commented-out user_model_id or
//       engine_host line no longer corrupts the live value.
//
// Frozen-shape note: the wire ops, spellings and member names below are the
// frozen §4.4 vocabulary; only comment TRIVIA differs from the golden bytes.
// ---------------------------------------------------------------------------

namespace {

// One valid-corpus case: a complete wire message plus the frozen op it must
// parse as. Acceptance is judged through the same JsonParseObject the frozen
// Parse* helpers sit on, then representative cases get full typed field
// verification below.
struct E1aValidCase {
    const char* label;
    const char* json;
    const char* op;
};

// One invalid-corpus case: a malformed wire message plus the typed parser
// that must reject it (dispatch by op family).
enum class E1aInvalidParser { Hello, Welcome, Translate, Result, Error, Cancel };

struct E1aInvalidCase {
    const char* label;
    const char* json;
    E1aInvalidParser parser;
};

bool E1aParseWith(const char* json, E1aInvalidParser parser) {
    namespace eh = emebalachat::enginehost;
    switch (parser) {
        case E1aInvalidParser::Hello: {
            eh::HelloMsg m;
            return eh::ParseHello(json, m);
        }
        case E1aInvalidParser::Welcome: {
            eh::WelcomeMsg m;
            return eh::ParseWelcome(json, m);
        }
        case E1aInvalidParser::Translate: {
            eh::TranslateMsg m;
            return eh::ParseTranslate(json, m);
        }
        case E1aInvalidParser::Result: {
            eh::ResultMsg m;
            return eh::ParseResult(json, m);
        }
        case E1aInvalidParser::Error: {
            eh::ErrorMsg m;
            return eh::ParseError(json, m);
        }
        case E1aInvalidParser::Cancel: {
            eh::CancelMsg m;
            return eh::ParseCancel(json, m);
        }
    }
    return false;
}

} // namespace

void TestEngineHostWireCommentValidCorpus() {
    std::cout << "[RUN] Testing engine host wire-comment VALID corpus (261119 E1-A)..."
              << std::endl;
    const int failures_before = g_failed_count;
    namespace eh = emebalachat::enginehost;

    // N valid host wire messages carrying comments. Every message parses
    // under the frozen Parse* contract; each row additionally asserts the
    // op member through JsonParseObject (fresh pair vector per case —
    // JsonParseObject appends to the caller's vector by design).
    const E1aValidCase cases[] = {
        // 1: line comment between members.
        {"hello: line comment between members",
         "{\"op\":\"hello\", // session token\n\"protocol\":1,\"token\":\"t\","
         "\"client\":\"c\",\"client_version\":\"0.10.1\"}",
         "hello"},
        // 2: block comment around a member separator.
        {"hello: block comment around separator",
         "{\"op\":\"hello\" /* c */, \"protocol\":1, \"token\":\"t\" /* c */, "
         "\"client\":\"c\", \"client_version\":\"v\"}",
         "hello"},
        // 3: trailing block comment before '}' (pre-fix this FAILED: the
        // post-value separator check stopped ON the '/').
        {"hello: trailing comment before close",
         "{\"op\":\"hello\",\"protocol\":1,\"token\":\"t\",\"client\":\"c\","
         "\"client_version\":\"v\" /* trailing */ }",
         "hello"},
        // 4: whole leading comment line (Notepad shape).
        {"hello: leading line comment",
         "// emebala engine host config note\n{\"op\":\"hello\",\"protocol\":1,"
         "\"token\":\"t\",\"client\":\"c\",\"client_version\":\"v\"}",
         "hello"},
        // 5: CRLF line comment (Windows Notepad save shape).
        {"hello: CRLF line comment",
         "{\"op\":\"hello\",// note\r\n\"protocol\":1,\"token\":\"t\","
         "\"client\":\"c\",\"client_version\":\"v\"}",
         "hello"},
        // 6: a commented-out member line (the CEO defect shape) — warmup
        // must stay false and the live members must bind.
        {"hello: commented-out member line",
         "{\"op\":\"hello\",\"protocol\":1,\"token\":\"t\",// \"warmup\":true\n"
         "\"client\":\"c\",\"client_version\":\"v\"}",
         "hello"},
        // 7: block comment containing '}' and ']' — must NOT end the group.
        {"hello: close delimiters inside a comment",
         "{\"op\":\"hello\", /* } ] }} */ \"protocol\":1,\"token\":\"t\","
         "\"client\":\"c\",\"client_version\":\"v\"}",
         "hello"},
        // 8: comments around the key and the ':'.
        {"hello: comments around key and colon",
         "{\"op\" /* k */ : /* v */ \"hello\",\"protocol\":1,\"token\":\"t\","
         "\"client\":\"c\",\"client_version\":\"v\"}",
         "hello"},
        // 9: a comment spanning multiple lines with JSON-looking content.
        {"hello: multi-line block comment",
         "{\"op\":\"hello\",\n/* line1 \"protocol\": 99,\n   line2 } */\n"
         "\"protocol\":1,\"token\":\"t\",\"client\":\"c\",\"client_version\":\"v\"}",
         "hello"},
        // 10: '//' inside a STRING is string content, never a comment.
        {"hello: '//' inside a string value",
         "{\"op\":\"hello\",\"protocol\":1,\"token\":\"http://x//y\","
         "\"client\":\"c\",\"client_version\":\"v\"}",
         "hello"},
        // 11: a comment where a value separator is expected.
        {"hello: comment as value separator",
         "{\"op\": // the op\n \"hello\",\"protocol\":1,\"token\":\"t\","
         "\"client\":\"c\",\"client_version\":\"v\"}",
         "hello"},
        // 12: translate with mixed comments; full field check below.
        {"translate: comments around values",
         "{\"op\":\"translate\",/* id */\"id\":7,\"src\":\"ko\",// pair\n"
         "\"tgt\":\"en\",\"text\":\"x\",\"timeout_ms\":30000}",
         "translate"},
        // 13: translate with trailing line comment at EOF (no newline).
        {"translate: trailing line comment at EOF",
         "{\"op\":\"translate\",\"id\":7,\"src\":\"ko\",\"tgt\":\"en\","
         "\"text\":\"x\",\"timeout_ms\":30000} // done",
         "translate"},
        // 14: result ok with comments around the status value.
        {"result: comments around status",
         "{\"op\":\"result\",\"id\":42, /* s */ \"status\":\"ok\","
         "\"text\":\"done\"}",
         "result"},
        // 15: error with a comment before the code.
        {"error: comment before code",
         "{\"op\":\"error\", // c\n\"code\":\"unauthorized\"}",
         "error"},
        // 16: cancel with a block comment around the id.
        {"cancel: block comment around id",
         "{\"op\":\"cancel\", /* c */ \"id\":99}",
         "cancel"},
        // 17: welcome with a comment between the members and comments BETWEEN
        // the capabilities elements (JsonParseStringArray trivia).
        {"welcome: comments inside the capabilities array",
         "{\"op\":\"welcome\",\"protocol\":1,\"model\":\"m\",\"model_sha256\":\"s\","
         "/* c */\"capabilities\":[\"translate\" /* x */, // y\n\"asr\"]}",
         "welcome"},
        // 18: welcome with a ']' inside a block comment — the array end must
        // not be found early (the PR9-08 shape at the array level).
        {"welcome: ']' inside a comment does not end the array",
         "{\"op\":\"welcome\",\"protocol\":1,\"model\":\"m\",\"model_sha256\":\"s\","
         "\"capabilities\":[\"a\" /* ] not the end */, \"b\"]}",
         "welcome"},
        // 19: unknown object-valued field with comments inside (ScanBalanced
        // raw capture; ParseHello ignores the unknown field per §4.3).
        {"hello: unknown object value with comments",
         "{\"op\":\"hello\",\"protocol\":1,\"token\":\"t\",\"client\":\"c\","
         "\"client_version\":\"v\",\"extra\":{ // c\n\"a\":1}}",
         "hello"},
        // 20: unknown array-valued field with a commented-out '}' inside.
        {"hello: unknown array value with commented close",
         "{\"op\":\"hello\",\"protocol\":1,\"token\":\"t\",\"client\":\"c\","
         "\"client_version\":\"v\",\"extra\":[1, /* } */ 2]}",
         "hello"},
        // 21: deeply nested mixed groups with comments at every level.
        {"hello: nested mixed groups with comments",
         "{\"op\":\"hello\",\"protocol\":1,\"token\":\"t\",\"client\":\"c\","
         "\"client_version\":\"v\",\"extra\":{ /* a */ \"a\":[ { \"b\":2 } "
         "/* c */ ] }}",
         "hello"},
        // 22: contract member with a comment (typed integer after trivia).
        {"hello: contract member after a comment",
         "{\"op\":\"hello\",\"protocol\":1,\"token\":\"t\",\"client\":\"c\","
         "\"client_version\":\"v\", /* c */ \"contract\":1}",
         "hello"},
        // 23: translate model_id echo with a comment.
        {"translate: model_id after a comment",
         "{\"op\":\"translate\",\"id\":7,\"src\":\"ko\",\"tgt\":\"en\","
         "\"text\":\"x\",// pin\n\"model_id\":\"user-llama3\"}",
         "translate"},
        // 24: result served-model echo members with comments (C16 diagnostics
        // are still strictly-typed strings).
        {"result: model echo members with comments",
         "{\"op\":\"result\",\"id\":1,\"status\":\"ok\",\"text\":\"d\","
         "/* c */\"model\":\"m\",/* c */\"model_format\":\"gguf\","
         "/* c */\"model_quant\":\"q8_0\"}",
         "result"},
        // 25 (261119 R17-04a): the PR9-08 HEADLINE shape at the nested level —
        // a '// }' note INSIDE an object value. Pre-fix, ScanBalanced counted
        // that '}' and the captured group ended mid-member; the members behind
        // the comment line and the sibling after the group must still bind.
        {"hello: nested object value with a '// }' note",
         "{\"op\":\"hello\",\"protocol\":1,\"token\":\"t\",\"client\":\"c\","
         "\"client_version\":\"v\",\"extra\":{\n  \"a\":1 // } is not the group end\n"
         "  ,\"b\":2},\"contract\":1}",
         "hello"},
    };
    const int kExpectedValid = static_cast<int>(sizeof(cases) / sizeof(cases[0]));
    int valid = 0;
    int rejected = 0;
    for (const auto& kase : cases) {
        eh::JsonPairs pairs;
        const bool ok = eh::JsonParseObject(kase.json, pairs);
        const auto* op = ok ? eh::detail::FindField(pairs, "op") : nullptr;
        const bool op_ok = op && op->is_string && op->text == kase.op;
        if (ok && op_ok) {
            ++valid;
        } else {
            ++rejected;
            TEST_CHECK(false, std::string("e1a valid corpus case rejected: ") + kase.label);
        }
    }
    std::cout << "[INFO] e1a wire-comment valid corpus: valid=" << valid
              << " rejected=" << rejected << " of N=" << kExpectedValid << std::endl;
    TEST_CHECK(valid == kExpectedValid && rejected == 0,
               "e1a valid corpus: valid=N rejected=0 (all valid host wire "
               "messages with comments accepted)");

    // ---- typed field verification on the representative cases -------------
    // (acceptance alone cannot prove the comments did not shift a binding).
    {
        eh::HelloMsg h;
        TEST_CHECK(eh::ParseHello(cases[5].json, h) && h.protocol == 1 &&
                       h.token == "t" && !h.warmup && h.client == "c" &&
                       h.client_version == "v",
                   "e1a: hello with a commented-out warmup line keeps warmup=false");
        TEST_CHECK(eh::ParseHello(cases[6].json, h) && h.protocol == 1 && h.token == "t",
                   "e1a: close delimiters inside a comment do not end the object");
        TEST_CHECK(eh::ParseHello(cases[9].json, h) && h.token == "http://x//y",
                   "e1a: '//' inside a string stays string content");
        TEST_CHECK(eh::ParseHello(cases[10].json, h) && h.protocol == 1,
                   "e1a: a comment where a value was expected is skipped trivia");
        TEST_CHECK(eh::ParseHello(cases[21].json, h) && h.contract == 1,
                   "e1a: contract member parses through a comment");

        // 261119 R17-04a: the nested '// }' shape — the raw ScanBalanced
        // capture must span the comment AND the member behind it, and the
        // outer object must still bind the member after the nested group.
        eh::JsonPairs nested;
        TEST_CHECK(eh::JsonParseObject(cases[24].json, nested),
                   "e1a: nested object value with a '// }' note parses");
        const auto* extra = eh::detail::FindField(nested, "extra");
        TEST_CHECK(extra && extra->text.find("\"b\":2") != std::string::npos,
                   "e1a: '// }' inside a nested object does not truncate the captured group");
        TEST_CHECK(extra && extra->text.find("// } is not the group end") != std::string::npos,
                   "e1a: the nested capture keeps the comment text (nothing swallowed)");
        TEST_CHECK(eh::ParseHello(cases[24].json, h) && h.contract == 1 && h.token == "t",
                   "e1a: the member after a nested '// }' group still binds");

        eh::TranslateMsg t;
        TEST_CHECK(eh::ParseTranslate(cases[11].json, t) && t.id == 7 &&
                       t.src == "ko" && t.tgt == "en" && t.text == "x" &&
                       t.timeout_ms == 30000,
                   "e1a: translate fields bind correctly through comments");
        TEST_CHECK(eh::ParseTranslate(cases[12].json, t) && t.id == 7 && t.text == "x",
                   "e1a: trailing line comment at EOF is ignored");
        TEST_CHECK(eh::ParseTranslate(cases[22].json, t) && t.model_id == "user-llama3",
                   "e1a: model_id binds through a comment");

        eh::ResultMsg r;
        TEST_CHECK(eh::ParseResult(cases[13].json, r) && r.id == 42 &&
                       r.status == eh::HostStatus::Ok && r.text == "done",
                   "e1a: result fields bind correctly through comments");
        TEST_CHECK(eh::ParseResult(cases[23].json, r) && r.model == "m" &&
                       r.model_format == "gguf" && r.model_quant == "q8_0",
                   "e1a: served-model echo members bind through comments");

        eh::WelcomeMsg w;
        TEST_CHECK(eh::ParseWelcome(cases[16].json, w) && w.protocol == 1 &&
                       w.capabilities.size() == 2 && w.capabilities[0] == "translate" &&
                       w.capabilities[1] == "asr",
                   "e1a: comments between capabilities elements parse");
        TEST_CHECK(eh::ParseWelcome(cases[17].json, w) &&
                       w.capabilities.size() == 2 && w.capabilities[0] == "a" &&
                       w.capabilities[1] == "b",
                   "e1a: ']' inside a comment does not truncate the array");

        eh::ErrorMsg e;
        TEST_CHECK(eh::ParseError(cases[14].json, e) && e.code == "unauthorized",
                   "e1a: error code binds through a comment");
        eh::CancelMsg c;
        TEST_CHECK(eh::ParseCancel(cases[15].json, c) && c.id == 99,
                   "e1a: cancel id binds through a comment");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] Engine host wire-comment valid corpus passed (valid="
                  << valid << " rejected=0)." << std::endl;
    } else {
        std::cout << "[FAIL] Engine host wire-comment valid corpus: "
                  << (g_failed_count - failures_before) << " check(s) failed."
                  << std::endl;
    }
}

void TestEngineHostWireCommentInvalidCorpus() {
    std::cout << "[RUN] Testing engine host wire-comment INVALID corpus (261119 E1-A)..."
              << std::endl;
    const int failures_before = g_failed_count;

    // Malformed messages — every one must be rejected.
    //
    // WHAT THIS CORPUS IS NOT (261119 R17-04c): NO row here depends on the
    // E1-A change. Each was rejected by the pre-E1-A comment-blind scanner too
    // (comments are plain text to it, and every group shape below is an
    // unclosed/mismatched one it also refused), so the corpus CANNOT by itself
    // demonstrate that comment-awareness works. It is a VERDICT-STABILITY
    // CONTROL: it proves the new scanner did not start accepting anything it
    // should refuse. The positive evidence for the fix lives in the VALID
    // corpus and the DIFFERENTIAL above. Rows a reader could mistake for
    // coverage carry an explicit "NON-LOAD-BEARING:" line.
    const E1aInvalidCase cases[] = {
        // Structural: broken separators / unterminated shapes.
        // NON-LOAD-BEARING (R17-04c): no closing '}' anywhere — rejected by
        // any scanner, comment-aware or not.
        {"hello: unterminated object",
         "{\"op\":\"hello\",\"protocol\":1,\"token\":\"t\"", E1aInvalidParser::Hello},
        {"hello: missing comma after string value",
         "{\"op\":\"hello\" \"protocol\":1,\"token\":\"t\"}", E1aInvalidParser::Hello},
        {"hello: missing comma after scalar value",
         "{\"op\":\"hello\",\"protocol\":1 \"token\":\"t\"}", E1aInvalidParser::Hello},
        {"hello: unterminated string in token",
         "{\"op\":\"hello\",\"protocol\":1,\"token\":\"t", E1aInvalidParser::Hello},
        {"hello: scalar value followed by garbage",
         "{\"op\":\"hello\",\"protocol\":1,\"token\":t x}", E1aInvalidParser::Hello},
        {"translate: missing id",
         "{\"op\":\"translate\",\"src\":\"ko\",\"tgt\":\"en\",\"text\":\"x\"}",
         E1aInvalidParser::Translate},
        {"translate: garbage id literal",
         "{\"op\":\"translate\",\"id\":7x,\"src\":\"ko\",\"tgt\":\"en\",\"text\":\"x\"}",
         E1aInvalidParser::Translate},
        {"welcome: capabilities as string",
         "{\"op\":\"welcome\",\"protocol\":1,\"model\":\"m\",\"model_sha256\":\"x\","
         "\"capabilities\":\"translate\"}",
         E1aInvalidParser::Welcome},
        {"welcome: capabilities non-string element",
         "{\"op\":\"welcome\",\"protocol\":1,\"model\":\"m\",\"model_sha256\":\"x\","
         "\"capabilities\":[\"translate\",5]}",
         E1aInvalidParser::Welcome},
        {"result: unknown status",
         "{\"op\":\"result\",\"id\":1,\"status\":\"garbage\"}", E1aInvalidParser::Result},
        {"result: missing status",
         "{\"op\":\"result\",\"id\":1}", E1aInvalidParser::Result},
        {"result: mistyped model echo (C16 strict typing)",
         "{\"op\":\"result\",\"id\":1,\"status\":\"ok\",\"text\":\"x\",\"model\":42}",
         E1aInvalidParser::Result},
        {"error: missing code", "{\"op\":\"error\"}", E1aInvalidParser::Error},
        {"cancel: missing id", "{\"op\":\"cancel\"}", E1aInvalidParser::Cancel},
        // Group-shape malformations chosen so BOTH scanners (the old per-kind
        // depth counter and the new own-group stack) reject them — the verdict
        // is identical even though the failing stage can differ.
        // NON-LOAD-BEARING (R17-04c): the group never closes (EOF), so the old
        // scanner returned npos too; the stack changes the STAGE, not the
        // verdict. The narrowing the stack does cause ('{"a":1]' followed by a
        // closing '}' was accepted before) is deliberately NOT exercised here:
        // this corpus stays verdict-stable.
        {"hello: mismatched closer inside object value, EOF",
         "{\"op\":\"hello\",\"protocol\":1,\"token\":\"t\",\"client\":\"c\","
         "\"client_version\":\"v\",\"extra\":{\"a\":1]",
         E1aInvalidParser::Hello},
        {"hello: nested group closer mismatch, EOF",
         "{\"op\":\"hello\",\"protocol\":1,\"token\":\"t\",\"client\":\"c\","
         "\"client_version\":\"v\",\"extra\":[{ \"a\":1 ]",
         E1aInvalidParser::Hello},
        {"hello: array value never closed",
         "{\"op\":\"hello\",\"protocol\":1,\"token\":\"t\",\"client\":\"c\","
         "\"client_version\":\"v\",\"extra\":[1,2}",
         E1aInvalidParser::Hello},
        // Comment-involving malformations: rejected on BOTH scanners.
        // NON-LOAD-BEARING (R17-04c): to the pre-fix scanner these are plain
        // bytes — the unterminated-comment rows fail on the truncated structure
        // anyway, so the verdict does not move with the fix.
        {"hello: unterminated block comment swallows the rest",
         "{\"op\":\"hello\", /* never closed \"protocol\":1,\"token\":\"t\"}",
         E1aInvalidParser::Hello},
        {"hello: unterminated block comment inside an object value",
         "{\"op\":\"hello\",\"protocol\":1,\"token\":\"t\",\"client\":\"c\","
         "\"client_version\":\"v\",\"extra\":{\"a\": /* never closed",
         E1aInvalidParser::Hello},
        // NON-LOAD-BEARING (R17-04c): the EOF row — the object is unterminated
        // with or without comment support, so it is NOT evidence that a line
        // comment before the close is handled.
        {"hello: line comment to EOF before the close",
         "{\"op\":\"hello\",\"protocol\":1,\"token\":\"t\" // done",
         E1aInvalidParser::Hello},
        {"welcome: unterminated comment inside capabilities",
         "{\"op\":\"welcome\",\"protocol\":1,\"model\":\"m\",\"model_sha256\":\"x\","
         "\"capabilities\":[\"a\" /* never closed]}",
         E1aInvalidParser::Welcome},
        // 261119 R17-04b: a comment HIDING a missing separator. The '/* , */'
        // sits exactly where the ',' between two members belongs, so a
        // trivia-blind reader could read it as the separator; the grammar check
        // after the value still sees a '"' where ',' or '}' is required and
        // fails closed. NON-LOAD-BEARING (R17-04c): verdict-stable too — the
        // pre-fix scanner stops ON the '/' and fails at the same check.
        {"hello: comment hides a missing separator",
         "{\"op\":\"hello\",\"protocol\":1,\"token\":\"t\" /* , */\n"
         "\"client\":\"c\",\"client_version\":\"v\"}",
         E1aInvalidParser::Hello},
    };
    const int kInvalid = static_cast<int>(sizeof(cases) / sizeof(cases[0]));
    int rejected = 0;
    int accepted = 0;
    for (const auto& kase : cases) {
        if (E1aParseWith(kase.json, kase.parser)) {
            ++accepted;
            TEST_CHECK(false, std::string("e1a invalid corpus case accepted: ") + kase.label);
        } else {
            ++rejected;
        }
    }
    std::cout << "[INFO] e1a wire-comment invalid corpus: invalid=" << kInvalid
              << " rejected=" << rejected << " accepted=" << accepted
              << " (verdict-stability control; 0 of " << kInvalid
              << " rows discriminate the fix — see NON-LOAD-BEARING labels)" << std::endl;
    TEST_CHECK(rejected == kInvalid && accepted == 0,
               "e1a invalid corpus: every row still rejected (no acceptance widening; the "
               "corpus is a verdict-stability control, not evidence for comment-awareness)");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] Engine host wire-comment invalid corpus passed (rejected="
                  << rejected << " of " << kInvalid << ")." << std::endl;
    } else {
        std::cout << "[FAIL] Engine host wire-comment invalid corpus: "
                  << (g_failed_count - failures_before) << " check(s) failed."
                  << std::endl;
    }
}

void TestEngineHostWireCommentDifferential() {
    std::cout << "[RUN] Testing engine host wire-comment DIFFERENTIAL (261119 E1-A)..."
              << std::endl;
    const int failures_before = g_failed_count;
    namespace eh = emebalachat::enginehost;

    // The same logical message in three spellings — no comment, '//' line
    // comments, '/* */' block comments — must parse to IDENTICAL field
    // values. Any drift means a comment changed a binding.
    struct Spell3 {
        const char* plain;
        const char* line;
        const char* block;
    };
    const Spell3 messages[] = {
        {"{\"op\":\"hello\",\"protocol\":1,\"token\":\"tk\",\"client\":\"emebala-chat\","
         "\"client_version\":\"0.10.1\",\"warmup\":true,\"contract\":1}",
         "// greeting\n{\"op\":\"hello\", // p\n\"protocol\":1,\"token\":\"tk\","
         "\"client\":\"emebala-chat\",\"client_version\":\"0.10.1\","
         "// \"warmup\":false\n\"warmup\":true,\"contract\":1}",
         "{\"op\":\"hello\", /* p */ \"protocol\":1, /* t */ \"token\":\"tk\","
         "\"client\":\"emebala-chat\",\"client_version\":\"0.10.1\","
         "/* \"warmup\":false */ \"warmup\":true,\"contract\":1 /* tail */}"},
        {"{\"op\":\"translate\",\"id\":7,\"src\":\"ko\",\"tgt\":\"en\",\"text\":\"x\","
         "\"timeout_ms\":30000,\"model_id\":\"user-llama3\"}",
         "{\"op\":\"translate\",// id\n\"id\":7,\"src\":\"ko\",\"tgt\":\"en\","
         "\"text\":\"x\",\"timeout_ms\":30000,// pin\n\"model_id\":\"user-llama3\"}",
         "{\"op\":\"translate\",/* id*/\"id\":7,/* s*/\"src\":\"ko\",\"tgt\":\"en\","
         "\"text\":\"x\",\"timeout_ms\":30000,/* pin */\"model_id\":\"user-llama3\"}"},
        {"{\"op\":\"result\",\"id\":42,\"status\":\"ok\",\"text\":\"done\",\"model\":\"m\","
         "\"model_format\":\"gguf\",\"model_quant\":\"q8_0\"}",
         "{\"op\":\"result\",\"id\":42, // status\n\"status\":\"ok\",\"text\":\"done\","
         "// echo\n\"model\":\"m\",\"model_format\":\"gguf\",\"model_quant\":\"q8_0\"}",
         "{\"op\":\"result\",\"id\":42, /* status */ \"status\":\"ok\",\"text\":\"done\","
         "/* echo */\"model\":\"m\",\"model_format\":\"gguf\",\"model_quant\":\"q8_0\"}"},
    };
    for (const auto& m : messages) {
        eh::JsonPairs p0, p1, p2;
        TEST_CHECK(eh::JsonParseObject(m.plain, p0) && eh::JsonParseObject(m.line, p1) &&
                       eh::JsonParseObject(m.block, p2),
                   "e1a differential: all three spellings parse");
        TEST_CHECK(p0.size() == p1.size() && p0.size() == p2.size(),
                   "e1a differential: identical pair counts across spellings");
        bool identical = p0.size() == p1.size() && p1.size() == p2.size();
        for (std::size_t i = 0; identical && i < p0.size(); ++i) {
            if (p0[i].first != p1[i].first || p0[i].first != p2[i].first ||
                p0[i].second.text != p1[i].second.text ||
                p0[i].second.text != p2[i].second.text ||
                p0[i].second.is_string != p1[i].second.is_string ||
                p0[i].second.is_string != p2[i].second.is_string) {
                identical = false;
            }
        }
        TEST_CHECK(identical, "e1a differential: identical key/value pairs across spellings");
    }
    // Typed differential: hello's warmup/contract flags survive commenting.
    {
        eh::HelloMsg a, b, c;
        TEST_CHECK(eh::ParseHello(messages[0].plain, a) && eh::ParseHello(messages[0].line, b) &&
                       eh::ParseHello(messages[0].block, c),
                   "e1a differential: hello parses in all spellings");
        TEST_CHECK(a.warmup == b.warmup && b.warmup == c.warmup && c.warmup,
                   "e1a differential: warmup identical (true) in all spellings");
        TEST_CHECK(a.contract == b.contract && b.contract == c.contract && c.contract == 1,
                   "e1a differential: contract identical (1) in all spellings");
        TEST_CHECK(a.token == b.token && b.token == c.token && c.token == "tk",
                   "e1a differential: token identical across spellings");
    }

    // ---- end-to-end CEO legs: the REAL engine boot readers over a temp ----
    // %LOCALAPPDATA% root. A commented-out user_model_id / engine_host line in
    // config.json must not corrupt the live value — this is the live defect
    // the fix closes (engine_host_config_reader.cpp parses config.json through
    // the frozen JsonParseObject).
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        // 261119 R17-06: per-process root. A fixed name let two concurrently
        // running runners (two build trees, a re-run beside a smoke run) write
        // and remove the SAME config.json; the PID suffix makes each run's tree
        // disjoint. Test-only data, same-user tier, still removed at the end.
        const fs::path root = fs::temp_directory_path(ec) /
                              (L"emebala_e1a_wire_comment_cfg_" +
                               std::to_wstring(::GetCurrentProcessId()));
        if (ec) {
            TEST_CHECK(false, "e1a: temp dir unavailable");
        } else {
            fs::remove_all(root, ec);
            ec.clear();
            fs::create_directories(root / L"Emebalachat", ec);
            if (ec) {
                TEST_CHECK(false, "e1a: temp config dir creation failed");
            } else {
                const std::wstring lad = root.wstring();
                auto write_config = [&](const std::string& body) {
                    std::ofstream out(root / L"Emebalachat" / L"config.json",
                                      std::ios::binary | std::ios::trunc);
                    if (!out.is_open()) {
                        TEST_CHECK(false, "e1a: temp config.json write failed");
                        return;
                    }
                    out << body;
                };

                // Leg 1 (CEO shape): a retired user_model_id commented out, the
                // live id on its own line — the live id must win.
                write_config("{\n  \"engine_type\": \"user_gguf\",\n"
                             "  // \"user_model_id\": \"retired-model\",\n"
                             "  \"user_model_id\": \"live-model\"\n}");
                TEST_CHECK(enginehost::LoadUserModelIdFromConfig(lad) == "live-model",
                           "e1a: commented-out user_model_id line, live id wins");
                // Block-commented retired id, same expectation.
                write_config("{\n  \"engine_type\": \"user_gguf\",\n"
                             "  /* \"user_model_id\": \"retired-model\" */\n"
                             "  \"user_model_id\": \"live-model\"\n}");
                TEST_CHECK(enginehost::LoadUserModelIdFromConfig(lad) == "live-model",
                           "e1a: block-commented retired user_model_id, live id wins");
                // The C1 gate is intact: engine_type=local keeps the pinned path
                // even with a (commented) user_gguf history in the file.
                write_config("{\n  // \"engine_type\": \"user_gguf\",\n"
                             "  \"engine_type\": \"local\",\n"
                             "  \"user_model_id\": \"stale\"\n}");
                TEST_CHECK(enginehost::LoadUserModelIdFromConfig(lad).empty(),
                           "e1a: commented engine_type does not defeat the C1 gate");
                // Leg 2: the nested engine_host block with a commented-out
                // worker_idle_ms line — the live clamp source must win.
                write_config("{\n  \"engine_type\": \"local\",\n"
                             "  \"engine_host\": {\n"
                             "    // \"worker_idle_ms\": 600000,\n"
                             "    \"worker_idle_ms\": 600000\n"
                             "  }\n}");
                TEST_CHECK(enginehost::LoadHostBootConfig(lad).worker_idle_ms == 600000,
                           "e1a: commented-out worker_idle_ms line, live value wins");
                // Leg 3: a commented-out engine_host BLOCK member + a live one;
                // diag flag still reads and the default holds for the absent one.
                write_config("{\n  // \"engine_host\": { \"worker_idle_ms\": 5000 },\n"
                             "  \"engine_host\": { \"worker_idle_ms\": 600000 },\n"
                             "  \"diag_log_enabled\": true\n}");
                const enginehost::HostBootConfig boot =
                    enginehost::LoadHostBootConfig(lad);
                TEST_CHECK(boot.worker_idle_ms == 600000,
                           "e1a: commented-out engine_host block, live block wins");
                TEST_CHECK(boot.diag_log_enabled,
                           "e1a: diag_log_enabled still reads beside commented members");
                TEST_CHECK(boot.user_model_id.empty(),
                           "e1a: engine_type local keeps user_model_id empty (C1)");
            }
            fs::remove_all(root, ec);
            ec.clear();
        }
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] Engine host wire-comment differential passed." << std::endl;
    } else {
        std::cout << "[FAIL] Engine host wire-comment differential: "
                  << (g_failed_count - failures_before) << " check(s) failed."
                  << std::endl;
    }
}
