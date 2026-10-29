#include "model_downloader.hpp"

#include "diag_logger.hpp"
#include "model_verify_sample.hpp" // R1-19 content sample (shared with engine_core)
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
    // REQ-060: the bundled slot is ondemand/evict — the worker is spawned on
    // demand and evicted after the configured idle budget (worker_idle_ms),
    // so a co-resident Listener ASR worker is never pinned out of VRAM.
    e.resource.residency = "ondemand";
    e.resource.eviction = "evict";
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
    // R1-19 (U-H4b): 4 lines, same scheme as engine_core's WriteVerifyMarker -
    // hex, raw file_clock epoch ticks, size, then the content sample (SHA-256
    // over the three fixed 1 MiB windows). Writing only the legacy 3 lines made
    // every completed download a guaranteed cache MISS on the first engine
    // load, i.e. one full ~1.9 GB re-hash (~2 s) for no reason. The stamp is
    // captured BEFORE the sample, exactly like VerifyModelSha256's pre-hash
    // stat: a concurrent writer bumps mtime afterwards and the next check is a
    // miss instead of pairing fresh metadata with older bytes.
    std::string sample_hex;
    if (!modelverify::ComputeSampleSha256(model_path, static_cast<std::uint64_t>(size),
                                          sample_hex)) {
        // Nothing content-bound to write. Leave the marker unwritten (the
        // engine re-hashes once and writes the 4-line form itself) rather than
        // emit a 3-line marker that is guaranteed to miss.
        return false;
    }
    const std::filesystem::path marker =
        model_path.parent_path() / (model_path.filename().native() + L".sha256ok");
    std::ofstream out(marker, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << kModelSha256Pin << '\n'
        << static_cast<long long>(mtime.time_since_epoch().count()) << '\n'
        << static_cast<unsigned long long>(size) << '\n'
        << sample_hex << '\n';
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

// R1-12 (code review mimoflash F-03, docs mining N2 cluster): the registry
// WRITE DECISION, kept pure so the truth table is pinned headlessly.
// LoadDefaultRegistry collapses every non-Ok status to an EMPTY registry
// (engine_host_registry.hpp "Parse result: status != Ok => registry is left
// EMPTY"), so merging + writing on a damaged/unreadable/locked document would
// replace it with a bundled-only one: every origin:"user" entry disappears
// (the .gguf files survive on disk, the entries do not, and the bound
// user_model_id goes stale). This mirrors RegisterUserGgufModel's
// never-overwrite rule (main.cpp "a damaged/schema-rejected document is a
// LOUD failure and is NEVER overwritten"):
//   Ok       clean document          -> merge + rewrite (adds/keeps the slot)
//   Missing  no file on this machine -> the bundled doc IS the first document
//   ReadError / SchemaVersion / NotJson / Malformed (damaged or locked)
//            -> refuse: the file stays byte-identical, the caller reports the
//               existing FetchOutcome::RegistryFailed path (no new message id)
bool ShouldPersistRegistryMerge(engine_host_registry::LoadStatus status) {
    switch (status) {
        case engine_host_registry::LoadStatus::Ok:
        case engine_host_registry::LoadStatus::Missing:
            return true;
        case engine_host_registry::LoadStatus::ReadError:
        case engine_host_registry::LoadStatus::SchemaVersion:
        case engine_host_registry::LoadStatus::NotJson:
        case engine_host_registry::LoadStatus::Malformed:
            return false;
    }
    return false; // unknown future status: fail closed, never overwrite
}

FetchOutcome FetchAndInstallModel(
    const std::function<void(std::uint64_t received, std::uint64_t total)>& on_progress,
    const std::atomic<bool>& cancel,
    const std::function<void(FetchPhase)>& on_phase) {
    const auto phase = [&](FetchPhase p) { if (on_phase) on_phase(p); };

    // R1-24 (docs mining N4): a cancel that is already requested must not
    // start a transfer at all. The dialog closes AND joins this worker on
    // every cancel path, so every early return here shortens that join.
    if (cancel.load(std::memory_order_relaxed)) return FetchOutcome::Cancelled;

    const auto dir = engine_host_registry::DefaultModelsDir();
    if (dir.empty()) {
        DIAG_F("MODELDL/001: no models dir (LOCALAPPDATA absent)\n");
        return FetchOutcome::Failed;
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec); // fresh-machine store may not exist yet

    const std::filesystem::path final_path = dir / ToUtf16(kModelFilename);
    if (std::filesystem::is_regular_file(final_path, ec) && !ec) {
        // R1-24 (N2 comment truth): this early exit returns BEFORE the
        // bundled registry merge at the bottom of this function, so a model
        // that is already in the store is NOT (re)registered here. A store
        // holding the file but missing the bundled entry stays that way until
        // another writer (installer / model manager) merges the slot. The old
        // note claimed the merge was "still (re)written below" for this path;
        // that was never true. Making this path self-heal the entry is a
        // separate behaviour change (N2, still open) and deliberately out of
        // the R1-12 write-gate scope.
        return FetchOutcome::Ok;
    }

    // R1-12 pre-flight, same predicate as the post-transfer gate below: when
    // registry.json is already damaged/locked the merge write is guaranteed
    // to be refused, so do not spend a ~1.9 GB download on an install whose
    // entry could never be persisted. Both loads are cheap (small document);
    // the snapshot is re-read after the transfer because another writer may
    // legitimately merge/register models while the download runs.
    {
        const auto pre = engine_host_registry::LoadDefaultRegistry();
        if (!ShouldPersistRegistryMerge(pre.status)) {
            DIAG_F("MODELDL/006: registry.json load status=%d before the transfer; "
                   "refusing a download whose bundled entry could not be persisted\n",
                   static_cast<int>(pre.status));
            return FetchOutcome::RegistryFailed;
        }
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
    // R1-24 (docs mining N4), the cancel bound stated honestly: DownloadUpdate
    // polls `cancel` before every 256 KiB read, so a cancel while bytes are
    // flowing returns within one chunk. The one unbounded-feeling case is a
    // STALLED socket: WinHttpQueryDataAvailable() then blocks on the session's
    // 30 s receive timeout set inside DownloadUpdate, and that wait is outside
    // this file. Shortening it needs a per-read cancel poll / handle-close seam
    // in update_checker.hpp (disclosed follow-up, not this unit's boundary);
    // nothing in THIS file waits on a timer.
    const UpdateDownloadStatus st = DownloadUpdate(
        kModelDownloadUrl, temp_path.wstring(), kModelExpectedBytes,
        std::string(kModelSha256Pin), on_progress, cancel, opts);
    switch (st) {
        case UpdateDownloadStatus::Ok: break; // hash-verified against the pin inside
        case UpdateDownloadStatus::HashMismatch: return FetchOutcome::HashMismatch;
        case UpdateDownloadStatus::Cancelled: return FetchOutcome::Cancelled;
        default: return FetchOutcome::Failed;
    }

    // Deliberately NO cancel check between here and the registry write. A
    // cancel that lands during the final hash still leaves fully verified
    // bytes; returning Cancelled would make TempGuard delete ~1.9 GB of good
    // download, and finishing the install is also what keeps the store from
    // ending up placed-but-unregistered (the N2 class the R1-12 gate is about).
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
    //
    // R1-12: the load STATUS is part of the write decision (see
    // ShouldPersistRegistryMerge). LoadDefaultRegistry hands back an EMPTY
    // registry for every non-Ok status, so merging + writing here without the
    // status check would replace a damaged/locked registry.json (and every
    // origin:"user" entry in it) with a bundled-only document. On refusal the
    // file is left byte-identical and the pre-existing RegistryFailed path
    // reports the failure (dialog body ModelDlFailed, Retry/Later; no new
    // localized string).
    const auto loaded = engine_host_registry::LoadDefaultRegistry();
    if (!ShouldPersistRegistryMerge(loaded.status)) {
        DIAG_F("MODELDL/007: registry.json load status=%d (damaged/locked); NOT overwriting it "
               "(model placed, bundled entry not persisted)\n",
               static_cast<int>(loaded.status));
        return FetchOutcome::RegistryFailed;
    }
    engine_host_registry::Registry reg = std::move(loaded.registry);
    if (loaded.status == engine_host_registry::LoadStatus::Missing) {
        // Adjacent defect fixed with the R1-12 gate: a default-constructed
        // Registry carries schema_version 0, and SerializeRegistry writes that
        // value verbatim. On a fresh machine (Missing) the merge would then
        // have produced a document the parser REJECTS (schema_version != 1),
        // i.e. the download itself manufactured the damaged state this gate
        // refuses. RegisterUserGgufModel already sets the version on its
        // Missing branch; mirror it here.
        reg.schema_version = engine_host_registry::kRegistrySchemaVersion;
    }
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
