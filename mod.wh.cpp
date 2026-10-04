// ==WindhawkMod==
// @id              context-menu-overhaul
// @name            Context Menu Overhaul
// @description     Replaces the Explorer context menu with an instantly-opening cached menu, then discovers and caches shell extension items asynchronously.
// @version         0.1
// @include         explorer.exe
// @architecture    x86-64
// @compilerOptions -lole32 -lshlwapi -luuid -lcomctl32 -ladvapi32 -lgdi32
// @license         MIT
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Context Menu Overhaul

Replaces the Windows Explorer file context menu with a custom menu that opens
instantly. The native menu is slow because every registered shell extension is
loaded synchronously before it can be shown; this mod shows a cached menu right
away and discovers extension items asynchronously in the background.

Hold Shift while right-clicking to get the untouched native menu.

Design document: `docs/superpowers/specs/2026-10-04-context-menu-overhaul-design.md`
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- enableShiftBypass: true
  $name: Shift bypass
  $description: Hold Shift while right-clicking to show the untouched native menu.
- showMoreOptionsItem: true
  $name: Show more options item
  $description: Add a "Show more options" entry at the bottom of the replacement menu.
- warmupExtensions: [".txt", ".pdf", ".zip", ".rar", ".7z", ".jpg", ".png", ".mp4", ".mp3", ".docx", ".xlsx", ".exe", ".lnk"]
  $name: Warm-up extensions
  $description: File types whose menus are pre-built at Explorer startup.
- warmupDelaySeconds: 5
  $name: Warm-up delay
  $description: Seconds to wait after Explorer starts before warming the cache.
- clearCache: false
  $name: Clear cache
  $description: Turn on to delete the cached menu models; they rebuild on next use.
- debugLogging: false
  $name: Debug logging
  $description: Log timing and diagnostics for troubleshooting.
*/
// ==/WindhawkModSettings==

#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>

#include <windhawk_utils.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// ===========================================================================
// [CMO:Signature] Context signature: what identifies a cached menu model.
// ===========================================================================
namespace cmo {

enum class Scope : uint8_t { Files, Folders, Background, Desktop, Drive, NavPane, Other };
enum class Shape : uint8_t { Single, Multi };
enum class Variant : uint8_t { Normal, Extended };

inline uint64_t HashCombine(uint64_t seed, uint64_t value) {
    value += 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
    return seed ^ value;
}

inline uint64_t HashString(std::wstring_view text) {
    // FNV-1a 64-bit.
    uint64_t hash = 1469598103934665603ULL;
    for (wchar_t c : text) {
        hash ^= static_cast<uint64_t>(c);
        hash *= 1099511628211ULL;
    }
    return hash;
}

struct ContextSignature {
    Scope scope;
    std::wstring typeKey;
    Shape shape;
    Variant variant;

    bool operator==(const ContextSignature&) const = default;

    uint64_t Hash() const {
        uint64_t hash = HashCombine(static_cast<uint64_t>(scope), HashString(typeKey));
        hash = HashCombine(hash, static_cast<uint64_t>(shape));
        hash = HashCombine(hash, static_cast<uint64_t>(variant));
        return hash;
    }
};

// Lowercased extension including the dot (".txt"), or "*" when the name has
// no usable extension: leading-dot names, trailing dots, or no dot at all.
inline std::wstring MakeExtensionKey(std::wstring_view path) {
    size_t nameStart = path.find_last_of(L"\\/");
    nameStart = (nameStart == std::wstring_view::npos) ? 0 : nameStart + 1;

    size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring_view::npos || dot < nameStart || dot == nameStart ||
        dot + 1 >= path.size()) {
        return L"*";
    }

    std::wstring extension(path.substr(dot));
    CharLowerBuffW(extension.data(), static_cast<DWORD>(extension.size()));
    return extension;
}

// The shared type key of a selection: the common extension, "mixed" when the
// selection spans several extensions, or "*" for an empty selection.
inline std::wstring MakeTypeKey(const std::vector<std::wstring>& paths) {
    if (paths.empty()) {
        return L"*";
    }

    std::wstring first = MakeExtensionKey(paths.front());
    for (size_t i = 1; i < paths.size(); ++i) {
        if (MakeExtensionKey(paths[i]) != first) {
            return L"mixed";
        }
    }
    return first;
}

}  // namespace cmo

// ===========================================================================
// [CMO:Model] Menu item / menu model definitions. (Task 4)
// ===========================================================================

// ===========================================================================
// [CMO:Cache] In-memory and persistent menu model cache. (Tasks 4/7)
// ===========================================================================

