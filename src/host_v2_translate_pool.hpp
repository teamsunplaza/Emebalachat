#pragma once

// ---------------------------------------------------------------------------
// host_v2_translate_pool — Plan-B (REQ-B003) §3.3 pool-key -> worker-family
// derivation. Pure function (no I/O, no Win32), lifted to its own header so
// run_tests can unit-pin it WITHOUT pulling in host_main.cpp (which is where
// the family constant lives today).
//
// Contract (design §3.3, verbatim):
//   PoolFamilyForModel("")       == kWorkerFamilyTranslate        (L"ggml-translate")
//   PoolFamilyForModel("user-X") == L"ggml-translate-user-X"      (widened, dash-joined)
//   PoolFamilyForModel("bad;id") == L""                           (fail-closed)
//
// The sanitiser accepts exactly [A-Za-z0-9._-] per the registry id
// conventions. Any other character -> empty result: the CALLER treats an
// empty family as model_missing (fail-closed; §3.4 step 3).
// ---------------------------------------------------------------------------

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <cctype>
#include <string>

// Family constant, mirrored from host_main.cpp (a wchar_t[] there, frozen
// installer contract). Duplicated here as a constexpr so this header stays
// free of host_main.cpp — the B-T1 unit pin asserts the exact bytes.
namespace emebalachat {
namespace host_v2 {
namespace translate_pool {

constexpr wchar_t kWorkerFamilyTranslate[] = L"ggml-translate";

inline std::wstring PoolFamilyForModel(const std::string& model_id) {
    if (model_id.empty()) return std::wstring(kWorkerFamilyTranslate);
    std::wstring out(kWorkerFamilyTranslate);
    out += L"-";
    for (unsigned char c : model_id) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.') {
            out += static_cast<wchar_t>(c);
        } else {
            return L""; // invalid id chars -> caller answers model_missing
        }
    }
    return out;
}

} // namespace translate_pool
} // namespace host_v2
} // namespace emebalachat
