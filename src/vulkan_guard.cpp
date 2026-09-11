// vulkan_guard implementation — P5-F1. See vulkan_guard.hpp for the full
// crash-chain rationale and why b6099 offers no env-var disable.
#include "vulkan_guard.hpp"

#include "diag_logger.hpp"

#include <cstring>

// NOTE (linkage): the two __pfnDli*Hook2 definitions MUST have external C
// linkage exactly as delayimp.h declares them (global scope). If they lived in
// an anonymous namespace they would be different (mangled, internal) symbols
// and delayimp would silently run with no hooks — the guard would not fire.

namespace emebalachat {
namespace {

// Exact delay-import DLL name (CMakeLists: /DELAYLOAD:vulkan-1.dll; the delay
// helper hands us szDll from the import descriptor, ASCII).
constexpr char kVulkanDllA[] = "vulkan-1.dll";
constexpr wchar_t kVulkanDllW[] = L"vulkan-1.dll";

// VK_ERROR_INITIALIZATION_FAILED (vulkan_core.h). Vulkan-Hpp's
// detail::resultCheck throws an InitializationFailedError (subclass of
// vk::SystemError) for any non-success VkResult, which is precisely the type
// ggml_backend_vk_reg() catches (ggml-vulkan.cpp:11273) and maps to
// "backend not registered". Other error codes would work too; this value
// carries the clearest intent.
constexpr INT32 kVkErrorInitializationFailed = -3;

// Support/triage kill-switch: when set, the failure hook stays inert and a
// driverless machine reverts to the pre-F1 SEH behavior. Documented in the
// completion report; never set by the app itself.
constexpr char kGuardDisableEnv[] = "GGML_VK_GUARD_DISABLE";

// Universal x64 stub. The Microsoft x64 ABI leaves argument cleanup to the
// caller (first 4 args in registers, extra stack args caller-owned), so a
// fixed 4-slot prototype is stack-safe against ANY real Vulkan signature
// (exactly how delayimp's own generated thunks work). RAX = -3 sign-extends:
// callers reading EAX see 0xFFFFFFFD = (int)-3, callers reading RAX see
// (int64)-3 — the right shape for both "VkResult" and "PFN pointer" returns
// (a non-null, harmless function pointer for the latter).
INT64 WINAPI vk_stub_any(void*, void*, void*, void*) {
    return static_cast<INT64>(kVkErrorInitializationFailed);
}

// Typed stubs for the names ggml_vk_instance_init() touches first, per the
// built image's delay-import table (61 names total, tools_tmp_f1_probe7.py)
// and ggml-vulkan.cpp L4033/L4042/L4085 + the Vulkan-Hpp static-dispatch
// pattern (verified session 260909_0004 F1 research):

// vkEnumerateInstanceVersion(uint32_t* pApiVersion) -> VkResult.
// Zero the output so no caller can read an uninitialized apiVersion even if a
// future wrapper skipped resultCheck; then the VkResult error.
INT32 WINAPI vk_stub_enumerate_instance_version(UINT32* p_api_version) {
    if (p_api_version) {
        *p_api_version = 0;
    }
    return kVkErrorInitializationFailed;
}

// vkEnumerateInstanceExtensionProperties / vkEnumerateInstanceLayerProperties
// (const char* pLayerName, uint32_t* pCount, Vk*Properties* p) -> VkResult.
// pCount=0 mirrors "loader present, zero properties"; the -3 result still
// makes resultCheck throw, and any non-throwing consumer sees an empty list
// instead of an out-of-range count.
INT32 WINAPI vk_stub_enumerate_props(const char*, UINT32* p_count, void*) {
    if (p_count) {
        *p_count = 0;
    }
    return kVkErrorInitializationFailed;
}

// vkGetInstanceProcAddr(VkInstance, const char*) -> PFN_vkVoidFunction.
// Vulkan-Hpp's extension loaders resolve command pointers through this name;
// returning the universal stub keeps every resolved pointer non-null and
// error-producing (-3) rather than inviting a null-call AV at a later site.
void* WINAPI vk_stub_get_instance_proc_addr(void*, const char*) {
    return reinterpret_cast<void*>(&vk_stub_any);
}

// Exact (case-sensitive, like the PE import table) mapping of vulkan-1.dll
// import names to stubs. Unknown names fall back to the universal stub —
// every Vulkan entry point's first observable value shape is either a VkResult
// (fits RAX=-3) or a PFN pointer (non-null stub), so the fallback is total:
// no import name can escape into a null-pointer call or a success code.
FARPROC StubForName(const char* name) {
    if (name) {
        if (std::strcmp(name, "vkGetInstanceProcAddr") == 0) {
            return reinterpret_cast<FARPROC>(&vk_stub_get_instance_proc_addr);
        }
        if (std::strcmp(name, "vkEnumerateInstanceVersion") == 0) {
            return reinterpret_cast<FARPROC>(&vk_stub_enumerate_instance_version);
        }
        if (std::strcmp(name, "vkEnumerateInstanceExtensionProperties") == 0 ||
            std::strcmp(name, "vkEnumerateInstanceLayerProperties") == 0) {
            return reinterpret_cast<FARPROC>(&vk_stub_enumerate_props);
        }
    }
    return reinterpret_cast<FARPROC>(&vk_stub_any);
}

inline bool IsVulkanDll(const DelayLoadInfo* pdli) {
    return pdli && pdli->szDll && _stricmp(pdli->szDll, kVulkanDllA) == 0;
}

} // namespace

bool VulkanGuardDisabledByEnv() {
    return ::GetEnvironmentVariableA(kGuardDisableEnv, nullptr, 0) > 0;
}

// Public seam for tests: never null, deterministic per name.
FARPROC VulkanGuardStubForImport(const char* proc_name) {
    return StubForName(proc_name);
}

// Failure hook — the F1 enforcement seam, installed as __pfnDliFailureHook2
// below (extern "C" per the declaration in vulkan_guard.hpp).
// dliFailLoadLib fires ONLY after the OS loader genuinely failed to find
// vulkan-1.dll (the driverless case; on a machine with a driver this hook
// never runs for this DLL). Handing back the host-exe handle lets the helper
// bind its IAT against a module with zero vk* exports, so each name then
// arrives as dliFailGetProc and receives a stub. Net effect: ggml's first
// touches return VkResult=-3 -> resultCheck throws vk::SystemError ->
// ggml_backend_vk_reg() catches it (its own existing code, ggml-vulkan.cpp
// L11273) -> nullptr -> register_backend skips it (ggml-backend-reg.cpp
// L211-213) -> NO Vulkan devices, no 0xC06D007E anywhere, and EnsureLoaded's
// n_gpu_layers 99->0 retry path runs the model on CPU.
extern "C" FARPROC WINAPI VulkanGuardFailureHook(unsigned dliNotify, PDelayLoadInfo pdli) {
    if (!IsVulkanDll(pdli)) {
        return nullptr; // cublas etc.: preserve the pre-F1 (proven-safe) behavior
    }
    if (VulkanGuardDisabledByEnv()) {
        return nullptr; // kill-switch: pre-F1 SEH behavior for triage
    }
    switch (dliNotify) {
        case dliFailLoadLib: {
            // Handle of our own exe image, WITHOUT a refcount bump: it exports
            // no vk* symbols, so every helper GetProcAddress against it fails
            // and re-enters this hook as dliFailGetProc (where StubForName
            // answers) — turning the would-be SEH into ggml's caught path.
            HMODULE hself = nullptr;
            if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                      nullptr, &hself)) {
                return nullptr; // cannot substitute; keep pre-F1 behavior
            }
            DIAG_LOG("VULKAN", "guard/001: vulkan-1.dll unresolvable at first touch; "
                               "stub module bound so ggml_backend_vk_reg receives "
                               "vk::SystemError (REQ-104 CPU fallback)");
            return reinterpret_cast<FARPROC>(hself);
        }
        case dliFailGetProc: {
            const char* name = (pdli->dlp.fImportByName && pdli->dlp.szProcName)
                                   ? pdli->dlp.szProcName
                                   : nullptr; // ordinal imports -> universal stub
            return StubForName(name);
        }
        default:
            return nullptr;
    }
}

