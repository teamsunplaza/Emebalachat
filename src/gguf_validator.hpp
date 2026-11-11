#pragma once

// ---------------------------------------------------------------------------
// 261111_0001 (v0.10.7) U6 — early GGUF validation at user-model registration
// (research R1 P1-4). Today a user model is registered by extension only
// (.gguf tail + bare filename), so a truncated download, a renamed text file
// or a non-llama container fails LATE — at the first translate, inside the
// engine host. This module is the chat-side early gate: it walks the GGUF
// header + metadata KV structure (never the multi-GB tensor data) and
// confirms the structural minimum llama.cpp requires:
//   * magic "GGUF" (little-endian 0x46554747),
//   * container version 2 or 3 (llama.cpp dropped v1),
//   * sane tensor/metadata counts,
//   * a "general.architecture" string key in the metadata KV pairs,
//   * R2-D (261111_0001): ALL declared metadata KVs parse AND the first
//     tensor-info record (name, n_dims <= bound, dims, type, offset) parses
//     too — a file truncated right after the arch field no longer passes
//     registration. Truncation inside tensor-info records 2..N is accepted
//     here (llama.cpp rejects at load; the gate stays bounded).
//   * G-01 (261111_0001 round 2): the metadata section is STREAMED through a
//     bounded sliding window (kStreamWindowBytes) instead of one fixed probe,
//     so real tokenizer-heavy GGUFs whose metadata alone runs to several MB
//     (bundled Hy-MT2-1.8B-Q8_0: 5.07 MB; command-r: 10.87 MB) validate Ok.
//     Round-1's 2 MiB probe cap false-rejected every such model at
//     registration (MalformedHeader). SkipValue semantics are preserved:
//     every declared value's type/shape is still walked, so a truncated or
//     corrupt value still fails.
// It deliberately does NOT duplicate llama.cpp's architecture table —
// support for a specific arch stays llama.cpp's load-time decision; this
// gate only rejects files that can never load. llama-free by construction
// (the Chat exe must not gain a llama symbol).
//
// Wired into the model-manager registration paths
// (src/ui/gguf_model_manager_window.cpp): the HF-download completed path
// and the orphan-file reuse path validate BEFORE the registry write, so a
// bad file is rejected AT REGISTRATION with a localized message. The
// file-picker pipeline (main.cpp RegisterUserGgufModel) is main.cpp-owned
// this session — the same one-call gate is documented for its owner.
//
// Pure/stream helpers: no GUI, no logging side effects, no exceptions out.
// The unit suite (tests/v107_user_model_tests.inc) pins the parser.
// ---------------------------------------------------------------------------

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace emebalachat {
namespace ggufvalidator {

// The structural verdict. Distinct values let the caller log/pin WHY a file
// was refused without parsing message text.
enum class GgufValidationResult {
    Ok,                 // header + metadata keys confirm a llama.cpp-loadable GGUF
    FileUnreadable,     // open/read failure (or not a regular file)
    TooSmall,           // smaller than the 24-byte fixed header
    BadMagic,           // not "GGUF"
    UnsupportedVersion, // container version other than 2/3
    MalformedHeader,    // insane counts, unknown value type, or truncated/garbage
                        // metadata / first tensor-info record
    NoArchitecture,     // metadata parsed (or the input ended) without a
                        // non-empty "general.architecture" string
};

// The fields a successful probe extracts. `architecture` holds the
// general.architecture value (e.g. "llama", "qwen2"); empty on non-Ok.
struct GgufHeaderInfo {
    std::uint32_t version = 0;
    std::uint64_t tensor_count = 0;
    std::uint64_t metadata_count = 0;
    std::string architecture;
};

// Header size: magic u32 + version u32 + tensor_count u64 + metadata_count u64.
inline constexpr std::size_t kGgufFixedHeaderBytes = 4 + 4 + 8 + 8;
static_assert(kGgufFixedHeaderBytes == 24, "GGUF fixed header is 24 bytes");

// G-01 (261111_0001 round 2): the sliding read window for file validation.
// The metadata section can be many megabytes (real tokenizer arrays), so the
// validator re-reads the file through ONE window-sized buffer as the cursor
// advances and skips large values by offset — peak allocation is this window
// plus the tiny captured strings, never the metadata size. This replaces
// round-1's kMaxProbeBytes (2 MiB hard cap), which meant "metadata must fit
// 2 MiB" and false-rejected valid models; a constant of that shape is not
// kept.
inline constexpr std::size_t kStreamWindowBytes = 64 * 1024;

// G-01: strings are materialized (keys compared, general.architecture
// stored) only up to this many bytes; longer strings are consumed but never
// copied into memory. Real GGUF keys and arch values are well under 64
// bytes, so 256 is ample headroom. A "general.architecture" VALUE longer
// than this is treated as unreadable -> NoArchitecture (the same verdict a
// truncated arch value has always produced).
inline constexpr std::size_t kMaxCapturedStringBytes = 256;

// Sanity ceilings for the header counts (a real model has thousands of
// tensors at most; a garbage file claims billions).
inline constexpr std::uint64_t kMaxTensorCount = 1000000;
inline constexpr std::uint64_t kMaxMetadataCount = 1000000;
// R2-D (261111_0001): ceiling for a tensor-info record's n_dims (GGUF allows
// at most 4 meaningful dims; a garbage file can claim billions, which would
// otherwise loop the probe scan).
inline constexpr std::uint32_t kMaxTensorDims = 8;

// PURE parser over an in-memory byte buffer (the whole file when small, or
// any prefix a caller already holds). Walks the SAME structural scan as
// ValidateGgufFile. Never throws; fills *out_info only on Ok (partial
// fields may be set on failure — callers must not rely on them).
GgufValidationResult ParseGgufHeader(std::string_view bytes, GgufHeaderInfo* out_info);

// Streaming file validation: opens `path` and runs the structural scan with
// a bounded sliding window (kStreamWindowBytes) over the metadata section —
// no multi-MB probe, no cap on how large the declared metadata may be.
// Never throws.
GgufValidationResult ValidateGgufFile(const std::filesystem::path& path,
                                      GgufHeaderInfo* out_info);

} // namespace ggufvalidator
} // namespace emebalachat