// ===========================================================================
// [CMO:Classify] Popup owner classification.
// ===========================================================================
namespace cmo {

enum class ShellViewKind : uint8_t { None, Desktop, ShellDefView, NavPane, Other };

inline ShellViewKind ClassifyClassChain(const std::vector<std::wstring>& ancestors,
                                        bool isDesktopRoot) {
    if (isDesktopRoot) {
        return ShellViewKind::Desktop;
    }
    for (const std::wstring& name : ancestors) {
        if (name == L"SHELLDLL_DefView") {
            return ShellViewKind::ShellDefView;
        }
    }
    for (const std::wstring& name : ancestors) {
        if (name == L"NamespaceTreeControl") {
            return ShellViewKind::NavPane;
        }
    }
    return ShellViewKind::None;
}

inline bool IsReplaceableKind(ShellViewKind kind) {
    return kind == ShellViewKind::Desktop || kind == ShellViewKind::ShellDefView;
}

inline bool IsDesktopRootWindow(HWND hwnd) {
    HWND root = GetAncestor(hwnd, GA_ROOT);
    if (!root) {
        return false;
    }
    if (root == GetShellWindow()) {
        return true;
    }
    wchar_t className[64] = {};
    if (!GetClassNameW(root, className, ARRAYSIZE(className))) {
        return false;
    }
    if (wcscmp(className, L"Progman") != 0 && wcscmp(className, L"WorkerW") != 0) {
        return false;
    }
    return FindWindowExW(root, nullptr, L"SHELLDLL_DefView", nullptr) != nullptr;
}

inline ShellViewKind ClassifyOwner(HWND owner) {
    if (!owner) {
        return ShellViewKind::None;
    }
    std::vector<std::wstring> ancestors;
    for (HWND window = owner; window; window = GetAncestor(window, GA_PARENT)) {
        if (IsDesktopRootWindow(window)) {
            return ClassifyClassChain(ancestors, true);
        }
        wchar_t className[128] = {};
        if (GetClassNameW(window, className, ARRAYSIZE(className))) {
            ancestors.emplace_back(className);
        }
    }
    return ClassifyClassChain(ancestors, false);
}

}  // namespace cmo

// ===========================================================================
// [CMO:Discovery] Real shell menu population capture and replay.
// ===========================================================================
namespace cmo {

struct PendingCapture {
    IContextMenu* obj = nullptr;
    UINT indexMenu = 0;
    UINT idCmdFirst = 0;
    UINT idCmdLast = 0;
    UINT flags = 0;
    HWND owner = nullptr;
    ULONGLONG tick = 0;
    bool used = false;
};

class PendingQueue {
public:
    void Push(PendingCapture capture) {
        Clear();
        capture_ = capture;
        valid_ = true;
    }

    // Returns the pending capture and transfers ownership of its COM
    // reference to the caller. Returns nullptr when nothing is pending.
    PendingCapture* Take() {
        if (!valid_) {
            return nullptr;
        }
        valid_ = false;
        return &capture_;
    }

    void Clear() {
        if (valid_ && capture_.obj) {
            capture_.obj->Release();
        }
        valid_ = false;
        capture_ = {};
    }

    // Drops a capture older than maxAgeMs; no caller holds it at that point.
    void ExpireOlderThan(ULONGLONG now, ULONGLONG maxAgeMs) {
        if (valid_ && now - capture_.tick > maxAgeMs) {
            Clear();
        }
    }

