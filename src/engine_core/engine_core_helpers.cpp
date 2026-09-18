// engine_core_helpers implementation — REQ-043 (M6 T1, design §4.2/D-1). Every
// function in this file is a VERBATIM move from src/engine.cpp (provenance
// history: M3, F3, REQ-R01, SEC-B2, P7-F2); see engine_core_helpers.hpp for
// the extraction rationale and the conservative boundary decisions. Physical
// move only — no logic change.

#include "engine_core_helpers.hpp"

#include "config.hpp"        // AppConfig::GetLocalAppDataConfigPath (F3 marker-cache fallback dir)
#include "diag_logger.hpp"   // DIAG_F (shape-only diagnostics)
#include "unicode_utils.hpp" // EqualsIgnoreCaseAscii / IsPathContainedIgnoreCaseAscii (M3)

#include <algorithm>
#include <fstream>
#include <vector>

#include <windows.h>
#include <bcrypt.h>

#pragma comment(lib, "bcrypt.lib")

namespace emebalachat {

// M3 (security): fail-closed validation of a GGUF model path before it is handed
// to the llama.cpp loader. Rejections log an ENGINE/IsValidModelPath/NNN code to
// stderr (never to a persisted log). The check is purely lexical + regular-file
// based, so it is unit-testable without loading any model.
bool IsValidModelPath(std::string_view path, std::string_view base_dir) {
    if (path.empty()) {
        DIAG_F("ENGINE/IsValidModelPath/001: empty model path rejected\n");
        return false;
    }

    const std::filesystem::path p{std::string(path)};

    // Extension must be exactly ".gguf" (case-insensitive) so the GGUF parser
    // never touches arbitrary files chosen via a tampered config.json.
    // A6/W3: lazy fold on a view; the temporary extension string still
    // allocates (filesystem::path::extension has no view API), but the
    // second lowered-copy allocation is gone. The temporary lives until the
    // end of the full expression, so the view cannot dangle.
    if (!EqualsIgnoreCaseAscii<char>(p.extension().string(), ".gguf")) {
        DIAG_F("ENGINE/IsValidModelPath/003: non-.gguf model path rejected: %s\n",
                std::string(path).c_str());
        return false;
    }

    // For relative paths, resolve against base_dir (default: current working
    // directory) and collapse '.'/'..' components BEFORE any filesystem access,
    // then verify the resolved target stayed inside base_dir (path-traversal
    // check). Absolute paths define their own location; containment does not
    // apply to them. Existence is checked on the resolved target so the loader
    // (which resolves against the process cwd, i.e. the default base_dir) and
    // this validation agree on which file is being loaded.
    std::filesystem::path target = p;
    if (p.is_relative()) {
        std::error_code ec;
        std::filesystem::path base;
        if (base_dir.empty()) {
            base = std::filesystem::current_path(ec);
            if (ec) {
                DIAG_F("ENGINE/IsValidModelPath/004: cannot resolve base directory; relative model path rejected: %s\n",
                        std::string(path).c_str());
                return false;
            }
        } else {
            base = std::filesystem::path{std::string(base_dir)};
        }

        std::filesystem::path joined = (base / p).lexically_normal();
        std::filesystem::path norm_base = base.lexically_normal();
        // A6/W3: the trailing-'/' trim and the equality-or-prefix+boundary
        // test now live in the shared IsPathContainedIgnoreCaseAscii (byte-
        // equivalent to the old lowered-string expression: the +32 fold is
        // idempotent and length-preserving, and '/' is outside the fold set,
        // so trimming before or after folding makes no difference). Saves two
        // lowered-copy allocations per validation.
        const std::string j = joined.generic_string();
        const std::string b = norm_base.generic_string();
        const bool contained = IsPathContainedIgnoreCaseAscii<char>(j, b);
        if (!contained) {
            DIAG_F("ENGINE/IsValidModelPath/004: relative model path escapes base directory via '..' (path traversal) rejected: %s\n",
                    std::string(path).c_str());
            return false;
        }
        target = joined;
    }

    // Must exist as a regular file (not a directory, device, or missing entry).
    std::error_code ec;
    if (!std::filesystem::is_regular_file(target, ec) || ec) {
        DIAG_F("ENGINE/IsValidModelPath/002: model file does not exist or is not a regular file: %s\n",
                std::string(path).c_str());
        return false;
    }

    return true;
}

// F3 (security, session 260909_0002): SHA-256 over an arbitrary file using
// Windows CNG (bcrypt.dll). Streams in 4 MiB chunks so a 1.91 GB model never
// loads into memory. BCRYPT_FLAG_ALGSCOPE_EXPLICIT is not needed: BCrypt*
// signatures below are the exact SDK declarations (SDK 10.0.26100.0,
// shared/bcrypt.h lines 1213/1522/1535/1545/1591/1303 - verified during
// implementation; dependency-hallucination-check passed against the header).
bool ComputeFileSha256(const std::filesystem::path& file, std::string& out_hex) {
    out_hex.clear();

    HANDLE hFile = ::CreateFileW(file.c_str(), GENERIC_READ,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                                 nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        DIAG_F("ENGINE/ComputeFileSha256/010: CreateFileW failed (err=%lu) path=%s\n",
               ::GetLastError(), file.string().c_str());
        return false;
    }

    // RAII guards keep every early-return path handle-and-API leak free.
    struct HandleGuard {
        HANDLE h;
        ~HandleGuard() { if (h != INVALID_HANDLE_VALUE) ::CloseHandle(h); }
    } file_guard{hFile};

    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    NTSTATUS st = ::BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (st < 0) { // NT_SUCCESS == (st >= 0)
        DIAG_F("ENGINE/ComputeFileSha256/011: BCryptOpenAlgorithmProvider failed (st=0x%08lx)\n",
               static_cast<unsigned long>(st));
        return false;
    }
    struct AlgGuard {
        BCRYPT_ALG_HANDLE* p;
        ~AlgGuard() { if (p && *p) ::BCryptCloseAlgorithmProvider(*p, 0); }
    } alg_guard{&hAlg};

    st = ::BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0);
    if (st < 0) {
        DIAG_F("ENGINE/ComputeFileSha256/012: BCryptCreateHash failed (st=0x%08lx)\n",
               static_cast<unsigned long>(st));
        return false;
    }
    struct HashGuard {
        BCRYPT_HASH_HANDLE* p;
        ~HashGuard() { if (p && *p) ::BCryptDestroyHash(*p); }
    } hash_guard{&hHash};

