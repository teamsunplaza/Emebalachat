// Progress + cancel seam for multi-chunk translations (session 261121_0002).
// Pure STL header: no Win32, no network, no llama — includable from the cloud
// clients, the engine router, and the tests without new link edges. A
// default-constructed context reproduces the pre-feature behavior exactly.
#pragma once

#include <atomic>
#include <cstddef>
#include <functional>

namespace emebalachat {

// Per-request translation task context. Owned by the CALLER (the pipeline
// worker); alive for the whole Translate() call; read-only inside the
// clients.
// - cancel: when non-null and true, the chunk loops unwind at the next
//   between-chunk check and the manager reports TranslationStatus::Canceled.
// - on_chunk: invoked on the calling (worker) thread as (index_1based,
//   total) immediately BEFORE each network/pipe-bearing chunk request;
//   total > 1 is the multi-chunk signal. The caller marshals to the GUI
//   thread itself.
struct TranslationTaskContext {
    const std::atomic<bool>* cancel = nullptr;
    std::function<void(std::size_t index_1based, std::size_t total)> on_chunk;
};

// Progress numbers are only shown for genuine multi-chunk runs; a single
// chunk (or an empty plan) keeps the badge byte-identical to the
// pre-feature build.
constexpr bool ShowChunkProgress(std::size_t total) { return total > 1; }

// A set cancel flag always wins over the loop-budget check at each boundary.
constexpr bool ChunkLoopCancelRequested(const std::atomic<bool>* f) {
    return f != nullptr && f->load(std::memory_order_acquire);
}

}  // namespace emebalachat
