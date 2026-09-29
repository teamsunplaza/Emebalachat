// host_v2_worker_manager implementation — REQ-043 (M6 T3, design §1.1).
// The Win32 plumbing behind the header's seam surface: real spawn (hidden,
// same user, token on the command line), private pipe creation with the
// user-only SD (the host_main.cpp BuildUserOnlySd pattern, restated here so
// the orchestrator does not depend on the frozen v1 TU), the M-2 ordered
// graceful stop, and the 250 ms reaper thread with exponential backoff.

#include "host_v2_worker_manager.hpp"
#include "host_v2_translate_pool.hpp" // Plan-B §3.3 PoolFamilyForModel (pure)
#include "engine_host_vram_gate.hpp"  // Plan-B B-T3 DecideSpawnVram (pure, host_v2 member)

#include <bcrypt.h>   // BCryptGenRandom (boot-scoped worker token)
#include <sddl.h>
#include <shlobj.h>

#include "diag_logger.hpp" // DIAG_LOG / DIAG_F (shape-only diagnostics)

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "bcrypt.lib")

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cwchar>

namespace emebalachat {
namespace host_v2 {

namespace {

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ---- user-only security descriptor (§4.1 pattern; design §10 row 1) --------
struct SecurityDescriptorHolder {
    SecurityDescriptorHolder() = default;
    PSECURITY_DESCRIPTOR sd = nullptr;
    ~SecurityDescriptorHolder() { if (sd) ::LocalFree(sd); }
    SecurityDescriptorHolder(const SecurityDescriptorHolder&) = delete;
    SecurityDescriptorHolder& operator=(const SecurityDescriptorHolder&) = delete;
};

bool BuildUserOnlySd(SecurityDescriptorHolder& holder) {
    HANDLE hToken = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &hToken)) return false;
    DWORD len = 0;
    ::GetTokenInformation(hToken, TokenUser, nullptr, 0, &len);
    if (::GetLastError() != ERROR_INSUFFICIENT_BUFFER || len == 0) {
        ::CloseHandle(hToken);
        return false;
    }
    std::vector<BYTE> buf(len);
    const BOOL okInfo = ::GetTokenInformation(hToken, TokenUser, buf.data(), len, &len);
    ::CloseHandle(hToken);
    if (!okInfo) return false;
    const auto* user = reinterpret_cast<const TOKEN_USER*>(buf.data());
    LPWSTR sidStr = nullptr;
    if (!::ConvertSidToStringSidW(user->User.Sid, &sidStr) || !sidStr) return false;
    const std::wstring sddl = std::wstring(L"D:P(A;;GA;;;") + sidStr + L")";
    ::LocalFree(sidStr);
    return ::ConvertStringSecurityDescriptorToSecurityDescriptorW(
               sddl.c_str(), SDDL_REVISION_1, &holder.sd, nullptr) != FALSE;
}

// Boot-scoped token (§4.2 pattern): 128-bit random hex32, lives on the child
// command line only — NO token file, nothing to clean up (design §10 row 2).
bool GenerateTokenHex32(std::string& out) {
    BYTE bytes[16] = {};
    const NTSTATUS st = ::BCryptGenRandom(nullptr, bytes, sizeof(bytes),
                                          BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (st < 0) return false;
    static const char kHex[] = "0123456789abcdef";
    out.clear();
    out.reserve(32);
    for (BYTE b : bytes) {
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0x0F]);
    }
    return true;
}

// Private worker pipe name: \\.\pipe\emebala-engine-worker-<pid>-<rand>
// (design §1.2 transport row; unique per spawn attempt).
std::wstring MakeWorkerPipeName() {
    wchar_t buf[128];
    swprintf(buf, 128, L"\\\\.\\pipe\\emebala-engine-worker-%lu-%u",
             static_cast<unsigned long>(::GetCurrentProcessId()),
             static_cast<unsigned>(::GetTickCount()));
    return buf;
}

constexpr DWORD kPipeBufSize = (1u << 20) + 4;

// Plan-B (REQ-B004) B-T3, design §4.3 step 2: a model absent from the boot
// registry snapshot is charged this conservative default (Hy-MT2 class,
// 2048 MiB) so the gate always runs on a concrete number.
constexpr unsigned long long kDefaultTranslateModelVramBytes = 2048ull * 1024 * 1024;

// B-T3, technical gate A-4: the child-environment key the CPU leg forces.
constexpr wchar_t kMtGpuEnvName[] = L"EMEBALA_MT_GPU";

// Hard kill for a child that cannot be ordered down (handshake failure /
// orphan guard). Best effort: a race-lost kill still gets closed by the
// caller's CloseProcess (the reaper never waits on a zombie handle).
void TerminateProcessHandle(HANDLE process) {
    if (process) ::TerminateProcess(process, 1);
}

// ---- default Win32 seams ----------------------------------------------------
SpawnResult LaunchWorkerProcess(const SpawnRequest& req, void* /*user*/) {
    SpawnResult r;
    if (req.exe_path.empty() || req.pipe_name.empty() || req.token.size() != 32) {
        r.last_error = ERROR_INVALID_PARAMETER;
        return r;
    }
    // hex32 chars are ASCII: explicit per-char widen (no /W4 C4244 warning).
    std::wstring token_w;
    token_w.reserve(req.token.size());
    for (char c : req.token) token_w.push_back(static_cast<wchar_t>(c));
    // B3 fix (session 260928_0001): forward the family so the child derives a
    // PER-FAMILY single-instance mutex (wp::MutexNameForFamily) instead of the
    // legacy family-agnostic constant. The family charset is already
    // sanitizer-restricted to [A-Za-z0-9._-] (PoolFamilyForModel), so it
    // contains no spaces/quotes and appends safely unquoted, exactly like the
    // existing --pipe value. An EMPTY family (defensive; every real call site
    // populates it from w->family) is omitted — the worker then falls back to
    // the exact legacy constant (arg-absent backward compat).
    std::wstring cmd = L"\"" + req.exe_path + L"\" --pipe " + req.pipe_name +
                       L" --token " + token_w;
    if (!req.family.empty()) cmd += L" --mutex " + req.family;
    // P2-1 stabilization observability (temporary diagnostic): when THIS
    // host's own stderr is a real handle (a logging run started with
    // redirection), hand the child exactly that handle through
    // STARTF_USESTDHANDLES. bInheritHandles stays FALSE, so the child gets
    // ONLY the two standard handles named here (no other host handle leaks
    // in, pipe EOF semantics unchanged). Do NOT combine with
    // PROC_THREAD_ATTRIBUTE_HANDLE_LIST: std handles are implicitly
    // inherited with STARTF_USESTDHANDLES and listing them too fails the
    // create with ERROR_INVALID_PARAMETER (measured). In a normal GUI spawn
    // stderr is null/invalid and this is a no-op.
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE; // design §1.1 rule 1: hidden background child
    HANDLE stderr_handle = ::GetStdHandle(STD_ERROR_HANDLE);
    if (stderr_handle != nullptr && stderr_handle != INVALID_HANDLE_VALUE) {
        si.dwFlags |= STARTF_USESTDHANDLES;
        si.hStdError = stderr_handle;
        si.hStdOutput = stderr_handle;
    }
    PROCESS_INFORMATION pi = {};
    // bInheritHandles=TRUE is required for STARTF_USESTDHANDLES delivery;
    // it leaks nothing here because NO host handle is created inheritable
    // (every CreateEventW/CreateNamedPipeW/CreateFileW uses a non-inheritable
    // default or an explicit FALSE), so the child receives exactly the two
    // standard handles named above.
    // B-T3 (technical gate A-4): per-spawn environment. An empty
    // environment_block keeps the pre-B-T3 behavior (lpEnvironment=nullptr ->
    // inherit the parent env verbatim); a non-empty block is the built
    // parent-env copy + overrides (double-null-terminated Unicode), which
    // CreateProcessW forwards as lpEnvironment (the block itself is read-only
    // here and dies with this scope — the child received a COPY).
    // (const-cast: the block is read-only here; CreateProcessW only reads it.)
    const void* env = nullptr;
    if (!req.environment_block.empty()) env = req.environment_block.data();
    const BOOL ok = ::CreateProcessW(req.exe_path.c_str(), cmd.data(), nullptr, nullptr,
                                     TRUE, CREATE_NO_WINDOW,
                                     const_cast<LPVOID>(env), nullptr, &si, &pi);
    if (!ok) {
        r.last_error = ::GetLastError();
        return r;
    }
    ::CloseHandle(pi.hThread);
    // The process handle doubles as the done_event (signaled on child exit) —
    // the ReaperLoop's 250 ms WaitForSingleObject target (design §1.1 rule 3).
    r.ok = true;
    r.process = pi.hProcess;
    r.done_event = pi.hProcess;
    return r;
}