    bool HasPending() const { return valid_; }

private:
    PendingCapture capture_{};
    bool valid_ = false;
};

inline thread_local PendingQueue g_pending;

using QueryContextMenu_t =
    HRESULT(STDMETHODCALLTYPE*)(IContextMenu*, HMENU, UINT, UINT, UINT, UINT);
inline QueryContextMenu_t QueryContextMenu_Original = nullptr;

HRESULT STDMETHODCALLTYPE QueryContextMenu_Hook(IContextMenu* pThis, HMENU hmenu,
                                                UINT indexMenu, UINT idCmdFirst,
                                                UINT idCmdLast, UINT uFlags) {
    // A capture that never reached TrackPopupMenu* is stale; release it
    // before capturing the new one.
    if (PendingCapture* previous = g_pending.Take()) {
        if (previous->obj) {
            previous->obj->Release();
        }
    }

    PendingCapture capture{};
    capture.obj = pThis;
    capture.indexMenu = indexMenu;
    capture.idCmdFirst = idCmdFirst;
    capture.idCmdLast = idCmdLast;
    capture.flags = uFlags;
    capture.tick = GetTickCount64();
    if (pThis) {
        pThis->AddRef();
    }
    g_pending.Push(capture);

    Wh_Log(L"QueryContextMenu deferred: this=%p idFirst=%u flags=%08X", pThis,
           idCmdFirst, uFlags);

    // Report an empty menu; the caller shows it and our TrackPopupMenu* hook
    // substitutes the cached one.
    return MAKE_HRESULT(SEVERITY_SUCCESS, 0, 0);
}

// Runs the real population into the given menu.
void ReplayInto(IContextMenu* obj, HMENU hMenu, UINT indexMenu, UINT idCmdFirst,
                UINT idCmdLast, UINT flags) {
    if (!obj || !QueryContextMenu_Original) {
        return;
    }
    QueryContextMenu_Original(obj, hMenu, indexMenu, idCmdFirst, idCmdLast, flags);
}

// Takes the pending capture (if any), replays the real population into the
// caller's menu, and releases the captured object.
bool ConsumePendingAndReplay(HWND owner, HMENU hMenu) {
    PendingCapture* capture = g_pending.Take();
    if (!capture) {
        return false;
    }
    ReplayInto(capture->obj, hMenu, capture->indexMenu, capture->idCmdFirst,
               capture->idCmdLast, capture->flags);
    if (capture->obj) {
        capture->obj->Release();
    }
    Wh_Log(L"Replayed native population for owner=%p", owner);
    return true;
}

// Creates a throwaway default context menu to read the shared vtable of
// shell32's default context menu implementation and hook its
// QueryContextMenu slot.
bool InstallVtableHook() {
    HRESULT initResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    IShellFolder* desktop = nullptr;
    PIDLIST_ABSOLUTE pidl = nullptr;
    IContextMenu* menu = nullptr;
    bool installed = false;

    HRESULT hr = SHGetDesktopFolder(&desktop);
    if (SUCCEEDED(hr) && desktop) {
        hr = SHGetKnownFolderIDList(FOLDERID_Windows, 0, nullptr, &pidl);
        if (SUCCEEDED(hr) && pidl) {
            PCUITEMID_CHILD children[1] = {pidl};
            DEFCONTEXTMENU dcm = {};
            dcm.psf = desktop;
            dcm.cidl = 1;
            dcm.apidl = children;

            hr = SHCreateDefaultContextMenu(&dcm, IID_IContextMenu, (void**)&menu);
            if (SUCCEEDED(hr) && menu) {
                void** vtable = *reinterpret_cast<void***>(menu);
                void* queryContextMenu = vtable[3];
                installed = Wh_SetFunctionHook(queryContextMenu,
                                               (void*)QueryContextMenu_Hook,
                                               (void**)&QueryContextMenu_Original);
                if (!installed) {
                    Wh_Log(L"Wh_SetFunctionHook(QueryContextMenu) failed");
                }
            } else {
                Wh_Log(L"SHCreateDefaultContextMenu failed: %08X", hr);
            }
        } else {
            Wh_Log(L"SHGetKnownFolderIDList failed: %08X", hr);
        }
    } else {
        Wh_Log(L"SHGetDesktopFolder failed: %08X", hr);
    }

    if (menu) {
        menu->Release();
    }
    if (pidl) {
        CoTaskMemFree(pidl);
    }
    if (desktop) {
        desktop->Release();
    }
    // Balancing the call is only safe for S_OK; S_FALSE means this thread was
    // already initialized by someone else.
    if (initResult == S_OK) {
        CoUninitialize();
    }

    return installed;
}

// Version-independent fallback: locate CDefaultContextMenu::QueryContextMenu
// through the symbol server if the vtable discovery failed.
bool InstallSymbolHook() {
    HMODULE shell32 = GetModuleHandleW(L"shell32.dll");
    if (!shell32) {
        return false;
    }

    WH_FIND_SYMBOL_OPTIONS options = {};
    options.optionsSize = sizeof(options);
    options.symbolServer = nullptr;
    options.noUndecoratedSymbols = FALSE;

    WH_FIND_SYMBOL symbol = {};
    HANDLE search = Wh_FindFirstSymbol(shell32, &options, &symbol);
    if (!search) {
        Wh_Log(L"Symbol enumeration failed");
        return false;
    }

    void* target = nullptr;
    do {
        if (symbol.symbol &&
            wcsstr(symbol.symbol, L"CDefaultContextMenu::QueryContextMenu")) {
            target = symbol.address;
            break;
        }
    } while (Wh_FindNextSymbol(search, &symbol));
    Wh_FindCloseSymbol(search);

    if (!target) {
        Wh_Log(L"CDefaultContextMenu::QueryContextMenu not found in symbols");
        return false;
    }

    Wh_Log(L"Found CDefaultContextMenu::QueryContextMenu at %p", target);
    return Wh_SetFunctionHook(target, (void*)QueryContextMenu_Hook,
                              (void**)&QueryContextMenu_Original);
}

bool InstallPopulationHook() {
    if (QueryContextMenu_Original) {
        return true;
    }
    if (InstallVtableHook()) {
        return true;
    }
    Wh_Log(L"Vtable discovery failed; trying the symbol fallback");
    return InstallSymbolHook();
}

}  // namespace cmo

