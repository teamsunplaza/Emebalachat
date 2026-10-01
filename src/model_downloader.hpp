#pragma once

// ---------------------------------------------------------------------------
// REQ-MD (session 260930_0004, decisions.md D9): on-demand download of the
// bundled Hy-MT2-1.8B model. When the user picks the built-in local engine
// (or the boot probe finds the store incomplete) and ONLY the pinned model
// file is absent, the "engine unavailable" notice becomes actionable:
// consent -> progress -> pinned SHA-256 verify -> atomic move into the shared
// store -> bundled registry.json entry -> the next translation serves.
//
// Reuses the update checker's DownloadUpdate engine (src/update_checker.hpp)
// through the generalized DownloadOptions seam: an HF host allowlist and a
// 4 GiB absolute cap, without changing the update path's pinned behavior.
// The SHA-256 pin MIRRORS the installer/engine constants — SYNC NOTE below.
//
// Failure contract (same as the update checker): quiet inline states in the
// dialog, fail-closed on hash mismatch (a bad file is never placed/served),
// partial temp always deleted, atomic rename into the store.
// ---------------------------------------------------------------------------

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "engine_host_registry.hpp"

namespace emebalachat {
namespace modeldownloader {

// Canonical source (installer/setup.iss MODEL_URL — read-only reference).
inline constexpr std::string_view kModelDownloadUrl =
    "https://huggingface.co/tencent/Hy-MT2-1.8B-GGUF/resolve/main/Hy-MT2-1.8B-Q8_0.gguf";
inline constexpr std::string_view kModelFilename = "Hy-MT2-1.8B-Q8_0.gguf";

// SYNC NOTE: this pin MUST equal kExpectedModelSha256 in
// src/engine_host_client.hpp (client pin), src/engine_core/engine_core_helpers.hpp
// (runtime verify pin) and EXPECTED_MODEL_SHA256 in installer/setup.iss.
// tools/check_model_sha_sync.py is the 3-way gate for those; run_tests pins
// THIS mirror to engine_host::kExpectedModelSha256 so a rotation updates all
// copies together (release procedure: certutil -hashfile <model> SHA256).
inline constexpr std::string_view kModelSha256Pin =
    "5c3fe0b1408a5ceb0143184ef247b11b579c525f4b02b060e6c851bb76fef1a4";

// The known-good byte size of the pinned file (advertised cross-check input;
// a wildly different Content-Length/stream fails the download cap).
inline constexpr std::uint64_t kModelExpectedBytes = 1908528192ull;

// Registry: the installer's pinned bundled id + entry shape
// (installer/setup.iss REGISTRY_BUNDLED_ID / REGISTRY_BUNDLED_ITEM_*).
inline constexpr std::string_view kBundledRegistryId = "hy-mt2-1.8b-q8";

// Disk pre-flight: the download needs its own bytes plus a safety margin
// (the temp is a store sibling; the atomic move is a same-volume rename, so
// peak usage ~= size + margin — mirroring the installer's 3 GB pre-flight).
inline constexpr std::uint64_t kDiskReserveBytes = 512ull * 1024 * 1024;

// ---- pure helpers (headless-testable) -------------------------------------

// REQ-MD: HF host allowlist for the generalized download seam. huggingface.co
// serves the file and 302s to its CDN (*.huggingface.co / *.hf.co); anything
// else fails closed. Pure + unit-pinned.
inline bool IsAllowedModelHost(std::string_view host) {
    constexpr std::string_view kHfSuffix = ".huggingface.co";
    constexpr std::string_view kHfCoSuffix = ".hf.co";
    if (host == "huggingface.co") return true;
    const auto ends_with = [host](std::string_view suffix) {
        return host.size() > suffix.size() &&
               host.compare(host.size() - suffix.size(), suffix.size(), suffix) == 0;
    };
    return ends_with(kHfSuffix) || ends_with(kHfCoSuffix);
}

// The bootstrapped component-check "missing" list uses an "engine/" or
// "models/" prefix per entry. The download can ONLY fix missing models/-
// entries (engine binaries are not fetchable here). Pure + unit-pinned.
inline bool MissingIsModelOnly(const std::vector<std::string>& missing) {
    if (missing.empty()) return false;
    for (const std::string& entry : missing) {
        if (entry.rfind("models/", 0) != 0) return false;
    }
    return true;
}

enum class OfferPlan {
    Nothing,     // model present / engine binaries missing -> old notice path
    Offer,       // show the consent dialog
    NoDiskSpace, // show the consent dialog with the disk-space line only
};
inline OfferPlan PlanOffer(bool engine_components_ok,
                           bool model_present,
                           std::uint64_t free_bytes) {
    if (!engine_components_ok || model_present) return OfferPlan::Nothing;
    if (free_bytes < kModelExpectedBytes + kDiskReserveBytes) return OfferPlan::NoDiskSpace;
    return OfferPlan::Offer;
}

// Append the pinned bundled entry to `reg` when no bundled entry exists yet
// (pure; the caller persists with WriteRegistryAtomically). Mirrors the
// installer shape exactly. Unit-pinned via a parse round-trip.
void MergeBundledEntry(engine_host_registry::Registry& reg);

// Build the pinned bundled ModelEntry (shape mirror of installer/setup.iss
// REGISTRY_BUNDLED_ITEM_*). Exposed for tests.
engine_host_registry::ModelEntry BuildBundledEntry();

// Write the engine_core-compatible <model>.sha256ok marker (3 lines:
// hex, raw file_clock epoch ticks, size) so the worker's VerifyModelSha256
// marker cache skips the one-time 1.91 GB re-hash. Mirrors
// engine_core_helpers WriteVerifyMarker/VerifyModelSha256 semantics. The
// mtime/size are read AFTER the atomic move (rename preserves both).
bool WriteSha256OkMarker(const std::filesystem::path& model_path);

// ---- I/O ------------------------------------------------------------------

// Free bytes on the volume hosting `dir` (0 when unknown).
std::uint64_t FreeBytesOnVolume(const std::filesystem::path& dir);

enum class FetchPhase {
    Downloading,
    Verifying,
    Placing, // hash OK: atomic move + registry entry + marker
};
enum class FetchOutcome {
    Ok,            // downloaded, hash-verified, moved, registry + marker done
    Failed,        // network/IO (temp deleted) — quiet retry state
    HashMismatch,  // pinned-hash failure (fail-closed; temp deleted; the red state)
    Cancelled,     // user cancel (temp deleted)
    NoDiskSpace,   // pre-flight failure
    MoveFailed,    // verified temp could not be renamed into the store
    RegistryFailed // file placed but the bundled entry could not be persisted
};

// The whole post-consent pipeline: pre-flight -> streaming download (HF
// allowlist + 4 GiB cap + pinned-hash verify inside DownloadUpdate) ->
// atomic move into %LOCALAPPDATA%\Emebala\Common\models -> registry merge ->
// .sha256ok marker. Blocking: run on a worker thread, never the GUI thread.
FetchOutcome FetchAndInstallModel(
    const std::function<void(std::uint64_t received, std::uint64_t total)>& on_progress,
    const std::atomic<bool>& cancel,
    const std::function<void(FetchPhase)>& on_phase = {});

// True when the pinned model file already sits in the shared store.
bool ModelFilePresent();

} // namespace modeldownloader
} // namespace emebalachat
