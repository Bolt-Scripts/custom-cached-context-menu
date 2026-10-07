#pragma once
// Test-only stub for Windhawk's windhawk_utils.h. Never shipped in the mod.
#include <windows.h>
#include <string>
#include <vector>

namespace WindhawkUtils {

struct SYMBOL_HOOK {
    std::vector<std::wstring> symbols;
    void** pOriginalFunction;
    void* hookFunction;
    bool optional;
};

inline bool HookSymbols(HMODULE module, SYMBOL_HOOK* hooks, size_t hookCount) {
    (void)module;
    (void)hooks;
    (void)hookCount;
    return true;
}

template <typename T>
inline bool SetFunctionHook(T targetFunction, T hookFunction, T* originalFunction) {
    return Wh_SetFunctionHook((void*)targetFunction, (void*)hookFunction,
                              (void**)originalFunction);
}

template <typename T>
inline bool Wh_SetFunctionHookT(T targetFunction, T hookFunction, T* originalFunction) {
    return Wh_SetFunctionHook((void*)targetFunction, (void*)hookFunction,
                              (void**)originalFunction);
}

}  // namespace WindhawkUtils