VulkanGuardResult EnsureVulkanGuard() {
    VulkanGuardResult res;
    // Item (b)/(d): default LoadLibraryW probe. main.cpp already applied
    // SetDllDirectoryW(L"") (empty string removes the CWD from the search
    // order; NULL would restore it) and SetDefaultDllDirectories is
    // NOT called anywhere (see CMakeLists hardening comment), so the search
    // order here (app dir -> system dirs -> PATH) is the same order the delay
    // helper uses for vulkan-1.dll — matching item (d) "do NOT restrict
    // search". delayimp itself calls LoadLibraryExW with no flags for bare
    // names, i.e. the identical standard search.
    HMODULE h = ::LoadLibraryW(kVulkanDllW);
    res.loader_resolved = (h != nullptr);
    if (h) {
        // Drop our reference immediately (task d: driver machines keep the
        // exact old behavior; the real first touch re-resolves independently).
        ::FreeLibrary(h);
    }
    res.guard_disabled_by_env = VulkanGuardDisabledByEnv();
    res.backend_expected_disabled =
        VulkanBackendShouldBeDisabled(res.loader_resolved, res.guard_disabled_by_env);

    if (res.guard_disabled_by_env) {
        DIAG_LOG("VULKAN", "guard/002: %s set - F1 stub guard DISABLED by environment; "
                           "driverless machines will hit the original delay-load SEH crash",
                 kGuardDisableEnv);
    } else if (res.loader_resolved) {
        DIAG_LOG("VULKAN", "guard/003: vulkan-1.dll resolved by the OS loader (probe ok); "
                           "Vulkan backend stays enabled; failure hook remains armed "
                           "for late-resolution failures as a safety net");
    } else {
        DIAG_LOG("VULKAN", "guard/004: vulkan-1.dll NOT resolvable on this machine; "
                           "stub fallback will keep the ggml Vulkan backend out of the "
                           "registry - inference runs on CPU (REQ-104)");
    }
    return res;
}

} // namespace emebalachat

// Notify hook: deliberately inert (returns nullptr = "helper, do your normal
// thing"). A healthy machine must bind the REAL vulkan-1.dll untouched
// (task item d); we only intervene on the failure path. Internal linkage here
// is fine: only its address goes into the hook pointer below.
static FARPROC WINAPI guard_notify_hook(unsigned dliNotify, PDelayLoadInfo pdli) {
    (void)dliNotify;
    (void)pdli;
    return nullptr;
}

// Hook pointer definitions: global scope, external C linkage, matching the
// `extern "C" const PfnDliHook` declarations in delayimp.h (VS2015U3+ const
// form; DELAYIMP_INSECURE_WRITABLE_HOOKS is NOT defined). delayimp.h is
// included via vulkan_guard.hpp. MSVC does not encode language linkage in
// function-pointer types, so binding the emebalachat-scoped extern "C" hook
// here is type-safe.
extern "C" const PfnDliHook __pfnDliNotifyHook2 = &guard_notify_hook;
extern "C" const PfnDliHook __pfnDliFailureHook2 = &emebalachat::VulkanGuardFailureHook;
