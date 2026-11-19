#pragma once

// ---- 261116_0003 P9b-3 U3 (language-menu redesign, plan §5 P9): the first-use
// CAPABILITY PROBE runner + trigger. -----------------------------------------
//
// When a user-defined language is first used as a translation TARGET on an
// engine that judges by evidence (OpenAI-compatible / user-.gguf), the app
// sends ONE fixed probe sentence (kUserLanguageProbeSentence, config.hpp:312),
// evaluates the answer with the pinned pure evaluator (EvaluateProbeResult,
// config.cpp) and caches the verdict on the entry's probe slot through U1's
// locked mutator (AppConfig::RecordUserLanguageProbeResultFor). The Google
// engine is publisher-declared (D6) and never probed; the local built-in and
// Auto never probe (D7: UserLanguageEngineId returns ""). A verdict or a user
// override already answers the question - the trigger fires only while the
// effective status is Unknown (the contract at config.hpp:342-345, :351-355).
//
// THREADING. TranslationManager::Translate blocks up to ~30 s (engine.hpp:401-
// 408), so the engine call runs on a JOINABLE worker thread owned by this
// service (fix-list-round15 R15-11: never detached - a detached worker could
// outlive the engine its transport dereferences); the verdict travels back BY
// VALUE through a mutex-guarded deque and a parameter-less wake-up PostMessageW
// - the house SEC-B5 shape (kMsgApplyLanguageSync, main.cpp:424-558), never a
// pointer in LPARAM. The GUI thread (controller WndProc + the shutdown sweep)
// drains completions: the pure fold + the locked record verb run there. The
// shutdown sweep JOINS the workers with a bound (ShutdownJoinWorkers) after
// engine.RequestCancel() latched and before the engine leaves scope - a
// cancel-ignoring request that outlives the bound escalates to the safe
// terminal state instead of a use-after-free. fix-list-round15 R18 (unit F-4):
// the sweep ALSO latches BeginShutdown() before the language-sync drain that
// can spawn, so no probe starts after the exit intent - a probe started there
// meets the cancel latch inside Translate and would persist a false
// Unsupported verdict for a language that was never really probed. The
// in-flight set is GUI-thread-only, exactly like the AppConfig::user_languages
// read rule (config.hpp:999-1003): the trigger and every drain run on the GUI
// thread, so no lock is needed.
//
// CHECKING IS RUNTIME-ONLY. While a probe is in flight the runtime status may
// READ Checking (U4's badge fold combines IsInFlight with the persisted
// status); the service never WRITES Checking anywhere - the persisted reader
// maps a stored "checking" back to Unknown (ParseProbeStatusToken,
// config.cpp:1740-1748, pinned by the P9a suite and re-pinned here).
//
// SECTION A below (the transport alias, the message id, the pure folds and the
// trigger predicate) is header-inline so the unit suite drives it with a fake
// transport and zero network; SECTION B declares the service the suite links
// but never runs on a real engine.

#include "config.hpp" // UserLanguage, the D6 probe cache, the locked record verbs
#include "engine.hpp" // TranslationManager (the production transport's callee)

#include <Windows.h> // HWND / PostMessageW / WM_APP for the completion wake-up

#include <chrono> // fix-list-round15 R15-11: the shutdown-join bound type
#include <condition_variable> // fix-list-round15 R15-11: the worker-completion signal
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread> // fix-list-round15 R15-11: the owned joinable workers (never detached)
#include <vector>