    std::vector<BYTE> chunk(4 * 1024 * 1024);
    for (;;) {
        DWORD read = 0;
        if (!::ReadFile(hFile, chunk.data(), static_cast<DWORD>(chunk.size()), &read, nullptr)) {
            DIAG_F("ENGINE/ComputeFileSha256/013: ReadFile failed (err=%lu) path=%s\n",
                   ::GetLastError(), file.string().c_str());
            return false;
        }
        if (read == 0) {
            break; // EOF
        }
        st = ::BCryptHashData(hHash, chunk.data(), read, 0);
        if (st < 0) {
            DIAG_F("ENGINE/ComputeFileSha256/014: BCryptHashData failed (st=0x%08lx)\n",
                   static_cast<unsigned long>(st));
            return false;
        }
    }

    BYTE digest[32] = {};
    st = ::BCryptFinishHash(hHash, digest, sizeof(digest), 0);
    if (st < 0) {
        DIAG_F("ENGINE/ComputeFileSha256/015: BCryptFinishHash failed (st=0x%08lx)\n",
               static_cast<unsigned long>(st));
        return false;
    }

    static const char kHex[] = "0123456789abcdef";
    out_hex.reserve(64);
    for (BYTE b : digest) {
        out_hex.push_back(kHex[b >> 4]);
        out_hex.push_back(kHex[b & 0x0F]);
    }
    return true;
}

