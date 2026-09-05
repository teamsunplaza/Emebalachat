#pragma once

#include <string_view>

// ---------------------------------------------------------------------------
// Single source of truth for the product display name and version (REQ-006,
// architect plan §1.2/§1.3). The canonical value is CMakeLists.txt's
// project(Emebalachat VERSION 0.10.0); CMake forwards it to every consumer of
// Emebalachat_core (PUBLIC) as EMEBALACHAT_VERSION_STR="x.y.z" via
// target_compile_definitions. The #ifndef fallback keeps standalone includes
// (and any build that forgets the definition) compiling with the pinned
// release version instead of failing.
//
// This kills the future "forgot to update the About string" bug class:
// bumping project VERSION in CMakeLists.txt (and installer/setup.iss
// AppVersion) is now the ONLY version change a release needs.
// ---------------------------------------------------------------------------

// Two-level macro expansion so EMEBALACHAT_VERSION_STR is macro-expanded
// BEFORE being stringized / widened (plan §1.3).
#define EMBALA_VERSION_STR_IMPL(x) #x
#define EMBALA_VERSION_STR(x) EMBALA_VERSION_STR_IMPL(x)
#define EMBALA_VERSION_CATW_IMPL(s) L##s
#define EMBALA_VERSION_CATW(s) EMBALA_VERSION_CATW_IMPL(s)

#ifdef EMEBALACHAT_VERSION_STR
// CMake supplied: EMEBALACHAT_VERSION_STR=0.10.0 (tokens stringized here).
#define EMBALA_VERSION_ASCII EMBALA_VERSION_STR(EMEBALACHAT_VERSION_STR)
#else
// Fallback when the compile definition is absent (see header comment).
#define EMBALA_VERSION_ASCII "0.10.0"
#endif

#define EMBALA_VERSION_WIDE EMBALA_VERSION_CATW(EMBALA_VERSION_ASCII)

namespace emebalachat {

// Display name shown in the About window, tray, tooltips (REQ-004 rebrand).
inline constexpr std::wstring_view kAppNameW = L"Emebala Chat";

// Version as given by CMake PROJECT_VERSION, e.g. "0.10.0".
inline constexpr std::string_view kAppVersionA = EMBALA_VERSION_ASCII;
inline constexpr std::wstring_view kAppVersionW = EMBALA_VERSION_WIDE;

} // namespace emebalachat