void CloseWorkerProcess(HANDLE process, void* /*user*/) {
    if (process) ::CloseHandle(process);
}

// One message write on the orchestrator side. Ok -> the frame is queued;
// Broken -> the pipe is dead (peer gone, immediate error); Stalled -> the
// peer stopped draining and the bounded wait timed out (the pending write is
// cancelled — message-mode atomicity discards the whole frame).
FrameWriteResult WritePipeFrame(HANDLE pipe, std::string_view json) {
    if (pipe == nullptr || pipe == INVALID_HANDLE_VALUE) return FrameWriteResult::Broken;
    std::string frame;
    frame.reserve(4 + json.size());
    const uint32_t len = static_cast<uint32_t>(json.size());
    for (unsigned i = 0; i < 4; ++i) {
        frame.push_back(static_cast<char>((len >> (8 * i)) & 0xFF));
    }
    frame.append(json);
    OVERLAPPED ol = {};
    ol.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ol.hEvent) return FrameWriteResult::Broken;
    DWORD written = 0;
    BOOL ok = ::WriteFile(pipe, frame.data(), static_cast<DWORD>(frame.size()), &written, &ol);
    if (!ok && ::GetLastError() == ERROR_IO_PENDING) {
        // Bounded: a wedged peer must not pin the caller (the reaper judges
        // the process separately).
        const DWORD w = ::WaitForSingleObject(ol.hEvent, 15000);
        if (w == WAIT_OBJECT_0) {
            ok = ::GetOverlappedResult(pipe, &ol, &written, FALSE);
        } else {
            // P2-1 stabilization observability: the peer stopped draining for
            // the full window (a busy/stuck worker backs the pipe up). Shape-
            // only: byte count only, never content.
            DIAG_F("ENGINEHOST/wmgr/017: frame write stalled 15s, cancelling (bytes=%zu)\n",
                   json.size());
            // Cancel window race (same class as overlapped_io_util.hpp's fix):
            // the write may COMPLETE between the wait timeout and CancelIoEx.
            // The cancel-approve result decides: a completed write DELIVERED
            // the frame — report Ok so the caller and the pipe agree.
            // Reporting Stalled for a delivered frame leaves a dangling armed
            // state downstream (live worker/031: a "failed" feed META that
            // actually armed the worker, then the next session_open hit the
            // arm -> "expected binary PCM, got JSON" -> exit 3).
            ::CancelIoEx(pipe, &ol);
            DWORD completed = 0;
            if (::GetOverlappedResult(pipe, &ol, &completed, TRUE)) {
                written = completed;
                ok = TRUE;
            } else {
                ::CloseHandle(ol.hEvent);
                return FrameWriteResult::Stalled;
            }
        }
    }
    ::CloseHandle(ol.hEvent);
    if (!ok || written != frame.size()) return FrameWriteResult::Broken;
    return FrameWriteResult::Ok;
}

// One message read with a deadline. Ok -> json; Timeout -> empty, alive;
// IoError -> peer gone/cap violation.
enum class PipeRead { Ok, Timeout, IoError };

PipeRead ReadPipeFrame(HANDLE pipe, std::string& json, DWORD timeout_ms) {
    if (pipe == nullptr || pipe == INVALID_HANDLE_VALUE) return PipeRead::IoError;
    static thread_local std::vector<char> buf;
    if (buf.size() < kPipeBufSize) buf.resize(kPipeBufSize);
    OVERLAPPED ol = {};
    ol.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ol.hEvent) return PipeRead::IoError;
    DWORD read = 0;
    BOOL ok = ::ReadFile(pipe, buf.data(), kPipeBufSize, &read, &ol);
    if (!ok && ::GetLastError() == ERROR_IO_PENDING) {
        const DWORD w = ::WaitForSingleObject(ol.hEvent, timeout_ms);
        if (w == WAIT_TIMEOUT) {
            // Cancel window race: the read may COMPLETE between the wait
            // timeout and CancelIoEx. Honor the completion — discarding a
            // completed frame LOSES it (consumed from the pipe), e.g. a
            // worker `closed` answer vanishing into a bogus Timeout.
            ::CancelIoEx(pipe, &ol);
            DWORD completed = 0;
            if (::GetOverlappedResult(pipe, &ol, &completed, TRUE) && completed > 0) {
                read = completed;
                ok = TRUE;
            } else {
                ::CloseHandle(ol.hEvent);
                return PipeRead::Timeout;
            }
        } else if (w != WAIT_OBJECT_0) {
            ::CancelIoEx(pipe, &ol);
            ::CloseHandle(ol.hEvent);
            return PipeRead::IoError;
        } else {
            ok = ::GetOverlappedResult(pipe, &ol, &read, TRUE);
        }
    }
    ::CloseHandle(ol.hEvent);
    if (!ok) return PipeRead::IoError; // ERROR_BROKEN_PIPE = worker closed
    if (read < enginehost::kFrameHeaderSize) return PipeRead::IoError;
    uint32_t len = 0;
    enginehost::FrameReadLengthPrefix(buf.data(), len);
    if (len > enginehost::kMaxFrameBytes ||
        static_cast<size_t>(len) + enginehost::kFrameHeaderSize != read) {
        return PipeRead::IoError;
    }
    json.assign(buf.data() + enginehost::kFrameHeaderSize, static_cast<size_t>(len));
    return PipeRead::Ok;
}

} // namespace

// B-T3 (technical gate A-4): build a double-null-terminated Unicode environment
// block = a copy of the PARENT environment (GetEnvironmentStringsW) with
// `assignments` applied/overwritten. An empty `assignments` yields an empty
// out_block — the caller then passes lpEnvironment=nullptr (inherit verbatim,
// the unchanged pre-B-T3 GPU-permitted path). False only when the parent block
// cannot be read; the caller treats that as "no block" (inherit), never a
// spawn failure. Pure + testable: no CreateProcess here. (Defined OUTSIDE the
// anonymous namespace: this is the exported host_v2 symbol the tests link.)
bool BuildTranslateSpawnEnvironment(
    const std::vector<std::pair<std::wstring, std::wstring>>& assignments,
    std::vector<wchar_t>& out_block) {
    out_block.clear();
    if (assignments.empty()) return true; // inherit path
    const wchar_t* parent = ::GetEnvironmentStringsW();
    if (!parent) return false;
    size_t parent_chars = 0;
    while (parent[parent_chars] != L'\0' ||
           parent[parent_chars + 1] != L'\0') {
        ++parent_chars;
    }
    parent_chars += 2; // include the double-null terminator

    // Split into name->value (the name ends at the first L'='); an entry with
    // no '=' is skipped (malformed parent entries are not forwarded).
    std::vector<std::pair<std::wstring, std::wstring>> entries;
    entries.reserve(parent_chars / 8 + assignments.size());
    size_t i = 0;
    while (i < parent_chars && parent[i] != L'\0') {
        size_t j = i;
        while (j < parent_chars && parent[j] != L'\0' && parent[j] != L'=') ++j;
        if (j < parent_chars && parent[j] == L'=' && j > i) {
            entries.emplace_back(std::wstring(parent + i, parent + j),
                                 std::wstring(parent + j + 1));
        }
        while (i < parent_chars && parent[i] != L'\0') ++i;
        ++i; // past the NUL
    }
    ::FreeEnvironmentStringsW(const_cast<wchar_t*>(parent));

    // Apply/overwriting the assignments (last write wins; a parent value under
    // an assigned name is replaced, so the child sees exactly one occurrence).
    for (const auto& a : assignments) {
        bool replaced = false;
        for (auto& e : entries) {
            if (e.first == a.first) { e.second = a.second; replaced = true; break; }
        }
        if (!replaced) entries.emplace_back(a.first, a.second);
    }

    size_t total = 1; // the final block terminator
    for (const auto& e : entries) {
        total += e.first.size() + 1 + e.second.size() + 1;
    }
    out_block.assign(total, L'\0');
    size_t pos = 0;
    for (const auto& e : entries) {
        std::copy(e.first.begin(), e.first.end(), out_block.begin() + pos);
        pos += e.first.size();
        out_block[pos++] = L'=';
        std::copy(e.second.begin(), e.second.end(), out_block.begin() + pos);
        pos += e.second.size();
        out_block[pos++] = L'\0';
    }
    out_block[pos] = L'\0'; // the second (block) terminator
    return true;
}

