#pragma once

// ---------------------------------------------------------------------------
// engine_host_json_util — REQ-043 (M6 T2, design §1.3/§3): core-internal scan
// utilities shared by the three parser modules (engine_host_registry /
// engine_host_manifest / engine_host_components).
//
// REUSE DECISION (recorded in the T2 report): the minimal JSON primitives are
// REUSED VERBATIM from the frozen protocol header (JsonParseObject / JsonPairs
// / JsonValue / JsonParseStringArray / detail::FindField / detail::ParseInt /
// detail::ScanBalanced). engine_host_protocol.hpp itself is a frozen contract
// and is NOT modified — this header only ADDS the generic pieces the
// protocol header does not provide:
//   * SplitJsonArray — split a raw JSON array into balanced element texts
//     (the protocol reader keeps nested objects/arrays as RAW text, so
//     array-of-object documents like registry.json models[] need a splitter).
//   * StartsWithUtf8Bom / SkipUtf8Bom — ONE shared definition of the
//     leading-UTF-8-BOM tolerance (REQ-051, the 9cf0d40 config.cpp lesson:
//     a reader that rejects what a BOM-emitting editor wrote hands the
//     failure to whatever rewrite path runs next). Every reader entry
//     point — file or in-memory — routes through SkipUtf8Bom so the
//     tolerance lives in exactly one place.
//   * ReadTextFileUtf8 — whole-file reader with a UTF-8 BOM tolerance strip.
// Plus tiny typed accessors so the three parsers stay declarative.
//
// Privacy: none of these helpers ever log; shape-only DIAG lines live in the
// parser modules (ENGINEHOST/<site>/NNN discipline, diag_logger.hpp).
// ---------------------------------------------------------------------------

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "engine_host_protocol.hpp" // REQ-043 frozen header (unmodified)

namespace emebalachat {
namespace engine_host_json {

using enginehost::JsonPairs;
using enginehost::JsonValue;

// Splits the RAW text of a JSON array (as captured by JsonParseObject for a
// '[...]' value) into its top-level element texts. Objects/arrays keep their
// raw balanced '{...}'/'[...]' span; string elements keep their quotes (use
// enginehost::JsonParseStringArray when the array is known to be all strings —
// it also unescapes). Numbers/true/false/null keep their raw text.
// Returns false for non-array input, unbalanced brackets, or empty elements.
inline bool SplitJsonArray(std::string_view raw, std::vector<std::string>& out) {
    out.clear();
    auto is_ws = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    std::size_t pos = 0;
    while (pos < raw.size() && is_ws(raw[pos])) ++pos;
    if (pos >= raw.size() || raw[pos] != '[') return false;
    ++pos; // past '['
    for (;;) {
        while (pos < raw.size() && is_ws(raw[pos])) ++pos;
        if (pos >= raw.size()) return false; // unterminated
        if (raw[pos] == ']') return true;
        const std::size_t elem_start = pos;
        std::size_t elem_end = std::string_view::npos;
        const char c = raw[pos];
        if (c == '{' || c == '[') {
            const char close = (c == '{') ? '}' : ']';
            elem_end = enginehost::detail::ScanBalanced(raw, pos, c, close);
            if (elem_end == std::string_view::npos) return false;
        } else if (c == '\"') {
            ++pos; // past opening quote
            bool closed = false;
            while (pos < raw.size()) {
                const char s = raw[pos++];
                if (s == '\\') {
                    if (pos >= raw.size()) return false; // dangling escape
                    ++pos;                               // skip escaped char
                } else if (s == '\"') {
                    closed = true;
                    break;
                }
            }
            if (!closed) return false;
            elem_end = pos;
        } else {
            // number / true / false / null: scan to the next delimiter.
            while (pos < raw.size() && raw[pos] != ',' && raw[pos] != ']' &&
                   !is_ws(raw[pos])) {
                ++pos;
            }
            if (pos == elem_start) return false; // empty element
            elem_end = pos;
        }
        out.emplace_back(raw.substr(elem_start, elem_end - elem_start));
        pos = elem_end;
        while (pos < raw.size() && is_ws(raw[pos])) ++pos;
        if (pos < raw.size() && raw[pos] == ',') { ++pos; continue; }
        if (pos < raw.size() && raw[pos] == ']') return true;
        return false;
    }
}

// ---- typed field accessors (unknown-field rule: a MISSING key returns
// false and the caller decides whether absence is fatal for that field) ----

// Required string field: must exist and be a JSON string.
inline bool GetString(const JsonPairs& pairs, std::string_view key, std::string& out) {
    const JsonValue* v = enginehost::detail::FindField(pairs, key);
    if (!v || !v->is_string) return false;
    out = v->text;
    return true;
}

// String field with a default: absent -> default, present -> must be a string
// (a wrong-typed value still fails — type validation, §3.1).
inline bool GetStringOr(const JsonPairs& pairs, std::string_view key,
                        const char* default_value, std::string& out) {
    const JsonValue* v = enginehost::detail::FindField(pairs, key);
    if (!v) {
        out = default_value;
        return true;
    }
    if (!v->is_string) return false;
    out = v->text;
    return true;
}

// Required integer field (raw number text, from_chars; "1.5"/quoted "1" fail).
inline bool GetInt(const JsonPairs& pairs, std::string_view key, int& out) {
    const JsonValue* v = enginehost::detail::FindField(pairs, key);
    if (!v || v->is_string) return false;
    return enginehost::detail::ParseInt(v->text, out);
}

// Required raw object field: must exist and be a balanced '{...}' group; the
// raw text (unknown fields intact) is returned for passthrough/merge use.
inline bool GetRawObject(const JsonPairs& pairs, std::string_view key, std::string& out) {
    const JsonValue* v = enginehost::detail::FindField(pairs, key);
    if (!v || v->is_string) return false;
    if (v->text.empty() || v->text.front() != '{') return false;
    out = v->text;
    return true;
}

// Required raw array field: must exist and be a balanced '[...]' group.
inline bool GetRawArray(const JsonPairs& pairs, std::string_view key, std::string& out) {
    const JsonValue* v = enginehost::detail::FindField(pairs, key);
    if (!v || v->is_string) return false;
    if (v->text.empty() || v->text.front() != '[') return false;
    out = v->text;
    return true;
}

// ---- shared deployment-data filename safety (registry files[] entries and
// manifest pinned keys both MUST be bare filenames: the parsers resolve them
// against a fixed common directory, so any separator/escape is tampering) ----

// True when `name` is a bare filename: non-empty, not "."/"..", and containing
// no path separator ('/' or '\\'), no drive colon, and no control characters.
inline bool IsBareFilename(std::string_view name) {
    if (name.empty() || name == "." || name == "..") return false;
    if (name.find('/') != std::string_view::npos) return false;
    if (name.find('\\') != std::string_view::npos) return false;
    if (name.find(':') != std::string_view::npos) return false;
    for (const char c : name) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7F) return false;
    }
    return true;
}

