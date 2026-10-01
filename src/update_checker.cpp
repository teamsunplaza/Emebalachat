#include "update_checker.hpp"

// REQ-UC (session 260930_0004): translation unit for the update checker.
//
// The module is HEADER-ONLY by design (every function in update_checker.hpp
// is `inline`): both app-side consumers (src/ui/about_window.cpp and
// tests/run_tests.cpp) include the header directly. This TU compiles the
// header standalone (syntax/ODR check) and is LISTED in the Emebalachat_core
// source set in CMakeLists.txt (added with the REQ-MD model-download
// sources) — D-04: the earlier "add it later" note is stale. Keep the TU:
// exactly this one include, nothing more.