// ---- Impl -------------------------------------------------------------------
struct WorkerManager::Impl {
    // A-1 (technical gate, Plan-B): std::deque keeps element references/pointers
    // stable across push_back — a std::vector reallocates on growth and would
    // dangle every WorkerHandle* a dispatcher thread holds across a runtime
    // RegisterFamily (use-after-free under Plan-B on-demand registration).
    std::mutex mu;
    std::deque<WorkerHandle> workers;
    SpawnFn spawn_fn = nullptr;
    CloseProcessFn close_fn = nullptr;
    void* user = nullptr;

    // ---- per-model translate pool (Plan-B §3.2, REQ-B003) ------------------
    // translate_pool_mu_ serializes REGISTRATION and guards the map; relay
    // state is read under the per-family Busy lock (F-01 single-flight), NOT
    // under this mutex. NEVER hold translate_pool_mu_ across a spawn
    // (technical gate A-2: a first-spawn can run 30 s and must not convoy a
    // sibling dispatcher's NeedsRelay lookup).
    std::unordered_map<std::string, TranslatePoolEntry> translate_pool_;
    std::mutex translate_pool_mu_;
    // A-3: translate worker exe path recorded at boot (main()-scope local in
    // host_main.cpp) so TranslatePoolEnsure can reuse the SAME exe path for
    // runtime-registered families.
    std::wstring translate_exe_path_;

    // ---- B-T3 (REQ-B004 §4.3): reserved-bytes tracker + gate seams ----------
    // translate_pool_reserved_vram_bytes_ = sum of the RUNNING translate
    // families' model bytes (ASR NEVER counted, §4.5). Incremented when a
    // translate spawn reaches Ready (the entry's gate-charged bytes); the
    // reaper subtracts on exit. translate_pool_vram_mu_ guards the total.
    //   vram_resolver_/free_probe_ are the A-5 injections (unset resolver ->
    //   2048 MiB default; unset probe -> query_ok=false fail-closed).
    //   vram_override_value_ is the boot-read EMEBALA_MT_GPU (sentinel = unset).
    //   last_gate_ is the last DecideSpawnVram decision (unit pins; guarded by
    //   translate_pool_mu_ alongside the map).
    unsigned long long translate_pool_reserved_vram_bytes_ = 0;
    std::mutex translate_pool_vram_mu_;
    VramBytesResolver vram_resolver_;
    FreeVramProbe free_probe_;
    int vram_override_value_ = -1; // -1 = absent/auto
    WorkerManager::VramGateSnapshot last_gate_{};

    std::thread reaper_thread;
    std::atomic<bool> reaper_stop{false};
    HANDLE reaper_wake = nullptr; // auto-reset event: StopReaper beats the 250 ms poll

    Impl(SpawnFn s, CloseProcessFn c, void* u)
        : spawn_fn(s ? s : &LaunchWorkerProcess),
          close_fn(c ? c : &CloseWorkerProcess),
          user(u) {
        reaper_wake = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }
    ~Impl() {
        if (reaper_wake) ::CloseHandle(reaper_wake);
    }

    WorkerHandle* FindLocked(const std::wstring& family) {
        for (auto& w : workers) {
            if (w.family == family) return &w;
        }
        return nullptr;
    }

    // B-T3: the pool map is keyed by model_id while the WorkerHandle/reaper
    // know only the family, so a family is resolved back to its model_id
    // ("" is the only id whose family equals the bare prefix). Returns "" for
    // a non-translate family (the caller's pool lookup then misses, which is
    // exactly how ASR exits are excluded from the tracker, §4.5).
    std::string PoolModelIdForFamily(const std::wstring& family) {
        if (family == translate_pool::kWorkerFamilyTranslate) return std::string();
        const std::wstring prefix =
            std::wstring(translate_pool::kWorkerFamilyTranslate) + L"-";
        if (family.rfind(prefix, 0) == 0) {
            std::string out;
            out.reserve(family.size() - prefix.size());
            for (const wchar_t c : std::wstring(family.begin() + prefix.size(), family.end())) {
                out.push_back(c < 0x80 ? static_cast<char>(c) : '?');
            }
            return out;
        }
        return std::string();
    }

    // Tear a dead/broken pipe (process handle stays — the reaper still
    // watches it for the crash transition).
    void ClosePipeLocked(WorkerHandle& w) {
        if (w.pipe && w.pipe != INVALID_HANDLE_VALUE) {
            ::DisconnectNamedPipe(w.pipe);
            ::CloseHandle(w.pipe);
        }
        w.pipe = nullptr;
    }

    void CloseProcessLocked(WorkerHandle& w) {
        if (w.process) close_fn(w.process, user);
        w.process = nullptr;
    }
};

// ---- lifecycle --------------------------------------------------------------
WorkerManager::WorkerManager(SpawnFn spawn, CloseProcessFn close, void* user)
    : impl_(new Impl(spawn, close, user)) {}

WorkerManager::~WorkerManager() {
    StopReaper();
    ShutdownAll();
    delete impl_;
}

void WorkerManager::RegisterFamily(const std::wstring& family, const std::wstring& exe_path) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (impl_->FindLocked(family)) return; // idempotent
    WorkerHandle w;
    w.family = family;
    w.exe_path = exe_path;
    w.state = WorkerState::Stopped;
    impl_->workers.push_back(std::move(w));
    DIAG_LOG("ENGINEHOST", "wmgr/001: family registered (family=%ws exe_present=%d)",
             family.c_str(),
             (!exe_path.empty() &&
              ::GetFileAttributesW(exe_path.c_str()) != INVALID_FILE_ATTRIBUTES) ? 1 : 0);
}

WorkerState WorkerManager::State(const std::wstring& family) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    const WorkerHandle* w = impl_->FindLocked(family);
    return w ? w->state : WorkerState::Unavailable;
}

WorkerHandle* WorkerManager::Find(const std::wstring& family) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->FindLocked(family);
}

// ---- per-model translate pool (Plan-B §3, REQ-B003; B-T1 machinery) ---------
bool WorkerManager::SetTranslateExePath(const std::wstring& exe_path) {
    std::lock_guard<std::mutex> lk(impl_->translate_pool_mu_);
    impl_->translate_exe_path_ = exe_path;
    return true;
}

std::wstring WorkerManager::TranslateExePath() const {
    std::lock_guard<std::mutex> lk(impl_->translate_pool_mu_);
    return impl_->translate_exe_path_;
}

bool WorkerManager::RegisterTranslatePoolBootEntry() {
    std::lock_guard<std::mutex> lk(impl_->translate_pool_mu_);
    if (impl_->translate_pool_.count(std::string()) != 0) return false; // already
    TranslatePoolEntry e;
    e.model_id.clear();
    e.family = translate_pool::PoolFamilyForModel(e.model_id); // kWorkerFamilyTranslate
    e.registered = true; // RegisterFamily(kWorkerFamilyTranslate,...) ran at boot
    impl_->translate_pool_.emplace(std::string(), std::move(e));
    return true;
}