// ---- leading UTF-8 BOM tolerance (REQ-051: one shared definition) -----------

// The UTF-8 BOM is the three bytes EF BB BF. Windows editors (Notepad,
// PowerShell Set-Content/Out-File) prepend it to UTF-8 text; the frozen
// JsonReader::SkipWs does not recognize it, so a BOM-prefixed document would
// be rejected as NotJson — the exact defect class of the 9cf0d40 config.cpp
// self-destruction (load-side rejection -> defaults -> rewrite destroyed the
// user's file). Skipping the BOM is pure prefix tolerance: it weakens NO
// structural validation of what follows.
inline bool StartsWithUtf8Bom(std::string_view text) {
    return text.size() >= 3 &&
           static_cast<unsigned char>(text[0]) == 0xEF &&
           static_cast<unsigned char>(text[1]) == 0xBB &&
           static_cast<unsigned char>(text[2]) == 0xBF;
}

// Returns `text` with a single leading UTF-8 BOM removed, or `text` unchanged
// (empty input, partial BOM bytes, and non-BOM prefixes all pass through).
// Only ONE BOM is ever skipped — a double-BOM document stays malformed.
inline std::string_view SkipUtf8Bom(std::string_view text) {
    return StartsWithUtf8Bom(text) ? text.substr(3) : text;
}

// ---- whole-file reader (UTF-8 text with a BOM-tolerance strip) -------------

enum class FileReadOutcome { Ok, Missing, ReadError };

// Reads the whole file as bytes (the JSON documents are UTF-8 text). A UTF-8
// BOM is stripped for tolerance (installer/app output is plain UTF-8). No
// exceptions: filesystem errors are reported through the outcome enum.
inline FileReadOutcome ReadTextFileUtf8(const std::filesystem::path& path, std::string& out) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec) {
        // A missing file and an unreadable stat both converge to Missing; the
        // open failure below still distinguishes true ReadError cases.
        std::error_code ec2;
        if (!std::filesystem::exists(path, ec2) || ec2) return FileReadOutcome::Missing;
        return FileReadOutcome::ReadError; // exists but not a regular file
    }
    std::ifstream f(path, std::ios::binary);
    if (!f) return FileReadOutcome::ReadError;
    std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (f.bad()) return FileReadOutcome::ReadError;
    // REQ-051: the BOM strip routes through the shared StartsWithUtf8Bom
    // predicate (one definition for every JSON reader) — byte-identical to
    // the previous inline check.
    if (StartsWithUtf8Bom(data)) {
        data.erase(0, 3);
    }
    out = std::move(data);
    return FileReadOutcome::Ok;
}

} // namespace engine_host_json
} // namespace emebalachat