namespace {

// F3 marker cache: the 1.91 GB hash must run at most ONCE per model file.
// A marker file named "<model>.sha256ok" (next to the model, or in the
// caller-supplied marker_dir for tests) stores:
//   line 1: verified SHA-256 hex
//   line 2: raw last-write mtime (file_clock epoch ticks)
//   line 3: file size in bytes
// VerifyModelSha256() skips the full hash when the marker exists AND both
// mtime and size still match the model on disk - the attacker model behind
// F2/F3 (tamper the GGUF in place) changes mtime, invalidating the cache.
// Touching ONLY the mtime without content change is a local-privileged
// scenario the installer (F2) also cannot distinguish; accepted trade-off,
// documented here.
// Production default (marker_dir empty) checks TWO locations because the
// shipped model lives in {app}\models (Program Files, admin-written by the
// installer) where a non-elevated app process may not be able to create the
// marker: first next to the model, then %LOCALAPPDATA%\Emebalachat.
std::vector<std::filesystem::path> MarkerCandidates(
        const std::filesystem::path& model_path,
        const std::filesystem::path& marker_dir) {
    std::wstring marker_name = model_path.filename().native() + L".sha256ok";
    std::vector<std::filesystem::path> dirs;
    if (!marker_dir.empty()) {
        dirs.push_back(marker_dir);
    } else {
        dirs.push_back(model_path.parent_path());
        // LocalAppData fallback dir: parent of %LOCALAPPDATA%\Emebalachat\config.json
        const auto cfg = AppConfig::GetLocalAppDataConfigPath();
        if (!cfg.empty()) {
            std::filesystem::path la = cfg.parent_path();
            if (la != dirs.front()) {
                dirs.push_back(la);
            }
        }
    }
    std::vector<std::filesystem::path> out;
    out.reserve(dirs.size());
    for (const auto& d : dirs) {
        out.push_back(d / marker_name);
    }
    return out;
}

bool WriteVerifyMarker(const std::filesystem::path& marker,
                       const std::string& hex,
                       const std::filesystem::file_time_type& mtime,
                       uintmax_t size) {
    // Raw file_clock epoch count (100 ns ticks on Windows). No wall-clock
    // conversion is needed: the marker only has to be SELF-CONSISTENT with
    // the values MarkerMatchesFile() recomputes, and the raw count is
    // stable across runs and locale/timezone changes.
    const long long stamp = static_cast<long long>(mtime.time_since_epoch().count());
    std::ofstream out(marker, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out << hex << '\n' << stamp << '\n' << size << '\n';
    return static_cast<bool>(out);
}

bool IsPinnedModelName(const std::filesystem::path& model_path) {
    // A6/W3: pure equality on views — kPinnedModelFilename is already a
    // std::string_view, and the filename temporary lives until the end of the
    // full expression, so no lowered-copy allocations remain here at all
    // (beyond the unavoidable wide→UTF-8 filename conversion).
    return EqualsIgnoreCaseAscii<char>(model_path.filename().string(),
                                       kPinnedModelFilename);
}

bool MarkerMatchesFile(const std::filesystem::path& marker,
                       const std::filesystem::path& model_path) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(marker, ec) || ec) {
        return false;
    }
    std::ifstream in(marker, std::ios::binary);
    if (!in) {
        return false;
    }
    std::string hex;
    long long epoch = -1;
    unsigned long long size = 0;
    if (!(in >> hex >> epoch >> size)) {
        return false;
    }
    // A marker authorizes skipping the hash ONLY in the same situations
    // VerifyModelSha256 itself would authorize: (a) hash equals the pin, or
    // (b) a consent marker for a non-pinned, user-configured filename. A
    // marker naming the PINNED file but carrying a different hash (pin
    // rotation, or a forged consent marker for the pinned name) never hits -
    // the file must re-hash and be compared against the current pin.
    if (hex != kExpectedModelSha256 && IsPinnedModelName(model_path)) {
        return false;
    }
    const auto actual_size = static_cast<unsigned long long>(
        std::filesystem::file_size(model_path, ec));
    if (ec || actual_size != size) {
        return false;
    }
    const auto mtime = std::filesystem::last_write_time(model_path, ec);
    if (ec) {
        return false;
    }
    // Same raw file_clock epoch count that WriteVerifyMarker persisted.
    const auto actual_epoch = static_cast<long long>(mtime.time_since_epoch().count());
    return actual_epoch == epoch;
}

} // namespace