TranslatePoolEntry* WorkerManager::TranslatePoolEnsure(const std::string& model_id) {
    // Existing entry fast path — pool mu only guards the O(1) lookup (A-2).
    {
        std::lock_guard<std::mutex> lk(impl_->translate_pool_mu_);
        const auto it = impl_->translate_pool_.find(model_id);
        if (it != impl_->translate_pool_.end()) return &it->second;
    }
    // Absent: derive the family (fail-closed on invalid id chars).
    const std::wstring family = translate_pool::PoolFamilyForModel(model_id);
    if (family.empty()) return nullptr;
    std::wstring exe;
    {
        std::lock_guard<std::mutex> lk(impl_->translate_pool_mu_);
        exe = impl_->translate_exe_path_;
    }
    if (exe.empty()) return nullptr;

    // B-T3 VRAM gate (design §4.3) — runs ONLY for a NEW entry: the gate
    // decides AT SPAWN (§4.2), and an already-registered entry took the fast
    // path above with NO re-run. Reads are lock-free or brief (A-2); the gate
    // itself is pure (enginehost::vramgate::DecideSpawnVram). Every family
    // derived from PoolFamilyForModel carries the kWorkerFamilyTranslate
    // prefix, so the entry is inherently a translate family (§4.5 — ASR never
    // reaches here).
    namespace vramgate = emebalachat::enginehost::vramgate;
    vramgate::SpawnVramDecision decision;
    {
        unsigned long long free_bytes = 0;
        bool query_ok = false;
        if (impl_->free_probe_) query_ok = impl_->free_probe_(free_bytes);
        unsigned long long reserved = 0;
        {
            std::lock_guard<std::mutex> vram_lk(impl_->translate_pool_vram_mu_);
            reserved = impl_->translate_pool_reserved_vram_bytes_;
        }
        unsigned long long model_bytes = 0;
        if (impl_->vram_resolver_) model_bytes = impl_->vram_resolver_(model_id);
        const bool model_from_registry = model_bytes != 0ull;
        if (!model_from_registry) {
            model_bytes = kDefaultTranslateModelVramBytes; // §4.3 step 2
        }
        const int override_value = impl_->vram_override_value_;
        decision = vramgate::DecideSpawnVram(override_value, query_ok, free_bytes,
                                             reserved, model_bytes);
        {
            std::lock_guard<std::mutex> snap_lk(impl_->translate_pool_mu_);
            WorkerManager::VramGateSnapshot& g = impl_->last_gate_;
            g.override_value = override_value;
            g.query_ok = query_ok;
            g.free_bytes = free_bytes;
            g.reserved_bytes = reserved;
            g.model_vram_bytes = model_bytes;
            g.allow_gpu = decision.allow_gpu;
            g.allow_cpu = decision.allow_cpu;
            g.reason = vramgate::SpawnVramReasonString(decision.reason);
        }
        // Shape-only decision log (invariant #5): family / reason / numbers
        // only — never user text. model_from_registry=0 marks the conservative
        // default charge.
        DIAG_LOG("ENGINEHOST",
                 "wmgr/021: spawn vram gate (family=%ws reason=%s free=%llu reserved=%llu "
                 "model=%llu model_from_registry=%d override=%d allow_gpu=%d)",
                 family.c_str(), vramgate::SpawnVramReasonString(decision.reason),
                 decision.estimated_free_bytes, decision.reserved_bytes, model_bytes,
                 model_from_registry ? 1 : 0, override_value,
                 decision.allow_gpu ? 1 : 0);
    }
    // §4.4 / v2 §A2 branch table — the CPU leg is ALWAYS permitted, so this
    // never refuses here. !allow_gpu -> the spawn passes EMEBALA_MT_GPU=0 in
    // the child environment (technical gate A-4); allow_gpu -> inherit as-is.
    const bool cpu_leg = !decision.allow_gpu;
    std::vector<wchar_t> env_block;
    if (cpu_leg) {
        if (BuildTranslateSpawnEnvironment({{kMtGpuEnvName, L"0"}}, env_block)) {
            // Empty block (build fell back to inherit) -> the spawn inherits;
            // the override pin then reads the parent env, matching pre-B-T3.
        }
    }

    // Runtime family registration OUTSIDE pool mu (A-2): RegisterFamily takes
    // the manager mu; never hold translate_pool_mu_ across it.
    RegisterFamily(family, exe);
    TranslatePoolEntry e;
    e.model_id = model_id;
    e.family = family;
    e.registered = true;
    e.spawn_gpu_env_override = cpu_leg;
    std::lock_guard<std::mutex> lk(impl_->translate_pool_mu_);
    // Double-check under the lock: a sibling may have inserted while we were
    // registering. RegisterFamily is idempotent, so reusing the winner's entry
    // is correct.
    const auto result = impl_->translate_pool_.emplace(model_id, std::move(e));
    return &result.first->second;
}

// ---- B-T3 (REQ-B004 §4.3): reserved-bytes tracker + gate seams --------------
unsigned long long WorkerManager::TranslatePoolReservedVramBytes() const {
    std::lock_guard<std::mutex> lk(impl_->translate_pool_vram_mu_);
    return impl_->translate_pool_reserved_vram_bytes_;
}

void WorkerManager::SetVramBytesResolver(VramBytesResolver resolver) {
    impl_->vram_resolver_ = std::move(resolver);
}

void WorkerManager::SetFreeVramProbe(FreeVramProbe probe) {
    impl_->free_probe_ = std::move(probe);
}

void WorkerManager::SetVramOverrideValue(int override_value) {
    impl_->vram_override_value_ = override_value;
}

WorkerManager::VramGateSnapshot WorkerManager::TranslatePoolLastGateDecision() const {
    std::lock_guard<std::mutex> lk(impl_->translate_pool_mu_);
    return impl_->last_gate_;
}

bool WorkerManager::TranslatePoolNeedsRelay(const std::string& model_id,
                                            HANDLE current_process) {
    std::lock_guard<std::mutex> lk(impl_->translate_pool_mu_);
    const auto it = impl_->translate_pool_.find(model_id);
    if (it == impl_->translate_pool_.end()) return true; // no entry: relay required
    const TranslatePoolEntry& e = it->second;
    return e.relayed_model_id != model_id || e.relayed_worker_process != current_process;
}

void WorkerManager::TranslatePoolMarkRelayed(const std::string& model_id, HANDLE process) {
    std::lock_guard<std::mutex> lk(impl_->translate_pool_mu_);
    const auto it = impl_->translate_pool_.find(model_id);
    if (it == impl_->translate_pool_.end()) return;
    it->second.relayed_model_id = model_id;
    it->second.relayed_worker_process = process;
}

size_t WorkerManager::TranslatePoolSize() const {
    std::lock_guard<std::mutex> lk(impl_->translate_pool_mu_);
    return impl_->translate_pool_.size();
}

void WorkerManager::SetBusy(const std::wstring& family, bool busy) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    WorkerHandle* w = impl_->FindLocked(family);
    if (!w) return;
    if (busy && w->state == WorkerState::Ready) w->state = WorkerState::Busy;
    else if (!busy && w->state == WorkerState::Busy) w->state = WorkerState::Ready;
}

// 260927_0003 F-01 (review round 2): single-flight acquire — see the header
// contract (atomic Ready->Busy under one mu acquisition; deadlock-free: mu is
// never held across a wait, the loser never blocks on the owner).
bool WorkerManager::TrySetBusy(const std::wstring& family) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    WorkerHandle* w = impl_->FindLocked(family);
    if (!w || w->state != WorkerState::Ready) return false;
    w->state = WorkerState::Busy;
    return true;
}

bool WorkerManager::SendToWorker(const std::wstring& family, std::string_view json) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    WorkerHandle* w = impl_->FindLocked(family);
    if (!w || (w->state != WorkerState::Ready && w->state != WorkerState::Busy)) return false;
    const FrameWriteResult rc = WritePipeFrame(w->pipe, json);
    if (rc == FrameWriteResult::Ok) return true;
    if (WorkerWriteFailureNeedsKill(rc)) {
        // P2-1 stabilization (live wedge): the worker is alive but not
        // draining. Kill it NOW — otherwise its per-family single-instance
        // mutex rejects every duplicate spawn while the stuck process holds
        // it, and the family wedges in backoff-respawn churn (persistent
        // `unavailable`). We finalize the crash right here (the reaper only
        // watches Ready/Busy/Spawning, and state is Crashed from here on);
        // EnsureSpawned's backoff gate paces the respawn.
        DIAG_F("ENGINEHOST/wmgr/019: stalled worker terminated (family=%ws)\n",
               family.c_str());
        TerminateProcessHandle(w->process);
        impl_->ClosePipeLocked(*w);
        impl_->CloseProcessLocked(*w);
        w->state = WorkerState::Crashed;
        w->spawn_failures++;
        w->next_spawn_allowed_ms = NowMs() + BackoffDelayMs(w->spawn_failures);
        return false;
    }
    // A dead pipe is a crashed worker in waiting: mark it so the reaper's
    // next pass finalizes the transition (no respawn from this thread).
    w->state = WorkerState::Crashed;
    DIAG_F("ENGINEHOST/wmgr/002: frame write failed; family marked crashed (family=%ws)\n",
           family.c_str());
    return false;
}