// ===========================================================================
// [CMO:Invoker] Executing a chosen menu item. (Tasks 4/6)
// ===========================================================================

// ===========================================================================
// [CMO:View] Rendering abstraction, native implementation. (Task 4)
// ===========================================================================

// ===========================================================================
// [CMO:Warmup] Background cache warm-up. (Task 8)
// ===========================================================================

// ===========================================================================
// [CMO:Invalidation] Cache invalidation. (Task 7)
// ===========================================================================

// ===========================================================================
// [CMO:Hooks] Hook functions and interception state.
// ===========================================================================
namespace cmo {

using TrackPopupMenuEx_t = decltype(&TrackPopupMenuEx);
inline TrackPopupMenuEx_t TrackPopupMenuEx_Original = nullptr;

using TrackPopupMenu_t = decltype(&TrackPopupMenu);
inline TrackPopupMenu_t TrackPopupMenu_Original = nullptr;

BOOL WINAPI TrackPopupMenuEx_Hook(HMENU hMenu, UINT uFlags, int x, int y, HWND hWnd,
                                  LPTPMPARAMS lptpm) {
    ShellViewKind kind = ClassifyOwner(hWnd);
    bool replayed = ConsumePendingAndReplay(hWnd, hMenu);
    Wh_Log(L"TrackPopupMenuEx hwnd=%p kind=%d flags=%08X replayed=%d", hWnd,
           static_cast<int>(kind), uFlags, replayed);
    return TrackPopupMenuEx_Original(hMenu, uFlags, x, y, hWnd, lptpm);
}

BOOL WINAPI TrackPopupMenu_Hook(HMENU hMenu, UINT uFlags, int x, int y, int nReserved,
                                HWND hWnd, const RECT* prcRect) {
    ShellViewKind kind = ClassifyOwner(hWnd);
    bool replayed = ConsumePendingAndReplay(hWnd, hMenu);
    Wh_Log(L"TrackPopupMenu hwnd=%p kind=%d flags=%08X replayed=%d", hWnd,
           static_cast<int>(kind), uFlags, replayed);
    return TrackPopupMenu_Original(hMenu, uFlags, x, y, nReserved, hWnd, prcRect);
}

// --- Windows 11 modern menu suppression ------------------------------------
// Technique adapted from the explorer-context-menu-classic mod by m417z
// (MIT-licensed Windhawk mod collection): fail the presenter lookup so
// Explorer falls back to the classic IContextMenu path, and block the
// desktop mini menu. The Shift key opts out, matching stock behavior.

bool IsWindows11OrGreater() {
    static const bool isWin11 = [] {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (!ntdll) {
            return false;
        }
        auto rtlGetVersion =
            (LONG(WINAPI*)(OSVERSIONINFOW*))GetProcAddress(ntdll, "RtlGetVersion");
        if (!rtlGetVersion) {
            return false;
        }
        OSVERSIONINFOW info = {};
        info.dwOSVersionInfoSize = sizeof(info);
        if (rtlGetVersion(&info) != 0) {
            return false;
        }
        return info.dwMajorVersion >= 10 && info.dwBuildNumber >= 22000;
    }();
    return isWin11;
}

// GUIDs from shell32.dll, CDefView::TryGetContextMenuPresenter.
constexpr GUID kContextMenuPresenterService = {
    0xb306c5b1, 0xb4f2, 0x473c, {0xb6, 0xff, 0x70, 0x1b, 0x24, 0x6c, 0xe2, 0xd2}};
constexpr GUID kContextMenuPresenterIid = {
    0x706461d1, 0xac5f, 0x4730, {0xbf, 0xe3, 0xca, 0xc6, 0xca, 0xd5, 0xef, 0x5e}};
// Changed to this version in update KB5052093 of Windows 11 version 24H2.
constexpr GUID kContextMenuPresenterIid24H2 = {
    0x37a472f7, 0x63cf, 0x4ccf, {0xa8, 0x8b, 0x52, 0x31, 0xa3, 0xc7, 0xd8, 0xb6}};

using IUnknown_QueryService_t = decltype(&IUnknown_QueryService);
inline IUnknown_QueryService_t IUnknown_QueryService_Original = nullptr;

HRESULT WINAPI IUnknown_QueryService_Hook(IUnknown* punk, REFGUID guidService,
                                          REFIID riid, void** ppvOut) {
    if (IsEqualGUID(guidService, kContextMenuPresenterService) &&
        (IsEqualGUID(riid, kContextMenuPresenterIid) ||
         IsEqualGUID(riid, kContextMenuPresenterIid24H2))) {
        if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) == 0) {
            Wh_Log(L"Blocking modern context menu presenter");
            if (ppvOut) {
                *ppvOut = nullptr;
            }
            return E_FAIL;
        }
    }
    return IUnknown_QueryService_Original(punk, guidService, riid, ppvOut);
}

