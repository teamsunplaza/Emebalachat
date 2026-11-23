// V10F (261122_0002 P2-B, L5-B): contract / static-assert TU for the pure
// translation coordinator.
//
// The coordinator's definitions are deliberately INLINE in
// translation_coordinator.hpp (see that header's "Implementation note"): the
// core is pure STL + this repo's unicode_utils conversion declarations, and
// keeping it header-only means the engine router AND the unit suite can use it
// with no new link edge and no CMake source-list change. This TU therefore
// defines NO symbols and is optional to build (the src/update_checker.cpp
// precedent: a header-only module's standalone syntax/ODR TU); it exists so the
// header's contract constants are checked by the compiler in one dedicated
// place, and so a future move of the definitions into this file only has to
// move code (the declarations already live in the header).
//
// Contract pins checked here:
//   * the frame budget is 768 KiB and stays under the frozen 1 MiB host cap;
//   * the worst-case escape factor (6 bytes per source byte) is consistent
//     with the budget arithmetic the engine's cheap pre-check relies on;
//   * the partial/cancel sentinel can never collide with a real frame index.
// The behavioral defaults (a default CoordinatorOptions = the legacy
// all-or-nothing shape, a default CoordinatorResult = "nothing ran") are
// pinned by tests/p2_b_tests.inc instead: those structs own std::function and
// std::wstring members, so they are not literal types and cannot be asserted
// here.

#include "translation_coordinator.hpp"

#include <limits>

namespace {

static_assert(emebalachat::kCoordinatorMaxFrameBytes == 768u * 1024u,
              "L5-B: the coordinator frame budget is exactly 768 KiB");
static_assert(emebalachat::kCoordinatorMaxFrameBytes < (1u << 20),
              "L5-B: the frame budget must stay under the frozen 1 MiB wire cap");
static_assert(emebalachat::kCoordinatorMaxEscapeFactor == 6,
              "L5-B: kCoordinatorMaxEscapeFactor is the \\u00xx cost of one control byte");
static_assert(emebalachat::kCoordinatorMaxFrameBytes / emebalachat::kCoordinatorMaxEscapeFactor ==
                  131072,
              "L5-B: the no-split pre-check threshold is 131,072 UTF-16 code units");
static_assert(emebalachat::kCoordinatorNoFrame == std::numeric_limits<std::size_t>::max(),
              "L5-B: 'no frame' is the max size_t sentinel, never a real index");

// The options/result defaults are behavioral (they own std::function and
// std::wstring, so they are not literal types and cannot be asserted here):
// tests/p2_b_tests.inc pins that a default CoordinatorOptions reproduces the
// legacy all-or-nothing shape and that a default CoordinatorResult is the
// "nothing ran" state.

} // namespace