bool VerifyModelSha256(const std::filesystem::path& model_path,
                       const std::filesystem::path& marker_dir) {
    if (model_path.empty()) {
        DIAG_F("ENGINE/VerifyModelSha256/001: empty model path rejected\n");
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(model_path, ec) || ec) {
        DIAG_F("ENGINE/VerifyModelSha256/002: model file missing or not regular: %s\n",
               model_path.string().c_str());
        return false;
    }

    const auto markers = MarkerCandidates(model_path, marker_dir);
    for (const auto& marker : markers) {
        if (MarkerMatchesFile(marker, model_path)) {
            // Cache hit: the file was fully verified before and neither its
            // size nor its last-write time changed since. Skip the 1.91 GB hash.
            return true;
        }
    }

    // Capture size+mtime BEFORE hashing and bind the marker to THAT stamp.
    // Re-stating after the ~2 s hash would let a mid-hash in-place rewrite
    // pair a stale-content hash with a fresh stamp (marker cache poisoning).
    // If the file changes during/after hashing, the next run's stamp check
    // fails and the file simply re-hashes.
    const auto pre_mtime = std::filesystem::last_write_time(model_path, ec);
    if (ec) {
        DIAG_F("ENGINE/VerifyModelSha256/003: cannot stat model file: %s\n",
               model_path.string().c_str());
        return false;
    }
    const auto pre_size = std::filesystem::file_size(model_path, ec);
    if (ec) {
        DIAG_F("ENGINE/VerifyModelSha256/003: cannot size model file: %s\n",
               model_path.string().c_str());
        return false;
    }

    std::string hex;
    if (!ComputeFileSha256(model_path, hex)) {
        DIAG_F("ENGINE/VerifyModelSha256/003: hash computation failed: %s\n",
               model_path.string().c_str());
        return false;
    }

    if (hex == kExpectedModelSha256) {
        // Success: persist the marker (first writable candidate) so the next
        // launch is instant. A marker write failure is NOT a load failure
        // (worst case: re-hash next run).
        bool marker_written = false;
        for (const auto& marker : markers) {
            std::error_code mec;
            std::filesystem::create_directories(marker.parent_path(), mec);
            if (WriteVerifyMarker(marker, hex, pre_mtime, pre_size)) {
                marker_written = true;
                break;
            }
        }
        if (!marker_written) {
            DIAG_F("ENGINE/VerifyModelSha256/004: could not write verification marker for: %s\n",
                   model_path.string().c_str());
        }
        return true;
    }

    // Mismatch. Strict fail-closed ONLY for the pinned filename (see the
    // header contract): that exact name is what installer/setup.iss downloads
    // and what the pin was computed for, so any other bytes are corruption or
    // tampering. Other filenames are models the user deliberately configured
    // (e.g. a self-downloaded Q4 quant): consent-by-config, warn and allow.
    if (IsPinnedModelName(model_path)) {
        DIAG_F("ENGINE/VerifyModelSha256/006: SHA-256 MISMATCH for pinned model (expected %s, got %s); load blocked: %s\n",
               kExpectedModelSha256, hex.c_str(), model_path.string().c_str());
        return false;
    }
    // Consent path: persist a marker keyed on the file's OWN hash so the
    // "once per changed file" rule holds for user models too. MarkerMatchesFile
    // only accepts a non-pin marker for non-pinned filenames, so this can
    // never authorize the shipped model.
    for (const auto& marker : markers) {
        std::error_code mec;
        std::filesystem::create_directories(marker.parent_path(), mec);
        if (WriteVerifyMarker(marker, hex, pre_mtime, pre_size)) {
            break;
        }
    }
    DIAG_F("ENGINE/VerifyModelSha256/005: SHA-256 does not match the shipped-model pin (expected %s, got %s), but the path is a user-configured model name; proceeding on explicit-config consent basis: %s\n",
           kExpectedModelSha256, hex.c_str(), model_path.string().c_str());
    return true;
}

// SEC-B2 (session 260911_0002, verify 233020 §6): pure scrub core. Removes
// every occurrence of every control-class token text from user-controlled
// source text BEFORE BuildPrompt embeds it into the prompt. A single linear
// pass is provably insufficient against split-token reassembly ("<｜hy_"
// + "<｜hy_User｜>" + "User｜>" -> one naive erase RE-FORMS <｜hy_User｜>),
// so the pass loop reruns until a full sweep over the whole ordered token set
// erases nothing. Termination: every iteration of the outer loop either
// strictly shrinks the buffer (>=1 erase) or returns; the buffer cannot shrink
// below zero, so no iteration cap is needed and none exists (a cap would be a
// silent give-up path - unacceptable for a security guard). The post-erase
// rewind keeps each sweep linear without missing a spliced marker (see the
// in-loop comment); cross-TOKEN splicing is caught by the outer rescan.
// The result invariant: it contains no token text from the set anymore.
// Only allocation failure can escape (documented in engine.hpp) - there is
// deliberately NO fallback to unsanitized text.
std::wstring ScrubControlTokenTexts(std::wstring_view text, const std::vector<std::wstring>& tokens) {
    std::wstring out(text);
    if (out.empty() || tokens.empty()) {
        return out;
    }

    // Longest-first: a shorter marker that is a substring of a longer one must
    // never block the longer one's removal (mirrors llama.cpp's own
    // cache_special_tokens sort by decreasing text length).
    std::vector<const std::wstring*> ordered;
    ordered.reserve(tokens.size());
    for (const std::wstring& tok : tokens) {
        if (!tok.empty()) {
            ordered.push_back(&tok);
        }
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const std::wstring* a, const std::wstring* b) { return a->size() > b->size(); });

    bool removed_any = true;
    while (removed_any) {
        removed_any = false;
        for (const std::wstring* tok : ordered) {
            size_t pos = 0;
            while ((pos = out.find(*tok, pos)) != std::wstring::npos) {
                out.erase(pos, tok->size());
                removed_any = true;
                // Reassembly check without a full restart: deletion only shifts
                // content left, so a marker newly SPLICED by the erase must
                // overlap the erase point - its start lies within the last
                // tok->size()-1 units before it (e.g. "<｜hy_" + "<｜hy_User｜>"
                // + "User｜>" -> erase leaves "<｜hy_User｜>" starting at 0).
                // Rewinding the search to that window sees every reassembly
                // while keeping each sweep linear. Cross-token reassembly
                // (marker of token A spliced by erasing token B) is caught by
                // the outer removed_any rescan.
                const size_t rewind = tok->size() > 0 ? tok->size() - 1 : 0;
                pos = pos > rewind ? pos - rewind : 0;
            }
        }
    }
    return out;
}

