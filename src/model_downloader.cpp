#include "model_downloader.hpp"

#include "diag_logger.hpp"
#include "unicode_utils.hpp" // ToUtf16 (string_view -> wstring)
#include "update_checker.hpp"

#include <cstdio>
#include <fstream>
#include <system_error>
#include <windows.h>

namespace emebalachat {
namespace modeldownloader {

engine_host_registry::ModelEntry BuildBundledEntry() {
    engine_host_registry::ModelEntry e;
    e.id = std::string(kBundledRegistryId);
    e.family = "ggml-translate";
    e.files.push_back(std::string(kModelFilename));
    e.capabilities.push_back("translate");
    e.origin = "bundled";
    e.resource.vram_mb = 2400;
    e.resource.ctx = 4096;
    e.resource.max_sessions = 1;
    e.resource.residency = "preload";
    e.resource.eviction = "sticky";
    e.resource.priority = 9;
    engine_host_registry::SamplingProfile def;
    def.name = "default";
    def.temperature = 0.0;
    def.top_p = 0.6;
    def.top_k = 20;
    def.rep_pen = 1.05;
    def.prompt_template_ref = "hymt2-official";
    e.profiles.emplace("default", def);
    e.lang_pairs.emplace_back("*");
    return e;
}

void MergeBundledEntry(engine_host_registry::Registry& reg) {
    for (const auto& m : reg.models) {
        if (m.origin == "bundled") return; // slot already owned — never duplicate
    }
    reg.models.push_back(BuildBundledEntry());
}

bool WriteSha256OkMarker(const std::filesystem::path& model_path) {
    std::error_code ec;
    const auto mtime = std::filesystem::last_write_time(model_path, ec);
    if (ec) return false;
    const auto size = std::filesystem::file_size(model_path, ec);
    if (ec) return false;
    // Mirror engine_core WriteVerifyMarker: 3 lines (hex, raw file_clock
    // epoch ticks, size). VerifyModelSha256's MarkerMatchesFile re-derives
    // the same values and requires hex == the pin for the pinned name.
    const std::filesystem::path marker =
        model_path.parent_path() / (model_path.filename().native() + L".sha256ok");
    std::ofstream out(marker, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << kModelSha256Pin << '\n'
        << static_cast<long long>(mtime.time_since_epoch().count()) << '\n'
        << static_cast<unsigned long long>(size) << '\n';
    return static_cast<bool>(out);
}

std::uint64_t FreeBytesOnVolume(const std::filesystem::path& dir) {
    std::wstring root;
    if (dir.has_root_path()) {
        root = dir.root_path().wstring();
    } else {
        root = std::filesystem::current_path().root_path().wstring();
    }
    ULARGE_INTEGER free_bytes = {};
    if (::GetDiskFreeSpaceExW(root.empty() ? nullptr : root.c_str(), &free_bytes, nullptr, nullptr) == 0) {
        return 0;
    }
    return static_cast<std::uint64_t>(free_bytes.QuadPart);
}

bool ModelFilePresent() {
    const auto dir = engine_host_registry::DefaultModelsDir();
    if (dir.empty()) return false;
    std::error_code ec;
    return std::filesystem::is_regular_file(dir / ToUtf16(kModelFilename), ec) && !ec;
}

FetchOutcome FetchAndInstallModel(
    const std::function<void(std::uint64_t received, std::uint64_t total)>& on_progress,
    const std::atomic<bool>& cancel,
    const std::function<void(FetchPhase)>& on_phase) {
    const auto phase = [&](FetchPhase p) { if (on_phase) on_phase(p); };

    const auto dir = engine_host_registry::DefaultModelsDir();
    if (dir.empty()) {
        DIAG_F("MODELDL/001: no models dir (LOCALAPPDATA absent)\n");
        return FetchOutcome::Failed;
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec); // fresh-machine store may not exist yet

    const std::filesystem::path final_path = dir / ToUtf16(kModelFilename);
    if (std::filesystem::is_regular_file(final_path, ec) && !ec) {
        return FetchOutcome::Ok; // already there — still (re)write registry below
    }

    // Disk pre-flight (mirror installer MIN_DISK_SPACE_MB + move margin).
    if (FreeBytesOnVolume(dir) < kModelExpectedBytes + kDiskReserveBytes) {
        DIAG_F("MODELDL/002: disk pre-flight failed (< %llu free)\n",
               static_cast<unsigned long long>(kModelExpectedBytes + kDiskReserveBytes));
        return FetchOutcome::NoDiskSpace;
    }

    // Sibling temp + RAII partial cleanup; the atomic move is a same-volume
    // rename, so the verified bytes never leave the volume.
    const std::filesystem::path temp_path =
        dir / (ToUtf16(kModelFilename) + L".emebala-download");
    struct TempGuard {
        const std::filesystem::path* p = nullptr;
        ~TempGuard() {
            if (p && !p->empty()) {
                ::DeleteFileW(p->c_str()); // partial never survives a non-Ok exit
            }
        }
    } guard{&temp_path};

    updatelogic::DownloadOptions opts;
    opts.absolute_cap = updatelogic::kModelMaxDownloadBytes;
    opts.host_allowed = [](std::string_view host) { return IsAllowedModelHost(host); };
    if (on_phase) {
        opts.phase_cb = [&phase](updatelogic::DownloadPhase) { phase(FetchPhase::Verifying); };
    }
    const UpdateDownloadStatus st = DownloadUpdate(
        kModelDownloadUrl, temp_path.wstring(), kModelExpectedBytes,
        std::string(kModelSha256Pin), on_progress, cancel, opts);
    switch (st) {
        case UpdateDownloadStatus::Ok: break; // hash-verified against the pin inside
        case UpdateDownloadStatus::HashMismatch: return FetchOutcome::HashMismatch;
        case UpdateDownloadStatus::Cancelled: return FetchOutcome::Cancelled;
        default: return FetchOutcome::Failed;
    }

    phase(FetchPhase::Placing);
    // Atomic place in the shared store (rename; REPLACE_EXISTING for a
    // same-name leftover). MoveFileExW preserves mtime+size, keeping the
    // marker stamp consistent with the moved file.
    if (::MoveFileExW(temp_path.c_str(), final_path.c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
        DIAG_F("MODELDL/003: atomic move failed (GLE %lu)\n", ::GetLastError());
        return FetchOutcome::MoveFailed;
    }
    guard.p = nullptr; // the temp IS the final file now

    // Bundled registry entry (atomic writer; M7 A-2 multi-writer safe).
    const auto loaded = engine_host_registry::LoadDefaultRegistry();
    engine_host_registry::Registry reg = std::move(loaded.registry);
    MergeBundledEntry(reg);
    if (engine_host_registry::WriteRegistryAtomically(reg, dir) !=
        engine_host_registry::WriteOutcome::Ok) {
        DIAG_F("MODELDL/004: registry write failed; model placed but entry not persisted\n");
        return FetchOutcome::RegistryFailed;
    }

    // Marker cache (best-effort: a failure only costs the one-time re-hash).
    if (!WriteSha256OkMarker(final_path)) {
        DIAG_F("MODELDL/005: .sha256ok marker write failed (non-fatal; engine_core re-hashes once)\n");
    }
    return FetchOutcome::Ok;
}

} // namespace modeldownloader
} // namespace emebalachat
