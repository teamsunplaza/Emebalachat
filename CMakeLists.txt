cmake_minimum_required(VERSION 3.24)

project(Emebalachat VERSION 0.10.0 LANGUAGES C CXX RC)

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

# Core static library for Emebalachat modules
add_library(Emebalachat_core STATIC
    src/diag_logger.cpp
    src/config.cpp
    src/bidi_utils.cpp
    src/unicode_utils.cpp
    src/smart_bypass.cpp
    src/sound.cpp
    src/win32_input.cpp
    src/google_translate.cpp
    src/engine.cpp
    src/i18n.cpp
    src/ui/about_window.cpp
    src/ui/asset_loader.cpp
    src/ui/badge.cpp
    src/ui/dpi.cpp
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
# into "\"0.10.0\"" at preprocessing. PUBLIC: every consumer of the core lib
# (Emebalachat exe, run_tests) sees the same value.
target_compile_definitions(Emebalachat_core PUBLIC EMEBALACHAT_VERSION_STR=${PROJECT_VERSION})

# Win32 system dependencies
target_link_libraries(Emebalachat_core PUBLIC
    user32
    shell32
    kernel32
    imm32
    winmm
    winhttp
    wtsapi32
    d2d1
    dwrite
    dxgi
    normaliz
    delayimp
    ole32
    oleaut32
    windowscodecs
)

if (TARGET llama)
    target_link_libraries(Emebalachat_core PUBLIC llama)
    target_compile_definitions(Emebalachat_core PUBLIC HAVE_LLAMA_CPP=1)
endif()

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
    Emebalachat_core
)

if (MSVC)
    target_compile_options(Emebalachat PRIVATE /W4 /utf-8 /permissive-)
    if (TARGET llama)
        # L3 (security, LOW): CUDA DLLs are delay-loaded, so they resolve through
        # the standard search order at first use. Hardening applied: main.cpp calls
        # SetDllDirectoryW(nullptr) at startup, removing the CWD hijack vector while
        # keeping application-dir + PATH resolution intact (the installer ships no
        # CUDA DLLs; GPU machines resolve them via PATH). SetDefaultDllDirectories
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

enable_testing()
add_test(NAME CoreTests COMMAND run_tests)
