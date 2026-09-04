#pragma once

#include <string>
#include <string_view>

namespace emebalachat {

// Normalizes input wide string to Unicode NFC form using Win32 NormalizeString(NormalizationC).
std::wstring NormalizeNFC(std::wstring_view input);

// Converts a UTF-16 wide string to UTF-8 encoded std::string.
std::string ToUtf8(std::wstring_view wstr);

// Converts a UTF-8 encoded string to UTF-16 std::wstring.
std::wstring ToUtf16(std::string_view str);

} // namespace emebalachat