// M6 T4 (orchestrator dispatch): bounded single-frame read on the family
// pipe. Ok -> json; Timeout -> alive, nothing yet; IoError -> the pipe is
// dead (caller answers engine_failed; the reaper judges the respawn).
//
// P2-1 stabilization R4 (session 260925_0001-GPU-R4, live wedge): SPLIT
// PHASE — the bounded kernel wait runs WITHOUT the manager mutex. The relay
// polls at kAsrRelayPollMs while holding nothing, so the feed path
// (SendToWorker) and the translate dispatch never serialize behind the poll
// (the ":369 convoy" the original comment deferred; measured relay ceiling
// ~4.3 chunks/s vs the 6.25 chunks/s feed). Phase 1 validates and captures
// the pipe under the lock; phase 3 re-validates the SAME pipe + serving
// state under the lock — a teardown/replace/stall-kill that ran mid-read
// returns IoError (the frame is discarded; the reader's cleanup owns the
// outcome). Single-reader-per-family by design (the asr claim / one
// dispatcher owns each pipe), unchanged. Closing the handle with a pending
// overlapped read completes that read with an error, so the GracefulStop /
// reaper close paths stay race-free.
WorkerManager::WorkerRead WorkerManager::ReadFromWorker(const std::wstring& family,
                                                         std::string& json,
                                                         int timeout_ms) {
    HANDLE pipe = nullptr;
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        WorkerHandle* w = impl_->FindLocked(family);
        if (!w || (w->state != WorkerState::Ready && w->state != WorkerState::Busy)) {
            return WorkerRead::IoError;
        }
        pipe = w->pipe;
    }
    const PipeRead rc = ReadPipeFrame(pipe, json, static_cast<DWORD>(timeout_ms));
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        WorkerHandle* w = impl_->FindLocked(family);
        if (!w || w->pipe != pipe ||
            (w->state != WorkerState::Ready && w->state != WorkerState::Busy)) {
            json.clear();
            return WorkerRead::IoError;
        }
    }
    switch (rc) {
        case PipeRead::Ok:      return WorkerRead::Ok;
        case PipeRead::Timeout: return WorkerRead::Timeout;
        case PipeRead::IoError: return WorkerRead::IoError;
    }
    return WorkerRead::IoError;
}

// REQ-049: non-blocking drain of frames the worker queued on the family pipe
// while the dispatcher was idle (heartbeat cadence, or a stray final left by a
// timed-out previous job). The manager mutex keeps the handle lifecycle
// serialized exactly like ReadFromWorker; timeout 0 makes each ReadPipeFrame a
// pure poll — PipeRead::Timeout means the pipe is empty and the drain is done.
// No per-frame log: heartbeats are routine traffic.
int WorkerManager::DrainWorkerPipe(const std::wstring& family, int max_frames) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    WorkerHandle* w = impl_->FindLocked(family);
    if (!w || (w->state != WorkerState::Ready && w->state != WorkerState::Busy) ||
        !w->pipe || w->pipe == INVALID_HANDLE_VALUE) {
        return 0;
    }
    int drained = 0;
    std::string json; // discarded: only the count matters to the caller
    while (drained < max_frames && ReadPipeFrame(w->pipe, json, 0) == PipeRead::Ok) {
        ++drained;
    }
    return drained;
}

// 260927_0003 (MT audit Q1 fix A): bounded settle — see the header contract.
WorkerManager::SettleOutcome WorkerManager::SettleWorkerPipe(
    const std::wstring& family, int timeout_ms, const std::atomic<bool>* cancel,
    int max_frames) {
    const int64_t started_ms = NowMs();
    HANDLE pipe = nullptr;
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        WorkerHandle* w = impl_->FindLocked(family);
        if (!w || (w->state != WorkerState::Ready && w->state != WorkerState::Busy) ||
            !w->pipe || w->pipe == INVALID_HANDLE_VALUE) {
            DIAG_LOG("ENGINEHOST",
                     "wmgr/020: settle end (family=%ws outcome=pipe_io_error drained=0 "
                     "elapsed_ms=%lld)",
                     family.c_str(), static_cast<long long>(NowMs() - started_ms));
            return SettleOutcome::PipeIoError;
        }
        pipe = w->pipe; // snapshot: read WITHOUT the mutex (ReadFromWorker pattern)
    }
    const int64_t deadline = NowMs() + timeout_ms;
    int drained = 0;
    SettleOutcome outcome = SettleOutcome::DeadlineExpired;
    for (;;) {
        if (cancel && cancel->load(std::memory_order_acquire)) {
            outcome = SettleOutcome::Cancelled;
            break;
        }
        const int64_t now = NowMs();
        if (now >= deadline || drained >= max_frames) {
            outcome = SettleOutcome::DeadlineExpired;
            break;
        }
        // Bounded read: never past the remaining budget, never past one idle
        // tick, so cancel/deadline re-checks stay responsive.
        const DWORD wait_ms = static_cast<DWORD>(
            std::min<int64_t>(int64_t{250}, deadline - now));
        std::string json;
        const PipeRead rc = ReadPipeFrame(pipe, json, wait_ms);
        if (rc == PipeRead::IoError) {
            outcome = SettleOutcome::PipeIoError;
            break;
        }
        if (rc == PipeRead::Timeout) continue;
        ++drained;
        wp::EventMsg ev;
        const JobWaitFrame frame = ClassifyJobWaitFrame(json, ev);
        if (frame == JobWaitFrame::Final || frame == JobWaitFrame::Error) {
            // The given-up job's terminal event: discarded here, so it can
            // NEVER be re-stamped into the next job. Exactly one terminal
            // event exists per job (worker T3 contract), so this frame is the
            // given-up job's by construction (family-wide single-flight,
            // F-01: TrySetBusy gates the sibling dispatcher) — no new job is
            // in flight while this settle runs.
            outcome = SettleOutcome::TerminalDiscarded;
            break;
        }
        // Heartbeat / progress / malformed: consumed, keep settling.
    }
    // 260927_0003 F-04 (review round 2): one shape-only line per settle —
    // kinds/counts/timing only, never content (Chat privacy rules): the
    // outcome + drained + elapsed distinguish "stray discarded as designed"
    // from "hung worker burned the budget" (drained ~0, elapsed ~budget)
    // from "flooding worker" (drained ~max_frames, elapsed << budget) in a
    // future incident.
    const char* outcome_name = "?";
    switch (outcome) {
        case SettleOutcome::TerminalDiscarded: outcome_name = "terminal_discarded"; break;
        case SettleOutcome::DeadlineExpired:   outcome_name = "deadline_expired";   break;
        case SettleOutcome::PipeIoError:       outcome_name = "pipe_io_error";      break;
        case SettleOutcome::Cancelled:         outcome_name = "cancelled";          break;
    }
    DIAG_LOG("ENGINEHOST",
             "wmgr/020: settle end (family=%ws outcome=%s drained=%d elapsed_ms=%lld)",
             family.c_str(), outcome_name, drained,
             static_cast<long long>(NowMs() - started_ms));
    return outcome;
}

