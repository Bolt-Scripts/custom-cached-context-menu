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
#include <shlwapi.h>

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
// [CMO:Classify] Popup owner classification. (Task 2)
// ===========================================================================

// ===========================================================================
// [CMO:Discovery] Real shell menu population capture. (Tasks 3/5)
// ===========================================================================

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
// [CMO:Hooks] Hook functions and interception state. (Tasks 2/3/9)
// ===========================================================================

// ===========================================================================
// [CMO:ModLifecycle] Windhawk entry points.
// ===========================================================================

BOOL Wh_ModInit() {
    Wh_Log(L"Context Menu Overhaul init");
    return TRUE;
}

void Wh_ModUninit() {
    Wh_Log(L"Context Menu Overhaul uninit");
}

void Wh_ModSettingsChanged() {
    Wh_Log(L"Context Menu Overhaul settings changed");
}
