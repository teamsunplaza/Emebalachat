cmake_minimum_required(VERSION 3.24)

project(Emebalachat VERSION 0.10.0 LANGUAGES C CXX)

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
    set(CMAKE_CUDA_ARCHITECTURES "75" CACHE STRING "Target CUDA architecture" FORCE)
    set(CMAKE_CUDA_FLAGS "${CMAKE_CUDA_FLAGS} -DCCCL_IGNORE_MSVC_TRADITIONAL_PREPROCESSOR_WARNING -Xcompiler=/Zc:preprocessor" CACHE STRING "CUDA flags" FORCE)
    set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "Build examples" FORCE)
    set(LLAMA_BUILD_TESTS OFF CACHE BOOL "Build tests" FORCE)
    set(LLAMA_BUILD_SERVER OFF CACHE BOOL "Build server" FORCE)

    message(STATUS "Fetching llama.cpp (tag b6099)...")
    FetchContent_Declare(
        llama_cpp
        GIT_REPOSITORY https://github.com/ggerganov/llama.cpp.git
        GIT_TAG b6099
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(llama_cpp)
endif()

# Core static library for Emebalachat modules
add_library(Emebalachat_core STATIC
    src/config.cpp
    src/unicode_utils.cpp
    src/smart_bypass.cpp
    src/sound.cpp
    src/win32_input.cpp
    src/google_translate.cpp
    src/engine.cpp
    src/i18n.cpp
    src/ui/asset_loader.cpp
    src/ui/badge.cpp
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

# Win32 system dependencies
target_link_libraries(Emebalachat_core PUBLIC
    user32
    shell32
    kernel32
    imm32
    winmm
    winhttp
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
add_executable(Emebalachat WIN32
    src/main.cpp
)

target_link_libraries(Emebalachat PRIVATE
    Emebalachat_core
)

if (MSVC)
    target_compile_options(Emebalachat PRIVATE /W4 /utf-8 /permissive-)
    if (TARGET llama)
        target_link_options(Emebalachat PRIVATE
            "/DELAYLOAD:cublas64_13.dll"
            "/DELAYLOAD:cublasLt64_13.dll"
            "/DELAYLOAD:cudart64_13.dll"
        )
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
        target_link_options(run_tests PRIVATE
            "/DELAYLOAD:cublas64_13.dll"
            "/DELAYLOAD:cublasLt64_13.dll"
            "/DELAYLOAD:cudart64_13.dll"
        )
    endif()
endif()

enable_testing()
add_test(NAME CoreTests COMMAND run_tests)