// ---- spawn + handshake (design §1.1 rules 1-2 / §3.4) -----------------------
// REQ-044 (P4-3, item 5): the blocking ConnectNamedPipe wait + announce
// ReadPipeFrame below run while impl_->mu is held. This is a deliberate
// lock convoy, not a self-deadlock (std::mutex is non-recursive and no
// path from here re-enters mu: FindLocked / ClosePipeLocked /
// CloseProcessLocked are Impl:: "Locked"-suffix helpers that assume the
// caller already holds mu, and none of them take a lock_guard). It is
// safe because handles in Spawning state are not reachable concurrently:
// SendToWorker / ReadFromWorker early-return on
// state != Ready/Busy, and ReaperPass skips states other
// than Ready/Busy/Spawning, so w->pipe is only ever touched by
// this thread while the lock is held. The convoy only matters if M7 adds
// parallel multi-family worker spawns - revisit then with an explicit
// state-machine design before narrowing the lock scope.
WorkerHandle* WorkerManager::EnsureSpawned(const std::wstring& family) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    WorkerHandle* w = impl_->FindLocked(family);
    if (!w) return nullptr;

    if (w->state == WorkerState::Ready || w->state == WorkerState::Busy) return w;
    if (w->state == WorkerState::Unavailable) return nullptr; // wrong deployment: NO respawn

    // Backoff gate (R-4): a recent crash defers the respawn.
    const int64_t now = NowMs();
    if (w->state == WorkerState::Crashed && now < w->next_spawn_allowed_ms) {
        return nullptr;
    }

    if (w->exe_path.empty() ||
        ::GetFileAttributesW(w->exe_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        // Not deployed yet: unavailable WITHOUT counting a spawn failure
        // (the bootstrapper/installer owns deployment; design §V2-8).
        w->state = WorkerState::Unavailable;
        DIAG_F("ENGINEHOST/wmgr/003: worker exe missing; family unavailable (family=%ws)\n",
               family.c_str());
        return nullptr;
    }

    // Fresh per-spawn pipe + token (boot-scoped; nothing persisted).
    impl_->ClosePipeLocked(*w);
    w->pipe_name = MakeWorkerPipeName();
    if (!GenerateTokenHex32(w->token)) {
        DIAG_F("ENGINEHOST/wmgr/004: token generation failed (family=%ws)\n", family.c_str());
        w->state = WorkerState::Crashed;
        w->spawn_failures++;
        w->next_spawn_allowed_ms = NowMs() + BackoffDelayMs(w->spawn_failures);
        return nullptr;
    }

    SecurityDescriptorHolder sd;
    if (!BuildUserOnlySd(sd)) {
        DIAG_F("ENGINEHOST/wmgr/005: user-only SD failed (err=%lu)\n", ::GetLastError());
        w->state = WorkerState::Crashed;
        w->spawn_failures++;
        w->next_spawn_allowed_ms = NowMs() + BackoffDelayMs(w->spawn_failures);
        return nullptr;
    }
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = sd.sd;
    sa.bInheritHandle = FALSE;
    w->pipe = ::CreateNamedPipeW(
        w->pipe_name.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
        1,                      // exactly one worker client per pipe (§1.2)
        kPipeBufSize, kPipeBufSize, 0, &sa);
    if (w->pipe == INVALID_HANDLE_VALUE) {
        w->pipe = nullptr;
        DIAG_F("ENGINEHOST/wmgr/006: CreateNamedPipe failed (err=%lu)\n", ::GetLastError());
        w->state = WorkerState::Crashed;
        w->spawn_failures++;
        w->next_spawn_allowed_ms = NowMs() + BackoffDelayMs(w->spawn_failures);
        return nullptr;
    }

    // Spawn (hidden, same user, token on the command line — no file).
    impl_->CloseProcessLocked(*w);
    SpawnRequest req{w->exe_path, w->pipe_name, w->token};
    // B3 fix (session 260928_0001): stamp the family so LaunchWorkerProcess
    // forwards `--mutex <family>` and the worker derives its per-family
    // single-instance mutex (wp::MutexNameForFamily). Every family registered
    // via RegisterFamily/TranslatePoolEnsure carries a non-empty w->family.
    req.family = w->family;
    // B-T3 (A-4) + P5a.5 C2 (195700_code-reviewer-planb-p5a5.md F-2): a CPU-leg
    // translate-family spawn (the gate set spawn_gpu_env_override at
    // registration) carries EMEBALA_MT_GPU=0 so the worker-side RT-C gate reads
    // a forced CPU leg. A GPU-permitted spawn leaves the block empty ->
    // lpEnvironment=nullptr (inherit, unchanged). The override is read from the
    // POOL ENTRY (persists across a crash) on EVERY launch of the family —
    // initial spawn, backoff respawn, graceful-restart respawn alike. The old
    // code gated the block on `state == Stopped`, which silently skipped the
    // Crashed->backoff->respawn path (REQ-B004 honesty break: a respawned
    // CPU-leg worker could take the GPU leg). The entry flag is the single
    // source of truth for the family's leg; the handle state is irrelevant.
    // C1/F-3: pool mu guards the map lookup AND the F-3 snapshot reset. The
    // scope is brief (no spawn under it — technical gate A-2) and reads the
    // entry flag inline (`spawn_gpu_env_override`; the Ready-charge / reaper
    // sites update `reserved_vram_bytes` / `relayed_*` under their own pool-mu
    // scopes). Nesting/order (P5a.5 re-review NEW-3/NEW-4 wording fix): this
    // scope nests inside EnsureSpawned's function-scope impl_->mu — the SAFE
    // direction of the single global order impl_->mu -> translate_pool_mu_ ->
    // translate_pool_vram_mu_; pool mu is NOT leaf-level in general. What this
    // scope must never do is invert the order (no spawn / RegisterFamily under
    // pool mu): the GetEnvironmentStringsW env-block snapshot inside
    // BuildTranslateSpawnEnvironment is a brief OS read accepted here as a
    // fast-path-map-consistency choice (it takes no lock; pre-fix it ran under
    // impl_->mu alone).
    if (w->family.rfind(translate_pool::kWorkerFamilyTranslate, 0) == 0) {
        std::lock_guard<std::mutex> pool_lk(impl_->translate_pool_mu_);
        const auto it = impl_->translate_pool_.find(impl_->PoolModelIdForFamily(w->family));
        if (it != impl_->translate_pool_.end() && it->second.spawn_gpu_env_override) {
            if (BuildTranslateSpawnEnvironment({{kMtGpuEnvName, L"0"}}, req.environment_block)) {
                // Empty block (build fell back to inherit) -> the spawn
                // inherits; the override pin then reads the parent env, the
                // same honest-CPU behavior as pre-B-T3.
            }
        }
        // P5a.5 F-3 (relay-snapshot HANDLE-reuse defeat): launching a NEW process
        // for an existing entry invalidates the relay snapshot up front.
        // Windows recycles a just-freed handle value for the new child with
        // high probability (close->CreateProcess adjacency reuses the lowest
        // free slot), so the old `relayed_worker_process != current_process`
        // comparison can compare a numerically EQUAL handle and wrongly answer
        // "no re-relay" — the fresh worker (active_model_id == "") would then
        // serve user-model jobs on the pinned default (silent wrong-model).
        // Forcing the snapshot to the initial sentinel here makes
        // TranslatePoolNeedsRelay unconditionally true on the first job after
        // ANY (re)launch — same observable semantics as the pre-existing
        // process-CHANGE re-relay, so the REQ-058/B010 "restart epoch re-relays
        // exactly once" pins hold unchanged.
        if (it != impl_->translate_pool_.end()) {
            it->second.relayed_worker_process = nullptr;
            it->second.relayed_model_id.clear();
        }
    }
    SpawnResult sr = impl_->spawn_fn(req, impl_->user);
    if (!sr.ok) {
        DIAG_F("ENGINEHOST/wmgr/007: spawn failed (family=%ws err=%lu)\n",
               family.c_str(), sr.last_error);
        impl_->ClosePipeLocked(*w);
        w->state = WorkerState::Crashed;
        w->spawn_failures++;
        w->next_spawn_allowed_ms = NowMs() + BackoffDelayMs(w->spawn_failures);
        return nullptr;
    }
    w->process = sr.process;
    w->state = WorkerState::Spawning;
    w->last_heartbeat_ms = NowMs();

    // ---- handshake phase 1: the worker connects to our pipe instance ----
    {
        OVERLAPPED ol = {};
        ol.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        bool connected = false;
        if (ol.hEvent) {
            const BOOL ok = ::ConnectNamedPipe(w->pipe, &ol);
            const DWORD err = ::GetLastError();
            if (!ok && err == ERROR_IO_PENDING) {
                DWORD dummy = 0;
                const DWORD wr = ::WaitForSingleObject(ol.hEvent,
                                                       wp::kWorkerSpawnConnectTimeoutMs);
                if (wr == WAIT_OBJECT_0) {
                    connected = ::GetOverlappedResult(w->pipe, &ol, &dummy, FALSE) != FALSE;
                } else {
                    ::CancelIoEx(w->pipe, &ol);
                }
            } else if (ok || err == ERROR_PIPE_CONNECTED) {
                connected = true; // the client already connected (spawn race)
            }
            ::CloseHandle(ol.hEvent);
        }
        if (!connected) {
            DIAG_F("ENGINEHOST/wmgr/008: worker never connected (family=%ws)\n", family.c_str());
            impl_->ClosePipeLocked(*w);
            TerminateProcessHandle(w->process);
            impl_->CloseProcessLocked(*w);
            w->state = WorkerState::Crashed;
            w->spawn_failures++;
            w->next_spawn_allowed_ms = NowMs() + BackoffDelayMs(w->spawn_failures);
            return nullptr;
        }
    }

    // ---- handshake phase 2: announce + token re-validation + family/abi/
    // protocol validation. Token re-validation (security audit finding 2,
    // M6 T6): the announce frame must echo the exact boot-scoped token we
    // issued on the command line, compared in constant time. A missing or
    // mismatched token means the connected party is NOT the worker we spawned
    // (same-user pipe spoofing) — tear the session; the family goes
    // Unavailable (a spoofed peer is a wrong deployment; no respawn).
    bool announced_ok = false;
    wp::WorkerManifest announced;
    {
        std::string json;
        const PipeRead rc = ReadPipeFrame(w->pipe, json, wp::kWorkerSpawnConnectTimeoutMs);
        wp::AnnounceMsg am;
        if (rc == PipeRead::Ok && wp::ParseAnnounce(json, am)) {
            if (!wp::AnnounceTokenMatches(am.token, w->token)) {
                DIAG_F("ENGINEHOST/wmgr/013: announce token mismatch (family=%ws); tearing the session (audit finding 2)\n",
                       family.c_str());
                w->state = WorkerState::Unavailable;
                impl_->ClosePipeLocked(*w);
                TerminateProcessHandle(w->process);
                impl_->CloseProcessLocked(*w);
                return nullptr;
            }
            announced = am.manifest;
            const wp::AnnounceStatus st = ValidateHandshake(announced);
            if (st == wp::AnnounceStatus::Ok) {
                announced_ok = true;
            } else {
                // §3.4: a mismatch is a WRONG DEPLOYMENT — unavailable + log,
                // NO respawn (respawning cannot fix a bad binary).
                DIAG_F("ENGINEHOST/wmgr/009: announce rejected (%s announced_family=%s)\n",
                       wp::AnnounceStatusToString(st).data(), announced.family.c_str());
                w->state = WorkerState::Unavailable;
                impl_->ClosePipeLocked(*w);
                TerminateProcessHandle(w->process);
                impl_->CloseProcessLocked(*w);
                return nullptr;
            }
        } else {
            DIAG_F("ENGINEHOST/wmgr/010: announce unreadable (rc=%d family=%ws)\n",
                   static_cast<int>(rc), family.c_str());
        }
    }
    if (!announced_ok) {
        impl_->ClosePipeLocked(*w);
        impl_->CloseProcessLocked(*w);
        w->state = WorkerState::Crashed;
        w->spawn_failures++;
        w->next_spawn_allowed_ms = NowMs() + BackoffDelayMs(w->spawn_failures);
        return nullptr;
    }

    w->manifest = announced;
    w->spawn_failures = 0; // a clean handshake resets the crash streak
    w->state = WorkerState::Ready;
    // B-T3 (REQ-B004 §4.3): a translate-family worker reached Ready -> count
    // its model bytes in the reserved tracker. The gate charged the model at
    // registration (entry.reserved_vram_bytes == 0 until now, so a respawn
    // never double-counts; a fresh registration on a running family is a
    // no-op). ONLY translate families are counted (§4.5): a non-translate
    // family (ASR) is excluded up front — PoolModelIdForFamily would otherwise
    // map the bare-prefix ASR family to the boot "" id and wrongly charge it.
    if (w->family.rfind(translate_pool::kWorkerFamilyTranslate, 0) == 0) {
        const std::string model_id = impl_->PoolModelIdForFamily(w->family);
        // P5a.5 C1 (195700 F-1): the entry-field read AND the conditional write
        // below run under translate_pool_mu_ (the map's own mutex). Before this,
        // the find + reserved_vram_bytes access held only impl_->mu — no
        // happens-before edge to a concurrent TranslatePoolEnsure emplace ->
        // data race (UB) on the bucket chain. Lock shape (P5a.5 re-review
        // NEW-3/NEW-4 wording fix): this scope nests translate_pool_vram_mu_
        // INSIDE pool mu (impl_->mu -> pool mu -> vram mu, consistent global
        // order, no reverse path exists), so pool mu is NOT leaf-level here.
        // The one registry model-bytes resolve below (vram_resolver_) is a
        // brief OS read accepted under pool mu as a fast-path-map-consistency
        // choice; no spawn runs inside the pool-mu scope (technical gate A-2
        // holds).
        {
            std::lock_guard<std::mutex> pool_lk(impl_->translate_pool_mu_);
            const auto it = impl_->translate_pool_.find(model_id);
            if (it != impl_->translate_pool_.end()) {
                const unsigned long long charged = it->second.reserved_vram_bytes;
                if (charged == 0ull) {
                    // Charge the model bytes (§4.3 step 1: the registry lookup). The
                    // gate already resolved this for a runtime-registered entry; the
                    // boot "" entry (never gated) resolves here. Absent -> the §4.3
                    // step 2 conservative default.
                    unsigned long long model_bytes =
                        impl_->vram_resolver_ ? impl_->vram_resolver_(model_id) : 0ull;
                    if (model_bytes == 0ull) model_bytes = kDefaultTranslateModelVramBytes;
                    it->second.reserved_vram_bytes = model_bytes;
                    std::lock_guard<std::mutex> vram_lk(impl_->translate_pool_vram_mu_);
                    impl_->translate_pool_reserved_vram_bytes_ += model_bytes;
                }
            }
        }
    }
    DIAG_LOG("ENGINEHOST", "wmgr/011: worker ready (family=%ws engine=%s/%s abi=%d proto=%d-%d)",
             family.c_str(), w->manifest.engine.c_str(), w->manifest.engine_version.c_str(),
             w->manifest.abi_version, w->manifest.protocol_min, w->manifest.protocol_max);
    return w;
}

