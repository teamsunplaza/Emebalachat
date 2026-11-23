// 261116_0003 P9b-3 U3 (language-menu redesign, plan §5 P9): the first-use
// capability-probe runner. See user_language_probe.hpp for the contract; this
// TU holds the production transport (TranslationManager::Translate wrapper)
// and the service implementation. The verdict fold, the trigger predicate and
// the engine/model rules are header-inline (SECTION A) so the suite drives
// them headlessly with a fake transport.

#include "user_language_probe.hpp"

#include "diag_logger.hpp"   // DIAG_LOG (shape-only: statuses, counts, keys)
#include "unicode_utils.hpp" // ToUtf8 / ToUtf16

#include <cstdlib> // fix-list-round15 R15-11: std::abort behind the TerminateProcess escalation
#include <thread>
#include <utility>

namespace emebalachat {

UserLanguageProbeTransport MakeProductionUserLanguageProbeTransport(
    TranslationManager& engine, const std::atomic<bool>* shutdown_token) {
    return [&engine, shutdown_token](std::string_view sentence, std::string_view engine_id,
                                     std::string_view model, std::string_view target_name) -> std::wstring {
        (void)engine_id; // the engine is latched by the manager's own config;
        (void)model;     // the key halves ride along for the cache, not the call
        TranslationStatus status = TranslationStatus::Ok;
        // Source = the AUTO-style constant (the probe judges TARGET capability,
        // research-report-p9b3-map §5); the status-aware form distinguishes an
        // engine failure from an empty output for the (shape-only) log line.
        // V10F (BUG-09): the per-service token rides as ctx.cancel so an
        // in-flight probe unwinds promptly once BeginShutdown() trips it
        // (a nullptr token — the one-arg factory — is the byte-identical
        // pre-V10F default context).
        TranslationTaskContext ctx;
        ctx.cancel = shutdown_token;
        const std::wstring output =
            engine.Translate(ToUtf16(sentence), "Auto Detect", target_name, &status, ctx);
        if (status != TranslationStatus::Ok || output.empty()) {
            DIAG_LOG("PROBE", "transport status=%u out_len=%zu (fold -> Unsupported)",
                     static_cast<unsigned>(status), output.size());
            return {};
        }
        return output;
    };
}

UserLanguageProbeTransport MakeProductionUserLanguageProbeTransport(
    TranslationManager& engine) {
    return MakeProductionUserLanguageProbeTransport(engine, nullptr);
}

UserLanguageProbeService::UserLanguageProbeService(AppConfig& config,
                                                   TranslationManager& engine,
                                                   HWND controller)
    : UserLanguageProbeService(config, controller) {
    // V10F (BUG-09): hand the transport the service-owned token; BeginShutdown()
    // trips it so an in-flight probe cancels through the manager's per-request
    // seam instead of outliving the 2 s shutdown-join bound.
    state_->transport =
        MakeProductionUserLanguageProbeTransport(engine, &state_->cancel_token);
}

UserLanguageProbeService::UserLanguageProbeService(AppConfig& config, HWND controller)
    : config_(config), controller_(controller), state_(std::make_shared<SharedState>()) {
}

UserLanguageProbeService::~UserLanguageProbeService() {
    Retire();
    // fix-list-round15 R15-11 backstop: the production path joins explicitly at
    // the shutdown sweep BEFORE the engine leaves scope; the destructor repeats
    // the bounded join so the invariant is class-local - the service is never
    // destroyed while one of its workers could still touch the engine reference
    // the transport captured. Bound-expiry escalates (never returns), the same
    // safe terminal state as the explicit path.
    ShutdownJoinWorkers(std::chrono::milliseconds(kUserLanguageProbeShutdownJoinBoundMs));
}

void UserLanguageProbeService::SetTransportForTest(UserLanguageProbeTransport transport) {
    state_->transport = std::move(transport);
}

std::string UserLanguageProbeService::InFlightKey(std::string_view stored_name,
                                                  std::string_view engine,
                                                  std::string_view model) {
    // '\n' cannot appear in a stored name (the C0 strip set removes it) and is
    // not trimmed from engine/model, so the triple joins unambiguously.
    std::string key(stored_name);
    key.push_back('\n');
    key.append(engine);
    key.push_back('\n');
    key.append(model);
    return key;
}

bool UserLanguageProbeService::IsInFlight(std::string_view stored_name,
                                          std::string_view engine,
                                          std::string_view model) const {
    return in_flight_.find(InFlightKey(stored_name, engine, model)) != in_flight_.end();
}

bool UserLanguageProbeService::StartProbe(LanguageEngine engine,
                                          const UserLanguage& entry,
                                          std::string engine_id, std::string model) {
    (void)engine; // the engine choice already shaped engine_id/model
    // fix-list-round15 R18 (unit F-4): reclaim the finished handles BEFORE the
    // new spawn, so the vector holds the in-flight threads plus at most the
    // ones started while another probe was running - a long session with many
    // first-use picks no longer accumulates one handle per probe.
    ReapFinishedWorkers();
    const std::string stored = UserLanguageStoredName(entry);
    in_flight_.insert(InFlightKey(stored, engine_id, model));

    Outcome seed;
    seed.stored_name = stored;
    seed.engine = std::move(engine_id);
    seed.model = std::move(model);
    seed.script_mask = entry.script_mask;

    std::shared_ptr<SharedState> state = state_;
    const HWND controller = controller_;
    {
        std::lock_guard<std::mutex> lk(state->workers_mu);
        ++state->active_workers; // before the spawn: the worker may finish fast
    }
    // fix-list-round15 R15-11: an OWNED, JOINABLE worker (never detached) - it
    // is pushed into workers_ (GUI-thread-only) and joined by
    // ShutdownJoinWorkers before the engine leaves scope; R18 (unit F-4) also
    // reclaims it opportunistically through ReapFinishedWorkers once idle.
    std::thread worker([state, controller, seed = std::move(seed)] {
        Outcome outcome = seed;
        try {
            if (state->transport) {
                outcome.output = state->transport(kUserLanguageProbeSentence,
                                                  outcome.engine, outcome.model,
                                                  outcome.stored_name);
            }
        } catch (...) {
            // An engine failure must fold to Unsupported, never crash the
            // runner (a throwing transport leaves output L"").
            outcome.output.clear();
        }
        {
            std::lock_guard<std::mutex> lk(state->outcomes_mu);
            state->outcomes.push_back(std::move(outcome));
        }
        // SEC-B5 shape: the payload traveled by value above; the wake-up is
        // parameter-less. A failed post leaves the entry queued for the next
        // drain (same contract as kMsgApplyLanguageSync).
        if (controller != nullptr &&
            ::PostMessageW(controller, kMsgUserLanguageProbeDone, 0, 0) == FALSE) {
            DIAG_LOG("PROBE", "completion post failed gle=%lu (outcome stays queued)",
                     ::GetLastError());
        }
        // R15-11: the LAST statement. The outcome is already queued, so
        // active_workers == 0 guarantees every outcome is drainable and this
        // thread is one statement from exit - the bounded shutdown join below
        // can then never block on engine work.
        {
            std::lock_guard<std::mutex> lk(state->workers_mu);
            --state->active_workers;
        }
        state->workers_cv.notify_all();
    });
    workers_.push_back(std::move(worker));
    return true;
}

bool UserLanguageProbeService::MaybeTriggerForPick(LanguageEngine engine,
                                                   std::string_view target_canonical) {
    // fix-list-round15 R18 (unit F-4): no NEW probe may start once the
    // shutdown intent is latched (main.cpp latches it before the language-sync
    // drain, whose coordinator reaches this seam). A probe started there would
    // meet the cancel latch inside Translate and persist a false Unsupported
    // verdict. `retired` additionally discards a completion that lands late.
    if (state_->shutdown_intent || state_->retired) {
        return false;
    }
    const UserLanguage* entry = FindUserLanguage(config_.user_languages, target_canonical);
    const std::string engine_id = UserLanguageEngineId(engine);
    const std::string model = UserLanguageProbeModelFor(engine, config_);
    const std::string stored = entry ? UserLanguageStoredName(*entry) : std::string();
    const UserLanguageProbe* probe =
        entry ? FindUserLanguageProbe(*entry, engine_id, model) : nullptr;
    if (!UserLanguageProbeFirstUseWanted(engine, entry, probe,
                                         IsInFlight(stored, engine_id, model))) {
        return false;
    }
    return StartProbe(engine, *entry, engine_id, model);
}

bool UserLanguageProbeService::RequestRetest(LanguageEngine engine,
                                             std::string_view stored_name) {
    // fix-list-round15 R18 (unit F-4): same shutdown-intent gate as the
    // first-use seam - a retest started during teardown would also fold the
    // canceled/empty output to Unsupported and persist it.
    if (state_->shutdown_intent || state_->retired) {
        return false;
    }
    const UserLanguage* entry = FindUserLanguage(config_.user_languages, stored_name);
    if (entry == nullptr || !UserLanguageProbeEngineJudgedByEvidence(engine)) {
        return false;
    }
    const std::string engine_id = UserLanguageEngineId(engine);
    const std::string model = UserLanguageProbeModelFor(engine, config_);
    if (IsInFlight(UserLanguageStoredName(*entry), engine_id, model)) {
        return false;
    }
    return StartProbe(engine, *entry, engine_id, model);
}

void UserLanguageProbeService::CompleteOutcome(const Outcome& outcome) {
    const ProbeEvaluation evaluation =
        FoldUserLanguageProbeOutput(ToUtf8(outcome.output), outcome.script_mask);
    const UserLanguageWriteResult wr = config_.RecordUserLanguageProbeResultFor(
        outcome.stored_name, outcome.engine, outcome.model, evaluation,
        UserLanguageProbeTimestampNowUtc());
    // The slot frees regardless of the write result: the probe finished, and
    // an entry removed mid-flight must not pin its row to "checking" forever.
    in_flight_.erase(InFlightKey(outcome.stored_name, outcome.engine, outcome.model));
    if (wr == UserLanguageWriteResult::Applied && recorded_cb_) {
        recorded_cb_();
    }
    // Shape-only: statuses, the write result and the evidence LENGTH. No name,
    // no model string, no output text (the evidence string itself is shape-only
    // by construction, config.cpp:2365-2369).
    DIAG_LOG("PROBE", "verdict=%u write=%u evidence_len=%zu",
             static_cast<unsigned>(evaluation.status), static_cast<unsigned>(wr),
             evaluation.evidence.size());
}

std::size_t UserLanguageProbeService::DrainOutcomes() {
    std::deque<Outcome> batch;
    {
        std::lock_guard<std::mutex> lk(state_->outcomes_mu);
        batch.swap(state_->outcomes);
    }
    for (const Outcome& outcome : batch) {
        if (state_->retired) {
            in_flight_.erase(InFlightKey(outcome.stored_name, outcome.engine,
                                         outcome.model));
            continue; // after the exit intent a finished probe is dropped
        }
        CompleteOutcome(outcome);
    }
    // fix-list-round15 R18 (unit F-4): once the drain leaves no probe active,
    // the finished handles are reclaimed here too - the vector stays bounded
    // even if the session spawns a single probe and then goes idle.
    ReapFinishedWorkers();
    return batch.size();
}

void UserLanguageProbeService::SetRecordedCallback(std::function<void()> callback) {
    recorded_cb_ = std::move(callback);
}

void UserLanguageProbeService::Retire() {
    state_->retired = true;
}

void UserLanguageProbeService::BeginShutdown() {
    // fix-list-round15 R18 (unit F-4): GUI-thread-only, like every other
    // service mutation. Idempotent - the shutdown block latches once.
    state_->shutdown_intent = true;
    // V10F (BUG-09, 261122_0002 P1-U4): trip the probe transport's cancel
    // token — main.cpp latches BeginShutdown() at the top of the shutdown
    // sweep (before the language-sync drain and the bounded join), so an
    // in-flight probe Translate() unwinds through ctx.cancel instead of
    // holding the 2 s join bound, and its in-flight engine call classifies
    // Canceled rather than racing the manager's REQ-R16 latch alone.
    state_->cancel_token.store(true, std::memory_order_release);
}

void UserLanguageProbeService::ReapFinishedWorkers() {
    {
        std::lock_guard<std::mutex> lk(state_->workers_mu);
        if (state_->active_workers != 0) {
            return; // a probe is still inside the engine: nothing reclaimable
        }
    }
    // active_workers == 0 means every owned worker has already queued its
    // outcome and is one statement from exit (the R15-11 invariant), so each
    // join returns immediately; this is the same guarantee ShutdownJoinWorkers
    // relies on. GUI thread only and the same thread that spawns, so workers_
    // cannot grow underneath the loop.
    for (std::thread& t : workers_) {
        if (t.joinable()) {
            t.join();
        }
    }
    workers_.clear();
}

void UserLanguageProbeService::ShutdownJoinWorkers(std::chrono::milliseconds bound) {
    {
        std::unique_lock<std::mutex> lk(state_->workers_mu);
        // Bounded: engine.RequestCancel() (latched by the caller before this)
        // unwinds a cancel-honoring probe well inside the bound. A worker
        // decrements active_workers only AFTER its outcome is queued, so a
        // satisfied predicate means every outcome is drainable and every
        // thread is one statement from exit.
        const bool all_done =
            state_->workers_cv.wait_for(lk, bound,
                                        [this] { return state_->active_workers == 0; });
        if (!all_done) {
            // A cancel-IGNORING engine request outlived the bound. Joining the
            // stuck thread would block forever, and destroying the engine (or
            // this service's members) under it is a use-after-free. Escalate
            // to the safe terminal state: terminate the process - no further
            // destructors run, the worker dies with the engine still alive,
            // and the teardown that would race the worker never happens.
            DIAG_F("PROBE/Shutdown/001: probe worker join bound expired with a worker still "
                   "inside the engine; terminating so the engine is never destroyed under "
                   "the live worker\n");
            ::TerminateProcess(::GetCurrentProcess(), 1);
            std::abort(); // unreachable (TerminateProcess never returns)
        }
    }
    // Every worker has queued its outcome and is about to exit: the joins
    // return promptly, then the caller's DrainOutcomes folds + records each
    // completion, so the in-flight set ends exact.
    for (std::thread& t : workers_) {
        if (t.joinable()) {
            t.join();
        }
    }
    workers_.clear();
}

bool UserLanguageProbeService::RunOneBlockingForTest(LanguageEngine engine,
                                                     std::string_view stored_name) {
    if (state_->retired) {
        return false;
    }
    const UserLanguage* entry = FindUserLanguage(config_.user_languages, stored_name);
    if (entry == nullptr || !UserLanguageProbeEngineJudgedByEvidence(engine)) {
        return false;
    }
    const std::string engine_id = UserLanguageEngineId(engine);
    const std::string model = UserLanguageProbeModelFor(engine, config_);
    in_flight_.insert(InFlightKey(UserLanguageStoredName(*entry), engine_id, model));

    Outcome outcome;
    outcome.stored_name = UserLanguageStoredName(*entry);
    outcome.engine = engine_id;
    outcome.model = model;
    outcome.script_mask = entry->script_mask;
    try {
        if (state_->transport) {
            outcome.output = state_->transport(kUserLanguageProbeSentence, outcome.engine,
                                               outcome.model, outcome.stored_name);
        }
    } catch (...) {
        outcome.output.clear();
    }
    CompleteOutcome(outcome);
    return true;
}

} // namespace emebalachat
