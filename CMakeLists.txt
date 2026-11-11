cmake_minimum_required(VERSION 3.24)

# REQ-046 P4-4 history: the version bump in commit 3b3a426 was withdrawn
# per user decision (2026-09-19) and shipped as 0.10.1 rereleases; promoted
# to 0.10.2 on 2026-09-30 (session 260930_0003, CEO decision); promoted to
# 0.10.4 on 2026-10-29 (session 261029_0001, see CHANGELOG "v0.10.4");
# promoted to 0.10.5 on 2026-11-04 (session 261104_0001, see CHANGELOG
# "v0.10.5"); promoted to 0.10.6 on 2026-11-08 (session 261108_0002, see
# CHANGELOG "v0.10.6").
project(Emebalachat VERSION 0.10.7 LANGUAGES C CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# MSVC Static CRT (/MT in Release, /MTd in Debug)
if (MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:/Zc:char8_t->")
endif()

# 261111_0001 U7 (v0.10.7): the shared engine (the Emebala.Engine orchestrator,
# the ggml-translate worker, the llama-coupled engine_core library, and the
# engine_host_* client/parsers) lives in its own repository and is consumed via
# add_subdirectory below. The llama.cpp FetchContent (pinned tag b6099), the
# CUDA/Vulkan backend selection, and the R18 third-party warning isolation all
# moved into the engine repo's CMakeLists so there is exactly one llama build,
# owned by the engine. These two options stay declared here as cache globals
# and are honored by the engine subdirectory.
option(ENABLE_LLAMA_FETCH "Fetch and configure llama.cpp native library (owned by the engine repo)" ON)
option(EMEBALA_VULKAN "Enable Vulkan GPU backend when Vulkan SDK is present (owned by the engine repo)" ON)

# The shared Emebala engine repository. Emebala Chat, Listener and Reader
# consume the same engine repo; override with -DEMEBALA_ENGINE_DIR=<path>.
set(EMEBALA_ENGINE_DIR "D:/OneDrive/Projects/Emebala_engine" CACHE PATH
    "Path to the shared Emebala engine repository (the v0.10.7 engine split, 261111_0001 U7)")
if (NOT EXISTS "${EMEBALA_ENGINE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "The shared Emebala engine repository was not found at '${EMEBALA_ENGINE_DIR}'. "
        "Clone it there, or reconfigure with -DEMEBALA_ENGINE_DIR=<path> "
        "(261111_0001 U7, v0.10.7: the engine sources moved out of this repo).")
endif()

# Core static library for Emebalachat modules
add_library(Emebalachat_core STATIC
    src/diag_logger.cpp
    src/config.cpp
    # P5-F1 (session 260909_0004): delay-load failure hook + loader probe that
    # keeps the ggml Vulkan backend out of the registry on driverless machines
    # (CPU fallback instead of SEH 0xC06D007E). Also defines __pfnDli*Hook2.
    src/vulkan_guard.cpp
    src/bidi_utils.cpp
    src/unicode_utils.cpp
    src/smart_bypass.cpp
    src/sound.cpp
    src/win32_input.cpp
    src/google_translate.cpp
    src/engine.cpp
    # REQ-045 P4-3 (design §3b, item 3b): OpenAI Compatible cloud engine —
    # WinHTTP client (google_translate pattern) + DPAPI key protect/unprotect
    # + CNG SHA-256 integrity digest + display masking. Chat exe links this
    # (core), so bcrypt/crypt32 are added to the core link set below.
    src/openai_compatible_client.cpp
    # 261111_0001 U6 (v0.10.7): registration-time GGUF header gate (llama-free).
    src/gguf_validator.cpp
    # REQ-MD (260930_0004, decisions.md D9): on-demand bundled-model download —
    # consent/progress dialog + the fetch/verify/place pipeline (reuses the
    # update checker's DownloadUpdate via the DownloadOptions seam).
    src/model_downloader.cpp
    src/ui/model_download_dialog.cpp
    # REQ-043 (M6 T1, design §4.2/D-1): translation_common.cpp and the
    # llama-adjacent helpers moved to the engine repo's Emebalachat_engine_core
    # (261111_0001 U7); TruncateHeadTailWindow and the TranslationManager
    # routing stay here (conservative boundary).
    # 261111_0001 U7 (v0.10.7): the engine_host_* client + deployment-data
    # parsers (client, registry, manifest, components, bootstrap client,
    # config reader, client policy) moved to the engine repo's
    # Emebala_engine_host static lib; Emebalachat_core links it PUBLIC below,
    # so every core consumer resolves the moved headers/symbols unchanged.
    src/i18n.cpp
    # REQ-UC (session 260930_0004): update checker — the module is header-only
    # (every function `inline` in update_checker.hpp); this TU compiles the
    # header standalone as a syntax/ODR check and adds nothing else.
    src/update_checker.cpp
    src/ui/about_window.cpp
    src/ui/gguf_model_manager_window.cpp # REQ-048 R2-D: registered-model rename/delete manager
    src/ui/openai_settings_window.cpp
    # REQ-060 (session 261009_0001): translation-model load-mode settings
    # dialog (engine_host.warmup) — the openai_settings_window pattern
    # (TemplateBuilder + CBS_DROPDOWNLIST combo + OK/Cancel).
    src/ui/engine_host_settings_window.cpp
    src/ui/asset_loader.cpp
    src/ui/badge.cpp
    src/ui/dpi.cpp
    src/ui/layered_renderer.cpp
    src/ui/tray.cpp
    src/ui/drag_icon.cpp
    src/ui/tooltip.cpp
    src/mouse_hook.cpp
    src/worker.cpp
    src/hook.cpp
)

target_include_directories(Emebalachat_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if (MSVC)
    target_compile_options(Emebalachat_core PRIVATE /W4 /utf-8 /permissive-)
endif()

# REQ-006 (plan §2.4): single source of truth for the runtime version string.
# src/version.hpp stringizes this token (EMBALA_VERSION_STR two-level macro),
# so the value MUST be passed UNQUOTED: quoting it here would double-stringize
# into "\"0.10.2\"" at preprocessing. PUBLIC: every consumer of the core lib
# (Emebalachat exe, run_tests) sees the same value.
target_compile_definitions(Emebalachat_core PUBLIC EMEBALACHAT_VERSION_STR=${PROJECT_VERSION})

# REQ-005 (M6 T6, design §9 R-3b ADOPTED): the repair-manifest URL the app-side
# bootstrapper (engine_host_bootstrap_client) silently fetches missing engine
# components from. A COMPILE-TIME CONSTANT, deliberately NOT a config.json key:
# an empty value means the repair path is COMPLETELY DISABLED (the bootstrapper
# reports "repair unavailable" and the caller converges on the user-facing
# guidance UX), and a non-empty value MUST be an https:// URL (the bootstrapper
# rejects anything else). Provide it at configure time, e.g.
#   cmake -B build -DEMEBALA_REPAIR_URL=https://downloads.example.com/emebala/repair
set(EMEBALA_REPAIR_URL "" CACHE STRING
    "REQ-005/M6 T6: engine-host repair-manifest URL (empty = repair disabled, https only)")
if(EMEBALA_REPAIR_URL STREQUAL "")
    # Empty -> stringize the value macro to "" (the bootstrapper reads that as
    # "repair disabled"). EMEBALA_REPAIR_URL is a plain object-like alias of the
    # value macro in BOTH branches so CompiledRepairUrl() compiles identically
    # whether or not a URL was provided.
    target_compile_definitions(Emebalachat_core PUBLIC
        EMEBALA_REPAIR_URL_VALUE=\"\"
        EMEBALA_REPAIR_URL=EMEBALA_REPAIR_URL_VALUE)
else()
    target_compile_definitions(Emebalachat_core PUBLIC
        EMEBALA_REPAIR_URL_VALUE=\"${EMEBALA_REPAIR_URL}\"
        EMEBALA_REPAIR_URL=EMEBALA_REPAIR_URL_VALUE)
endif()

# Win32 system dependencies
target_link_libraries(Emebalachat_core PUBLIC
    user32
    shell32
    kernel32
    imm32
    winmm
    winhttp
    wtsapi32
    # REQ-045 P4-5 (item 3a-2): GetOpenFileNameW (.gguf picker, main.cpp).
    comdlg32
    # REQ-045 P4-3 (design §3b): bcrypt = CNG SHA-256 (key integrity digest,
    # openai_compatible_client); crypt32 = DPAPI CryptProtectData key storage.
    bcrypt
    crypt32
    # REQ-050: the gguf-manager Hugging Face add dialog creates a
    # msctls_progress32 progress bar at runtime (InitCommonControlsEx).
    comctl32
    d2d1
    dwrite
    dxgi
    normaliz
    delayimp
    ole32
    oleaut32
    windowscodecs
)

# 261111_0001 U7 (v0.10.7): consume the shared engine repository. Added AFTER
# Emebalachat_core is defined: the engine targets link it for the shared
# support headers/symbols (diag_logger, unicode_utils, config, engine.hpp,
# smart_bypass, model_verify_sample, vulkan_guard) that deliberately stay in
# this repo. The engine CMakeLists skips its own llama FetchContent when the
# parent already provides the `llama` target; here the engine owns the fetch,
# so exactly one llama build exists per build tree.
add_subdirectory(${EMEBALA_ENGINE_DIR} emebala_engine_build)

# The engine_host_* client/parsers moved to the engine repo (Emebala_engine_host
# static lib; PUBLIC include dir = the engine src/, so `#include
# "engine_host_client.hpp"` and the engine_core/ headers keep resolving for
# every core consumer). PUBLIC (not PRIVATE) so the include dir reaches the
# Emebalachat exe and run_tests through Emebalachat_core. Deliberately NOT
# Emebalachat_engine_core: that lib links llama PUBLIC — linking it here would
# put llama.h on the Chat exe's include path and flip translation_common.hpp's
# __has_include("llama.h") contract, breaking the llama-free Chat exe (REQ-043
# M6 T5). The engine src/ dir carries the headers WITHOUT llama.h; the llama
# symbols reach only run_tests / the engine exes through the engine targets.
target_link_libraries(Emebalachat_core PUBLIC Emebala_engine_host)

# REQ-043 (M6 T5, plan §V2-8.6, design §4.2): the T1 transitional llama link
# on Emebalachat_core is REMOVED — the embedded inference path (the
# TranslationManager::LlamaEngine shim, the embedded call block, the preload
# seams) is deleted, so the core library (and therefore the Chat exe) carries
# NO llama symbol and HAVE_LLAMA_CPP is undefined in its TUs. llama links ONLY
# through the engine repo's Emebalachat_engine_core (host / worker /
# test-runner targets).

# 260930_0004 (0.10.2 blind-QA catch): the .rc VERSIONINFO blocks used to
# hardcode 0,10,1 / "0.10.1", so Explorer's Properties showed 0.10.1 while the
# in-app string (EMEBALACHAT_VERSION_STR) showed 0.10.2. Both RC compilations
# are now macro-driven from PROJECT_VERSION, keeping a version bump to the
# canonical 3 places (project() here, setup.iss AppVersion, README).
# PROJECT_VERSION_{MAJOR,MINOR,PATCH} are set automatically by project().
# rc.exe's preprocessor has NO stringize operator, so the string form is passed
# pre-quoted; the numeric parts feed FILEVERSION/PRODUCTVERSION directly.
set(EMEBALA_RC_VER_OPTIONS
    "/dEMEBALACHAT_RC_VER_MAJOR=${PROJECT_VERSION_MAJOR}"
    "/dEMEBALACHAT_RC_VER_MINOR=${PROJECT_VERSION_MINOR}"
    "/dEMEBALACHAT_RC_VER_PATCH=${PROJECT_VERSION_PATCH}"
    "/dEMEBALACHAT_RC_VER_STR=\"${PROJECT_VERSION}\""
)
# 261111_0001 U7: engine_icon.rc moved to the engine repo (the engine CMake
# drives its VERSIONINFO from the engine repo's own PROJECT_VERSION).
set_source_files_properties(src/app_icon.rc
    PROPERTIES COMPILE_OPTIONS "${EMEBALA_RC_VER_OPTIONS}")

# Target 1: Emebalachat (WIN32 executable)
# REQ-R2 (session 260905_0001): the internal target name stays "Emebalachat"
# (minimal-risk: no downstream target references change), but the produced
# binary is renamed via OUTPUT_NAME to the user-facing "Emebala_chat.exe".
# src/app_icon.rc embeds the branded multi-size app icon (see that file).
add_executable(Emebalachat WIN32
    src/main.cpp
    src/app_icon.rc
)

set_target_properties(Emebalachat PROPERTIES OUTPUT_NAME "Emebala_chat")

target_link_libraries(Emebalachat PRIVATE
    # REQ-043 (M6 T5): the T1 transitional Emebalachat_engine_core link is
    # REMOVED — the Chat exe is now llama-free (no llama/cublas/cudart symbol,
    # design §4.2). Only the shared-host client seam (in Emebalachat_core)
    # talks to the local engine, over a named pipe.
    Emebalachat_core
)

if (MSVC)
    target_compile_options(Emebalachat PRIVATE /W4 /utf-8 /permissive-)
    if (TARGET llama)
        # REQ-043 (M6 T5): the Chat exe no longer links llama, so it imports NO
        # CUDA/Vulkan symbols — the linker IGNORES every /DELAYLOAD below (the
        # design §4.2 note: "delayimp 항목 자체는 무해하나 /DELAYLOAD 대상이
        # exe에 없으면 링커가 무시"). The block is retained with a minimal diff;
        # vulkan_guard.cpp (Emebalachat_core) needs no delay-load entry — it
        # probes vulkan-1.dll via LoadLibrary at startup.
        #
        # L3 (security, LOW): CUDA DLLs are delay-loaded, so they resolve through
        # the standard search order at first use. Hardening applied: main.cpp calls
        # SetDllDirectoryW(L"") at startup - the EMPTY STRING removes the current
        # directory from the search order, closing the CWD hijack vector, while
        # keeping application-dir + PATH resolution intact (the installer ships no
        # CUDA DLLs; GPU machines resolve them via PATH). NOTE: passing NULL would
        # do the opposite (restore the default order incl. CWD) and was the original
        # silent no-op bug (audit Blocker 1, CWE-427). SetDefaultDllDirectories
        # is intentionally NOT used: it would drop PATH from the search and break
        # the CUDA load chain on toolkit-installed machines (CPU fallback would
        # silently replace GPU inference). Revisit if the installer ever bundles
        # the CUDA DLLs into {app}, at which point PATH restriction becomes safe.
        #
        # P4-B1 (P3 risk R5, REQ-104): vulkan-1.dll joins the delay-load list ONLY
        # when ggml-vulkan was actually built (target exists). Without delay-loading
        # it, a machine with no Vulkan driver would fail at process start (loader
        # cannot resolve the static import) instead of CPU-falling back. Guarded so
        # CUDA-only builds never reference a phantom delay-load.
        set(_eme_delayload
            "/DELAYLOAD:cublas64_13.dll"
            "/DELAYLOAD:cublasLt64_13.dll"
            "/DELAYLOAD:cudart64_13.dll")
        if (TARGET ggml-vulkan)
            list(APPEND _eme_delayload "/DELAYLOAD:vulkan-1.dll")
        endif()
        target_link_options(Emebalachat PRIVATE ${_eme_delayload})
    endif()
endif()

# Target 2: Native Unit Test Runner
add_executable(run_tests
    tests/run_tests.cpp
)

target_link_libraries(run_tests PRIVATE
    Emebalachat_core
    # REQ-043 (M6 T1, M-1 gate directive): the test runner compiles
    # HAVE_LLAMA_CPP-guarded tests (TestP7F2GpuOffloadParams etc.) and calls
    # the moved helpers (VerifyModelSha256, IsValidModelPath, Scrub/
    # CollectControlTokenTexts, TruncateHeadTailWindow) — it now links BOTH
    # libraries. Compile + link + green run of this target is the T1
    # completion gate (Total Checks 3,012 / Failures 0, baseline-no-regression).
    Emebalachat_engine_core
    # REQ-043 (M6 T3): TestWorkerManagerStateMachine drives the seam-injected
    # WorkerManager state machine — link its library.
    Emebalachat_host_v2
)

if (MSVC)
    target_compile_options(run_tests PRIVATE /W4 /utf-8 /permissive-)
    if (TARGET llama)
        # P4-B1: same guarded delay-load wiring as the Emebalachat target above.
        set(_eme_delayload_tests
            "/DELAYLOAD:cublas64_13.dll"
            "/DELAYLOAD:cublasLt64_13.dll"
            "/DELAYLOAD:cudart64_13.dll")
        if (TARGET ggml-vulkan)
            list(APPEND _eme_delayload_tests "/DELAYLOAD:vulkan-1.dll")
        endif()
        target_link_options(run_tests PRIVATE ${_eme_delayload_tests})
    endif()
endif()

# 261111_0001 U7 (v0.10.7 engine split): the engine sources moved to the
# EMEBALA_ENGINE_DIR repo, so ResolveRepoFile() gains the engine repo as an
# additional lookup base (wide-string literal macro; backslashes normalized to
# forward slashes for macro safety). Engine-source pins keep their historical
# L"src\\..." paths; chat-side bases still win for the files that stayed.
string(REPLACE "\\" "/" _eme_engine_dir_norm "${EMEBALA_ENGINE_DIR}")
target_compile_definitions(run_tests PRIVATE
    "EMEBALA_ENGINE_REPO_DIR=L\"${_eme_engine_dir_norm}\"")

# Plan-B (REQ-B009) P6 closure C-2: the §12.2 crash-isolation pin runs in its
# OWN process (the run_tests process cannot create the SECOND concurrent
# named-pipe client — see the PLACEMENT NOTE at the top of
# translate_pool_test.inc). A tiny standalone main hosts the SAME pin +
# handshake; launched via CTest below. Links only host_v2 (the WorkerManager +
# the pool machinery the pin drives).
add_executable(crash_iso_test tests/crash_iso_test_main.cpp)
# host_v2 pulls in diag::Printf / diag::MirrorF (the ENGINEHOST/wmgr/* DIAG
# lines), which live in diag_logger.cpp — compile it in + link host_v2 and
# engine_core, same libraries run_tests uses.
target_sources(crash_iso_test PRIVATE src/diag_logger.cpp src/unicode_utils.cpp)
target_link_libraries(crash_iso_test PRIVATE
    Emebalachat_host_v2
    Emebalachat_engine_core
)
if (MSVC)
    target_compile_options(crash_iso_test PRIVATE /W4 /utf-8 /permissive-)
endif()


# 261111_0001 U7 (v0.10.7): Targets 3/4 (EmebalaEngine -> Emebala.Engine.exe and
# EmebalaEngineGgmlTranslate -> Emebala.Engine.ggml-translate.exe + the
# worker.ggml-translate.manifest generation) moved to the shared engine repo
# (added above) with identical target/output names. CMAKE_RUNTIME_OUTPUT_DIRECTORY
# in the engine repo keeps both exes and the manifest landing in THIS build's
# root (build\Emebala.Engine.exe etc.), where the installer bundles them.

enable_testing()
add_test(NAME CoreTests COMMAND run_tests)
# Plan-B (REQ-B003) B-T1: translate-pool suite — a standalone ctest entry so
# `ctest -R translate_pool --output-on-failure` selects it (the aggregate
# CoreTests entry cannot ctest -R a sub-suite; technical gate item-7 note).
add_test(NAME translate_pool COMMAND run_tests "[translate-pool]")
# Plan-B (REQ-B004) B-T2: VRAM-gate suite — standalone ctest entry so
# `ctest -R vram_gate --output-on-failure` selects it (lowercase, matching the
# case-sensitive -R; same pattern + caveat as translate_pool above).
add_test(NAME vram_gate COMMAND run_tests "[vram-gate]")
# Plan-B (REQ-B006) B-T8: installer model-bundling pin — a standalone ctest entry
# so `ctest -R installer --output-on-failure` selects it (same pattern + caveat
# as translate_pool/vram_gate above: the pin body lives in the CoreTests
# aggregate run_tests and is tagged in its [RUN] banner).
add_test(NAME installer COMMAND run_tests "[installer]")
# Plan-B (REQ-B009) P6 closure C-2: §12.2 crash-isolation pin — a standalone
# process (see the target comment above); ctest -R crash_isolation runs just
# this pin.
add_test(NAME crash_isolation COMMAND crash_iso_test)