namespace emebalachat {

// SECTION A - pure, headless-testable surface --------------------------------

// The single engine call, behind an injectable seam (the FakeLocaleSource
// idiom, tests/p9a_user_languages_tests.inc:29): sentence = the fixed probe
// sentence, engine_id = UserLanguageEngineId, model = the OpenAI model string
// or the user-.gguf registry id, target_name = the entry's stored name (the
// free-form dispatch payload). An empty return means engine failure or empty
// output - the fold maps either to Unsupported, so the seam needs no status
// channel (the production transport logs the real status, shape-only).
using UserLanguageProbeTransport = std::function<std::wstring(
    std::string_view sentence, std::string_view engine_id,
    std::string_view model, std::string_view target_name)>;

// The completion wake-up. WM_APP + 0x600 sits above every allocated id (the
// controller's 0x300..0x502 block, main.cpp:424-661, and the surfaces' own
// ranges); the message carries NO payload (SEC-B5: any posted wParam/lParam is
// untrusted by design).
inline constexpr UINT kMsgUserLanguageProbeDone = WM_APP + 0x600;

// fix-list-round15 R15-11: the shutdown-join bound. Composed with the caller's
// engine.RequestCancel() latch (the main.cpp shutdown block): a cancel-honoring
// probe unwinds well inside it; a cancel-IGNORING engine request that outlives
// it escalates to the safe terminal state (TerminateProcess) - the engine is
// never destroyed under the live worker.
inline constexpr int kUserLanguageProbeShutdownJoinBoundMs = 2000;

// The verdict fold: one engine output -> the pinned evaluator's verdict.
// Header-inline so the suite drives it with a fake transport and no network.
inline ProbeEvaluation FoldUserLanguageProbeOutput(std::string_view output_text,
                                                   std::uint32_t expected_script_mask) {
    return EvaluateProbeResult(kUserLanguageProbeSentence, output_text,
                               expected_script_mask);
}

// D6/D7 engine rule: only the OpenAI-compatible and the user-.gguf engines
// judge a user language by evidence. Google is publisher-declared (the menu
// merge already filters its view by the kLangGoogle bit), and the local
// built-in plus Auto have no custom row at all (UserLanguageEngineId "").
inline bool UserLanguageProbeEngineJudgedByEvidence(LanguageEngine engine) {
    return engine == LanguageEngine::OpenAi || engine == LanguageEngine::UserGguf;
}

// The model half of the (engine, model) probe-cache key (probe.model semantics,
// config.hpp:181-184): the OpenAI block's model string, or the user-.gguf
// registry id. "" for every other engine (which never probe anyway).
inline std::string UserLanguageProbeModelFor(LanguageEngine engine,
                                             const AppConfig& config) {
    if (engine == LanguageEngine::OpenAi) {
        return config.openai.model;
    }
    if (engine == LanguageEngine::UserGguf) {
        return config.user_model_id;
    }
    return {};
}

// The first-use trigger contract, pure: fire only when ALL of these hold -
//   * the engine judges by evidence (Google/HyMt2/Auto never fire),
//   * the target resolved to a registered user entry (registry targets carry
//     a declared capability and need no probe),
//   * no verdict AND no override exists (EffectiveUserLanguageProbeStatus ==
//     Unknown, config.hpp:342-345, :351-355; the override wins the effective
//     fold, so one check covers both),
//   * no probe for the same (engine, model, language) is already in flight
//     (at most one in flight per key; the record path stays idempotent).
inline bool UserLanguageProbeFirstUseWanted(LanguageEngine engine,
                                            const UserLanguage* entry,
                                            const UserLanguageProbe* probe,
                                            bool in_flight) {
    if (in_flight) {
        return false;
    }
    if (!UserLanguageProbeEngineJudgedByEvidence(engine)) {
        return false; // D6 Google publisher-declared; D7 HyMt2/Auto no row
    }
    if (entry == nullptr) {
        return false; // a registry target carries a declared capability
    }
    if (probe != nullptr &&
        EffectiveUserLanguageProbeStatus(*probe) != UserLanguageProbeStatus::Unknown) {
        return false; // a cached verdict or a user override already answers
    }
    return true;
}

// SECTION B - the runner service (tests link it, production wires it) --------

// Owns the in-flight set (GUI-thread-only), the by-value completion queue, the
// transport and the joinable worker threads (fix-list-round15 R15-11).
// Constructed on the GUI thread once the controller window exists; the
// shutdown sweep joins the workers (ShutdownJoinWorkers, bounded) and Retire()
// runs there with the other coordinators.
class UserLanguageProbeService {
public:
    // Production: wraps TranslationManager::Translate (the status-aware form,
    // engine.hpp:419-424, request-serialized by translate_mutex_).
    UserLanguageProbeService(AppConfig& config, TranslationManager& engine, HWND controller);
    // Test seam: no production transport (the suite injects a fake before
    // firing; an empty transport folds every probe to Unsupported, never a
    // crash).
    UserLanguageProbeService(AppConfig& config, HWND controller);
    ~UserLanguageProbeService();

    UserLanguageProbeService(const UserLanguageProbeService&) = delete;
    UserLanguageProbeService& operator=(const UserLanguageProbeService&) = delete;

    void SetTransportForTest(UserLanguageProbeTransport transport);

    // ---- trigger entry points (GUI thread only) ----
    // The first-use trigger: `target_canonical` is the plan-resolved target
    // (ApplyLanguageChange canonicalizes a user pick to the stored name, so
    // FindUserLanguage resolves it). No-op unless UserLanguageProbeFirstUseWanted
    // fires; at most one in-flight probe per (engine, model, language).
    bool MaybeTriggerForPick(LanguageEngine engine, std::string_view target_canonical);
    // U4's "다시 시험": re-run for one entry on the current engine/model even
    // when a cached verdict exists. The record never retires a user override
    // (PR9-06 lives in the locked verb), so a retest cannot stomp the user's
    // answer. No-op for Google/HyMt2/Auto, unknown entries, or an in-flight key.
    bool RequestRetest(LanguageEngine engine, std::string_view stored_name);

    // ---- completion path (GUI thread only) ----
    // ControllerWndProc's kMsgUserLanguageProbeDone handler and the shutdown
    // sweep both drain here: fold + RecordUserLanguageProbeResultFor per
    // completion, then the recorded callback. Returns the drained count.
    std::size_t DrainOutcomes();
    // The runtime-only Checking probe (U4's badge fold: in flight -> Checking,
    // never read from the persisted slot).
    bool IsInFlight(std::string_view stored_name, std::string_view engine,
                    std::string_view model) const;
    // Invoked on the GUI thread after each verdict the locked verb applied
    // (U4 / the caller persists + refreshes surfaces; the service itself does
    // no file I/O).
    void SetRecordedCallback(std::function<void()> callback);