using ShouldShowMiniMenu_t = bool(WINAPI*)(void*, void*);
inline ShouldShowMiniMenu_t ShouldShowMiniMenu_Original = nullptr;

bool WINAPI ShouldShowMiniMenu_Hook(void* pThis, void* param) {
    if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) == 0) {
        Wh_Log(L"Blocking modern desktop mini menu");
        return false;
    }
    return ShouldShowMiniMenu_Original(pThis, param);
}

void InstallWin11Suppression() {
    if (!IsWindows11OrGreater()) {
        return;
    }

    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    if (shcore) {
        auto queryService =
            (IUnknown_QueryService_t)GetProcAddress(shcore, "IUnknown_QueryService");
        if (queryService) {
            if (!WindhawkUtils::Wh_SetFunctionHookT(queryService,
                                                    IUnknown_QueryService_Hook,
                                                    &IUnknown_QueryService_Original)) {
                Wh_Log(L"Failed to hook IUnknown_QueryService");
            }
        }
    } else {
        Wh_Log(L"Failed to load shcore.dll");
    }

    HMODULE explorerFrame = LoadLibraryW(L"explorerframe.dll");
    if (explorerFrame) {
        WindhawkUtils::SYMBOL_HOOK symbolHooks[] = {
            {
                {LR"(private: bool __cdecl CNscTree::ShouldShowMiniMenu(struct _TREEITEM *))"},
                (void**)&ShouldShowMiniMenu_Original,
                (void*)ShouldShowMiniMenu_Hook,
                false,
            },
        };
        if (!WindhawkUtils::HookSymbols(explorerFrame, symbolHooks,
                                        ARRAYSIZE(symbolHooks))) {
            Wh_Log(L"Failed to hook CNscTree::ShouldShowMiniMenu");
        }
    } else {
        Wh_Log(L"Failed to load explorerframe.dll");
    }
}

}  // namespace cmo

// ===========================================================================
// [CMO:ModLifecycle] Windhawk entry points.
// ===========================================================================

namespace {
bool g_populationHookDeferred = false;
}

BOOL Wh_ModInit() {
    Wh_Log(L"Context Menu Overhaul init");

    if (!Wh_SetFunctionHook((void*)TrackPopupMenuEx, (void*)cmo::TrackPopupMenuEx_Hook,
                            (void**)&cmo::TrackPopupMenuEx_Original)) {
        Wh_Log(L"Failed to hook TrackPopupMenuEx");
        return FALSE;
    }

    if (!Wh_SetFunctionHook((void*)TrackPopupMenu, (void*)cmo::TrackPopupMenu_Hook,
                            (void**)&cmo::TrackPopupMenu_Original)) {
        Wh_Log(L"Failed to hook TrackPopupMenu");
        return FALSE;
    }

    cmo::InstallWin11Suppression();

    if (cmo::InstallPopulationHook()) {
        Wh_Log(L"Population hook installed");
    } else {
        Wh_Log(L"Population hook deferred to Wh_ModAfterInit");
        g_populationHookDeferred = true;
    }

    return TRUE;
}

void Wh_ModAfterInit() {
    if (g_populationHookDeferred && cmo::InstallPopulationHook()) {
        g_populationHookDeferred = false;
        Wh_ApplyHookOperations();
        Wh_Log(L"Population hook installed after init");
    }
}

void Wh_ModUninit() {
    Wh_Log(L"Context Menu Overhaul uninit");
}

void Wh_ModSettingsChanged() {
    Wh_Log(L"Context Menu Overhaul settings changed");
}