// ---- M-2 graceful stop (tech gate order) ------------------------------------
bool WorkerManager::GracefulStop(WorkerHandle& w, int timeout_ms) {
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        if (!w.pipe) {
            // Nothing to order down: just finalize the process side.
            impl_->CloseProcessLocked(w);
            w.state = WorkerState::Stopped;
            return true;
        }
        w.state = WorkerState::Stopped; // refuse new dispatch immediately
    }
    // M-2 step 1: send shutdown (one synchronous frame write).
    const bool sent = WritePipeFrame(w.pipe, wp::BuildShutdown()) == FrameWriteResult::Ok;
    if (!sent) {
        // Pipe already dead: the worker cannot ack; fall through to the wait
        // (the done_event fires on the process exit we are waiting for).
        DIAG_F("ENGINEHOST/wmgr/012: shutdown write failed; waiting for exit (family=%ws)\n",
               w.family.c_str());
    }
    // M-2 step 2: wait for shutdown_ack OR pipe EOF (worker's last frame) OR
    // the process exiting — ALL prove no frame is in flight anymore.
    const int64_t deadline = NowMs() + timeout_ms;
    bool acked = false;
    while (NowMs() < deadline) {
        std::string json;
        const PipeRead rc = ReadPipeFrame(w.pipe, json, 250);
        if (rc == PipeRead::Ok) {
            if (wp::ParseShutdownAck(json)) { acked = true; break; }
            continue; // an in-flight event frame (e.g. a final) — consume it
        }
        if (rc == PipeRead::IoError) break;      // EOF: worker is done/exiting
        if (!sent) break; // the pipe was already dead when we started
        // Timeout: keep polling until the deadline (a slow Unload is legal).
    }
    // M-2 step 3: ONLY NOW close/disconnect the pipe (nothing in flight).
    // If the process still lives after the window, escalate to kill so an
    // unresponsive worker cannot outlive the orchestrator.
    bool killed = false;
    if (w.process) {
        const DWORD wr = ::WaitForSingleObject(w.process, 2000);
        if (wr != WAIT_OBJECT_0) {
            DIAG_F("ENGINEHOST/wmgr/013: worker did not exit after shutdown; terminating (family=%ws acked=%d)\n",
                   w.family.c_str(), acked ? 1 : 0);
            TerminateProcessHandle(w.process);
            killed = true;
        }
        impl_->close_fn(w.process, impl_->user);
        w.process = nullptr;
    }
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        impl_->ClosePipeLocked(w);
    }
    DIAG_LOG("ENGINEHOST", "wmgr/014: family stopped (family=%ws acked=%d killed=%d)",
             w.family.c_str(), acked ? 1 : 0, killed ? 1 : 0);
    return acked || killed;
}

