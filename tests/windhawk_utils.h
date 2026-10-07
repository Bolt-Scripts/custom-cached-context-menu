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

class StringSetting {
public:
    StringSetting() = default;
    explicit StringSetting(PCWSTR valueName) {
        PCWSTR value = Wh_GetStringSetting(valueName);
        m_value = value ? value : L"";
        Wh_FreeStringSetting(value);
    }

    operator PCWSTR() const { return m_value.c_str(); }
    PCWSTR get() const { return m_value.c_str(); }

    template <typename... Args>
    static StringSetting make(PCWSTR valueName, Args... args) {
        (void)sizeof...(args);
        return StringSetting(valueName);
    }

private:
    std::wstring m_value;
};

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
