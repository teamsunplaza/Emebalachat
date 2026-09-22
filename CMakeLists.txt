cmake_minimum_required(VERSION 3.24)

# REQ-046 P4-4: version kept at 0.10.1 per user decision (2026-09-19 21:45,
# docs/260919_0003_session_req046-reinstall-regression-fixes/decisions.md) —
# the version bump in commit 3b3a426 is withdrawn; this rerelease ships as
# 0.10.1.
project(Emebalachat VERSION 0.10.1 LANGUAGES C CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# MSVC Static CRT (/MT in Release, /MTd in Debug)
if (MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:/Zc:char8_t->")
endif()

# Option to fetch llama.cpp
option(ENABLE_LLAMA_FETCH "Fetch and configure llama.cpp native library" ON)

if (ENABLE_LLAMA_FETCH)
    include(FetchContent)

    set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared libraries" FORCE)
    set(GGML_STATIC ON CACHE BOOL "Build static ggml library" FORCE)
    set(LLAMA_STATIC ON CACHE BOOL "Build static llama library" FORCE)
    set(GGML_CUDA ON CACHE BOOL "Enable CUDA for llama.cpp" FORCE)
    # P4-B1 (universal GPU, REQ-102/107): multi-arch SASS — sm_75 retained (RTX 20),
    # 86 (RTX 30), 89 (RTX 40), 120 (RTX 50/Blackwell) added per decisions.md.
    set(CMAKE_CUDA_ARCHITECTURES "75;86;89;120" CACHE STRING "Target CUDA architectures" FORCE)
    set(CMAKE_CUDA_FLAGS "${CMAKE_CUDA_FLAGS} -DCCCL_IGNORE_MSVC_TRADITIONAL_PREPROCESSOR_WARNING -Xcompiler=/Zc:preprocessor" CACHE STRING "CUDA flags" FORCE)
    set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "Build examples" FORCE)
    set(LLAMA_BUILD_TESTS OFF CACHE BOOL "Build tests" FORCE)
    set(LLAMA_BUILD_SERVER OFF CACHE BOOL "Build server" FORCE)

    # P4-B1 (universal GPU, REQ-101/103): Vulkan SDK auto-detect. llama.cpp b6099
    # ggml-vulkan/CMakeLists.txt L4 hard-REQUIRES glslc (build-time shader codegen),
    # so the probe requests that component explicitly. SDK+glslc present -> enable
    # GGML_VULKAN on the FetchContent'd tree; absent -> CUDA-only build with a
    # WARNING, never a hard-fail (user decision 2026-09-09 14:03, decisions.md).
    option(EMEBALA_VULKAN "Enable Vulkan GPU backend when Vulkan SDK is present" ON)

    set(_eme_vulkan_ready OFF)
    if (EMEBALA_VULKAN)
        find_package(Vulkan COMPONENTS glslc QUIET)
        if (Vulkan_FOUND AND Vulkan_GLSLC_EXECUTABLE)
            set(_eme_vulkan_ready ON)
            message(STATUS "Emebalachat: Vulkan SDK ${Vulkan_VERSION} found (glslc=${Vulkan_GLSLC_EXECUTABLE}) - building ggml-vulkan backend")
        else()
            message(WARNING "Emebalachat: Vulkan SDK with glslc NOT found - building CUDA-only. Install LunarG Vulkan SDK to enable AMD/Intel/old-NVIDIA GPU support.")
        endif()
    endif()

    # Set the ggml option as a FORCED cache var BEFORE FetchContent_MakeAvailable so
    # ggml/src/CMakeLists.txt's ggml_add_backend(Vulkan) gate picks it up (its
    # option() call will not override an existing cache entry).
    if (_eme_vulkan_ready)
        set(GGML_VULKAN ON CACHE BOOL "Enable Vulkan for llama.cpp" FORCE)
    else()
        # Deterministic CUDA-only: an earlier configure with the SDK present may have
        # FORCE'd GGML_VULKAN=ON into this build dir's cache; forcing OFF here keeps
        # the EMEBALA_VULKAN toggle authoritative on reconfigures (fresh caches are
        # unaffected — ggml's own option() default is already OFF).
        set(GGML_VULKAN OFF CACHE BOOL "Enable Vulkan for llama.cpp" FORCE)
    endif()

    message(STATUS "Fetching llama.cpp (tag b6099)...")
    FetchContent_Declare(
        llama_cpp
        GIT_REPOSITORY https://github.com/ggerganov/llama.cpp.git
        GIT_TAG b6099
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(llama_cpp)

    # R18 (Zero-Tolerance) — third-party warning isolation for llama.cpp.
    # On full recompiles the fetched llama.cpp (b6099) surfaces third-party
    # advisories only: nvcc cudafe "#177-D" (variable declared but never
    # referenced) from ggml-cuda sources/headers (e.g. fattn-mma-f16.cuh), and
    # MSVC narrowing advisories C4244/C4267 (observed via llama-graph.cpp
    # llama-graph.h template instantiation into <memory>/<type_traits>, and
    # llama-quant.cpp into <thread>/<vector>). None originate in project code,
    # so the standard isolation is applied, all PRIVATE (project TUs in src/
    # and tests/ keep /W4 untouched):
    #   (1) nvcc: --diag-suppress=177 scoped to the CUDA language only.
    #   (2) MSVC host compilation of the third-party targets: /W0 blanket
    #       suppression plus explicit /wd4244 /wd4267. The /wd form is
    #       required in addition to /W0 because these two narrowings are
    #       re-enabled by in-source #pragma warnings in the toolchain/STL
    #       headers, and per MSVC semantics a command-line /wd override beats
    #       in-source pragmas while /W0 does not (verified empirically with
    #       cl.exe 14.44 against the exact ninja command line). /wd4244 and
    #       /wd4267 also mirror upstream's own intent: llama.cpp disables
    #       exactly these two codes in src/llama.cpp for the same narrowing
    #       pattern.
    #   (3) their INTERFACE includes are re-marked SYSTEM so llama.cpp/ggml
    #       header warnings cannot surface through consumer compilation
    #       either.
    # No third-party sources are modified; no versions are bumped; project
    # warning flags are unchanged.
    # P4-B1: ggml-vulkan added to the isolated set (C++-only TU, MSVC /W0 applies;
    # the if(TARGET) guard keeps the loop safe on CUDA-only builds).
    foreach(_r18_tp_target IN ITEMS llama ggml ggml-base ggml-cpu ggml-cuda ggml-vulkan common)
        if (TARGET ${_r18_tp_target})
            get_target_property(_r18_tp_inc ${_r18_tp_target} INTERFACE_INCLUDE_DIRECTORIES)
            if (_r18_tp_inc)
                set_target_properties(${_r18_tp_target} PROPERTIES
                    INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_r18_tp_inc}")
            endif()
            target_compile_options(${_r18_tp_target} PRIVATE
                $<$<COMPILE_LANGUAGE:CUDA>:--diag-suppress=177>
                $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:MSVC>>:/W0 /wd4244 /wd4267>
                $<$<AND:$<COMPILE_LANGUAGE:C>,$<CXX_COMPILER_ID:MSVC>>:/W0 /wd4244 /wd4267>)
        endif()
    endforeach()
endif()

# REQ-043 (M6 T1, design §4.2/D-1): Emebalachat_engine_core — the llama-coupled
# inference library, physically separated from Emebalachat_core so the Chat exe
# can drop llama entirely (T5 removes the embedded path; llama then links ONLY
# through this target). Contents are physical moves with ZERO logic changes:
#   * translation_common.cpp/.hpp (moved from src/) — LocalInferenceEngine
#     (plan §5.1-1 verbatim extraction, now one library for app/host/worker).
#   * engine_core_helpers.cpp/.hpp (moved verbatim from engine.cpp/.hpp) —
#     IsValidModelPath (M3), ComputeFileSha256/VerifyModelSha256 + F3 marker
#     cache, kExpectedModelSha256/kPinnedModelFilename pins, kLlama* budget
#     constants (REQ-R01), Scrub/CollectControlTokenTexts (SEC-B2),
#     SetGpuOffloadParams (P7-F2).
# Conservative boundary (design §4.2 + P2 §3 (2) keep-list): TruncateHeadTail
# Window, BuildPrompt and LocalPairReliable STAY in Emebalachat_core (engine.cpp
# / config.cpp) — google_translate.cpp (a permanent core member, the Chat exe
# cloud path) and translation_common.hpp's own header contract pin them there.
add_library(Emebalachat_engine_core STATIC
    src/engine_core/translation_common.cpp
    src/engine_core/engine_core_helpers.cpp
)

target_include_directories(Emebalachat_engine_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/engine_core
)

if (MSVC)
    target_compile_options(Emebalachat_engine_core PRIVATE /W4 /utf-8 /permissive-)
endif()

# REQ-043 (M6 T1): same PUBLIC version definition contract as the core lib
# (src/version.hpp stringizes it; UNQUOTED, per the core comment below).
target_compile_definitions(Emebalachat_engine_core PUBLIC EMEBALACHAT_VERSION_STR=${PROJECT_VERSION})

# REQ-043 (M6 T1; M6 T5): llama links PUBLIC here so every engine_core
# consumer (host, worker, tests) resolves HAVE_LLAMA_CPP and the llama
# symbols identically to the pre-T1 build. The Chat exe is NOT a consumer
# anymore (T5 removed the embedded engine and its transitional link).
# Baseline-no-regression rule: no TU loses or gains a compile definition.
if (TARGET llama)
    target_link_libraries(Emebalachat_engine_core PUBLIC llama)
    target_compile_definitions(Emebalachat_engine_core PUBLIC HAVE_LLAMA_CPP=1)
endif()

# REQ-043 (M6 T3, design §1.3/§4.2): the orchestrator-side worker lifecycle
# manager (spawn/handshake/reaper/M-2 graceful stop) — a member of the
# EmebalaEngine target, NOT the Chat exe (design §1.3 table: host_v2_* files
# live in EmebalaEngine). It depends only on worker_protocol.hpp (header-only,
# pure) + diag_logger + Win32, so it compiles identically in no-llama builds.
add_library(Emebalachat_host_v2 STATIC
    src/host_v2_worker_manager.cpp
    # REQ-043 (M6 T4): the orchestrator low layers — session table (§V2-4.3),
    # scheduler queue (§V2-4.6), health counters (REQ-008). All depend only on
    # worker_protocol/engine_host_protocol pure helpers + Win32-free STL, so
    # they compile identically in no-llama builds.
    src/host_v2_session.cpp
    src/host_v2_scheduler.cpp
    src/host_v2_health.cpp
)

target_include_directories(Emebalachat_host_v2 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if (MSVC)
    target_compile_options(Emebalachat_host_v2 PRIVATE /W4 /utf-8 /permissive-)
endif()

target_compile_definitions(Emebalachat_host_v2 PUBLIC EMEBALACHAT_VERSION_STR=${PROJECT_VERSION})

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
    # REQ-043 (M6 T1, design §4.2/D-1): translation_common.cpp moved out to
    # Emebalachat_engine_core (physical move). The llama-adjacent pure helpers
    # moved to src/engine_core/engine_core_helpers.cpp; TruncateHeadTailWindow
    # and the TranslationManager routing stay here (conservative boundary).
    # REQ-043 (plan §5.1-3): self-contained shared-inference-host reference
    # client (Win32 + STL only — never include other project headers here;
    # the other Emebala workspaces copy this pair verbatim).
    src/engine_host_client.cpp
    # REQ-043 (M6 T2, design §1.3): deployment-data parsers — registry.json
    # (§3.1), manifest.json + IManifestVerifier (§3.2), components.json (§3.3)
    # — live in Emebalachat_core. They reuse the FROZEN protocol header's JSON
    # primitives (engine_host_protocol.hpp, unmodified) and NEVER reference
    # Emebalachat_engine_core (T1 boundary: no core->engine_core reverse link;
    # the manifest hash check is caller-injected via HashProvider instead).
    src/engine_host_registry.cpp
    src/engine_host_manifest.cpp
    src/engine_host_components.cpp
    src/engine_host_bootstrap_client.cpp
    # REQ-046 P4-2 (Rev2 §B-4, Tech Gate 조건-1): the host's boot-time
    # config.json reader (C1 engine_type=="user_gguf" gate). Lives in core
    # (not host_main.cpp) so run_tests.exe can link + test it.
    src/engine_host_config_reader.cpp
    src/i18n.cpp
    src/ui/about_window.cpp
    src/ui/gguf_model_manager_window.cpp # REQ-048 R2-D: registered-model rename/delete manager
    src/ui/openai_settings_window.cpp
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
# into "\"0.10.1\"" at preprocessing. PUBLIC: every consumer of the core lib
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

# REQ-043 (M6 T5, plan §V2-8.6, design §4.2): the T1 transitional llama link
# on Emebalachat_core is REMOVED — the embedded inference path (the
# TranslationManager::LlamaEngine shim, the embedded call block, the preload
# seams) is deleted, so the core library (and therefore the Chat exe) carries
# NO llama symbol and HAVE_LLAMA_CPP is undefined in its TUs. llama links ONLY
# through Emebalachat_engine_core (host / worker / test-runner targets).

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

# Target 3: EmebalaEngine — REQ-043 shared inference host (plan §5.1).
# WIN32 GUI subsystem (no console), produced binary Emebala.Engine.exe, the
# fixed install location is %LOCALAPPDATA%\Emebala\Common\engine\ (plan §7.1 —
# the installer worker owns the packaging). Links Emebalachat_core so the
# translation core, SHA pin, diag logger and Vulkan guard are shared, not
# duplicated; CUDA (+Vulkan when built) are delay-loaded exactly like the app.
add_executable(EmebalaEngine WIN32
    src/host_main.cpp
)

set_target_properties(EmebalaEngine PROPERTIES OUTPUT_NAME "Emebala.Engine")

target_link_libraries(EmebalaEngine PRIVATE
    Emebalachat_core
    # REQ-043 (M6 T1): the host owns LocalInferenceEngine (translation_common,
    # now in engine_core) and the moved SHA-pin/marker-cache helpers, so it
    # links Emebalachat_engine_core (design §4.2: host = core + engine_core).
    Emebalachat_engine_core
    # REQ-043 (M6 T3, design §1.3): the worker lifecycle manager joins the
    # orchestrator (T4 wires EnsureSpawned into the scheduler; T3 ships the
    # module compiled + unit-tested standalone).
    Emebalachat_host_v2
)

if (MSVC)
    target_compile_options(EmebalaEngine PRIVATE /W4 /utf-8 /permissive-)
    if (TARGET llama)
        # REQ-043: same delay-load wiring as the Emebalachat target (CUDA via
        # PATH on toolkit/driver machines, vulkan-1.dll only when built).
        set(_eme_delayload_host
            "/DELAYLOAD:cublas64_13.dll"
            "/DELAYLOAD:cublasLt64_13.dll"
            "/DELAYLOAD:cudart64_13.dll")
        if (TARGET ggml-vulkan)
            list(APPEND _eme_delayload_host "/DELAYLOAD:vulkan-1.dll")
        endif()
        target_link_options(EmebalaEngine PRIVATE ${_eme_delayload_host})
    endif()
endif()

# Target 4: Emebalachat.Engine.ggml-translate — REQ-043 (M6 T3, design §1.2/
# §4.2, plan §V2-3): the ggml-translate WORKER process (the second frozen
# contract). WIN32 GUI subsystem (no console), owned by the orchestrator via
# host_v2_worker_manager (spawn --pipe/--token, announce handshake, shutdown).
#
# no-llama decision (design §4.2 no-llama rule, recorded for the T3 report):
# the worker's ONLY body is LocalInferenceEngine (llama), so with
# ENABLE_LLAMA_FETCH=OFF there is nothing to serve — the target is SKIPPED
# entirely (same rule that skips the llama link on Emebalachat_core). The
# installer (T7) MUST then also omit the worker exe + worker manifest [Files]
# entries; the orchestrator's missing-exe path (wmgr/003) answers
# unavailable, which is the correct degraded behavior for a cloud-only build.
if (TARGET llama)
    add_executable(EmebalaEngineGgmlTranslate WIN32
        src/ggml_translate_worker.cpp
    )

    set_target_properties(EmebalaEngineGgmlTranslate PROPERTIES
        OUTPUT_NAME "Emebalachat.Engine.ggml-translate")

    target_link_libraries(EmebalaEngineGgmlTranslate PRIVATE
        # The worker's low body: LocalInferenceEngine + SHA pin + marker cache
        # (llama PUBLIC transitively — design §4.2 "llama PUBLIC 전이").
        Emebalachat_engine_core
        # Prompt parity (BuildPrompt/NormalizeLanguageCode) + unicode + diag +
        # vulkan guard come from Emebalachat_core (design §4.2: "프롬프트
        # 패리티용 config/unicode 헬퍼" — engine_core deliberately does NOT
        # own them; the T1 boundary comment in engine_core_helpers.hpp).
        Emebalachat_core
    )

    if (MSVC)
        target_compile_options(EmebalaEngineGgmlTranslate PRIVATE /W4 /utf-8 /permissive-)
        # REQ-043: identical delay-load wiring as the host/app targets (CUDA
        # via PATH on toolkit machines, vulkan-1.dll only when built — the
        # driverless machine CPU-falls back via vulkan_guard + the EnsureLoaded
        # retry, never a startup failure).
        set(_eme_delayload_worker
            "/DELAYLOAD:cublas64_13.dll"
            "/DELAYLOAD:cublasLt64_13.dll"
            "/DELAYLOAD:cudart64_13.dll")
        if (TARGET ggml-vulkan)
            list(APPEND _eme_delayload_worker "/DELAYLOAD:vulkan-1.dll")
        endif()
        target_link_options(EmebalaEngineGgmlTranslate PRIVATE ${_eme_delayload_worker})
    endif()

    # REQ-043 (M6 T3, design §3.4 + T3 scope item 3): the worker manifest is
    # GENERATED next to the worker exe at build time (the exe-adjacent
    # deployment lookup). The INSTALLED copy is written by the installer (T7);
    # this file is the build-tree artifact the orchestrator finds when both
    # exes run from build/ (dev/smoke posture) and the byte-exact template the
    # installer deploys. Content = the §3.4 schema the worker_protocol parser
    # accepts (kept in sync with EmbeddedWorkerManifest()).
    # M7 A-1 (session 260922_0001): per-family filename scheme
    # worker.<family>.manifest so a second family (ggml-asr) deploying into
    # the same store can never overwrite this one (DEC-007: the hyphenated
    # family string is used verbatim). Schema bytes are UNCHANGED.
    set(_eme_worker_manifest "${CMAKE_CURRENT_BINARY_DIR}/worker.ggml-translate.manifest")
    file(WRITE "${_eme_worker_manifest}"
"{\n"
"  \"schema_version\": 1,\n"
"  \"family\": \"ggml-translate\",\n"
"  \"engine\": \"llama.cpp\",\n"
"  \"engine_version\": \"b6099\",\n"
"  \"ops\": [\"translate\"],\n"
"  \"abi_version\": 1,\n"
"  \"protocol_min\": 1,\n"
"  \"protocol_max\": 2\n"
"}\n")
    add_custom_command(TARGET EmebalaEngineGgmlTranslate POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_eme_worker_manifest}"
                "$<TARGET_FILE_DIR:EmebalaEngineGgmlTranslate>/worker.ggml-translate.manifest"
        COMMENT "Deploying worker.ggml-translate.manifest next to Emebalachat.Engine.ggml-translate.exe")
endif()

enable_testing()
add_test(NAME CoreTests COMMAND run_tests)
