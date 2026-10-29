#pragma once

// ---------------------------------------------------------------------------
// model_verify_sample - R1-19 (session 261029_0001, v0.10.4 sweep) content
// sample for the <model>.sha256ok marker cache, in a LLAMA-FREE home.
//
// WHY this file exists (U-H4b, session 261029_0001): the R1-19 sample was
// born file-local inside src/engine_core/engine_core_helpers.cpp, which lives
// in Emebalachat_engine_core (the llama-coupled library). The model
// DOWNLOADER (src/model_downloader.cpp, Emebalachat_core) must write the same
// 4-line marker after a completed download, and core must never link
// engine_core (M6 T1 boundary: the Chat exe is llama-free). Copying the
// geometry into the downloader would let the writer and the verifier drift
// apart silently - each drift costs a guaranteed cache MISS (a full 1.9 GB
// re-hash) with no test able to see it. So the scheme has exactly ONE
// definition, here, and both sides call it:
//   * writer + verifier: engine_core_helpers.cpp (ComputeSampleSha256 call
//     sites - cache check, marker write after a full hash)
//   * writer: model_downloader.cpp (WriteSha256OkMarker, after the download
//     has been hash-verified and atomically placed)
//
// THE SCHEME: line 4 of "<model>.sha256ok" is SHA-256 over three fixed 1 MiB
// windows of the model (start, middle, end). Cost: 3 MiB of reads - measured
// 12.9 ms on this machine (warm cache; CNG is the same order) and bounded by
// 3 MiB of cold I/O otherwise - against the ~1.5-2.5 s full hash of the
// 1.91 GB shipped model (2.2% of the full-hash cost at 256 MiB, and the
// sample cost is constant in file size while the hash scales). The "one full
// hash per model file" property the cache exists for survives.
//   Detection: a different model file (or a corrupted/truncated copy) differs
//   from the pinned bytes throughout, so at least one window differs; a swap
//   that keeps a stale marker therefore re-hashes, and a changed hash on the
//   pinned filename fails closed exactly like any other mismatch.
//   LIMIT (documented, not hidden): an attacker who can write BOTH the model
//   and the marker can compute the sample of their own bytes. The sample
//   raises the forgery cost from "write marker bytes, know nothing about the
//   file" to "hash the file you just installed" - it is not a cryptographic
//   boundary, and it does not move the threat tier (same-user, where swapping
//   the engine exe itself is a cheaper route to the same privilege).
//
// Header-only (inline) on purpose: it keeps the helper out of every static
// library's link graph, so engine_core, core and the standalone crash-isolation
// test all resolve the SAME body without a CMake or link-order change.
// ---------------------------------------------------------------------------

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "diag_logger.hpp" // DIAG_F (shape-only diagnostics)

#include <windows.h>
#include <bcrypt.h>

#if defined(_MSC_VER)
#pragma comment(lib, "bcrypt.lib")
#endif

