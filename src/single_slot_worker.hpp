#pragma once

// D2 (session 260910_0007, catalog W7): single-slot latest-wins job worker.
//
// Verified provenance: the drag_translate_worker loop and the retranslate_worker
// loop in src/main.cpp were token-compared line by line before extraction
// (audit 050600 §D2 verdict "TRUE pattern", re-verified this wave). After
// alpha-renaming the queue state, the two loop bodies had exactly THREE deltas:
//   1. job type: DragTranslateJob{x, y, gen} vs RetranslateJob{src, src_code,
//      new_tgt, x, y, gen}  ->  template parameter JobT;
//   2. handler call shape: run_drag_translate(job.x, job.y, job.gen) vs
//      run_retranslate(job)  ->  a std::function<void(const JobT&)>; the drag
//      site passes a three-field adapter lambda, the retranslate site passes
//      its job-taking lambda directly;
//   3. the pop was `job = drag_job;` (copy) on the drag side and
//      `job = std::move(retranslate_job);` on the retranslate side:
//      DragTranslateJob is trivially copyable so move == copy there, and the
//      slot is fully replaced by the next producer assignment in BOTH cases,
//      so the uniform std::move pop here is behavior-neutral.
// There were NO other deltas: same wait predicate (stop_requested() || pending),
// same stop-first-then-pop ordering (a pending job at Stop() time is discarded,
// "superseded anyway" comment in both), zero logging inside either loop (all
// DIAG_* traffic lives in the handlers), no thread naming anywhere in the tree
// (no SetThreadDescription), and zero exception guarding (a throwing handler
// escapes the worker body and std::terminate()s the process - this template
// preserves that contract by NOT adding a catch).
//
// Producer/consumer handshake invariants (do not "improve" them here):
//   - Submit() is a latest-wins OVERWRITE, never a busy-drop: a job that the
//     worker has not popped yet is replaced. A superseded result is made
//     harmless at render time by the tooltip's B1-H1 generation guard. The
//     generation stamp (TooltipWindow::BeginTranslationRequest) is owned by
//     the PRODUCER sites in main.cpp and MUST stay there - the template knows
//     nothing about generations (audit 050600 §D2 acceptance constraint).
//   - has_pending_ is written only under the mutex; notify_one() fires after
//     the lock is released (exactly what all three current producer sites do).
//   - Stop() mirrors today's explicit teardown tokens request_stop() followed
//     by cv.notify_all(): the loop registers NO stop_callback on the cv, so
//     request_stop alone could leave a cv.wait-blocked worker asleep while
//     ~jthread's join hangs. Join() mirrors `if (w.joinable()) w.join();`.
//     The jthread destructor remains the same unreachable-but-identical
//     fallback it is today (main.cpp always Stops+Joins before scope exit).

#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

namespace emebalachat {

// One persistent std::jthread consuming single-slot JobT jobs posted by any
// producer thread. JobT must be default-constructible (both shipped job
// structs are all-NSDMI aggregates) and move-assignable.
template <typename JobT>
class SingleSlotWorker {
public:
    // Construction starts the worker immediately - mirrors today's
    // `std::jthread name(...)` declaration sites, which start in their
    // initializer. handler_ is fully constructed before thread_ spawns
    // (member declaration order below).
    explicit SingleSlotWorker(std::function<void(const JobT&)> handler)
        : handler_(std::move(handler))
        , thread_([this](std::stop_token st) { Loop(st); }) {}

    SingleSlotWorker(const SingleSlotWorker&) = delete;
    SingleSlotWorker& operator=(const SingleSlotWorker&) = delete;

    // Producer side: overwrite the pending slot (latest-wins) under the
    // mutex, then wake one waiting consumer outside the lock.
    void Submit(JobT job) {
        {
            std::lock_guard<std::mutex> lk(mutex_);
            pending_job_ = std::move(job);
            has_pending_ = true;
        }
        cv_.notify_one();
    }

    // Today's shutdown tokens: worker.request_stop(); cv.notify_all();
    void Stop() {
        thread_.request_stop();
        cv_.notify_all();
    }

    // Today's shutdown tokens: if (worker.joinable()) worker.join();
    void Join() {
        if (thread_.joinable()) {
            thread_.join();
        }
    }

private:
    void Loop(std::stop_token st) {
        for (;;) {
            JobT job;
            {
                std::unique_lock<std::mutex> lk(mutex_);
                // GUI-thread producers set has_pending_ UNDER this mutex, so
                // the textbook cv protocol holds with no lost wakeup and no
                // time backstop (unlike the hook-thread producer in
                // hook.cpp, which must stay lock-free).
                cv_.wait(lk, [this, &st]() {
                    return st.stop_requested() || has_pending_;
                });
                if (st.stop_requested()) {
                    break; // a pending job is superseded anyway (guard drops it)
                }
                job = std::move(pending_job_);
                has_pending_ = false;
            }
            // Mutex NOT held across clipboard work / engine.Translate
            // (same rule as KeyboardHook::DoubleCtrlCWorkerLoop).
            handler_(job);
        }
    }

    std::function<void(const JobT&)> handler_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool has_pending_ = false;
    JobT pending_job_{};
    std::jthread thread_;
};

} // namespace emebalachat