#ifdef HAVE_LLAMA_CPP
// SEC-B2: enumerate the scrub set from the loaded vocab. Matches exactly the
// attr set llama-vocab.cpp L2396-2402 caches for parse_special=true substring
// partitioning (CONTROL | USER_DEFINED | UNKNOWN) so the scrub covers 1:1 the
// tokens the tokenizer would emit as genuine control tokens.
std::vector<std::wstring> CollectControlTokenTexts(const llama_vocab* vocab) {
    std::vector<std::wstring> out;
    if (!vocab) {
        return out;
    }
    const int32_t n = llama_vocab_n_tokens(vocab);
    out.reserve(static_cast<size_t>(n > 0 ? n : 0));
    for (llama_token id = 0; id < n; ++id) {
        const llama_token_attr attr = llama_vocab_get_attr(vocab, id);
        if (attr & (LLAMA_TOKEN_ATTR_CONTROL | LLAMA_TOKEN_ATTR_USER_DEFINED | LLAMA_TOKEN_ATTR_UNKNOWN)) {
            const char* text = llama_vocab_get_text(vocab, id);
            if (text && *text) {
                out.push_back(ToUtf16(text));
            }
        }
    }
    return out;
}
#endif // HAVE_LLAMA_CPP

#ifdef HAVE_LLAMA_CPP
// P7-F2: pure params-construction seam declared in src/engine.hpp (re-exported
// via engine_core_helpers.hpp). Single source of truth for the two model-load
// legs of EnsureLoaded (translation_common.cpp); pinned headlessly by
// TestP7F2GpuOffloadParams (tests/run_tests.cpp) so the CUDA+Vulkan
// layer-split prevention can never silently drift from the test.
void SetGpuOffloadParams(llama_model_params& params, bool gpu_offload) {
    if (gpu_offload) {
        // GPU leg: full offload, whole model pinned to device 0. WHY the pin:
        // with GGML_CUDA + GGML_VULKAN both statically linked, one NVIDIA
        // card registers as TWO devices with no dedup (llama.cpp L183-190),
        // and b6099's default LLAMA_SPLIT_MODE_LAYER would interleave ~half
        // the layers onto the slower Vulkan half of the SAME card (P5 F2).
        // NONE keeps only devices[main_gpu] (llama.cpp L200-213); ggml
        // registers CUDA before Vulkan (ggml-backend-reg.cpp L168 vs L177)
        // and the device list preserves that order, so device 0 = CUDA on
        // NVIDIA; on AMD/Intel (Vulkan-only) there is a single device and
        // this is a no-op. Evidence citations: build_gputest/_deps/
        // llama_cpp-src headers (b6099), session 260909_0004 P5 report.
        params.n_gpu_layers = 99; // Offload layers to RTX 2070 Turing GPU (sm_75)
        params.split_mode = LLAMA_SPLIT_MODE_NONE;
        params.main_gpu = 0;
    } else {
        // CPU fallback leg: n_gpu_layers=0 AND restore the b6099 default
        // split_mode. Un-pinning is NOT optional: llama.cpp validates
        // split_mode/main_gpu against the GPU-device list even at
        // n_gpu_layers=0 (llama.cpp L204-207 rejects main_gpu=0 when zero
        // GPU devices are enumerable), so carrying NONE into this retry would
        // hard-break the historical CPU fallback on CPU-only machines. With
        // LAYER + empty device list the load behaves exactly as pre-F2.
        params.n_gpu_layers = 0;
        params.split_mode = LLAMA_SPLIT_MODE_LAYER;
        // params.main_gpu left untouched (b6099 default is already 0).
    }
    // Invariant (both legs): no explicit device list and no tensor split -
    // the app never engages multi-device splitting by design (P7-F2).
    params.devices = nullptr;
    params.tensor_split = nullptr;
}
#endif // HAVE_LLAMA_CPP

} // namespace emebalachat