namespace emebalachat {
namespace modelverify {

// 1 MiB per window: large enough that a same-size swap cannot keep one window
// identical by luck, small enough that the warm cost stays in the low tens of
// milliseconds on the shipped 1.91 GB model.
inline constexpr std::uint64_t kSampleWindowBytes = 1024 * 1024;

struct SampleSpan {
    std::uint64_t offset;
    std::uint64_t length;
};

// Deterministic window plan for a given size: [0,w), [size/2, +w), [size-w, w),
// each clipped to the file, dropping a window already covered by an earlier one
// (a file smaller than two windows degenerates to "hash the whole file"). The
// plan is a pure function of `size`, so the writer and the verifier always
// agree on which bytes line 4 covers.
inline std::vector<SampleSpan> SampleSpans(std::uint64_t size) {
    std::vector<SampleSpan> spans;
    const std::uint64_t w = kSampleWindowBytes;
    if (size == 0) {
        return spans;
    }
    const auto add = [&spans](std::uint64_t offset, std::uint64_t length) {
        if (length == 0) {
            return;
        }
        for (const auto& kept : spans) {
            if (offset >= kept.offset && offset + length <= kept.offset + kept.length) {
                return; // already covered by an earlier window
            }
        }
        spans.push_back({offset, length});
    };
    const std::uint64_t edge = std::min<std::uint64_t>(w, size);
    add(0, edge);                                          // GGUF header + first tensors
    add(size / 2, std::min<std::uint64_t>(w, size - size / 2));  // middle tensor block
    add(size - edge, edge);                                // last tensors
    return spans;
}

// SHA-256 over those windows, streamed with the same CNG calls as
// ComputeFileSha256(). `file_size` is the size the caller already stat'ed and
// checked (marker comparison, or the pre-hash stat), so the plan cannot drift
// from a re-stat. Emits ENGINE/ComputeSampleSha256/NNN (shape-only: errno,
// status, offsets - never file content) on the failure paths.
inline bool ComputeSampleSha256(const std::filesystem::path& file, std::uint64_t file_size,
                                std::string& out_hex) {
    out_hex.clear();

    HANDLE hFile = ::CreateFileW(file.c_str(), GENERIC_READ,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        DIAG_F("ENGINE/ComputeSampleSha256/010: CreateFileW failed (err=%lu) path=%s\n",
               ::GetLastError(), file.string().c_str());
        return false;
    }
    struct HandleGuard {
        HANDLE h;
        ~HandleGuard() { if (h != INVALID_HANDLE_VALUE) ::CloseHandle(h); }
    } file_guard{hFile};

    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    NTSTATUS st = ::BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (st < 0) {
        DIAG_F("ENGINE/ComputeSampleSha256/011: BCryptOpenAlgorithmProvider failed (st=0x%08lx)\n",
               static_cast<unsigned long>(st));
        return false;
    }
    struct AlgGuard {
        BCRYPT_ALG_HANDLE* p;
        ~AlgGuard() { if (p && *p) ::BCryptCloseAlgorithmProvider(*p, 0); }
    } alg_guard{&hAlg};

    st = ::BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0);
    if (st < 0) {
        DIAG_F("ENGINE/ComputeSampleSha256/012: BCryptCreateHash failed (st=0x%08lx)\n",
               static_cast<unsigned long>(st));
        return false;
    }
    struct HashGuard {
        BCRYPT_HASH_HANDLE* p;
        ~HashGuard() { if (p && *p) ::BCryptDestroyHash(*p); }
    } hash_guard{&hHash};

    std::vector<BYTE> chunk(static_cast<size_t>(kSampleWindowBytes));
    for (const SampleSpan& span : SampleSpans(file_size)) {
        LARGE_INTEGER distance;
        distance.QuadPart = static_cast<LONGLONG>(span.offset);
        if (!::SetFilePointerEx(hFile, distance, nullptr, FILE_BEGIN)) {
            DIAG_F("ENGINE/ComputeSampleSha256/013: SetFilePointerEx failed (err=%lu) offset=%llu\n",
                   ::GetLastError(), static_cast<unsigned long long>(span.offset));
            return false;
        }
        std::uint64_t remaining = span.length;
        while (remaining > 0) {
            const DWORD want =
                static_cast<DWORD>(std::min<std::uint64_t>(remaining, chunk.size()));
            DWORD read = 0;
            if (!::ReadFile(hFile, chunk.data(), want, &read, nullptr) || read == 0) {
                DIAG_F("ENGINE/ComputeSampleSha256/014: ReadFile failed (err=%lu) offset=%llu\n",
                       ::GetLastError(), static_cast<unsigned long long>(span.offset));
                return false;
            }
            st = ::BCryptHashData(hHash, chunk.data(), read, 0);
            if (st < 0) {
                DIAG_F("ENGINE/ComputeSampleSha256/015: BCryptHashData failed (st=0x%08lx)\n",
                       static_cast<unsigned long>(st));
                return false;
            }
            remaining -= read;
        }
    }

    BYTE digest[32] = {};
    st = ::BCryptFinishHash(hHash, digest, sizeof(digest), 0);
    if (st < 0) {
        DIAG_F("ENGINE/ComputeSampleSha256/016: BCryptFinishHash failed (st=0x%08lx)\n",
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

} // namespace modelverify
} // namespace emebalachat
