#pragma once

// ---------------------------------------------------------------------------
// vulkan_guard — P5-F1 (REQ-104 runtime gap): CPU fallback on driverless machines.
//
// PROBLEM (session 260909_0004 debug report, finding F1): the exe links
// /DELAYLOAD:vulkan-1.dll. Delay-loading moves the load past process start, but
// on a machine where vulkan-1.dll is unresolvable (no Vulkan driver/loader),
// the FIRST TOUCH of any imported entry point raises SEH
// STATUS_DELAY_LOAD_MODULE_NOT_FOUND (0xC06D007E) from the delay helper — not a
// C++ exception. ggml_backend_vk_reg()'s catch (ggml-vulkan.cpp:11273) cannot
// intercept it under /EHsc (proven empirically by r5_seh_probe.exe: exit code
// 0xc06d007e, catch(...) never ran). The first touch happens inside the ggml
// backend-registry ctor on the model-load thread:
//   engine.cpp:601 llama_model_load_from_file
//   -> llama.cpp:143 ggml_backend_reg_count()
//   -> ggml-backend-reg.cpp:177-178 register_backend(ggml_backend_vk_reg()) [ctor]
//   -> ggml-vulkan.cpp:11271 ggml_vk_instance_init()
//   -> ggml-vulkan.cpp:4033 vk::enumerateInstanceVersion()  [FIRST vulkan-1.dll import]
// The unhandled SEH kills the process instead of degrading to CPU — exactly the
// user's "만일 gpu가 없더라도 cpu로 작동할 수 있도록" scenario.
//
// WHY NOT AN ENV VAR (task item a, b6099 source research):
//   * ggml-backend-reg.cpp:167-198: compiled-in backends register behind
//     compile-time #ifdefs only; NO runtime skip / "GGML_VK_DISABLE"-style env
//     gate exists in b6099 for the Vulkan registration.
//   * Every GGML_VK_* env var lives inside ggml_vk_instance_init()/device
//     setup (e.g. GGML_VK_VISIBLE_DEVICES read at ggml-vulkan.cpp:4102) — i.e.
//     strictly AFTER the L4033/L4042 first imports, so any env is read too late.
//   * ggml_backend_load()/ggml_backend_score() env-skip paths
//     (ggml-backend-reg.cpp:232-247) apply only to dynamically loaded
//     out-of-tree backends, not to the linked-in Vulkan registration.
//
// MECHANISM (task item b, app-side, zero upstream/CMake edits):
//   1. A pre-probe at startup (before the warmup thread and any model load):
//      LoadLibraryW(L"vulkan-1.dll") under the same search order the delay
//      helper will use (main.cpp already applied SetDllDirectoryW(L"") —
//      the empty string removes the CWD from the search order; NULL would
//      restore it; SetDefaultDllDirectories is NOT called anywhere, and the
//      probe is deliberately plain LoadLibraryW, matching delayimp's default
//      search).
//      Success => FreeLibrary immediately, everything proceeds unchanged.
//   2. A delay-load FAILURE hook (__pfnDliFailureHook2, delayimp.h) armed for
//      the whole process lifetime. dliFailLoadLib fires ONLY after the OS
//      loader genuinely failed (the driverless case): we substitute the
//      host-exe module handle; every subsequent GetProcAddress on it fails
//      (the exe exports no vk* symbols) and re-enters the hook as
//      dliFailGetProc, where we return typed stub entry points. Each stub
//      yields VkResult -3 (VK_ERROR_INITIALIZATION_FAILED) — Vulkan-Hpp's
//      detail::resultCheck then throws an InitializationFailedError, a
//      subclass of vk::SystemError, which ggml_backend_vk_reg() ALREADY
//      catches (ggml-vulkan.cpp:11273-11276) and returns nullptr;
//      register_backend ignores nullptr (ggml-backend-reg.cpp:211-213). The
//      Vulkan backend never initializes; CUDA (where present) and CPU
//      register normally; the existing n_gpu_layers=99→0 retry in
//      EnsureLoaded then runs inference on CPU. No SEH anywhere on the path.
//
// The pure decision + stub mapping + failure hook are exposed for
// tests/run_tests.cpp (TestVulkanGuard). Emergency support kill-switch: env
// var GGML_VK_GUARD_DISABLE=1 makes the hook inert (restores pre-F1 behavior
// for triage only; REQ-104 then does NOT hold).
// ---------------------------------------------------------------------------

#include <windows.h>
#include <delayimp.h> // PDelayLoadInfo / dliNotify constants for the hook surface

namespace emebalachat {

// Outcome of the pre-load Vulkan guard probe (logging + test surface).
struct VulkanGuardResult {
    bool loader_resolved = false;           // vulkan-1.dll resolvable by the OS loader
    bool guard_disabled_by_env = false;     // GGML_VK_GUARD_DISABLE present (kill-switch)
    bool backend_expected_disabled = false; // decision: Vulkan must stay out of the registry
};

// Pure decision function (unit-pinned): the Vulkan backend must be neutralized
// exactly when the loader cannot resolve vulkan-1.dll and the guard is on.
constexpr bool VulkanBackendShouldBeDisabled(bool loader_resolved, bool guard_disabled) {
    return !loader_resolved && !guard_disabled;
}

// Runs the loader probe, logs the decision through diag, returns it. Idempotent
// and side-effect-light (LoadLibraryW + immediate FreeLibrary on success). MUST
// be called once at startup BEFORE the warmup thread starts and before any
// llama_model_load_from_file (the registry ctor fires there). Never throws.
VulkanGuardResult EnsureVulkanGuard();

// True when the emergency kill-switch env var is set.
bool VulkanGuardDisabledByEnv();

// Delay-load failure hook installed as __pfnDliFailureHook2 (exposed for
// tests: run_tests synthesizes DelayLoadInfo and asserts the substitutions).
// extern "C" so the delayimp hook-pointer typedef binds without a language-
// linkage mismatch; the symbol still lives in namespace emebalachat.
extern "C" FARPROC WINAPI VulkanGuardFailureHook(unsigned dli_notify, PDelayLoadInfo pdli);

// Stub mapping used by the hook (exposed for tests): delay-imported
// vulkan-1.dll entry-point name -> stub FARPROC producing the VkResult-error /
// PFN-return shapes Vulkan-Hpp expects. Unknown/null names get the universal
// stub. Never returns nullptr.
FARPROC VulkanGuardStubForImport(const char* proc_name);

} // namespace emebalachat
