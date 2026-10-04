#pragma once
// Test-only stubs for the Windhawk mod API. Never shipped in the mod.
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <map>
#include <string>

#define WH_MOD_ID L"context-menu-overhaul"
#define WH_MOD_VERSION L"0.1"

inline std::map<std::wstring, std::wstring>& WhStubValues() {
    static std::map<std::wstring, std::wstring> values;
    return values;
}

inline void WhStubLog(const wchar_t* format, ...) {
    va_list args;
    va_start(args, format);
    vfwprintf(stderr, format, args);
    va_end(args);
    fputwc(L'\n', stderr);
}

#define Wh_Log(...) WhStubLog(__VA_ARGS__)

inline int Wh_GetIntSetting(const wchar_t* name, ...) {
    (void)name;
    return 0;
}

inline const wchar_t* Wh_GetStringSetting(const wchar_t* name, ...) {
    (void)name;
    static const wchar_t empty[] = L"";
    return empty;
}

inline void Wh_FreeStringSetting(const wchar_t* string) {
    (void)string;
}

inline int Wh_GetIntValue(const wchar_t* valueName, int defaultValue) {
    auto it = WhStubValues().find(valueName);
    return it == WhStubValues().end() ? defaultValue : _wtoi(it->second.c_str());
}

inline BOOL Wh_SetIntValue(const wchar_t* valueName, int value) {
    wchar_t buffer[32];
    swprintf(buffer, 32, L"%d", value);
    WhStubValues()[valueName] = buffer;
    return TRUE;
}

inline size_t Wh_GetStringValue(const wchar_t* valueName, wchar_t* stringBuffer,
                                size_t bufferChars) {
    auto it = WhStubValues().find(valueName);
    if (it == WhStubValues().end() || !stringBuffer || bufferChars == 0) {
        return 0;
    }
    wcsncpy(stringBuffer, it->second.c_str(), bufferChars - 1);
    stringBuffer[bufferChars - 1] = 0;
    return wcslen(stringBuffer);
}

inline BOOL Wh_SetStringValue(const wchar_t* valueName, const wchar_t* value) {
    WhStubValues()[valueName] = value ? value : L"";
    return TRUE;
}

inline size_t Wh_GetBinaryValue(const wchar_t* valueName, void* buffer, size_t bufferSize) {
    auto it = WhStubValues().find(valueName);
    if (it == WhStubValues().end() || !buffer) {
        return 0;
    }
    size_t bytes = it->second.size() * sizeof(wchar_t);
    if (bytes > bufferSize) {
        return 0;
    }
    memcpy(buffer, it->second.data(), bytes);
    return bytes;
}

inline BOOL Wh_SetBinaryValue(const wchar_t* valueName, const void* buffer, size_t bufferSize) {
    WhStubValues()[valueName] =
        std::wstring(static_cast<const wchar_t*>(buffer), bufferSize / sizeof(wchar_t));
    return TRUE;
}

inline BOOL Wh_DeleteValue(const wchar_t* valueName) {
    return WhStubValues().erase(valueName) ? TRUE : FALSE;
}

inline size_t Wh_GetModStoragePath(wchar_t* pathBuffer, size_t bufferChars) {
    if (!pathBuffer || bufferChars == 0) {
        return 0;
    }
    const wchar_t* path = L"cmo-test-storage";
    wcsncpy(pathBuffer, path, bufferChars - 1);
    pathBuffer[bufferChars - 1] = 0;
    return wcslen(pathBuffer);
}

inline BOOL Wh_SetFunctionHook(void* targetFunction, void* hookFunction, void** originalFunction) {
    (void)targetFunction;
    (void)hookFunction;
    if (originalFunction) {
        *originalFunction = targetFunction;
    }
    return TRUE;
}

inline BOOL Wh_ApplyHookOperations() {
    return TRUE;
}

typedef struct tagWH_FIND_SYMBOL_OPTIONS {
    size_t optionsSize;
    PCWSTR symbolServer;
    BOOL noUndecoratedSymbols;
} WH_FIND_SYMBOL_OPTIONS;

typedef struct tagWH_FIND_SYMBOL {
    void* address;
    PCWSTR symbol;
    PCWSTR symbolDecorated;
} WH_FIND_SYMBOL;

inline HANDLE Wh_FindFirstSymbol(HMODULE hModule, const WH_FIND_SYMBOL_OPTIONS* options,
                                 WH_FIND_SYMBOL* findData) {
    (void)hModule;
    (void)options;
    (void)findData;
    return nullptr;
}

inline BOOL Wh_FindNextSymbol(HANDLE symSearch, WH_FIND_SYMBOL* findData) {
    (void)symSearch;
    (void)findData;
    return FALSE;
}

inline void Wh_FindCloseSymbol(HANDLE symSearch) {
    (void)symSearch;
}