    // fix-list-round15 R18 (unit F-4): the shutdown-INTENT latch. Set ONCE at
    // the start of the shutdown sequence - after engine.RequestCancel() and
    // BEFORE the language-sync drain, whose accepted-plan path can still call
    // MaybeTriggerForPick. Both spawn seams decline while it is set, so no
    // probe ever starts once shutdown has begun: a probe started there would
    // find the cancel latch inside Translate, the transport would map the
    // empty output to {}, the fold would read Unsupported(reason=empty) and
    // the locked verb would PERSIST that false verdict - suppressing the
    // language's first-use trigger until the user runs a manual retest.
    // Deliberately wider than `retired`: intent stops NEW spawns, retired
    // additionally turns a landing completion into a discard. The join/drain
    // mechanics are untouched; the exhaustive claim of ShutdownJoinWorkers
    // holds a fortiori because the drain can no longer produce work.
    void BeginShutdown();

    // Shutdown: late completions drain as discards (the verdict of a probe
    // that finished after the exit intent is dropped, never recorded).
    void Retire();

    // fix-list-round15 R15-11: bounded shutdown join - the Stop()+Join()
    // discipline the other engine consumers use (main.cpp shutdown block).
    // The caller MUST have latched engine.RequestCancel() first so an
    // in-flight probe Translate unwinds, and BeginShutdown() so no new probe
    // can start while the bound runs: this waits at most `bound` for every
    // started probe to queue its outcome, then joins the owned threads (safe:
    // a worker decrements the active count only AFTER its outcome is queued,
    // so a satisfied wait means every thread is one statement from exit). The
    // caller drains afterwards, so the in-flight bookkeeping (IsInFlight)
    // ends exact. On bound expiry (a cancel-ignoring request still inside the
    // engine) this escalates to the safe terminal state and NEVER returns -
    // the engine cannot be destroyed under the live worker (no use-after-free
    // at exit).
    void ShutdownJoinWorkers(std::chrono::milliseconds bound);

    // ---- test-only synchronous path ----
    // Runs one probe for (engine, stored_name) ON THE CALLING THREAD through
    // the same completion code the drain uses (fold -> locked record -> slot
    // release). Production never calls this: Translate blocks the GUI thread.
    bool RunOneBlockingForTest(LanguageEngine engine, std::string_view stored_name);

private:
    struct Outcome {
        std::string stored_name;
        std::string engine;
        std::string model;
        std::uint32_t script_mask = 0;
        std::wstring output; // L"" = transport failure / empty output
    };
    // Shared with the worker threads: the threads capture this state (never
    // `this`; fix-list-round15 R15-11: the shutdown join guarantees a worker
    // finishes BEFORE the service is destroyed, and the by-value capture keeps
    // the design correct even so).
    struct SharedState {
        std::mutex outcomes_mu;
        std::deque<Outcome> outcomes;
        UserLanguageProbeTransport transport;
        bool retired = false;
        // fix-list-round15 R18 (unit F-4): the shutdown-intent latch (see
        // BeginShutdown) - set before the shutdown drain that can spawn and
        // checked by BOTH spawn seams. GUI-thread-only, exactly like retired.
        bool shutdown_intent = false;
        // R15-11 worker lifetime bookkeeping (the completion signal): a worker
        // decrements active_workers as its LAST statement - after its outcome
        // is queued - so active_workers == 0 guarantees every outcome is
        // drainable and every thread is one statement from exit (the bounded
        // shutdown join can then never block on engine work).
        std::mutex workers_mu;
        std::condition_variable workers_cv;
        std::size_t active_workers = 0;
    };

    static std::string InFlightKey(std::string_view stored_name,
                                   std::string_view engine, std::string_view model);
    // Snapshot the probe inputs by value, mark in-flight, spawn the worker.
    bool StartProbe(LanguageEngine engine, const UserLanguage& entry,
                    std::string engine_id, std::string model);
    // fix-list-round15 R18 (unit F-4): reclaim the handles of probes that have
    // already finished (the vector otherwise grows for the whole session).
    // Joins ONLY when the active count is 0, i.e. when every owned thread has
    // queued its outcome and is one statement from exit (the R15-11
    // invariant), so no join can block the GUI thread on engine work.
    void ReapFinishedWorkers();
    // The drain body: pure fold -> U1's locked record verb -> slot release ->
    // recorded callback. GUI thread only.
    void CompleteOutcome(const Outcome& outcome);

    AppConfig& config_;
    HWND controller_;
    std::shared_ptr<SharedState> state_;
    std::set<std::string> in_flight_; // GUI-thread-only
    std::function<void()> recorded_cb_;
    // fix-list-round15 R15-11: the owned JOINABLE workers, never detached.
    // GUI-thread-only (StartProbe, the drain and the shutdown join all run
    // there); ShutdownJoinWorkers joins and clears them before the engine
    // leaves scope, and ReapFinishedWorkers (R18, unit F-4) reclaims the
    // finished ones opportunistically while the service is idle.
    std::vector<std::thread> workers_;
};

} // namespace emebalachat