// ---- reaper (design §1.1 rule 3) --------------------------------------------
int WorkerManager::ReaperPass(int64_t now_ms) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    int crashes = 0;
    for (auto& w : impl_->workers) {
        if (w.state != WorkerState::Ready && w.state != WorkerState::Busy &&
            w.state != WorkerState::Spawning) {
            continue; // Stopped/Crashed/Unavailable have nothing to watch
        }
        if (!w.process) continue; // seam-injected state without a handle

        const DWORD wr = ::WaitForSingleObject(w.process, 0);
        if (wr != WAIT_OBJECT_0) {
            // Heartbeat staleness judge (§1.2: 15 s timeout) — only when the
            // pipe is ALREADY gone (a silent-but-piped worker is handled by
            // the orchestrator's ordered GracefulStop, not by the reaper).
            if (!w.pipe &&
                now_ms - w.last_heartbeat_ms > wp::kWorkerHeartbeatTimeoutMs) {
                w.state = WorkerState::Crashed;
                w.spawn_failures++;
                w.next_spawn_allowed_ms = now_ms + BackoffDelayMs(w.spawn_failures);
                ++crashes;
                DIAG_F("ENGINEHOST/wmgr/015: heartbeat timeout (family=%ws)\n",
                       w.family.c_str());
            }
            continue;
        }
        // ---- crash finalization ----
        DWORD exit_code = 0;
        ::GetExitCodeProcess(w.process, &exit_code);
        impl_->ClosePipeLocked(w);
        impl_->CloseProcessLocked(w);
        w.state = WorkerState::Crashed;
        w.spawn_failures++;
        w.next_spawn_allowed_ms = now_ms + BackoffDelayMs(w.spawn_failures);
        // B-T3 (REQ-B004 §4.3/§12.4): a translate-family worker exited ->
        // subtract EXACTLY its entry's recorded contribution (never below 0),
        // so the tracker cannot drift from the pool's own accounting. ONLY
        // translate families are counted (§4.5): a non-translate family (ASR)
        // is excluded up front — PoolModelIdForFamily would otherwise map the
        // bare-prefix ASR family to the boot "" id and wrongly subtract it.
        if (w.family.rfind(translate_pool::kWorkerFamilyTranslate, 0) == 0) {
            // P5a.5 C1: the reaper's find + entry-field read/write run under
            // translate_pool_mu_ (same data-race class as the Ready-charge
            // site). The tracker subtract bumps translate_pool_vram_mu_ INSIDE
            // the pool-mu scope, i.e. the vram lock is NESTED, not taken after
            // release (P5a.5 re-review NEW-3 wording fix: the old wording said
            // "copy out, release, then bump", which does not match this code —
            // safe because the global order impl_->mu -> pool mu -> vram mu is
            // consistent and no vram->pool path exists; pool mu is not
            // leaf-level). No spawn under pool mu (A-2).
            // P5a.5 F-3: a reaper-detected exit ALSO clears the entry's relay
            // snapshot (relayed_worker_process -> nullptr, relayed_model_id ->
            // ""). Windows recycles a closed handle value for a NEW process, so
            // keeping the dead worker's handle in the snapshot could make a
            // later NeedsRelay compare numerically-equal handles and wrongly
            // answer "no re-relay" after the respawn (silent wrong-model). The
            // launch-side reset (EnsureSpawned) is the primary guard; this
            // exit-side reset backstops any other path that finalizes an exit.
            std::lock_guard<std::mutex> pool_lk(impl_->translate_pool_mu_);
            const auto it = impl_->translate_pool_.find(impl_->PoolModelIdForFamily(w.family));
            if (it != impl_->translate_pool_.end()) {
                if (it->second.reserved_vram_bytes != 0ull) {
                    const unsigned long long contribution = it->second.reserved_vram_bytes;
                    {
                        std::lock_guard<std::mutex> vram_lk(impl_->translate_pool_vram_mu_);
                        if (impl_->translate_pool_reserved_vram_bytes_ >= contribution) {
                            impl_->translate_pool_reserved_vram_bytes_ -= contribution;
                        } else {
                            impl_->translate_pool_reserved_vram_bytes_ = 0;
                        }
                    }
                    it->second.reserved_vram_bytes = 0;
                }
                it->second.relayed_worker_process = nullptr;
                it->second.relayed_model_id.clear();
            }
        }
        ++crashes;
        // 장애 격리 (§V2-3): this family's in-flight jobs are ended here —
        // T4's dispatcher observes state==Crashed and answers engine_failed/
        // unavailable. Other families and other connections are untouched.
        w.jobs_failed_on_crash++;
        DIAG_F("ENGINEHOST/wmgr/016: worker crashed (family=%ws exit=%lu streak=%d backoff_ms=%d)\n",
               w.family.c_str(), static_cast<unsigned long>(exit_code),
               w.spawn_failures, BackoffDelayMs(w.spawn_failures));
    }
    return crashes;
}

void WorkerManager::StartReaper() {
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (impl_->reaper_thread.joinable()) return;
    impl_->reaper_stop.store(false, std::memory_order_release);
    impl_->reaper_thread = std::thread([this] {
        for (;;) {
            const DWORD wr = ::WaitForSingleObject(impl_->reaper_wake,
                                                   wp::kWorkerReaperPollMs);
            if (impl_->reaper_stop.load(std::memory_order_acquire)) return;
            (void)wr;
            // One pass per 250 ms tick (design §1.1: done_event 250 ms poll).
            (void)ReaperPass(NowMs());
        }
    });
}

void WorkerManager::StopReaper() {
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        if (!impl_->reaper_stop.exchange(true, std::memory_order_acq_rel)) {
            if (impl_->reaper_wake) ::SetEvent(impl_->reaper_wake);
        }
    }
    // Join OUTSIDE the lock (the reaper thread takes the same lock per pass).
    if (impl_->reaper_thread.joinable()) impl_->reaper_thread.join();
}

void WorkerManager::ShutdownAll() {
    StopReaper();
    std::vector<std::wstring> targets;
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        for (auto& w : impl_->workers) {
            if (w.state == WorkerState::Ready || w.state == WorkerState::Busy ||
                w.state == WorkerState::Spawning) {
                targets.push_back(w.family);
            }
        }
    }
    // REQ-043 (M6 fix, session 260918_0001): GracefulStop must run WITHOUT
    // the manager mutex — it takes the same lock in its step-1 scope and
    // again in its step-3 scope. With the pre-fix code the mutex was already
    // held here, so the GracefulStop lock acquisitions self-deadlocked; the
    // process then hit the CRT's deadlock timeout fail-fast (exit 0xc0000409,
    // observed 4/5 reproduction runs after a real worker-spawned translate)
    // instead of the designed exit 0. Each GracefulStop call now fetches its
    // own handle under a brief lock, stops the child (blocking <= its
    // timeout), then the orphan guard re-locks for the final sweep (design
    // §1.1 rule 5 unchanged).
    for (const auto& family : targets) {
        WorkerHandle* w = nullptr;
        {
            std::lock_guard<std::mutex> lk(impl_->mu);
            w = impl_->FindLocked(family);
        }
        if (w) (void)GracefulStop(*w);
    }
    // Orphan guard: anything still standing is killed (design §1.1 rule 5).
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        for (auto& w : impl_->workers) {
            impl_->ClosePipeLocked(w);
            if (w.process) {
                TerminateProcessHandle(w.process);
                impl_->close_fn(w.process, impl_->user);
                w.process = nullptr;
            }
            w.state = WorkerState::Stopped;
        }
    }
}

} // namespace host_v2
} // namespace emebalachat
