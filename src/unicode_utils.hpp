#pragma once

#include <string>
#include <string_view>

namespace emebalachat {

// Normalizes input wide string to Unicode NFC form using Win32 NormalizeString(NormalizationC).
std::wstring NormalizeNFC(std::wstring_view input);

// Multi-line block fix: normalizes every line break to the CRLF pair Windows
// edit controls expect. Lone '\n' and lone '\r' each become "\r\n"; an existing
// CRLF pair is preserved (never doubled). Everything else — including UTF-16
// surrogate pairs, since 0x0D/0x0A never participate in surrogates — passes
// through unchanged. Pure and script-agnostic: works identically for KO/EN/JA/
// ZH/VI/ES and any other text. Applied symmetrically to the captured source
// block and the translated result so the "translation == source" comparison in
// the worker never fails on line-ending representation alone.
std::wstring NormalizeNewlinesToCRLF(std::wstring_view input);

// Converts a UTF-16 wide string to UTF-8 encoded std::string.
std::string ToUtf8(std::wstring_view wstr);

// Converts a UTF-8 encoded string to UTF-16 std::wstring.
std::wstring ToUtf16(std::string_view str);

} // namespace emebalachat
