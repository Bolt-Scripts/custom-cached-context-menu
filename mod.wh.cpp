// ==WindhawkMod==
// @id              context-menu-overhaul
// @name            Context Menu Overhaul
// @description     Replaces the Explorer context menu with an instantly-opening cached menu, then discovers and caches shell extension items asynchronously.
// @version         0.5.0
// @include         explorer.exe
// @architecture    x86-64
// @compilerOptions -lole32 -lshlwapi -luuid -lcomctl32 -ladvapi32 -lgdi32 -luxtheme -lversion -ld3d11 -ld2d1 -ldwrite -ldcomp -ldxgi
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
- menuMode: 0
  $name: Menu mode
  $description: 0 shows the custom-rendered menu (falls back automatically on repeated failures); 1 keeps the classic owner-drawn menu.
- showMoreOptionsItem: true
  $name: Show classic menu item
  $description: Add a "Show classic menu" entry at the bottom of the replacement menu.
- submenuDelayMs: 150
  $name: Submenu open delay
  $description: Milliseconds before a hovered submenu opens while the replacement menu is shown. 0 opens instantly; -1 keeps the Windows setting.
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
- instantMenuFade: true
  $name: Instant menu open
  $description: Temporarily disables system menu animation (fade and slide) while this mod's menu opens, so it appears instantly. Session-only; the previous setting is restored immediately.
- advancedSubmenu: true
  $name: More options submenu
  $description: Move Windows extras and third-party shell extension entries into a submenu.
- advancedSubmenuLabel: More options
  $name: More options submenu label
  $description: Label of the submenu that collects extra items.
- advancedSubmenuItems: "Share, Add to Favorites, Cast to Device, Give access to, Restore previous versions, Pin to Start, Pin to Quick access, Open in Terminal"
  $name: Windows items to move
  $description: Comma-separated labels or verbs of Windows items to move into the submenu.
*/
// ==/WindhawkModSettings==

#include <windows.h>
#include <exdisp.h>
#include <servprov.h>
#include <shlguid.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <uxtheme.h>
#include <vsstyle.h>
#include <vssym32.h>

#include <d2d1.h>
#include <d2d1_1.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwrite.h>
#include <dxgi.h>
#include <dxgi1_2.h>

#include <commctrl.h>
#include <tlhelp32.h>
#include <windhawk_utils.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// ===========================================================================
// [CMO:Settings] User settings.
// ===========================================================================
namespace cmo {

struct Settings {
    bool enableShiftBypass = true;
    int menuMode = 0;
    bool showMoreOptionsItem = true;
    int submenuDelayMs = 150;
    int warmupDelaySeconds = 5;
    bool clearCache = false;
    bool debugLogging = false;
    bool instantMenuFade = true;
    bool advancedSubmenu = true;
    std::wstring advancedSubmenuLabel = L"More options";
    std::vector<std::wstring> advancedSubmenuItems;
};

inline Settings g_settings;

// Set in Wh_ModInit; only the UI thread may touch the render device/caches.
inline DWORD g_uiThreadId = 0;

std::wstring TrimWhitespace(const std::wstring& text) {
    const size_t first = text.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) {
        return L"";
    }
    const size_t last = text.find_last_not_of(L" \t\r\n");
    return text.substr(first, last - first + 1);
}

// The shell's raw menu labels carry accelerator markers ("Add to &Favorites")
// and trailing ellipses that are never visible; normalize them before
// comparing against anything the user sees or types.
std::wstring NormalizeMenuLabel(std::wstring text) {
    std::wstring out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == L'&') {
            if (i + 1 < text.size() && text[i + 1] == L'&') {
                out += L'&';
                ++i;
            }
            continue;
        }
        out += text[i];
    }
    out = TrimWhitespace(out);
    while (!out.empty() && (out.back() == L'.' || out.back() == 0x2026)) {
        out.pop_back();
    }
    return TrimWhitespace(out);
}

std::vector<std::wstring> ParseAdvancedItems(const std::wstring& text) {
    std::vector<std::wstring> items;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t comma = text.find(L',', start);
        const std::wstring token =
            TrimWhitespace(text.substr(start, comma == std::wstring::npos
                                                  ? std::wstring::npos
                                                  : comma - start));
        if (!token.empty()) {
            items.push_back(token);
        }
        if (comma == std::wstring::npos) {
            break;
        }
        start = comma + 1;
    }
    return items;
}

void LoadSettings() {
    g_settings.enableShiftBypass = Wh_GetIntSetting(L"enableShiftBypass") != 0;
    g_settings.menuMode = Wh_GetIntSetting(L"menuMode");
    g_settings.showMoreOptionsItem = Wh_GetIntSetting(L"showMoreOptionsItem") != 0;
    g_settings.submenuDelayMs = Wh_GetIntSetting(L"submenuDelayMs");
    g_settings.warmupDelaySeconds = Wh_GetIntSetting(L"warmupDelaySeconds");
    g_settings.clearCache = Wh_GetIntSetting(L"clearCache") != 0;
    g_settings.debugLogging = Wh_GetIntSetting(L"debugLogging") != 0;
    g_settings.instantMenuFade = Wh_GetIntSetting(L"instantMenuFade") != 0;
    g_settings.advancedSubmenu = Wh_GetIntSetting(L"advancedSubmenu") != 0;

    PCWSTR advancedLabel = Wh_GetStringSetting(L"advancedSubmenuLabel");
    g_settings.advancedSubmenuLabel =
        (advancedLabel && advancedLabel[0]) ? advancedLabel : L"More options";
    Wh_FreeStringSetting(advancedLabel);

    PCWSTR advancedItems = Wh_GetStringSetting(L"advancedSubmenuItems");
    g_settings.advancedSubmenuItems =
        ParseAdvancedItems(advancedItems ? advancedItems : L"");
    Wh_FreeStringSetting(advancedItems);
}

}  // namespace cmo

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
// [CMO:Model] Menu item / menu model definitions.
// ===========================================================================
namespace cmo {

enum class ItemKind : uint8_t { Command, Submenu, Separator, Header };
enum class ActionKind : uint8_t {
    ViewAction,
    ShellVerb,
    Fallback,
    Submenu,
    NewItem,
    SortBy,
    SortDirection,
    GroupBy,
    GroupDirection,
    CustomCommand,
    Builtin,
};

enum class BuiltinAction : uint8_t { None, CopyPath, OpenNewWindow, Properties };

// Documented view operations, dispatched through IFolderView2 / IShellView.
// The old FCIDM_* view command IDs are not defined by the Windows SDK and
// must not be guessed.
enum class ViewAction : uint32_t {
    None = 0,
    Rename,
    Refresh,
    ViewExtraLargeIcons,
    ViewLargeIcons,
    ViewMediumIcons,
    ViewSmallIcons,
    ViewList,
    ViewDetails,
    ViewTiles,
    ViewContent,
    AutoArrange,
    AlignToGrid,
};

enum ModelFlags : uint32_t {
    kModelNone = 0,
    kModelDefault = 1u << 0,
    kModelChecked = 1u << 1,
    kModelRadio = 1u << 2,
    kModelDisabled = 1u << 3,
    kModelOwnerDraw = 1u << 4,
    kModelSeparator = 1u << 5,
    kModelExtension = 1u << 6,
    kModelHasOffset = 1u << 7,
    kModelWarmup = 1u << 8,
    kModelThirdParty = 1u << 9,
};

struct MenuItem {
    uint32_t id = 0;
    ItemKind kind = ItemKind::Command;
    ActionKind action = ActionKind::ViewAction;
    std::wstring label;
    std::wstring canonicalVerb;
    uint32_t viewAction = static_cast<uint32_t>(ViewAction::None);
    uint32_t verbOffset = 0;
    uint32_t flags = kModelNone;
    std::wstring iconRef;
    std::wstring targetPath;
    // Index into the ShellNew template list for ActionKind::NewItem.
    uint32_t newIndex = 0;
    // Index into RulesConfig::commands for ActionKind::CustomCommand.
    uint32_t customCommandIndex = 0;
    // Transient display overrides (not serialized): per-item label and marker.
    std::wstring displayLabel;
    int markerOverride = -1;
    // Built-in action for ActionKind::Builtin.
    BuiltinAction builtinAction = BuiltinAction::None;
    // Sort/group field index into kShellPropertyKeys, and sort direction.
    uint32_t sortIndex = 0;
    bool sortAscending = true;
    // Icon size for icon view modes (-1 for the shell default).
    int32_t iconSize = -1;
    // 16x16 BGRA icon captured from the shell's own menu bitmap.
    std::vector<uint8_t> iconPixels;
    std::vector<MenuItem> children;
};

struct MenuModel {
    ContextSignature sig;
    std::vector<MenuItem> items;
    uint32_t flags = kModelNone;
    std::vector<std::wstring> handlerModules;
    uint64_t sourceStamp = 0;
};

void FlattenIdsInto(const std::vector<MenuItem>& items, std::vector<uint32_t>& out) {
    for (const MenuItem& item : items) {
        out.push_back(item.id);
        FlattenIdsInto(item.children, out);
    }
}

std::vector<uint32_t> FlattenIds(const MenuModel& model) {
    std::vector<uint32_t> ids;
    FlattenIdsInto(model.items, ids);
    return ids;
}

const MenuItem* FindByIdIn(const std::vector<MenuItem>& items, uint32_t id) {
    for (const MenuItem& item : items) {
        if (item.id == id) {
            return &item;
        }
        if (const MenuItem* found = FindByIdIn(item.children, id)) {
            return found;
        }
    }
    return nullptr;
}

const MenuItem* FindById(const MenuModel& model, uint32_t id) {
    return FindByIdIn(model.items, id);
}

// Verb when the item has one, otherwise the offset to invoke.
std::pair<std::wstring, uint32_t> ChooseInvokeDescriptor(const MenuItem& item) {
    if (!item.canonicalVerb.empty()) {
        return {item.canonicalVerb, 0};
    }
    return {L"", item.verbOffset};
}

std::wstring FormatMultiLabel(std::wstring_view verb, size_t count) {
    if (count <= 1) {
        return std::wstring(verb);
    }
    return std::wstring(verb) + L" " + std::to_wstring(count) + L" items";
}

// Prunes items that cannot be shown faithfully: non-separator items without
// a label, and submenus left with no children after their contents were
// pruned. Runs bottom-up so a submenu whose children are all pruned is
// removed together with them.
void PruneMenuItems(std::vector<MenuItem>& items) {
    for (MenuItem& item : items) {
        PruneMenuItems(item.children);
    }
    std::erase_if(items, [](const MenuItem& item) {
        if (item.kind == ItemKind::Separator) {
            return false;
        }
        if (item.label.empty()) {
            return true;
        }
        return item.kind == ItemKind::Submenu && item.children.empty();
    });
}

bool EqualsIgnoreCase(const std::wstring& left, const std::wstring& right) {
    return !left.empty() && !right.empty() &&
           _wcsicmp(left.c_str(), right.c_str()) == 0;
}

bool LabelsMatchIgnoreCase(const std::wstring& left, const std::wstring& right) {
    if (left.empty() || right.empty()) {
        return false;
    }
    const std::wstring a = NormalizeMenuLabel(left);
    const std::wstring b = NormalizeMenuLabel(right);
    return !a.empty() && !b.empty() && _wcsicmp(a.c_str(), b.c_str()) == 0;
}

// Verbs used by Windows' own menu items. Items with any other verb come from
// third-party shell extensions and belong in the advanced submenu. Windows
// extras that should stay on top are protected by this list and move only
// when named in the setting.
bool IsKnownWindowsVerb(const std::wstring& verb) {
    if (verb.empty()) {
        return false;
    }
    static const wchar_t* kVerbs[] = {
        L"open",       L"opennew",       L"opennewtab",  L"opennewwindow",
        L"openas",     L"openwith",      L"edit",        L"print",
        L"printto",    L"runas",         L"preview",     L"cut",
        L"copy",       L"paste",         L"pastelink",   L"delete",
        L"rename",     L"properties",    L"createshortcut", L"link",
        L"copyaspath", L"sortby",        L"refresh",     L"new",
        L"display",    L"personalize",   L"viewlarge",   L"viewsmall",
        L"viewlist",   L"viewdetails",   L"pintohome",   L"pintohomefile",
        L"pintostartscreen", L"previousversions", L"windows.modernshare",
        L"casttodevice", L"giveaccess",  L"share",       L"includeinlibrary",
    };
    for (const wchar_t* known : kVerbs) {
        if (_wcsicmp(verb.c_str(), known) == 0) {
            return true;
        }
    }
    return false;
}

// Advanced items are third-party handler entries plus configured Windows
// extras. Core commands and the native fallback never move.
bool IsAdvancedItem(const MenuItem& item) {
    if (item.kind == ItemKind::Separator || item.action == ActionKind::Fallback) {
        return false;
    }
    // An explicit setting entry always wins.
    for (const std::wstring& token : g_settings.advancedSubmenuItems) {
        if (LabelsMatchIgnoreCase(item.label, token) ||
            EqualsIgnoreCase(item.canonicalVerb, token)) {
            return true;
        }
    }
    if (item.flags & kModelThirdParty) {
        return true;
    }
    // Discovered items with a verb Windows does not use come from third-party
    // handlers, regardless of how they are registered.
    return (item.flags & kModelExtension) != 0 &&
           !IsKnownWindowsVerb(item.canonicalVerb);
}

// Windows extras (configured list or a known Windows verb) sort above
// third-party handlers inside the More options submenu.
bool IsBuiltinExtra(const MenuItem& item) {
    for (const std::wstring& token : g_settings.advancedSubmenuItems) {
        if (LabelsMatchIgnoreCase(item.label, token) ||
            EqualsIgnoreCase(item.canonicalVerb, token)) {
            return true;
        }
    }
    return IsKnownWindowsVerb(item.canonicalVerb);
}

// Removes separators left dangling or duplicated by moving items out.
void CollapseSeparators(std::vector<MenuItem>& items) {
    std::vector<MenuItem> out;
    out.reserve(items.size());
    for (MenuItem& item : items) {
        if (item.kind == ItemKind::Separator &&
            (out.empty() || out.back().kind == ItemKind::Separator)) {
            continue;
        }
        out.push_back(std::move(item));
    }
    while (!out.empty() && out.back().kind == ItemKind::Separator) {
        out.pop_back();
    }
    items = std::move(out);
}

// Moves advanced items into one submenu placed just above the native fallback
// entry. Runs at open time so the setting applies without a rediscovery.
void ReorganizeAdvancedItems(std::vector<MenuItem>& items) {
    if (!g_settings.advancedSubmenu || g_settings.advancedSubmenuLabel.empty()) {
        return;
    }

    std::vector<MenuItem> advanced;
    std::vector<MenuItem> kept;
    kept.reserve(items.size());
    for (MenuItem& item : items) {
        if (IsAdvancedItem(item)) {
            advanced.push_back(std::move(item));
        } else {
            kept.push_back(std::move(item));
        }
    }
    if (g_settings.debugLogging) {
        for (const MenuItem& item : advanced) {
            Wh_Log(L"More options: moved '%s' (verb '%s')", item.label.c_str(),
                   item.canonicalVerb.c_str());
        }
        for (const MenuItem& item : kept) {
            if (item.kind != ItemKind::Separator &&
                item.action != ActionKind::Fallback &&
                (item.flags & kModelExtension)) {
                Wh_Log(L"More options: kept '%s' (verb '%s')", item.label.c_str(),
                       item.canonicalVerb.c_str());
            }
        }
    }
    if (advanced.empty()) {
        return;
    }

    // Built-in Windows extras first, third-party handlers last; both groups
    // keep the shell's relative order. One separator splits the groups.
    const auto customStart =
        std::stable_partition(advanced.begin(), advanced.end(), IsBuiltinExtra);
    if (customStart != advanced.begin() && customStart != advanced.end()) {
        MenuItem separator{};
        separator.id = 0xF001;
        separator.kind = ItemKind::Separator;
        advanced.insert(customStart, std::move(separator));
    }

    MenuItem submenu{};
    submenu.id = 0xF000;
    submenu.kind = ItemKind::Submenu;
    submenu.action = ActionKind::Submenu;
    submenu.label = g_settings.advancedSubmenuLabel;
    submenu.iconRef = L"@glyph:E712";
    submenu.children = std::move(advanced);

    size_t insertAt = kept.size();
    for (size_t i = 0; i < kept.size(); ++i) {
        if (kept[i].action == ActionKind::Fallback) {
            insertAt = i;
            break;
        }
    }
    while (insertAt > 0 && kept[insertAt - 1].kind == ItemKind::Separator) {
        --insertAt;
    }
    kept.insert(kept.begin() + static_cast<std::ptrdiff_t>(insertAt),
                std::move(submenu));
    CollapseSeparators(kept);
    items = std::move(kept);
}

// ===========================================================================
// [CMO:NewMenu] The New submenu is built from ShellNew templates.
// ===========================================================================

// One entry of the New submenu. Enumerated once from the registry (Folder and
// Shortcut are synthetic) and referenced by index from the core model.
struct NewTemplate {
    enum class Kind : uint8_t { Folder, Shortcut, NullFile, Data, FileName, Command };
    Kind kind = Kind::NullFile;
    std::wstring displayName;
    std::wstring extension;      // includes the leading dot
    std::wstring fileName;       // FileName templates
    std::wstring command;        // Command/Shortcut templates
    std::vector<uint8_t> data;   // Data templates
};

std::mutex g_newTemplatesMutex;
std::atomic<bool> g_newTemplatesReady{false};
std::vector<NewTemplate> g_newTemplates;
std::wstring g_shortcutCommand;

std::wstring ExpandEnv(const std::wstring& text) {
    wchar_t buffer[1024] = {};
    if (ExpandEnvironmentStringsW(text.c_str(), buffer, ARRAYSIZE(buffer))) {
        return buffer;
    }
    return text;
}

bool RegKeyExists(HKEY root, const std::wstring& subkey) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subkey.c_str(), 0, KEY_READ, &key) == ERROR_SUCCESS) {
        RegCloseKey(key);
        return true;
    }
    return false;
}

std::wstring ReadRegString(HKEY root, const std::wstring& subkey, const wchar_t* name) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subkey.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return L"";
    }
    wchar_t buffer[1024] = {};
    DWORD bytes = sizeof(buffer);
    DWORD type = 0;
    const LONG rc =
        RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<LPBYTE>(buffer),
                         &bytes);
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS) {
        return L"";
    }
    if (type == REG_EXPAND_SZ) {
        return ExpandEnv(buffer);
    }
    return type == REG_SZ ? buffer : L"";
}

void AddShellNewTemplate(HKEY root, const std::wstring& classesPrefix,
                         const std::wstring& ext, std::vector<NewTemplate>& out) {
    const std::wstring extKey = classesPrefix + ext;
    std::wstring shellNew = extKey + L"\\ShellNew";
    if (!RegKeyExists(root, shellNew)) {
        const std::wstring progId = ReadRegString(root, extKey, nullptr);
        if (progId.empty()) {
            return;
        }
        shellNew = classesPrefix + progId + L"\\ShellNew";
        if (!RegKeyExists(root, shellNew)) {
            return;
        }
    }

    NewTemplate tmpl;
    tmpl.extension = ext;

    SHFILEINFOW info = {};
    if (SHGetFileInfoW(ext.c_str(), FILE_ATTRIBUTE_NORMAL, &info, sizeof(info),
                       SHGFI_USEFILEATTRIBUTES | SHGFI_TYPENAME) &&
        info.szTypeName[0]) {
        tmpl.displayName = info.szTypeName;
    } else {
        tmpl.displayName = ext;
    }
    if (tmpl.displayName.empty()) {
        return;
    }

    HKEY key = nullptr;
    if (RegOpenKeyExW(root, shellNew.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return;
    }

    // Existence checks pass a size pointer; a NULL/NULL query is not reliable
    // for this on all Windows versions.
    DWORD valueType = 0;
    DWORD valueSize = 0;
    if (RegQueryValueExW(key, L"NullFile", nullptr, &valueType, nullptr, &valueSize) ==
        ERROR_SUCCESS) {
        tmpl.kind = NewTemplate::Kind::NullFile;
        out.push_back(std::move(tmpl));
    } else if (RegQueryValueExW(key, L"Data", nullptr, &valueType, nullptr,
                                &valueSize) == ERROR_SUCCESS &&
               valueSize > 0 && valueSize <= 65536) {
        if (valueType == REG_BINARY) {
            tmpl.kind = NewTemplate::Kind::Data;
            tmpl.data.resize(valueSize);
            if (RegQueryValueExW(key, L"Data", nullptr, nullptr, tmpl.data.data(),
                                 &valueSize) == ERROR_SUCCESS) {
                out.push_back(std::move(tmpl));
            }
        } else if (valueType == REG_SZ || valueType == REG_EXPAND_SZ) {
            // Some templates store the payload as text; the shell converts it
            // to ANSI bytes.
            const std::wstring text = ReadRegString(root, shellNew, L"Data");
            if (!text.empty()) {
                const int needed = WideCharToMultiByte(CP_ACP, 0, text.c_str(), -1,
                                                       nullptr, 0, nullptr, nullptr);
                if (needed > 1) {
                    std::string narrow(static_cast<size_t>(needed - 1), '\0');
                    WideCharToMultiByte(CP_ACP, 0, text.c_str(), -1, narrow.data(),
                                        needed, nullptr, nullptr);
                    tmpl.kind = NewTemplate::Kind::Data;
                    tmpl.data.assign(narrow.begin(), narrow.end());
                    out.push_back(std::move(tmpl));
                }
            }
        }
    } else if (!(tmpl.fileName = ReadRegString(root, shellNew, L"FileName")).empty()) {
        tmpl.kind = NewTemplate::Kind::FileName;
        tmpl.fileName = ExpandEnv(tmpl.fileName);
        out.push_back(std::move(tmpl));
    } else if (!(tmpl.command = ReadRegString(root, shellNew, L"Command")).empty()) {
        tmpl.kind = NewTemplate::Kind::Command;
        out.push_back(std::move(tmpl));
    }
    RegCloseKey(key);
}

// ShellNew keys are looked up exactly like the shell does: the merged
// HKEY_CLASSES_ROOT view first, then the two hives directly (Wine does not
// merge HKCU into HKCR, and a hive key without ShellNew must not shadow the
// other hive's template).
struct ShellNewRoot {
    HKEY root;
    const wchar_t* classesPrefix;  // "" for HKCR, "Software\Classes\" otherwise
};

const ShellNewRoot kShellNewRoots[] = {
    {HKEY_CLASSES_ROOT, L""},
    {HKEY_CURRENT_USER, L"Software\\Classes\\"},
    {HKEY_LOCAL_MACHINE, L"Software\\Classes\\"},
    // Office and other 32-bit installers register here.
    {HKEY_LOCAL_MACHINE, L"Software\\Classes\\WOW6432Node\\"},
};

std::vector<NewTemplate> EnumerateShellNewTemplates() {
    std::vector<NewTemplate> templates;
    std::unordered_set<std::wstring> seenExtensions;
    for (const ShellNewRoot& entry : kShellNewRoots) {
        HKEY classes = entry.root;
        const bool opened = entry.classesPrefix[0] != L'\0';
        if (opened) {
            if (RegOpenKeyExW(entry.root, entry.classesPrefix, 0, KEY_READ,
                              &classes) != ERROR_SUCCESS) {
                continue;
            }
        }
        for (DWORD i = 0;; ++i) {
            wchar_t ext[256] = {};
            DWORD length = ARRAYSIZE(ext);
            const LONG rc = RegEnumKeyExW(classes, i, ext, &length, nullptr, nullptr,
                                          nullptr, nullptr);
            if (rc == ERROR_NO_MORE_ITEMS) {
                break;
            }
            if (rc != ERROR_SUCCESS) {
                continue;  // skip overlong names instead of aborting
            }
            if (ext[0] != L'.') {
                continue;
            }
            std::wstring lower = ext;
            std::transform(lower.begin(), lower.end(), lower.begin(), towlower);
            if (seenExtensions.count(lower)) {
                continue;  // earlier root wins
            }
            const size_t before = templates.size();
            AddShellNewTemplate(entry.root, entry.classesPrefix, ext, templates);
            if (templates.size() > before) {
                seenExtensions.insert(lower);
            }
        }
        if (opened) {
            RegCloseKey(classes);
        }
    }

    std::sort(templates.begin(), templates.end(),
              [](const NewTemplate& a, const NewTemplate& b) {
                  return _wcsicmp(a.displayName.c_str(), b.displayName.c_str()) < 0;
              });

    // Windows shows one entry per type name; keep the first.
    std::vector<NewTemplate> unique;
    std::unordered_set<std::wstring> seenNames;
    for (NewTemplate& tmpl : templates) {
        std::wstring nameKey = tmpl.displayName;
        std::transform(nameKey.begin(), nameKey.end(), nameKey.begin(), towlower);
        if (!seenNames.insert(nameKey).second) {
            continue;
        }
        unique.push_back(std::move(tmpl));
    }
    return unique;
}

// Debug: logs where ShellNew data for the standard types actually lives.
void LogShellNewProbe(const wchar_t* ext) {
    if (!g_settings.debugLogging) {
        return;
    }
    for (const ShellNewRoot& entry : kShellNewRoots) {
        const std::wstring extKey = std::wstring(entry.classesPrefix) + ext;
        const std::wstring shellNew = extKey + L"\\ShellNew";
        const bool extExists = RegKeyExists(entry.root, extKey);
        const bool shellNewExists = RegKeyExists(entry.root, shellNew);
        const std::wstring progId = ReadRegString(entry.root, extKey, nullptr);
        bool progShellNew = false;
        if (!progId.empty()) {
            progShellNew = RegKeyExists(
                entry.root, entry.classesPrefix + progId + L"\\ShellNew");
        }
        const wchar_t* rootName =
            entry.classesPrefix[0] == L'\0'
                ? L"HKCR"
                : (entry.root == HKEY_CURRENT_USER ? L"HKCU" : L"HKLM");
        Wh_Log(L"ShellNew probe %s root=%s ext=%d shellnew=%d prog='%s' progShellNew=%d",
               ext, rootName, extExists, shellNewExists, progId.c_str(), progShellNew);
        if (shellNewExists) {
            HKEY key = nullptr;
            if (RegOpenKeyExW(entry.root, shellNew.c_str(), 0, KEY_READ, &key) ==
                ERROR_SUCCESS) {
                for (DWORD i = 0;; ++i) {
                    wchar_t name[256] = {};
                    DWORD nameLength = ARRAYSIZE(name);
                    DWORD type = 0;
                    DWORD size = 0;
                    if (RegEnumValueW(key, i, name, &nameLength, nullptr, &type, nullptr,
                                      &size) != ERROR_SUCCESS) {
                        break;
                    }
                    Wh_Log(L"  value '%s' type=%lu size=%lu", name, type, size);
                }
                RegCloseKey(key);
            }
        }
    }
}

// Builds the template list once. Warm-up prebuilds it; a first open can build
// it synchronously as a one-time fallback.
void EnsureNewTemplates() {
    if (g_newTemplatesReady.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_newTemplatesMutex);
    if (g_newTemplatesReady.load(std::memory_order_relaxed)) {
        return;
    }

    if (g_settings.debugLogging) {
        for (const wchar_t* probe : {L".txt", L".bmp", L".rtf", L".pptx"}) {
            LogShellNewProbe(probe);
        }
    }

    std::vector<NewTemplate> templates = EnumerateShellNewTemplates();
    std::wstring shortcutCommand =
        L"%SystemRoot%\\System32\\rundll32.exe appwiz.cpl,NewLinkHere %1";
    std::vector<NewTemplate> filtered;
    for (NewTemplate& tmpl : templates) {
        if (_wcsicmp(tmpl.extension.c_str(), L".lnk") == 0) {
            if (tmpl.kind == NewTemplate::Kind::Command && !tmpl.command.empty()) {
                shortcutCommand = tmpl.command;
            }
            continue;
        }
        filtered.push_back(std::move(tmpl));
    }

    // "Compressed (zipped) Folder" is provided by the zip folder extension,
    // not always by a ShellNew key. Add it ourselves when the registry has no
    // .zip template: an empty ZIP end-of-central-directory record.
    bool hasZip = false;
    for (const NewTemplate& tmpl : filtered) {
        if (_wcsicmp(tmpl.extension.c_str(), L".zip") == 0) {
            hasZip = true;
            break;
        }
    }
    if (!hasZip) {
        NewTemplate zip;
        zip.kind = NewTemplate::Kind::Data;
        zip.displayName = L"Compressed (zipped) Folder";
        zip.extension = L".zip";
        zip.data = {0x50, 0x4B, 0x05, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
        filtered.push_back(std::move(zip));
    }

    // Windows 11 registers some built-in New items through the AppX/MRT
    // system instead of ShellNew; add the standard Windows types when the
    // registry provides nothing for them.
    struct BuiltinTemplate {
        const wchar_t* extension;
        const wchar_t* displayName;
        NewTemplate::Kind kind;
        const char* data;  // optional payload
    };
    const BuiltinTemplate kBuiltins[] = {
        {L".txt", L"Text Document", NewTemplate::Kind::NullFile, nullptr},
        {L".bmp", L"Bitmap image", NewTemplate::Kind::NullFile, nullptr},
        {L".rtf", L"Rich Text Document", NewTemplate::Kind::Data, "{\\rtf1}"},
    };
    for (const BuiltinTemplate& builtin : kBuiltins) {
        bool present = false;
        for (const NewTemplate& tmpl : filtered) {
            if (_wcsicmp(tmpl.extension.c_str(), builtin.extension) == 0) {
                present = true;
                break;
            }
        }
        if (present) {
            continue;
        }
        NewTemplate tmpl;
        tmpl.kind = builtin.kind;
        tmpl.displayName = builtin.displayName;
        tmpl.extension = builtin.extension;
        if (builtin.data) {
            tmpl.data.assign(builtin.data, builtin.data + strlen(builtin.data));
        }
        filtered.push_back(std::move(tmpl));
    }

    std::sort(filtered.begin(), filtered.end(),
              [](const NewTemplate& a, const NewTemplate& b) {
                  return _wcsicmp(a.displayName.c_str(), b.displayName.c_str()) < 0;
              });

    std::vector<NewTemplate> built;
    NewTemplate folder;
    folder.kind = NewTemplate::Kind::Folder;
    folder.displayName = L"Folder";
    built.push_back(std::move(folder));
    NewTemplate shortcut;
    shortcut.kind = NewTemplate::Kind::Shortcut;
    shortcut.displayName = L"Shortcut";
    shortcut.command = ExpandEnv(shortcutCommand);
    built.push_back(std::move(shortcut));
    for (NewTemplate& tmpl : filtered) {
        built.push_back(std::move(tmpl));
    }

    g_shortcutCommand = std::move(shortcutCommand);
    g_newTemplates = std::move(built);
    g_newTemplatesReady.store(true, std::memory_order_release);

    if (g_settings.debugLogging) {
        for (const NewTemplate& tmpl : g_newTemplates) {
            Wh_Log(L"New template: ext='%s' name='%s' kind=%d", tmpl.extension.c_str(),
                   tmpl.displayName.c_str(), static_cast<int>(tmpl.kind));
        }
    }
}

#ifdef CMO_TESTING
void ResetNewTemplatesForTesting() {
    std::lock_guard<std::mutex> lock(g_newTemplatesMutex);
    g_newTemplates.clear();
    g_newTemplatesReady.store(false, std::memory_order_relaxed);
}
#endif

void BuildNewMenuChildren(std::vector<MenuItem>& children, uint32_t& nextId) {
    // Never build templates on the open path: the warm-up thread prebuilds
    // them, and blocking on its registry walk can stall the menu right after
    // Explorer starts. New appears once the list is ready.
    if (!g_newTemplatesReady.load(std::memory_order_acquire)) {
        return;
    }
    for (size_t i = 0; i < g_newTemplates.size(); ++i) {
        const NewTemplate& tmpl = g_newTemplates[i];
        MenuItem item{};
        item.id = nextId++;
        item.kind = ItemKind::Command;
        item.action = ActionKind::NewItem;
        item.label = tmpl.displayName;
        item.newIndex = static_cast<uint32_t>(i);
        if (tmpl.kind == NewTemplate::Kind::Folder) {
            item.iconRef = L"@ext:folder";
        } else if (tmpl.kind == NewTemplate::Kind::Shortcut) {
            item.iconRef = L"@ext:.lnk";
        } else {
            item.iconRef = L"@ext:" + tmpl.extension;
        }
        children.push_back(std::move(item));
    }
}

// Returns a path in `directory` that does not exist yet, using Windows'
// "New folder (2)" naming pattern.
std::wstring MakeUniquePath(const std::wstring& directory, const std::wstring& baseName,
                            const std::wstring& extension) {
    std::wstring candidate = directory + L"\\" + baseName + extension;
    if (GetFileAttributesW(candidate.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return candidate;
    }
    for (int i = 2; i < 1000; ++i) {
        candidate = directory + L"\\" + baseName + L" (" + std::to_wstring(i) + L")" +
                    extension;
        if (GetFileAttributesW(candidate.c_str()) == INVALID_FILE_ATTRIBUTES) {
            return candidate;
        }
    }
    return L"";
}

// Creates one New item in `directory`; `createdPath` is empty for commands
// that create their item themselves (the shortcut wizard).
bool CreateNewItemInFolder(const NewTemplate& tmpl, const std::wstring& directory,
                           std::wstring& createdPath) {
    createdPath.clear();
    if (directory.empty()) {
        return false;
    }

    if (tmpl.kind == NewTemplate::Kind::Folder) {
        const std::wstring path = MakeUniquePath(directory, L"New folder", L"");
        if (path.empty() || !CreateDirectoryW(path.c_str(), nullptr)) {
            return false;
        }
        createdPath = path;
        return true;
    }

    if (tmpl.kind == NewTemplate::Kind::Shortcut ||
        tmpl.kind == NewTemplate::Kind::Command) {
        std::wstring command = ExpandEnv(tmpl.command);
        const size_t placeholder = command.find(L"%1");
        if (placeholder != std::wstring::npos) {
            command.replace(placeholder, 2, directory);
        }
        STARTUPINFOW si = {};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi = {};
        std::vector<wchar_t> mutableCommand(command.begin(), command.end());
        mutableCommand.push_back(L'\0');
        const BOOL ok = CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr,
                                       FALSE, 0, nullptr, directory.c_str(), &si, &pi);
        if (ok) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
        return ok != FALSE;
    }

    const std::wstring baseName = L"New " + tmpl.displayName;
    const std::wstring path = MakeUniquePath(directory, baseName, tmpl.extension);
    if (path.empty()) {
        return false;
    }

    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    if (tmpl.kind == NewTemplate::Kind::Data && !tmpl.data.empty()) {
        DWORD written = 0;
        WriteFile(file, tmpl.data.data(), static_cast<DWORD>(tmpl.data.size()), &written,
                  nullptr);
    }
    CloseHandle(file);

    if (tmpl.kind == NewTemplate::Kind::FileName && !tmpl.fileName.empty()) {
        if (!CopyFileW(tmpl.fileName.c_str(), path.c_str(), FALSE)) {
            DeleteFileW(path.c_str());
            return false;
        }
    }
    createdPath = path;
    return true;
}

// Logs any unlabeled item with its full descriptor so the extension behavior
// can be identified from a single run.
void DumpSuspiciousItems(const std::vector<MenuItem>& items, int depth) {
    for (const MenuItem& item : items) {
        if (item.kind != ItemKind::Separator && item.label.empty()) {
            Wh_Log(L"[suspicious d%d] kind=%d action=%d flags=%04X offset=%u "
                   L"verb='%s' children=%zu",
                   depth, static_cast<int>(item.kind), static_cast<int>(item.action),
                   item.flags, item.verbOffset, item.canonicalVerb.c_str(),
                   item.children.size());
        }
        DumpSuspiciousItems(item.children, depth + 1);
    }
}

// ===========================================================================
// [CMO:CustomConfig] v2 config file structures and parser.
// ===========================================================================

enum class AnimationKind : uint8_t { None, Fade, Slide };
enum class MarkerStyle : uint8_t { Dot, Check, Bar, None };
enum class FontWeightKind : uint8_t { Normal, Semibold, Bold };
enum class FontStyleKind : uint8_t { Normal, Italic };
enum class AcceleratorMode : uint8_t { Underline, Strip, Raw };

struct CornerRadii {
    int topLeft = 0;
    int topRight = 0;
    int bottomRight = 0;
    int bottomLeft = 0;
};

struct Appearance {
    uint32_t background = 0xF01E1E1E;
    bool blur = true;
    int blurStrength = 12;
    int cornerRadius = 8;
    uint32_t border = 0x22FFFFFF;
    int borderWidth = 1;
    bool shadow = true;
    int shadowSize = 12;
    std::wstring fontFace = L"Segoe UI";
    float fontSize = 9.0f;
    int itemHeight = 28;
    int iconSize = 16;
    int padding = 6;
    uint32_t separator = 0x18FFFFFF;
    uint32_t hoverBackground = 0x14FFFFFF;
    uint32_t pressedBackground = 0x22FFFFFF;
    uint32_t textColor = 0xFFFFFFFF;
    uint32_t disabledTextColor = 0x66FFFFFF;
    uint32_t submenuArrow = 0x99FFFFFF;
    AnimationKind animation = AnimationKind::None;
    int animationDuration = 120;
    int verticalPadding = 4;
    int minWidth = 0;
    int maxWidth = 0;
    int itemPadding = -1;
    int separatorSpacing = 0;
    int markerWidth = 14;
    FontWeightKind fontWeight = FontWeightKind::Normal;
    FontStyleKind fontStyle = FontStyleKind::Normal;
    CornerRadii cornerRadii;
    bool hasCornerRadii = false;
    int shadowOpacity = 120;
    int shadowBlur = 12;
    MarkerStyle marker = MarkerStyle::Dot;
    uint32_t markerColor = 0xFFFFFFFF;
    bool hasMarkerColor = false;
    uint32_t headerColor = 0x66FFFFFF;
    bool hasHeaderColor = false;
    AcceleratorMode acceleratorMode = AcceleratorMode::Underline;
};

struct ConfigParseError {
    int line = 0;
    std::wstring message;
};

enum class PredicateField : uint8_t { Label, Verb, Ext, Scope, Multi, ThirdParty };

struct Predicate {
    PredicateField field = PredicateField::Label;
    std::vector<std::wstring> values;
};

struct PredicateExpr {
    std::vector<Predicate> all;
};

enum class RuleKind : uint8_t { Hide, Keep, Move };

struct Rule {
    RuleKind kind = RuleKind::Hide;
    PredicateExpr match;
    std::wstring destination;
};

enum class RunAs : uint8_t { None, Admin };
enum class ShowWindow : uint8_t { Normal, Maximized, Minimized, Hidden };
enum class CommandSeparator : uint8_t { None, Before, After };
enum class CommandType : uint8_t { Command, Separator, Header };
enum class SubmenuPositionKind : uint8_t { Top, Bottom, After, Before };

struct CustomCommand {
    std::wstring label;
    std::wstring command;
    CommandType type = CommandType::Command;
    BuiltinAction action = BuiltinAction::None;
    std::wstring workingDir;
    std::wstring iconRef;
    PredicateExpr match;
    RunAs runAs = RunAs::None;
    ShowWindow showWindow = ShowWindow::Normal;
    CommandSeparator separator = CommandSeparator::None;
    std::wstring menuPath;
};

struct CustomSubmenu {
    std::wstring name;
    std::wstring iconRef;
    SubmenuPositionKind position = SubmenuPositionKind::Bottom;
    std::wstring positionLabel;
    PredicateExpr match;
};

struct ItemOverride {
    PredicateExpr match;
    std::wstring iconRef;
    std::wstring label;
    bool hasIcon = false;
    bool hasLabel = false;
    bool hasMarker = false;
    MarkerStyle marker = MarkerStyle::Dot;
};

struct RulesConfig {
    Appearance appearance;
    Appearance lightAppearance;
    Appearance darkAppearance;
    bool hasLightAppearance = false;
    bool hasDarkAppearance = false;
    std::vector<Rule> rules;
    std::vector<CustomCommand> commands;
    std::vector<CustomSubmenu> submenus;
    std::vector<ItemOverride> overrides;
    int schemaVersion = 0;
    uint64_t revision = 0;
};

std::wstring ToLowerCopy(const std::wstring& text) {
    std::wstring lower = text;
    std::transform(lower.begin(), lower.end(), lower.begin(), towlower);
    return lower;
}

bool ParseColor(const std::wstring& text, uint32_t& argb) {
    const std::wstring trimmed = TrimWhitespace(text);
    if (trimmed.size() != 7 && trimmed.size() != 9) {
        return false;
    }
    if (trimmed[0] != L'#') {
        return false;
    }
    uint32_t value = 0;
    for (size_t i = 1; i < trimmed.size(); ++i) {
        const wchar_t c = trimmed[i];
        uint32_t digit = 0;
        if (c >= L'0' && c <= L'9') {
            digit = static_cast<uint32_t>(c - L'0');
        } else if (c >= L'a' && c <= L'f') {
            digit = 10u + static_cast<uint32_t>(c - L'a');
        } else if (c >= L'A' && c <= L'F') {
            digit = 10u + static_cast<uint32_t>(c - L'A');
        } else {
            return false;
        }
        value = (value << 4) | digit;
    }
    argb = trimmed.size() == 7 ? (0xFF000000u | value) : value;
    return true;
}

bool ParseBool(const std::wstring& text, bool& value) {
    const std::wstring trimmed = ToLowerCopy(TrimWhitespace(text));
    if (trimmed == L"true" || trimmed == L"1" || trimmed == L"yes") {
        value = true;
        return true;
    }
    if (trimmed == L"false" || trimmed == L"0" || trimmed == L"no") {
        value = false;
        return true;
    }
    return false;
}

bool ParseIntValue(const std::wstring& text, int& value) {
    const std::wstring trimmed = TrimWhitespace(text);
    if (trimmed.empty()) {
        return false;
    }
    wchar_t* end = nullptr;
    const long parsed = wcstol(trimmed.c_str(), &end, 10);
    if (end == trimmed.c_str() || *end != L'\0') {
        return false;
    }
    value = static_cast<int>(parsed);
    return true;
}

bool ParseFont(const std::wstring& text, std::wstring& face, float& size) {
    const size_t comma = text.find(L',');
    if (comma == std::wstring::npos) {
        return false;
    }
    face = TrimWhitespace(text.substr(0, comma));
    if (face.size() >= 2 && face.front() == L'"' && face.back() == L'"') {
        face = face.substr(1, face.size() - 2);
    }
    const std::wstring sizeText = TrimWhitespace(text.substr(comma + 1));
    if (face.empty() || sizeText.empty()) {
        return false;
    }
    wchar_t* end = nullptr;
    const double parsed = wcstod(sizeText.c_str(), &end);
    if (end == sizeText.c_str() || *end != L'\0' || parsed <= 0.0) {
        return false;
    }
    size = static_cast<float>(parsed);
    return true;
}

bool ParseAnimationKind(const std::wstring& text, AnimationKind& kind) {
    const std::wstring lower = ToLowerCopy(TrimWhitespace(text));
    if (lower == L"none") {
        kind = AnimationKind::None;
        return true;
    }
    if (lower == L"fade") {
        kind = AnimationKind::Fade;
        return true;
    }
    if (lower == L"slide") {
        kind = AnimationKind::Slide;
        return true;
    }
    return false;
}

bool ParseMarkerStyle(const std::wstring& text, MarkerStyle& style) {
    const std::wstring lower = ToLowerCopy(TrimWhitespace(text));
    if (lower == L"dot") {
        style = MarkerStyle::Dot;
        return true;
    }
    if (lower == L"check") {
        style = MarkerStyle::Check;
        return true;
    }
    if (lower == L"bar") {
        style = MarkerStyle::Bar;
        return true;
    }
    if (lower == L"none") {
        style = MarkerStyle::None;
        return true;
    }
    return false;
}

bool ParseFontWeight(const std::wstring& text, FontWeightKind& weight) {
    const std::wstring lower = ToLowerCopy(TrimWhitespace(text));
    if (lower == L"normal") {
        weight = FontWeightKind::Normal;
        return true;
    }
    if (lower == L"semibold") {
        weight = FontWeightKind::Semibold;
        return true;
    }
    if (lower == L"bold") {
        weight = FontWeightKind::Bold;
        return true;
    }
    return false;
}

bool ParseFontStyle(const std::wstring& text, FontStyleKind& style) {
    const std::wstring lower = ToLowerCopy(TrimWhitespace(text));
    if (lower == L"normal") {
        style = FontStyleKind::Normal;
        return true;
    }
    if (lower == L"italic") {
        style = FontStyleKind::Italic;
        return true;
    }
    return false;
}

bool ParseAcceleratorMode(const std::wstring& text, AcceleratorMode& mode) {
    const std::wstring lower = ToLowerCopy(TrimWhitespace(text));
    if (lower == L"underline") {
        mode = AcceleratorMode::Underline;
        return true;
    }
    if (lower == L"strip") {
        mode = AcceleratorMode::Strip;
        return true;
    }
    if (lower == L"raw") {
        mode = AcceleratorMode::Raw;
        return true;
    }
    return false;
}

bool ParseBoundedInt(const std::wstring& text, int& value, int minValue,
                     int maxValue) {
    return ParseIntValue(text, value) && value >= minValue && value <= maxValue;
}

bool ParseCornerRadii(const std::wstring& value, CornerRadii& radii) {
    int values[4] = {};
    size_t start = 0;
    int count = 0;
    while (start <= value.size() && count < 4) {
        const size_t comma = value.find(L',', start);
        const std::wstring part = TrimWhitespace(
            value.substr(start, comma == std::wstring::npos ? std::wstring::npos
                                                            : comma - start));
        if (part.empty() ||
            !ParseBoundedInt(part, values[count], 0, 256)) {
            return false;
        }
        ++count;
        if (comma == std::wstring::npos) {
            break;
        }
        start = comma + 1;
    }
    if (count != 4) {
        return false;
    }
    // Reject a trailing extra value.
    if (value.find(L',', start) != std::wstring::npos) {
        return false;
    }
    radii.topLeft = values[0];
    radii.topRight = values[1];
    radii.bottomRight = values[2];
    radii.bottomLeft = values[3];
    return true;
}

enum class SettingType : uint8_t { Bool, Int, Color, Font, Enum, IntList };

// The build's schema version; bump when a row is added.
constexpr int kConfigSchemaVersion = 1;

struct ConfigSchemaEntry {
    const wchar_t* section;
    const wchar_t* key;
    SettingType type;
    const wchar_t* defaultValue;
    const wchar_t* validValues;  // Enum/IntList values, else nullptr
    int minValue;                // Int only
    int maxValue;                // Int only
    const wchar_t* description;
    int sinceVersion;
};

const ConfigSchemaEntry kAppearanceSchema[] = {
    {L"appearance", L"background", SettingType::Color, L"#F01E1E1E", nullptr, 0, 0,
     L"Panel background color (#AARRGGBB); also the blur tint.", 1},
    {L"appearance", L"blur", SettingType::Bool, L"true", nullptr, 0, 0,
     L"Blur the screen behind the menu.", 1},
    {L"appearance", L"blurstrength", SettingType::Int, L"12", nullptr, 0, 64,
     L"Blur strength.", 1},
    {L"appearance", L"cornerradius", SettingType::Int, L"8", nullptr, 0, 256,
     L"Corner radius in pixels.", 1},
    {L"appearance", L"border", SettingType::Color, L"#22FFFFFF", nullptr, 0, 0,
     L"Border color.", 1},
    {L"appearance", L"borderwidth", SettingType::Int, L"1", nullptr, 0, 64,
     L"Border width in pixels (0 hides it).", 1},
    {L"appearance", L"shadow", SettingType::Bool, L"true", nullptr, 0, 0,
     L"Draw the drop shadow.", 1},
    {L"appearance", L"shadowsize", SettingType::Int, L"12", nullptr, 0, 64,
     L"Shadow spread in pixels.", 1},
    {L"appearance", L"font", SettingType::Font, L"Segoe UI, 9", nullptr, 0, 0,
     L"Text font face and size.", 1},
    {L"appearance", L"itemheight", SettingType::Int, L"28", nullptr, 1, 256,
     L"Item height in pixels.", 1},
    {L"appearance", L"iconsize", SettingType::Int, L"16", nullptr, 1, 256,
     L"Icon size in pixels.", 1},
    {L"appearance", L"padding", SettingType::Int, L"6", nullptr, 0, 256,
     L"Base padding inside the panel.", 1},
    {L"appearance", L"separator", SettingType::Color, L"#18FFFFFF", nullptr, 0, 0,
     L"Separator line color.", 1},
    {L"appearance", L"hoverbackground", SettingType::Color, L"#14FFFFFF", nullptr, 0,
     0, L"Hovered item background.", 1},
    {L"appearance", L"pressedbackground", SettingType::Color, L"#22FFFFFF", nullptr,
     0, 0, L"Pressed item background.", 1},
    {L"appearance", L"textcolor", SettingType::Color, L"#FFFFFFFF", nullptr, 0, 0,
     L"Item text color.", 1},
    {L"appearance", L"disabledtextcolor", SettingType::Color, L"#66FFFFFF", nullptr,
     0, 0, L"Disabled item text color.", 1},
    {L"appearance", L"submenuarrow", SettingType::Color, L"#99FFFFFF", nullptr, 0,
     0, L"Submenu arrow color.", 1},
    {L"appearance", L"animation", SettingType::Enum, L"none", L"none|fade|slide", 0,
     0, L"Menu open/close animation.", 1},
    {L"appearance", L"animationduration", SettingType::Int, L"120", nullptr, 0,
     10000, L"Animation duration in milliseconds.", 1},
    {L"appearance", L"verticalpadding", SettingType::Int, L"4", nullptr, 0, 256,
     L"Padding above and below the items.", 2},
    {L"appearance", L"minwidth", SettingType::Int, L"0", nullptr, 0, 4096,
     L"Minimum panel width (0 = automatic).", 2},
    {L"appearance", L"maxwidth", SettingType::Int, L"0", nullptr, 0, 4096,
     L"Maximum panel width (0 = unlimited).", 2},
    {L"appearance", L"itempadding", SettingType::Int, L"6", nullptr, 0, 256,
     L"Horizontal inset of item content; defaults to padding.", 2},
    {L"appearance", L"separatorspacing", SettingType::Int, L"0", nullptr, 0, 256,
     L"Extra space above and below separators.", 2},
    {L"appearance", L"markerwidth", SettingType::Int, L"14", nullptr, 0, 256,
     L"Width of the selection marker column.", 2},
    {L"appearance", L"fontweight", SettingType::Enum, L"normal",
     L"normal|semibold|bold", 0, 0, L"Text weight.", 2},
    {L"appearance", L"fontstyle", SettingType::Enum, L"normal", L"normal|italic", 0,
     0, L"Text style.", 2},
    {L"appearance", L"cornerradii", SettingType::IntList, L"2, 4, 6, 8",
     L"tl,tr,br,bl", 0, 0, L"Per-corner radii; overrides cornerRadius.", 2},
    {L"appearance", L"shadowopacity", SettingType::Int, L"120", nullptr, 0, 255,
     L"Shadow alpha (0-255).", 2},
    {L"appearance", L"shadowblur", SettingType::Int, L"12", nullptr, 0, 64,
     L"Shadow blur radius in pixels.", 2},
    {L"appearance", L"marker", SettingType::Enum, L"dot", L"dot|check|bar|none", 0,
     0, L"Selection marker style.", 2},
    {L"appearance", L"markercolor", SettingType::Color, L"#FFFFFFFF", nullptr, 0, 0,
     L"Marker color; defaults to textColor.", 2},
    {L"appearance", L"headercolor", SettingType::Color, L"#66FFFFFF", nullptr, 0, 0,
     L"Header text color; defaults to disabledTextColor.", 2},
    {L"appearance", L"showaccelerators", SettingType::Enum, L"underline",
     L"underline|strip|raw", 0, 0, L"Mnemonic handling.", 2},
};

const ConfigSchemaEntry* SchemaFind(const std::wstring& key) {
    for (const ConfigSchemaEntry& entry : kAppearanceSchema) {
        if (_wcsicmp(entry.key, key.c_str()) == 0) {
            return &entry;
        }
    }
    return nullptr;
}

bool SchemaHasKey(const std::wstring& key) {
    return SchemaFind(key) != nullptr;
}

std::wstring AppearanceErrorFor(const std::wstring& key) {
    std::wstring message = L"invalid value for '" + key + L"'";
    const ConfigSchemaEntry* entry = SchemaFind(key);
    if (entry) {
        if (entry->validValues) {
            message += L": expected ";
            message += entry->validValues;
        } else if (entry->type == SettingType::Int) {
            message += L": expected an integer " + std::to_wstring(entry->minValue) +
                       L"-" + std::to_wstring(entry->maxValue);
        }
    }
    return message;
}

// Applies one appearance key/value; false means invalid key or value.
bool ApplyAppearanceValue(Appearance& appearance, const std::wstring& key,
                          const std::wstring& value) {
    if (!SchemaHasKey(key)) {
        return false;
    }
    if (key == L"background") return ParseColor(value, appearance.background);
    if (key == L"blur") return ParseBool(value, appearance.blur);
    if (key == L"blurstrength") {
        return ParseBoundedInt(value, appearance.blurStrength, 0, 64);
    }
    if (key == L"cornerradius") {
        return ParseBoundedInt(value, appearance.cornerRadius, 0, 256);
    }
    if (key == L"border") return ParseColor(value, appearance.border);
    if (key == L"borderwidth") {
        return ParseBoundedInt(value, appearance.borderWidth, 0, 64);
    }
    if (key == L"shadow") return ParseBool(value, appearance.shadow);
    if (key == L"shadowsize") {
        return ParseBoundedInt(value, appearance.shadowSize, 0, 64);
    }
    if (key == L"font") return ParseFont(value, appearance.fontFace, appearance.fontSize);
    if (key == L"itemheight") {
        return ParseBoundedInt(value, appearance.itemHeight, 1, 256);
    }
    if (key == L"iconsize") {
        return ParseBoundedInt(value, appearance.iconSize, 1, 256);
    }
    if (key == L"padding") {
        return ParseBoundedInt(value, appearance.padding, 0, 256);
    }
    if (key == L"separator") return ParseColor(value, appearance.separator);
    if (key == L"hoverbackground") return ParseColor(value, appearance.hoverBackground);
    if (key == L"pressedbackground") return ParseColor(value, appearance.pressedBackground);
    if (key == L"textcolor") return ParseColor(value, appearance.textColor);
    if (key == L"disabledtextcolor") return ParseColor(value, appearance.disabledTextColor);
    if (key == L"submenuarrow") return ParseColor(value, appearance.submenuArrow);
    if (key == L"animation") return ParseAnimationKind(value, appearance.animation);
    if (key == L"animationduration") {
        return ParseBoundedInt(value, appearance.animationDuration, 0, 10000);
    }
    if (key == L"verticalpadding") {
        return ParseBoundedInt(value, appearance.verticalPadding, 0, 256);
    }
    if (key == L"minwidth") {
        return ParseBoundedInt(value, appearance.minWidth, 0, 4096);
    }
    if (key == L"maxwidth") {
        return ParseBoundedInt(value, appearance.maxWidth, 0, 4096);
    }
    if (key == L"itempadding") {
        return ParseBoundedInt(value, appearance.itemPadding, 0, 256);
    }
    if (key == L"separatorspacing") {
        return ParseBoundedInt(value, appearance.separatorSpacing, 0, 256);
    }
    if (key == L"markerwidth") {
        return ParseBoundedInt(value, appearance.markerWidth, 0, 256);
    }
    if (key == L"fontweight") {
        return ParseFontWeight(value, appearance.fontWeight);
    }
    if (key == L"fontstyle") {
        return ParseFontStyle(value, appearance.fontStyle);
    }
    if (key == L"cornerradii") {
        if (!ParseCornerRadii(value, appearance.cornerRadii)) {
            return false;
        }
        appearance.hasCornerRadii = true;
        return true;
    }
    if (key == L"shadowopacity") {
        return ParseBoundedInt(value, appearance.shadowOpacity, 0, 255);
    }
    if (key == L"shadowblur") {
        return ParseBoundedInt(value, appearance.shadowBlur, 0, 64);
    }
    if (key == L"marker") {
        return ParseMarkerStyle(value, appearance.marker);
    }
    if (key == L"markercolor") {
        if (!ParseColor(value, appearance.markerColor)) {
            return false;
        }
        appearance.hasMarkerColor = true;
        return true;
    }
    if (key == L"headercolor") {
        if (!ParseColor(value, appearance.headerColor)) {
            return false;
        }
        appearance.hasHeaderColor = true;
        return true;
    }
    if (key == L"showaccelerators") {
        return ParseAcceleratorMode(value, appearance.acceleratorMode);
    }
    return false;
}

std::wstring GenerateDefaultConfigText() {
    std::wstring text;
    text += L"; Context Menu Overhaul configuration (schema ";
    text += std::to_wstring(kConfigSchemaVersion);
    text += L")\n";
    text += L"; UTF-8. Reloaded when a menu opens. Comments start with ';' or '#'.\n";
    text += L"; Errors are logged as menu.ini:<line>: <message>; the last good\n";
    text += L"; configuration stays in effect. Uncomment a line to override its default.\n";
    text += L"\n";

    const wchar_t* kSections[] = {L"appearance", L"appearance.light",
                                  L"appearance.dark"};
    for (const wchar_t* section : kSections) {
        text += L"[";
        text += section;
        text += L"]\n";
        if (wcscmp(section, L"appearance") != 0) {
            text += L"; Overrides for this theme; keys not listed use [appearance].\n";
        }
        for (const ConfigSchemaEntry& entry : kAppearanceSchema) {
            if (wcscmp(entry.section, L"appearance") != 0) {
                continue;
            }
            text += L"; ";
            text += entry.description;
            text += L" (default: ";
            text += entry.defaultValue;
            text += L")\n; ";
            text += entry.key;
            text += L" = ";
            text += entry.defaultValue;
            text += L"\n";
        }
        text += L"\n";
    }

    text += L"[rules]\n"
            L"; hide = label:\"Cast to Device\"\n"
            L"; keep = label:Share\n"
            L"; move = thirdParty -> \"More options\"\n"
            L"\n"
            L"[command \"Open in VS Code\"]\n"
            L"; command = code.exe \"%1\"\n"
            L"; workingDir = %dir%\n"
            L"; match.ext = .cs, .cpp\n"
            L"; menu = Tools\n"
            L"\n"
            L"[submenu \"Tools\"]\n"
            L"; icon = @glyph:E712\n"
            L"; position = top\n"
            L"\n"
            L"[item \"TortoiseSVN*\"]\n"
            L"; label = SVN\n"
            L"; icon = C:\\Tools\\svn.ico,0\n"
            L"; marker = bar\n"
            L"\n"
            L"[meta]\n"
            L"schemaVersion = ";
    text += std::to_wstring(kConfigSchemaVersion);
    text += L"\n";
    return text;
}

// Declared here and defined in [CMO:RulesEngine] below.
bool ParsePredicateExpr(const std::wstring& text, PredicateExpr& out,
                        std::wstring& error);

std::wstring ExtractQuoted(const std::wstring& text) {
    const std::wstring trimmed = TrimWhitespace(text);
    if (trimmed.size() < 2 || trimmed.front() != L'"' || trimmed.back() != L'"') {
        return L"";
    }
    return trimmed.substr(1, trimmed.size() - 2);
}

// Strips a ';' comment that is not inside a quoted value.
std::wstring StripInlineComment(const std::wstring& line) {
    bool inQuotes = false;
    for (size_t i = 0; i < line.size(); ++i) {
        if (line[i] == L'"') {
            inQuotes = !inQuotes;
        } else if (line[i] == L';' && !inQuotes) {
            return line.substr(0, i);
        }
    }
    return line;
}

bool AppendMatchValue(PredicateExpr& expr, const std::wstring& field,
                      const std::wstring& value) {
    if (field == L"multi" || field == L"thirdparty") {
        bool enabled = true;
        if (!ParseBool(value, enabled)) {
            return false;
        }
        if (enabled) {
            Predicate pred;
            pred.field = field == L"multi" ? PredicateField::Multi
                                           : PredicateField::ThirdParty;
            expr.all.push_back(std::move(pred));
        }
        return true;
    }
    if (field != L"label" && field != L"verb" && field != L"ext" &&
        field != L"scope") {
        return false;
    }
    std::wstring error;
    PredicateExpr parsed;
    if (!ParsePredicateExpr(field + L":" + value, parsed, error)) {
        return false;
    }
    for (Predicate& pred : parsed.all) {
        expr.all.push_back(std::move(pred));
    }
    return true;
}

bool ApplyCommandValue(CustomCommand& command, const std::wstring& key,
                       const std::wstring& value) {
    if (key == L"command") {
        command.command = value;
        return true;
    }
    if (key == L"workingdir") {
        command.workingDir = value;
        return true;
    }
    if (key == L"icon") {
        command.iconRef = value;
        return true;
    }
    if (key == L"menu") {
        command.menuPath = value;
        return true;
    }
    if (key == L"runas") {
        const std::wstring lower = ToLowerCopy(value);
        if (lower == L"none") {
            command.runAs = RunAs::None;
            return true;
        }
        if (lower == L"admin") {
            command.runAs = RunAs::Admin;
            return true;
        }
        return false;
    }
    if (key == L"showwindow") {
        const std::wstring lower = ToLowerCopy(value);
        if (lower == L"normal") {
            command.showWindow = ShowWindow::Normal;
            return true;
        }
        if (lower == L"maximized") {
            command.showWindow = ShowWindow::Maximized;
            return true;
        }
        if (lower == L"minimized") {
            command.showWindow = ShowWindow::Minimized;
            return true;
        }
        if (lower == L"hidden") {
            command.showWindow = ShowWindow::Hidden;
            return true;
        }
        return false;
    }
    if (key == L"separator") {
        const std::wstring lower = ToLowerCopy(value);
        if (lower == L"none") {
            command.separator = CommandSeparator::None;
            return true;
        }
        if (lower == L"before") {
            command.separator = CommandSeparator::Before;
            return true;
        }
        if (lower == L"after") {
            command.separator = CommandSeparator::After;
            return true;
        }
        return false;
    }
    if (key == L"action") {
        const std::wstring lower = ToLowerCopy(TrimWhitespace(value));
        if (lower == L"run") {
            command.action = BuiltinAction::None;
            return true;
        }
        if (lower == L"copypath") {
            command.action = BuiltinAction::CopyPath;
            return true;
        }
        if (lower == L"opennewwindow") {
            command.action = BuiltinAction::OpenNewWindow;
            return true;
        }
        if (lower == L"properties") {
            command.action = BuiltinAction::Properties;
            return true;
        }
        return false;
    }
    if (key == L"type") {
        const std::wstring lower = ToLowerCopy(TrimWhitespace(value));
        if (lower == L"command") {
            command.type = CommandType::Command;
            return true;
        }
        if (lower == L"separator") {
            command.type = CommandType::Separator;
            return true;
        }
        if (lower == L"header") {
            command.type = CommandType::Header;
            return true;
        }
        return false;
    }
    if (key.rfind(L"match.", 0) == 0) {
        return AppendMatchValue(command.match, key.substr(6), value);
    }
    return false;
}

bool ApplySubmenuValue(CustomSubmenu& submenu, const std::wstring& key,
                       const std::wstring& value) {
    if (key == L"icon") {
        submenu.iconRef = value;
        return true;
    }
    if (key == L"position") {
        const std::wstring lower = ToLowerCopy(TrimWhitespace(value));
        if (lower == L"top") {
            submenu.position = SubmenuPositionKind::Top;
            return true;
        }
        if (lower == L"bottom") {
            submenu.position = SubmenuPositionKind::Bottom;
            return true;
        }
        if (lower.rfind(L"after:", 0) == 0) {
            submenu.position = SubmenuPositionKind::After;
            submenu.positionLabel = ExtractQuoted(value.substr(6));
            return !submenu.positionLabel.empty();
        }
        if (lower.rfind(L"before:", 0) == 0) {
            submenu.position = SubmenuPositionKind::Before;
            submenu.positionLabel = ExtractQuoted(value.substr(7));
            return !submenu.positionLabel.empty();
        }
        return false;
    }
    if (key.rfind(L"match.", 0) == 0) {
        return AppendMatchValue(submenu.match, key.substr(6), value);
    }
    return false;
}

bool ApplyItemOverrideValue(ItemOverride& override, const std::wstring& key,
                            const std::wstring& value) {
    if (key == L"icon") {
        override.iconRef = value;
        override.hasIcon = true;
        return true;
    }
    if (key == L"label") {
        override.label = value;
        override.hasLabel = true;
        return true;
    }
    if (key == L"marker") {
        if (!ParseMarkerStyle(value, override.marker)) {
            return false;
        }
        override.hasMarker = true;
        return true;
    }
    if (key.rfind(L"match.", 0) == 0) {
        return AppendMatchValue(override.match, key.substr(6), value);
    }
    return false;
}

bool ApplyRuleLine(RulesConfig& config, const std::wstring& key,
                   const std::wstring& value, std::wstring& error) {
    RuleKind kind = RuleKind::Hide;
    if (key == L"hide") {
        kind = RuleKind::Hide;
    } else if (key == L"keep") {
        kind = RuleKind::Keep;
    } else if (key == L"move") {
        kind = RuleKind::Move;
    } else {
        error = L"unknown rule '" + key + L"'";
        return false;
    }

    Rule rule;
    rule.kind = kind;
    std::wstring predicateText = value;
    if (kind == RuleKind::Move) {
        const size_t arrow = value.find(L"->");
        if (arrow == std::wstring::npos) {
            error = L"move needs '-> \"Destination\"'";
            return false;
        }
        predicateText = TrimWhitespace(value.substr(0, arrow));
        rule.destination = ExtractQuoted(value.substr(arrow + 2));
        if (rule.destination.empty()) {
            error = L"move needs a quoted destination";
            return false;
        }
    }
    if (!ParsePredicateExpr(predicateText, rule.match, error)) {
        return false;
    }
    config.rules.push_back(std::move(rule));
    return true;
}

bool ParseRulesConfig(const std::wstring& text, RulesConfig& out,
                      std::vector<ConfigParseError>& errors) {
    errors.clear();

    enum class Section : uint8_t {
        None,
        Appearance,
        AppearanceLight,
        AppearanceDark,
        Rules,
        Command,
        Submenu,
        Item,
        Meta,
        Ignored,
    };
    Section section = Section::None;

    RulesConfig config;
    int currentCommand = -1;
    int currentSubmenu = -1;
    int currentOverride = -1;

    std::vector<std::pair<std::wstring, std::wstring>> baseValues;
    std::vector<std::pair<std::wstring, std::wstring>> lightValues;
    std::vector<std::pair<std::wstring, std::wstring>> darkValues;
    bool hasLight = false;
    bool hasDark = false;

    int lineNumber = 0;
    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t newline = text.find(L'\n', pos);
        std::wstring line =
            text.substr(pos, newline == std::wstring::npos ? std::wstring::npos
                                                           : newline - pos);
        pos = newline == std::wstring::npos ? text.size() + 1 : newline + 1;
        ++lineNumber;
        if (!line.empty() && line.back() == L'\r') {
            line.pop_back();
        }

        const std::wstring trimmed = TrimWhitespace(StripInlineComment(line));
        if (trimmed.empty() || trimmed[0] == L';') {
            continue;
        }

        if (trimmed[0] == L'[') {
            if (trimmed.back() != L']') {
                errors.push_back({lineNumber, L"malformed section header"});
                section = Section::Ignored;
                continue;
            }
            const std::wstring rawName =
                TrimWhitespace(trimmed.substr(1, trimmed.size() - 2));
            const std::wstring name = ToLowerCopy(rawName);
            currentCommand = -1;
            currentSubmenu = -1;
            currentOverride = -1;
            if (name == L"appearance") {
                section = Section::Appearance;
            } else if (name == L"appearance.light") {
                section = Section::AppearanceLight;
                hasLight = true;
            } else if (name == L"appearance.dark") {
                section = Section::AppearanceDark;
                hasDark = true;
            } else if (name == L"rules") {
                section = Section::Rules;
            } else if (name.rfind(L"command ", 0) == 0) {
                const std::wstring label = ExtractQuoted(rawName.substr(8));
                if (label.empty()) {
                    errors.push_back(
                        {lineNumber, L"command section needs a quoted label"});
                    section = Section::Ignored;
                } else {
                    config.commands.push_back(CustomCommand{});
                    config.commands.back().label = label;
                    currentCommand = static_cast<int>(config.commands.size()) - 1;
                    section = Section::Command;
                }
            } else if (name.rfind(L"item ", 0) == 0) {
                const std::wstring glob = ExtractQuoted(rawName.substr(5));
                if (glob.empty()) {
                    errors.push_back(
                        {lineNumber, L"item section needs a quoted label glob"});
                    section = Section::Ignored;
                } else {
                    ItemOverride override;
                    Predicate predicate;
                    predicate.field = PredicateField::Label;
                    predicate.values.push_back(NormalizeMenuLabel(glob));
                    override.match.all.push_back(std::move(predicate));
                    config.overrides.push_back(std::move(override));
                    currentOverride =
                        static_cast<int>(config.overrides.size()) - 1;
                    section = Section::Item;
                }
            } else if (name == L"meta") {
                section = Section::Meta;
            } else if (name.rfind(L"submenu ", 0) == 0) {
                const std::wstring submenuName = ExtractQuoted(rawName.substr(8));
                if (submenuName.empty()) {
                    errors.push_back(
                        {lineNumber, L"submenu section needs a quoted name"});
                    section = Section::Ignored;
                } else {
                    config.submenus.push_back(CustomSubmenu{});
                    config.submenus.back().name = submenuName;
                    currentSubmenu = static_cast<int>(config.submenus.size()) - 1;
                    section = Section::Submenu;
                }
            } else {
                errors.push_back({lineNumber, L"unknown section '" + name + L"'"});
                section = Section::Ignored;
            }
            continue;
        }

        const size_t equals = trimmed.find(L'=');
        if (equals == std::wstring::npos) {
            errors.push_back({lineNumber, L"expected key = value"});
            continue;
        }
        const std::wstring key = ToLowerCopy(TrimWhitespace(trimmed.substr(0, equals)));
        const std::wstring value = TrimWhitespace(trimmed.substr(equals + 1));

        if (section == Section::Appearance) {
            Appearance scratch = Appearance{};
            if (!ApplyAppearanceValue(scratch, key, value)) {
                errors.push_back({lineNumber, AppearanceErrorFor(key)});
            } else {
                baseValues.emplace_back(key, value);
            }
        } else if (section == Section::AppearanceLight) {
            Appearance scratch = Appearance{};
            if (!ApplyAppearanceValue(scratch, key, value)) {
                errors.push_back({lineNumber, AppearanceErrorFor(key)});
            } else {
                lightValues.emplace_back(key, value);
            }
        } else if (section == Section::AppearanceDark) {
            Appearance scratch = Appearance{};
            if (!ApplyAppearanceValue(scratch, key, value)) {
                errors.push_back({lineNumber, AppearanceErrorFor(key)});
            } else {
                darkValues.emplace_back(key, value);
            }
        } else if (section == Section::Rules) {
            std::wstring error;
            if (!ApplyRuleLine(config, key, value, error)) {
                errors.push_back({lineNumber, error});
            }
        } else if (section == Section::Command) {
            if (currentCommand < 0 ||
                !ApplyCommandValue(config.commands[currentCommand], key, value)) {
                errors.push_back({lineNumber,
                                  L"invalid command value for '" + key + L"'"});
            }
        } else if (section == Section::Submenu) {
            if (currentSubmenu < 0 ||
                !ApplySubmenuValue(config.submenus[currentSubmenu], key, value)) {
                errors.push_back({lineNumber,
                                  L"invalid submenu value for '" + key + L"'"});
            }
        } else if (section == Section::Item) {
            if (currentOverride < 0 ||
                !ApplyItemOverrideValue(config.overrides[currentOverride], key,
                                        value)) {
                errors.push_back({lineNumber,
                                  L"invalid item value for '" + key + L"'"});
            }
        } else if (section == Section::Meta) {
            if (key == L"schemaversion") {
                if (!ParseBoundedInt(value, config.schemaVersion, 0, 1000)) {
                    errors.push_back(
                        {lineNumber, L"invalid value for 'schemaVersion'"});
                }
            } else {
                errors.push_back(
                    {lineNumber, L"unknown key '" + key + L"' in [meta]"});
            }
        } else if (section == Section::Ignored) {
            // Intentionally ignored.
        } else {
            errors.push_back({lineNumber, L"key outside a section"});
        }
    }

    if (!errors.empty()) {
        return false;
    }

    for (const auto& pair : baseValues) {
        ApplyAppearanceValue(config.appearance, pair.first, pair.second);
    }
    config.hasLightAppearance = hasLight;
    if (hasLight) {
        config.lightAppearance = config.appearance;
        for (const auto& pair : lightValues) {
            ApplyAppearanceValue(config.lightAppearance, pair.first, pair.second);
        }
    }
    config.hasDarkAppearance = hasDark;
    if (hasDark) {
        config.darkAppearance = config.appearance;
        for (const auto& pair : darkValues) {
            ApplyAppearanceValue(config.darkAppearance, pair.first, pair.second);
        }
    }

    out = std::move(config);
    return true;
}

int ReadSchemaVersion(const std::wstring& text) {
    bool inMeta = false;
    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t newline = text.find(L'\n', pos);
        std::wstring line =
            text.substr(pos, newline == std::wstring::npos ? std::wstring::npos
                                                           : newline - pos);
        pos = newline == std::wstring::npos ? text.size() + 1 : newline + 1;
        if (!line.empty() && line.back() == L'\r') {
            line.pop_back();
        }
        const std::wstring trimmed = TrimWhitespace(StripInlineComment(line));
        if (trimmed.empty() || trimmed[0] == L';') {
            continue;
        }
        if (trimmed[0] == L'[') {
            if (trimmed.back() != L']') {
                inMeta = false;
                continue;
            }
            const std::wstring name = ToLowerCopy(
                TrimWhitespace(trimmed.substr(1, trimmed.size() - 2)));
            inMeta = (name == L"meta");
            continue;
        }
        if (!inMeta) {
            continue;
        }
        const size_t equals = trimmed.find(L'=');
        if (equals == std::wstring::npos) {
            continue;
        }
        const std::wstring key =
            ToLowerCopy(TrimWhitespace(trimmed.substr(0, equals)));
        if (key != L"schemaversion") {
            continue;
        }
        int version = 0;
        if (ParseBoundedInt(TrimWhitespace(trimmed.substr(equals + 1)), version, 0,
                            1000)) {
            return version;
        }
        return 0;
    }
    return 0;
}

// ===========================================================================
// [CMO:RulesEngine] v2 predicates and rule matching.
// ===========================================================================

struct ItemContext {
    Scope scope = Scope::Files;
    Shape shape = Shape::Single;
    std::vector<std::wstring> paths;
};

bool GlobMatches(const std::wstring& pattern, const std::wstring& text) {
    size_t p = 0;
    size_t t = 0;
    size_t star = std::wstring::npos;
    size_t mark = 0;
    while (t < text.size()) {
        if (p < pattern.size() && pattern[p] == L'*') {
            star = p++;
            mark = t;
        } else if (p < pattern.size() &&
                   towlower(pattern[p]) == towlower(text[t])) {
            ++p;
            ++t;
        } else if (star != std::wstring::npos) {
            p = star + 1;
            t = ++mark;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == L'*') {
        ++p;
    }
    return p == pattern.size();
}

bool ScopeFromName(const std::wstring& name, Scope& scope) {
    const std::wstring lower = ToLowerCopy(name);
    if (lower == L"files") {
        scope = Scope::Files;
        return true;
    }
    if (lower == L"folders") {
        scope = Scope::Folders;
        return true;
    }
    if (lower == L"background") {
        scope = Scope::Background;
        return true;
    }
    if (lower == L"desktop") {
        scope = Scope::Desktop;
        return true;
    }
    if (lower == L"drive") {
        scope = Scope::Drive;
        return true;
    }
    return false;
}

bool ItemIsThirdParty(const MenuItem& item) {
    return (item.flags & kModelThirdParty) != 0;
}

bool PredicateMatches(const Predicate& pred, const MenuItem& item,
                      const ItemContext& ctx) {
    switch (pred.field) {
        case PredicateField::Label: {
            const std::wstring label = NormalizeMenuLabel(item.label);
            for (const std::wstring& value : pred.values) {
                if (GlobMatches(value, label)) {
                    return true;
                }
            }
            return false;
        }
        case PredicateField::Verb:
            for (const std::wstring& value : pred.values) {
                if (_wcsicmp(value.c_str(), item.canonicalVerb.c_str()) == 0) {
                    return true;
                }
            }
            return false;
        case PredicateField::Ext:
            for (const std::wstring& path : ctx.paths) {
                const size_t dot = path.find_last_of(L'.');
                const size_t slash = path.find_last_of(L"\\/");
                if (dot == std::wstring::npos ||
                    (slash != std::wstring::npos && dot < slash)) {
                    continue;
                }
                const std::wstring ext = path.substr(dot);
                for (const std::wstring& value : pred.values) {
                    if (_wcsicmp(ext.c_str(), value.c_str()) == 0) {
                        return true;
                    }
                }
            }
            return false;
        case PredicateField::Scope:
            for (const std::wstring& value : pred.values) {
                Scope scope = Scope::Other;
                if (ScopeFromName(value, scope) && scope == ctx.scope) {
                    return true;
                }
            }
            return false;
        case PredicateField::Multi:
            return ctx.shape == Shape::Multi;
        case PredicateField::ThirdParty:
            return ItemIsThirdParty(item);
    }
    return false;
}

bool PredicateExprMatches(const PredicateExpr& expr, const MenuItem& item,
                          const ItemContext& ctx) {
    if (expr.all.empty()) {
        return false;
    }
    for (const Predicate& pred : expr.all) {
        if (!PredicateMatches(pred, item, ctx)) {
            return false;
        }
    }
    return true;
}

std::vector<std::wstring> SplitPredicateAnd(const std::wstring& text) {
    std::vector<std::wstring> parts;
    std::wstring current;
    bool inQuotes = false;
    for (size_t i = 0; i < text.size(); ++i) {
        const wchar_t c = text[i];
        if (c == L'"') {
            inQuotes = !inQuotes;
        }
        if (!inQuotes && i > 0 && i + 3 < text.size() &&
            iswspace(text[i - 1]) &&
            (c == L'a' || c == L'A') &&
            (text[i + 1] == L'n' || text[i + 1] == L'N') &&
            (text[i + 2] == L'd' || text[i + 2] == L'D') &&
            iswspace(text[i + 3])) {
            parts.push_back(current);
            current.clear();
            i += 2;
            continue;
        }
        current += c;
    }
    parts.push_back(current);
    return parts;
}

bool ParsePredicateExpr(const std::wstring& text, PredicateExpr& out,
                        std::wstring& error) {
    PredicateExpr expr;
    for (const std::wstring& rawPart : SplitPredicateAnd(text)) {
        const std::wstring part = TrimWhitespace(rawPart);
        if (part.empty()) {
            error = L"empty predicate";
            return false;
        }

        Predicate pred;
        const size_t colon = part.find(L':');
        if (colon == std::wstring::npos) {
            const std::wstring name = ToLowerCopy(part);
            if (name == L"multi") {
                pred.field = PredicateField::Multi;
            } else if (name == L"thirdparty") {
                pred.field = PredicateField::ThirdParty;
            } else {
                error = L"unknown predicate '" + part + L"'";
                return false;
            }
            expr.all.push_back(std::move(pred));
            continue;
        }

        const std::wstring fieldName =
            ToLowerCopy(TrimWhitespace(part.substr(0, colon)));
        if (fieldName == L"label") {
            pred.field = PredicateField::Label;
        } else if (fieldName == L"verb") {
            pred.field = PredicateField::Verb;
        } else if (fieldName == L"ext") {
            pred.field = PredicateField::Ext;
        } else if (fieldName == L"scope") {
            pred.field = PredicateField::Scope;
        } else {
            error = L"unknown predicate field '" + fieldName + L"'";
            return false;
        }

        std::vector<std::wstring> values;
        std::wstring current;
        bool inQuotes = false;
        const std::wstring valuesText = part.substr(colon + 1);
        for (size_t i = 0; i < valuesText.size(); ++i) {
            const wchar_t c = valuesText[i];
            if (c == L'"') {
                inQuotes = !inQuotes;
                continue;
            }
            if (c == L',' && !inQuotes) {
                values.push_back(TrimWhitespace(current));
                current.clear();
                continue;
            }
            current += c;
        }
        values.push_back(TrimWhitespace(current));

        for (std::wstring& value : values) {
            if (value.empty()) {
                error = L"empty value in '" + part + L"'";
                return false;
            }
            if (pred.field == PredicateField::Scope) {
                Scope scope = Scope::Other;
                if (!ScopeFromName(value, scope)) {
                    error = L"unknown scope '" + value + L"'";
                    return false;
                }
                value = ToLowerCopy(value);
            } else if (pred.field == PredicateField::Label) {
                value = NormalizeMenuLabel(value);
                if (value.empty()) {
                    error = L"empty label in '" + part + L"'";
                    return false;
                }
            }
            pred.values.push_back(std::move(value));
        }
        expr.all.push_back(std::move(pred));
    }

    if (expr.all.empty()) {
        error = L"empty predicate";
        return false;
    }
    out = std::move(expr);
    error.clear();
    return true;
}

struct RulesApplication {
    bool hasMoveRules = false;
};

void HideMatchingItems(std::vector<MenuItem>& items, const PredicateExpr& match,
                       const ItemContext& ctx) {
    std::erase_if(items, [&](const MenuItem& item) {
        return item.action != ActionKind::Fallback &&
               PredicateExprMatches(match, item, ctx);
    });
    for (MenuItem& item : items) {
        if (item.kind == ItemKind::Submenu) {
            HideMatchingItems(item.children, match, ctx);
        }
    }
}

void CollectProtectedIds(const std::vector<MenuItem>& items,
                         const PredicateExpr& match, const ItemContext& ctx,
                         std::unordered_set<uint32_t>& out) {
    for (const MenuItem& item : items) {
        if (PredicateExprMatches(match, item, ctx)) {
            out.insert(item.id);
        }
        CollectProtectedIds(item.children, match, ctx, out);
    }
}

RulesApplication ApplyRulesToModel(MenuModel& model, const RulesConfig& config,
                                   const ItemContext& ctx) {
    RulesApplication application;
    for (const Rule& rule : config.rules) {
        if (rule.kind == RuleKind::Move) {
            application.hasMoveRules = true;
            break;
        }
    }

    for (const Rule& rule : config.rules) {
        if (rule.kind == RuleKind::Hide) {
            HideMatchingItems(model.items, rule.match, ctx);
        }
    }

    std::unordered_set<uint32_t> protectedIds;
    for (const Rule& rule : config.rules) {
        if (rule.kind == RuleKind::Keep) {
            CollectProtectedIds(model.items, rule.match, ctx, protectedIds);
        }
    }

    std::vector<std::wstring> destinations;
    std::unordered_map<std::wstring, std::vector<MenuItem>> movedByDestination;
    for (const Rule& rule : config.rules) {
        if (rule.kind != RuleKind::Move) {
            continue;
        }
        for (size_t i = 0; i < model.items.size();) {
            MenuItem& item = model.items[i];
            if (item.action != ActionKind::Fallback &&
                protectedIds.count(item.id) == 0 &&
                PredicateExprMatches(rule.match, item, ctx)) {
                std::vector<MenuItem>& bucket = movedByDestination[rule.destination];
                if (bucket.empty()) {
                    destinations.push_back(rule.destination);
                }
                bucket.push_back(std::move(item));
                model.items.erase(model.items.begin() +
                                  static_cast<std::ptrdiff_t>(i));
                continue;
            }
            ++i;
        }
    }

    uint32_t destinationId = 0xF700;
    for (const std::wstring& destination : destinations) {
        auto bucket = movedByDestination.find(destination);
        if (bucket == movedByDestination.end() || bucket->second.empty()) {
            continue;
        }

        MenuItem* existing = nullptr;
        for (MenuItem& item : model.items) {
            if (item.kind == ItemKind::Submenu && item.label == destination) {
                existing = &item;
                break;
            }
        }
        if (existing) {
            for (MenuItem& child : bucket->second) {
                existing->children.push_back(std::move(child));
            }
            continue;
        }

        MenuItem submenu{};
        submenu.id = destinationId++;
        submenu.kind = ItemKind::Submenu;
        submenu.action = ActionKind::Submenu;
        submenu.label = destination;
        submenu.children = std::move(bucket->second);

        auto insertAt = model.items.end();
        for (auto it = model.items.begin(); it != model.items.end(); ++it) {
            if (it->action == ActionKind::Fallback) {
                insertAt = it;
                break;
            }
        }
        model.items.insert(insertAt, std::move(submenu));
    }

    CollapseSeparators(model.items);
    return application;
}

bool CommandMatchesContext(const PredicateExpr& expr, const ItemContext& ctx) {
    if (expr.all.empty()) {
        return true;
    }
    MenuItem dummy{};
    return PredicateExprMatches(expr, dummy, ctx);
}

std::vector<MenuItem>::iterator FallbackInsertPoint(std::vector<MenuItem>& items) {
    for (auto it = items.begin(); it != items.end(); ++it) {
        if (it->action == ActionKind::Fallback) {
            return it;
        }
    }
    return items.end();
}

struct PendingCustomSubmenu {
    std::wstring name;
    std::wstring iconRef;
    SubmenuPositionKind position = SubmenuPositionKind::Bottom;
    std::wstring positionLabel;
    std::vector<MenuItem> children;
};

void InsertCustomItems(MenuModel& model, const RulesConfig& config,
                       const ItemContext& ctx) {
    auto findSubmenuConfig =
        [&](const std::wstring& name) -> const CustomSubmenu* {
        for (const CustomSubmenu& submenu : config.submenus) {
            if (_wcsicmp(submenu.name.c_str(), name.c_str()) == 0) {
                return &submenu;
            }
        }
        return nullptr;
    };

    std::unordered_map<std::wstring, PendingCustomSubmenu> pending;
    std::vector<std::wstring> pendingOrder;
    std::unordered_set<std::wstring> filteredNames;
    uint32_t syntheticId = 0xF100;
    auto ensureTopLevel = [&](const std::wstring& name) -> PendingCustomSubmenu* {
        auto it = pending.find(name);
        if (it != pending.end()) {
            return filteredNames.count(name) ? nullptr : &it->second;
        }
        PendingCustomSubmenu entry;
        entry.name = name;
        if (const CustomSubmenu* submenu = findSubmenuConfig(name)) {
            if (!CommandMatchesContext(submenu->match, ctx)) {
                filteredNames.insert(name);
                return nullptr;
            }
            entry.iconRef = submenu->iconRef;
            entry.position = submenu->position;
            entry.positionLabel = submenu->positionLabel;
        }
        it = pending.emplace(name, std::move(entry)).first;
        pendingOrder.push_back(name);
        return &it->second;
    };

    auto resolveContainer =
        [&](const std::wstring& path) -> std::vector<MenuItem>* {
        std::vector<std::wstring> segments;
        size_t start = 0;
        while (start <= path.size()) {
            const size_t slash = path.find(L'/', start);
            std::wstring segment = TrimWhitespace(
                path.substr(start, slash == std::wstring::npos
                                        ? std::wstring::npos
                                        : slash - start));
            if (!segment.empty()) {
                segments.push_back(std::move(segment));
            }
            if (slash == std::wstring::npos) {
                break;
            }
            start = slash + 1;
        }
        if (segments.empty() || segments.size() > 3) {
            return nullptr;
        }

        PendingCustomSubmenu* top = ensureTopLevel(segments[0]);
        if (!top) {
            return nullptr;
        }
        std::vector<MenuItem>* current = &top->children;
        for (size_t s = 1; s < segments.size(); ++s) {
            MenuItem* nested = nullptr;
            for (MenuItem& child : *current) {
                if (child.kind == ItemKind::Submenu &&
                    _wcsicmp(child.label.c_str(), segments[s].c_str()) == 0) {
                    nested = &child;
                    break;
                }
            }
            if (!nested) {
                MenuItem submenu{};
                submenu.id = syntheticId++;
                submenu.kind = ItemKind::Submenu;
                submenu.action = ActionKind::Submenu;
                submenu.label = segments[s];
                if (const CustomSubmenu* sc = findSubmenuConfig(segments[s])) {
                    if (!CommandMatchesContext(sc->match, ctx)) {
                        return nullptr;
                    }
                    submenu.iconRef = sc->iconRef;
                }
                current->push_back(std::move(submenu));
                nested = &current->back();
            }
            current = &nested->children;
        }
        return current;
    };

    std::vector<MenuItem> topLevelCommands;
    for (size_t i = 0; i < config.commands.size(); ++i) {
        const CustomCommand& command = config.commands[i];
        if (!CommandMatchesContext(command.match, ctx)) {
            continue;
        }

        MenuItem item{};
        item.id = 0xF200 + static_cast<uint32_t>(i);
        item.label = command.label;
        if (command.type == CommandType::Separator) {
            item.kind = ItemKind::Separator;
            item.action = ActionKind::ViewAction;
        } else if (command.type == CommandType::Header) {
            item.kind = ItemKind::Header;
            item.action = ActionKind::ViewAction;
        } else if (command.action != BuiltinAction::None) {
            item.kind = ItemKind::Command;
            item.action = ActionKind::Builtin;
            item.builtinAction = command.action;
            item.iconRef = command.iconRef;
        } else {
            item.kind = ItemKind::Command;
            item.action = ActionKind::CustomCommand;
            item.iconRef = command.iconRef;
            item.customCommandIndex = static_cast<uint32_t>(i);
        }

        std::vector<MenuItem>* container = nullptr;
        if (!command.menuPath.empty()) {
            container = resolveContainer(command.menuPath);
            if (!container) {
                // Context-filtered or invalid destination: skip the command.
                continue;
            }
        }
        if (!container) {
            topLevelCommands.push_back(std::move(item));
            continue;
        }

        if (command.type == CommandType::Command &&
            command.separator == CommandSeparator::Before) {
            MenuItem separator{};
            separator.id = 0xF500 + static_cast<uint32_t>(i);
            separator.kind = ItemKind::Separator;
            container->push_back(std::move(separator));
        }
        container->push_back(std::move(item));
        if (command.type == CommandType::Command &&
            command.separator == CommandSeparator::After) {
            MenuItem separator{};
            separator.id = 0xF501 + static_cast<uint32_t>(i);
            separator.kind = ItemKind::Separator;
            container->push_back(std::move(separator));
        }
    }

    // Configured submenus with no referencing command still exist in matching
    // contexts (empty ones are pruned later).
    for (const CustomSubmenu& submenu : config.submenus) {
        if (CommandMatchesContext(submenu.match, ctx)) {
            ensureTopLevel(submenu.name);
        }
    }

    uint32_t pendingId = syntheticId;
    for (const std::wstring& name : pendingOrder) {
        PendingCustomSubmenu& entry = pending[name];
        MenuItem submenu{};
        submenu.id = pendingId++;
        submenu.kind = ItemKind::Submenu;
        submenu.action = ActionKind::Submenu;
        submenu.label = entry.name;
        submenu.iconRef = entry.iconRef;
        submenu.children = std::move(entry.children);

        std::vector<MenuItem>::iterator insertAt = model.items.end();
        if (entry.position == SubmenuPositionKind::Top) {
            insertAt = model.items.begin();
        } else if (entry.position == SubmenuPositionKind::After ||
                   entry.position == SubmenuPositionKind::Before) {
            const std::wstring target = NormalizeMenuLabel(entry.positionLabel);
            for (size_t i = 0; i < model.items.size(); ++i) {
                if (NormalizeMenuLabel(model.items[i].label) == target) {
                    insertAt = model.items.begin() + static_cast<std::ptrdiff_t>(
                        entry.position == SubmenuPositionKind::After ? i + 1 : i);
                    break;
                }
            }
            if (insertAt == model.items.end()) {
                insertAt = FallbackInsertPoint(model.items);
            }
        } else {
            insertAt = FallbackInsertPoint(model.items);
        }
        model.items.insert(insertAt, std::move(submenu));
    }

    for (MenuItem& item : topLevelCommands) {
        const size_t index = item.id - 0xF200;
        const bool separatorBefore =
            index < config.commands.size() &&
            config.commands[index].separator == CommandSeparator::Before;
        const bool separatorAfter =
            index < config.commands.size() &&
            config.commands[index].separator == CommandSeparator::After;
        if (separatorBefore) {
            MenuItem separator{};
            separator.id = 0xF600 + static_cast<uint32_t>(index);
            separator.kind = ItemKind::Separator;
            model.items.insert(FallbackInsertPoint(model.items), std::move(separator));
        }
        model.items.insert(FallbackInsertPoint(model.items), std::move(item));
        if (separatorAfter) {
            MenuItem separator{};
            separator.id = 0xF601 + static_cast<uint32_t>(index);
            separator.kind = ItemKind::Separator;
            model.items.insert(FallbackInsertPoint(model.items), std::move(separator));
        }
    }
}

// ===========================================================================
// [CMO:ConfigStore] Live-reloaded menu.ini.
// ===========================================================================

std::vector<uint8_t> EncodeConfigText(const std::wstring& text) {
    std::vector<uint8_t> bytes;
    bytes.push_back(0xEF);
    bytes.push_back(0xBB);
    bytes.push_back(0xBF);
    if (text.empty()) {
        return bytes;
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                         static_cast<int>(text.size()), nullptr, 0,
                                         nullptr, nullptr);
    if (size <= 0) {
        return bytes;
    }
    const size_t offset = bytes.size();
    bytes.resize(offset + static_cast<size_t>(size));
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                        reinterpret_cast<char*>(bytes.data() + offset), size,
                        nullptr, nullptr);
    return bytes;
}

bool DecodeConfigBytes(const std::vector<uint8_t>& bytes, std::wstring& text) {
    if (bytes.empty()) {
        text.clear();
        return true;
    }

    auto decodeUtf8 = [&](size_t offset, std::wstring& out) {
        const int length = static_cast<int>(bytes.size() - offset);
        if (length <= 0) {
            out.clear();
            return true;
        }
        const int wide = MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS,
            reinterpret_cast<const char*>(bytes.data() + offset), length, nullptr, 0);
        if (wide <= 0) {
            return false;
        }
        out.resize(static_cast<size_t>(wide));
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                            reinterpret_cast<const char*>(bytes.data() + offset),
                            length, out.data(), wide);
        return true;
    };
    auto decodeUtf16 = [&](size_t offset, bool bigEndian, std::wstring& out) {
        const size_t count = (bytes.size() - offset) / 2;
        out.resize(count);
        for (size_t i = 0; i < count; ++i) {
            const uint8_t first = bytes[offset + i * 2];
            const uint8_t second = bytes[offset + i * 2 + 1];
            out[i] = static_cast<wchar_t>(bigEndian
                                              ? (first << 8) | second
                                              : first | (second << 8));
        }
        return true;
    };

    if (bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB &&
        bytes[2] == 0xBF) {
        return decodeUtf8(3, text);
    }
    if (bytes.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE) {
        return decodeUtf16(2, false, text);
    }
    if (bytes.size() >= 2 && bytes[0] == 0xFE && bytes[1] == 0xFF) {
        return decodeUtf16(2, true, text);
    }

    // Strict UTF-8, but reject results containing embedded NULs: those are
    // almost certainly UTF-16 read as bytes.
    std::wstring utf8Text;
    if (decodeUtf8(0, utf8Text) &&
        utf8Text.find(L'\0') == std::wstring::npos) {
        text = std::move(utf8Text);
        return true;
    }

    // BOM-less UTF-16LE heuristic: even length and enough NUL high bytes.
    if (bytes.size() % 2 == 0) {
        size_t zeros = 0;
        for (size_t i = 1; i < bytes.size(); i += 2) {
            if (bytes[i] == 0) {
                ++zeros;
            }
        }
        if (zeros >= bytes.size() / 4) {
            return decodeUtf16(0, false, text);
        }
    }

    text.clear();
    return false;
}

bool ReadConfigFile(const std::wstring& path, std::wstring& text) {
    HANDLE file =
        CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(file, &size) || size.QuadPart > 1024 * 1024) {
        CloseHandle(file);
        return false;
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    const BOOL ok =
        bytes.empty() ||
        ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read,
                 nullptr);
    CloseHandle(file);
    if (!ok) {
        return false;
    }
    bytes.resize(read);
    return DecodeConfigBytes(bytes, text);
}

bool WriteConfigFile(const std::wstring& path, const std::wstring& text) {
    const std::vector<uint8_t> bytes = EncodeConfigText(text);
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    const BOOL ok =
        bytes.empty() ||
        WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written,
                  nullptr);
    CloseHandle(file);
    return ok && written == bytes.size();
}

std::wstring ConfigFilePath() {
    wchar_t storagePath[MAX_PATH] = {};
    if (!Wh_GetModStoragePath(storagePath, ARRAYSIZE(storagePath))) {
        return L"";
    }
    return std::wstring(storagePath) + L"\\menu.ini";
}

class ConfigStore {
public:
    void EnsureLoaded() {
        if (loaded_) {
            return;
        }
        loaded_ = true;
        const std::wstring path = ConfigFilePath();
        if (path.empty()) {
            return;
        }
        const size_t slash = path.find_last_of(L'\\');
        if (slash != std::wstring::npos) {
            CreateDirectoryW(path.substr(0, slash).c_str(), nullptr);
        }
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
            WriteConfigFile(path, GenerateDefaultConfigText());
        }
        std::wstring text;
        if (ReadConfigFile(path, text)) {
            ApplyText(text);
        } else if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
            Wh_Log(L"menu.ini: unable to read or decode the file");
        }
        UpdateStamp(path);
    }

    // Called at menu open: reloads only when the file changed.
    void RefreshIfChanged() {
        EnsureLoaded();
        const std::wstring path = ConfigFilePath();
        if (path.empty()) {
            return;
        }
        WIN32_FILE_ATTRIBUTE_DATA attributes = {};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes)) {
            return;  // Deleted: keep the last good snapshot.
        }
        const uint64_t size =
            (static_cast<uint64_t>(attributes.nFileSizeHigh) << 32) |
            attributes.nFileSizeLow;
        if (stampValid_ && size == stampSize_ &&
            attributes.ftLastWriteTime.dwLowDateTime == stampTimeLow_ &&
            attributes.ftLastWriteTime.dwHighDateTime == stampTimeHigh_) {
            return;
        }
        stampValid_ = true;
        stampSize_ = size;
        stampTimeLow_ = attributes.ftLastWriteTime.dwLowDateTime;
        stampTimeHigh_ = attributes.ftLastWriteTime.dwHighDateTime;

        std::wstring text;
        if (ReadConfigFile(path, text)) {
            ApplyText(text);
        } else if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
            Wh_Log(L"menu.ini: unable to read or decode the file");
        }
    }

    std::shared_ptr<const RulesConfig> Snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return config_;
    }

    uint64_t Revision() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return config_ ? config_->revision : 0;
    }

    bool ApplyTextForTesting(const std::wstring& text) {
        return ApplyText(text);
    }

private:
    void UpdateStamp(const std::wstring& path) {
        WIN32_FILE_ATTRIBUTE_DATA attributes = {};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes)) {
            stampValid_ = false;
            return;
        }
        stampValid_ = true;
        stampSize_ = (static_cast<uint64_t>(attributes.nFileSizeHigh) << 32) |
                     attributes.nFileSizeLow;
        stampTimeLow_ = attributes.ftLastWriteTime.dwLowDateTime;
        stampTimeHigh_ = attributes.ftLastWriteTime.dwHighDateTime;
    }

    bool ApplyText(const std::wstring& text) {
        RulesConfig parsed;
        std::vector<ConfigParseError> errors;
        if (!ParseRulesConfig(text, parsed, errors)) {
            for (const ConfigParseError& error : errors) {
                Wh_Log(L"menu.ini:%d: %s", error.line, error.message.c_str());
            }
            return false;
        }
        uint64_t previousRevision = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            previousRevision = config_ ? config_->revision : 0;
        }
        parsed.revision = previousRevision + 1;
        auto snapshot = std::make_shared<const RulesConfig>(std::move(parsed));
        std::lock_guard<std::mutex> lock(mutex_);
        config_ = std::move(snapshot);
        return true;
    }

    mutable std::mutex mutex_;
    std::shared_ptr<const RulesConfig> config_;
    bool loaded_ = false;
    bool stampValid_ = false;
    uint64_t stampSize_ = 0;
    DWORD stampTimeLow_ = 0;
    DWORD stampTimeHigh_ = 0;
};

inline ConfigStore g_configStore;

void ApplyItemOverrides(std::vector<MenuItem>& items, const RulesConfig& config,
                        const ItemContext& ctx) {
    for (MenuItem& item : items) {
        for (const ItemOverride& override : config.overrides) {
            if (!PredicateExprMatches(override.match, item, ctx)) {
                continue;
            }
            if (override.hasIcon) {
                item.iconRef = override.iconRef;
            }
            if (override.hasLabel) {
                item.displayLabel = override.label;
            }
            if (override.hasMarker) {
                item.markerOverride = static_cast<int>(override.marker);
            }
        }
        ApplyItemOverrides(item.children, config, ctx);
    }
}

RulesApplication ApplyRulesConfigToModel(MenuModel& model,
                                         const RulesConfig& config,
                                         const ItemContext& ctx) {
    RulesApplication application = ApplyRulesToModel(model, config, ctx);
    ApplyItemOverrides(model.items, config, ctx);
    InsertCustomItems(model, config, ctx);
    return application;
}

// ===========================================================================
// [CMO:Layout] Appearance resolution, metrics, and render-ready layout.
// ===========================================================================

Appearance ResolveAppearance(const RulesConfig& config, bool darkTheme) {
    if (darkTheme) {
        return config.hasDarkAppearance ? config.darkAppearance : config.appearance;
    }
    return config.hasLightAppearance ? config.lightAppearance : config.appearance;
}

struct LayoutMetrics {
    int itemHeight = 28;
    int separatorHeight = 7;
    int iconSize = 16;
    int padding = 6;
    int gutterWidth = 22;
    int submenuArrowWidth = 16;
    int cornerRadius = 8;
    int borderWidth = 1;
    int shadowSize = 12;
    float fontSize = 9.0f;
    std::wstring fontFace = L"Segoe UI";
    uint32_t textColor = 0xFFFFFFFF;
    uint32_t disabledTextColor = 0x66FFFFFF;
    uint32_t hoverBackground = 0x14FFFFFF;
    uint32_t pressedBackground = 0x22FFFFFF;
    uint32_t separator = 0x18FFFFFF;
    uint32_t submenuArrow = 0x99FFFFFF;
    int verticalPadding = 4;
    int itemPadding = 6;
    int separatorSpacing = 0;
    int minWidth = 0;
    int maxWidth = 0;
    int markerWidth = 14;
    FontWeightKind fontWeight = FontWeightKind::Normal;
    FontStyleKind fontStyle = FontStyleKind::Normal;
    CornerRadii cornerRadii;
    bool hasCornerRadii = false;
    int shadowOpacity = 120;
    int shadowBlur = 12;
    MarkerStyle marker = MarkerStyle::Dot;
    uint32_t markerColor = 0xFFFFFFFF;
    uint32_t headerColor = 0x66FFFFFF;
    AcceleratorMode acceleratorMode = AcceleratorMode::Underline;
};

DWRITE_FONT_WEIGHT FontWeightToDwrite(FontWeightKind weight) {
    switch (weight) {
        case FontWeightKind::Semibold:
            return DWRITE_FONT_WEIGHT_SEMI_BOLD;
        case FontWeightKind::Bold:
            return DWRITE_FONT_WEIGHT_BOLD;
        default:
            return DWRITE_FONT_WEIGHT_NORMAL;
    }
}

DWRITE_FONT_STYLE FontStyleToDwrite(FontStyleKind style) {
    return style == FontStyleKind::Italic ? DWRITE_FONT_STYLE_ITALIC
                                          : DWRITE_FONT_STYLE_NORMAL;
}

LayoutMetrics ResolveLayoutMetrics(const Appearance& appearance, uint32_t dpi,
                                   bool darkTheme) {
    (void)darkTheme;
    const int scale = dpi == 0 ? 96 : static_cast<int>(dpi);

    LayoutMetrics metrics;
    metrics.itemHeight = MulDiv(appearance.itemHeight, scale, 96);
    metrics.iconSize = MulDiv(appearance.iconSize, scale, 96);
    metrics.padding = MulDiv(appearance.padding, scale, 96);
    metrics.gutterWidth =
        metrics.markerWidth + metrics.padding / 2 + metrics.iconSize;
    metrics.separatorHeight = MulDiv(7, scale, 96);
    metrics.submenuArrowWidth = MulDiv(16, scale, 96);
    metrics.cornerRadius = MulDiv(appearance.cornerRadius, scale, 96);
    metrics.borderWidth = std::max(1, MulDiv(appearance.borderWidth, scale, 96));
    metrics.shadowSize = MulDiv(appearance.shadowSize, scale, 96);
    metrics.fontFace = appearance.fontFace;
    metrics.fontSize = appearance.fontSize * (static_cast<float>(scale) / 96.0f);
    metrics.textColor = appearance.textColor;
    metrics.disabledTextColor = appearance.disabledTextColor;
    metrics.hoverBackground = appearance.hoverBackground;
    metrics.pressedBackground = appearance.pressedBackground;
    metrics.separator = appearance.separator;
    metrics.submenuArrow = appearance.submenuArrow;
    metrics.verticalPadding = MulDiv(appearance.verticalPadding, scale, 96);
    metrics.itemPadding = appearance.itemPadding >= 0
                              ? MulDiv(appearance.itemPadding, scale, 96)
                              : metrics.padding;
    metrics.separatorSpacing = MulDiv(appearance.separatorSpacing, scale, 96);
    metrics.minWidth = MulDiv(appearance.minWidth, scale, 96);
    metrics.maxWidth = MulDiv(appearance.maxWidth, scale, 96);
    metrics.markerWidth = MulDiv(appearance.markerWidth, scale, 96);
    metrics.fontWeight = appearance.fontWeight;
    metrics.fontStyle = appearance.fontStyle;
    metrics.cornerRadii = {
        MulDiv(appearance.cornerRadii.topLeft, scale, 96),
        MulDiv(appearance.cornerRadii.topRight, scale, 96),
        MulDiv(appearance.cornerRadii.bottomRight, scale, 96),
        MulDiv(appearance.cornerRadii.bottomLeft, scale, 96)};
    metrics.hasCornerRadii = appearance.hasCornerRadii;
    metrics.shadowOpacity = appearance.shadowOpacity;
    metrics.shadowBlur = MulDiv(appearance.shadowBlur, scale, 96);
    metrics.marker = appearance.marker;
    metrics.markerColor = appearance.hasMarkerColor ? appearance.markerColor
                                                    : appearance.textColor;
    metrics.headerColor = appearance.hasHeaderColor ? appearance.headerColor
                                                    : appearance.disabledTextColor;
    metrics.acceleratorMode = appearance.acceleratorMode;
    return metrics;
}

struct InvocationDescriptor {
    uint32_t id = 0;
    ActionKind action = ActionKind::ViewAction;
    uint32_t viewAction = 0;
    uint32_t verbOffset = 0;
    uint32_t sortIndex = 0;
    uint32_t customCommandIndex = 0;
    uint32_t newIndex = 0;
    std::wstring canonicalVerb;
    std::wstring targetPath;
    bool hasOffset = false;
};

struct LayoutItemResources {
    IDWriteTextLayout* text = nullptr;
    ID2D1Bitmap* icon = nullptr;

    ~LayoutItemResources() {
        if (text) {
            text->Release();
        }
        if (icon) {
            icon->Release();
        }
    }
};

struct LayoutItem {
    RECT rect = {};
    RECT gutterRect = {};
    RECT markerRect = {};
    RECT iconRect = {};
    RECT textRect = {};
    int markerStyle = -1;
    ItemKind kind = ItemKind::Command;
    std::wstring label;
    std::wstring iconRef;
    std::vector<uint8_t> iconPixels;
    uint32_t flags = 0;
    uint32_t textColor = 0;
    uint32_t hoverTextColor = 0;
    int submenuIndex = -1;
    InvocationDescriptor invocation;
    std::shared_ptr<LayoutItemResources> resources;
};

struct LayoutPanel {
    SIZE size = {};
    std::vector<LayoutItem> items;
    std::vector<LayoutPanel> children;
};

struct AcceleratorText {
    std::wstring text;
    std::vector<std::pair<size_t, size_t>> underlineRanges;
};

// Parses shell mnemonics: && -> literal &, a single & underlines the next
// character and is removed, a trailing & is literal.
AcceleratorText StripAccelerators(const std::wstring& label) {
    AcceleratorText result;
    result.text.reserve(label.size());
    for (size_t i = 0; i < label.size(); ++i) {
        const wchar_t c = label[i];
        if (c != L'&') {
            result.text.push_back(c);
            continue;
        }
        if (i + 1 < label.size() && label[i + 1] == L'&') {
            result.text.push_back(L'&');
            ++i;
            continue;
        }
        if (i + 1 < label.size()) {
            result.underlineRanges.push_back(
                {result.text.size(), result.text.size() + 1});
            result.text.push_back(label[i + 1]);
            ++i;
            continue;
        }
        result.text.push_back(L'&');
    }
    return result;
}

// Renderer supplies a DirectWrite-based measurer; the default estimate keeps
// the builder pure and testable.
using TextMeasureFn = int (*)(const wchar_t*, size_t, const LayoutMetrics&);

int EstimateTextWidth(const wchar_t* label, size_t length,
                      const LayoutMetrics& metrics) {
    const std::wstring raw(label, length);
    const size_t displayLength = StripAccelerators(raw).text.size();
    return static_cast<int>(static_cast<float>(displayLength) * metrics.fontSize *
                            0.6f) +
           4;
}

LayoutPanel BuildLayoutPanel(const std::vector<MenuItem>& items,
                             const LayoutMetrics& metrics,
                             TextMeasureFn measure = nullptr) {
    LayoutPanel panel;
    const int markerLeft = metrics.itemPadding;
    const int iconLeft =
        metrics.padding + metrics.markerWidth + metrics.padding / 2;
    const int textLeft = iconLeft + metrics.iconSize + metrics.padding;
    const int textGap = metrics.padding;

    int y = metrics.verticalPadding;
    int contentWidth = 0;
    for (const MenuItem& item : items) {
        const int height =
            item.kind == ItemKind::Separator
                ? metrics.separatorHeight + 2 * metrics.separatorSpacing
                : metrics.itemHeight;
        if (item.kind != ItemKind::Separator) {
            const std::wstring& measureLabel =
                item.displayLabel.empty() ? item.label : item.displayLabel;
            const int textWidth =
                measure ? measure(measureLabel.c_str(), measureLabel.size(), metrics)
                        : EstimateTextWidth(measureLabel.c_str(),
                                            measureLabel.size(), metrics);
            const int arrowSpace = item.kind == ItemKind::Submenu
                                       ? metrics.submenuArrowWidth
                                       : 0;
            contentWidth = std::max(
                contentWidth, textLeft + textWidth + arrowSpace + textGap);
        }
        y += height;
    }
    int panelWidth = std::max(contentWidth, 80);
    if (metrics.minWidth > 0) {
        panelWidth = std::max(panelWidth, metrics.minWidth);
    }
    if (metrics.maxWidth > 0) {
        // maxWidth is a soft cap: never narrower than the widest item.
        panelWidth =
            std::min(panelWidth, std::max(metrics.maxWidth, contentWidth));
    }
    panel.size = {panelWidth, y + metrics.verticalPadding};

    int offset = metrics.verticalPadding;
    for (const MenuItem& item : items) {
        LayoutItem layout{};
        layout.kind = item.kind;
        layout.label = item.displayLabel.empty() ? item.label : item.displayLabel;
        layout.iconRef = item.iconRef;
        layout.iconPixels = item.iconPixels;
        layout.flags = item.flags;
        layout.textColor = (item.flags & kModelDisabled) ? metrics.disabledTextColor
                                                         : metrics.textColor;
        layout.hoverTextColor = metrics.textColor;
        layout.invocation.id = item.id;
        layout.invocation.action = item.action;
        layout.invocation.viewAction = item.viewAction;
        layout.invocation.verbOffset = item.verbOffset;
        layout.invocation.sortIndex = item.sortIndex;
        layout.invocation.customCommandIndex = item.customCommandIndex;
        layout.invocation.newIndex = item.newIndex;
        layout.invocation.canonicalVerb = item.canonicalVerb;
        layout.invocation.targetPath = item.targetPath;
        layout.invocation.hasOffset = (item.flags & kModelHasOffset) != 0;

        const int height =
            item.kind == ItemKind::Separator
                ? metrics.separatorHeight + 2 * metrics.separatorSpacing
                : metrics.itemHeight;
        layout.rect = {0, offset, panel.size.cx, offset + height};

        if (item.kind != ItemKind::Separator) {
            layout.markerRect = {markerLeft, offset,
                                 markerLeft + metrics.markerWidth, offset + height};
            layout.markerStyle =
                item.markerOverride >= 0
                    ? item.markerOverride
                    : static_cast<int>(metrics.marker);
            layout.gutterRect = {metrics.padding, offset, textLeft - metrics.padding,
                                 offset + height};
            layout.iconRect = {iconLeft, offset + (height - metrics.iconSize) / 2,
                               iconLeft + metrics.iconSize,
                               offset + (height + metrics.iconSize) / 2};
            const int arrowSpace =
                item.kind == ItemKind::Submenu ? metrics.submenuArrowWidth : 0;
            layout.textRect = {textLeft, offset,
                               panel.size.cx - textGap - arrowSpace, offset + height};
        }

        if (item.kind == ItemKind::Submenu) {
            layout.submenuIndex = static_cast<int>(panel.children.size());
            panel.children.push_back(
                BuildLayoutPanel(item.children, metrics, measure));
        }
        panel.items.push_back(std::move(layout));
        offset += height;
    }
    return panel;
}

POINT ClampPanelPosition(POINT anchor, SIZE panelSize, const RECT& workArea) {
    int x = anchor.x;
    int y = anchor.y;
    if (x + panelSize.cx > workArea.right) {
        x = anchor.x - panelSize.cx;
    }
    if (y + panelSize.cy > workArea.bottom) {
        y = anchor.y - panelSize.cy;
    }
    const int left = static_cast<int>(workArea.left);
    const int top = static_cast<int>(workArea.top);
    x = std::clamp(x, left, std::max(left, static_cast<int>(workArea.right) - static_cast<int>(panelSize.cx)));
    y = std::clamp(y, top, std::max(top, static_cast<int>(workArea.bottom) - static_cast<int>(panelSize.cy)));
    return POINT{x, y};
}

POINT SubmenuPosition(const RECT& parentItemScreenRect, SIZE childSize,
                      const RECT& workArea, int overlapPx) {
    int x = parentItemScreenRect.right - overlapPx;
    if (x + childSize.cx > workArea.right) {
        x = parentItemScreenRect.left - childSize.cx;
    }
    const int left = static_cast<int>(workArea.left);
    const int top = static_cast<int>(workArea.top);
    x = std::clamp(x, left, std::max(left, static_cast<int>(workArea.right) - static_cast<int>(childSize.cx)));

    int y = parentItemScreenRect.top;
    if (y + childSize.cy > workArea.bottom) {
        y = workArea.bottom - childSize.cy;
    }
    y = std::clamp(y, top, std::max(top, static_cast<int>(workArea.bottom) - static_cast<int>(childSize.cy)));
    return POINT{x, y};
}

uint64_t ModelFingerprint(const std::vector<MenuItem>& items) {
    uint64_t hash = 1469598103934665603ull;
    for (const MenuItem& item : items) {
        hash = HashCombine(hash, item.id);
        hash = HashCombine(hash, static_cast<uint64_t>(item.kind));
        hash = HashCombine(hash, static_cast<uint64_t>(item.action));
        hash = HashCombine(hash, item.flags);
        hash = HashCombine(hash, static_cast<uint64_t>(item.children.size()));
        for (wchar_t c : item.label) {
            hash = HashCombine(hash, static_cast<uint64_t>(c));
        }
        hash = HashCombine(hash, ModelFingerprint(item.children));
    }
    return hash;
}

struct LayoutKey {
    ContextSignature sig;
    uint64_t rulesRevision = 0;
    uint64_t appearanceRevision = 0;
    uint32_t dpi = 96;
    bool darkTheme = false;
    uint64_t modelFingerprint = 0;

    bool operator==(const LayoutKey&) const = default;

    uint64_t Hash() const {
        uint64_t hash = sig.Hash();
        hash = HashCombine(hash, rulesRevision);
        hash = HashCombine(hash, appearanceRevision);
        hash = HashCombine(hash, dpi);
        hash = HashCombine(hash, darkTheme ? 1 : 0);
        hash = HashCombine(hash, modelFingerprint);
        return hash;
    }
};

LayoutKey MakeLayoutKey(const ContextSignature& sig, const RulesConfig& config,
                        uint32_t dpi, bool darkTheme, const MenuModel& model) {
    LayoutKey key{};
    key.sig = sig;
    key.rulesRevision = config.revision;
    key.appearanceRevision = config.revision;
    key.dpi = dpi;
    key.darkTheme = darkTheme;
    key.modelFingerprint = ModelFingerprint(model.items);
    return key;
}

class LayoutCache {
public:
    std::shared_ptr<const LayoutPanel> Find(const LayoutKey& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = map_.find(key.Hash());
        if (it == map_.end() || !(it->second.key == key)) {
            return nullptr;
        }
        order_.splice(order_.begin(), order_, it->second.orderIt);
        return it->second.panel;
    }

    void Put(const LayoutKey& key, std::shared_ptr<const LayoutPanel> panel) {
        std::lock_guard<std::mutex> lock(mutex_);
        const uint64_t hash = key.Hash();
        auto it = map_.find(hash);
        if (it != map_.end() && it->second.key == key) {
            it->second.panel = std::move(panel);
            order_.splice(order_.begin(), order_, it->second.orderIt);
            return;
        }

        order_.push_front(hash);
        Entry entry;
        entry.key = key;
        entry.panel = std::move(panel);
        entry.orderIt = order_.begin();
        map_[hash] = std::move(entry);

        while (map_.size() > maxEntries_ && !order_.empty()) {
            const uint64_t victim = order_.back();
            order_.pop_back();
            map_.erase(victim);
        }
    }

    void InvalidateDevice() { Clear(); }
    void InvalidateAll() { Clear(); }

    void SetMaxEntries(size_t maxEntries) {
        maxEntries_ = maxEntries == 0 ? 1 : maxEntries;
    }

    size_t Size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return map_.size();
    }

private:
    struct Entry {
        LayoutKey key;
        std::shared_ptr<const LayoutPanel> panel;
        std::list<uint64_t>::iterator orderIt;
    };

    void Clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        map_.clear();
        order_.clear();
    }

    mutable std::mutex mutex_;
    std::unordered_map<uint64_t, Entry> map_;
    std::list<uint64_t> order_;
    size_t maxEntries_ = 32;
};

inline LayoutCache g_layoutCache;

// Bounded least-recently-used map; Find promotes, Insert evicts the oldest
// and releases its value.
template <typename T>
class LruMap {
public:
    void SetMaxEntries(size_t maxEntries) {
        maxEntries_ = maxEntries == 0 ? 1 : maxEntries;
    }

    size_t MaxEntries() const { return maxEntries_; }
    size_t Size() const { return map_.size(); }

    T* Find(const std::wstring& key) {
        auto it = map_.find(key);
        if (it == map_.end()) {
            return nullptr;
        }
        order_.splice(order_.begin(), order_, it->second.orderIt);
        return &it->second.value;
    }

    template <typename F>
    void Insert(const std::wstring& key, T value, F release) {
        auto it = map_.find(key);
        if (it != map_.end()) {
            it->second.value = std::move(value);
            order_.splice(order_.begin(), order_, it->second.orderIt);
            return;
        }
        order_.push_front(key);
        Entry entry;
        entry.value = std::move(value);
        entry.orderIt = order_.begin();
        map_.emplace(key, std::move(entry));

        while (map_.size() > maxEntries_ && !order_.empty()) {
            const std::wstring victim = order_.back();
            order_.pop_back();
            auto victimIt = map_.find(victim);
            if (victimIt != map_.end()) {
                release(victimIt->second.value);
                map_.erase(victimIt);
            }
        }
    }

    template <typename F>
    void Clear(F release) {
        for (auto& pair : map_) {
            release(pair.second.value);
        }
        map_.clear();
        order_.clear();
    }

private:
    struct Entry {
        T value;
        std::list<std::wstring>::iterator orderIt;
    };

    std::unordered_map<std::wstring, Entry> map_;
    std::list<std::wstring> order_;
    size_t maxEntries_ = 256;
};

// ===========================================================================
// [CMO:Mode] Custom vs HMENU mode selection and failure fallback.
// ===========================================================================

enum class MenuMode : uint8_t { Custom, HMenu };

MenuMode ResolveMenuMode(int settingValue, int consecutiveCustomFailures) {
    if (settingValue != 0) {
        return MenuMode::HMenu;
    }
    if (consecutiveCustomFailures >= 3) {
        return MenuMode::HMenu;
    }
    return MenuMode::Custom;
}

class ModeController {
public:
    MenuMode Current() const { return mode_; }

    void SetMode(MenuMode mode) { mode_ = mode; }

    void RecordSuccess() { consecutiveFailures_ = 0; }

    void RecordFailure() {
        if (consecutiveFailures_ < 3) {
            ++consecutiveFailures_;
        }
        if (consecutiveFailures_ >= 3) {
            mode_ = MenuMode::HMenu;
        }
    }

    int ConsecutiveFailures() const { return consecutiveFailures_; }

private:
    MenuMode mode_ = MenuMode::Custom;
    int consecutiveFailures_ = 0;
};

inline ModeController g_modeController;

// ===========================================================================
// [CMO:RenderDevice] Shared D3D11/D2D/DWrite/DComp device.
// ===========================================================================

// MinGW declares the interfaces but does not export the IID symbols from the
// import libraries, so carry local copies.
const GUID kIidD2D1Factory = {
    0x06152247, 0x6f50, 0x465a, {0x92, 0x45, 0x11, 0x8b, 0xfd, 0x3b, 0x60, 0x07}};
const GUID kIidD2D1Factory1 = {
    0xbb12d362, 0xdaee, 0x4b9a, {0xaa, 0x1d, 0x14, 0xba, 0x40, 0x1c, 0xfa, 0x1f}};
const GUID kIidDWriteFactory = {
    0xb859ee5a, 0xd838, 0x4b5b, {0xa2, 0xe8, 0x1a, 0xdc, 0x7d, 0x93, 0xdb, 0x48}};
const GUID kIidDCompositionDevice = {
    0xc37ea93a, 0xe7aa, 0x450d, {0xb1, 0x6f, 0x97, 0x46, 0xcb, 0x04, 0x07, 0xf3}};
const GUID kIidIDXGIDevice = {
    0x54ec77fa, 0x1377, 0x44e6, {0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c}};

class RenderDevice {
public:
    bool Initialize() {
        if (ready_) {
            return true;
        }
        if (failed_) {
            return false;
        }

        static const D3D_FEATURE_LEVEL kLevels[] = {
            D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
        D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_10_0;
        HRESULT hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            kLevels, ARRAYSIZE(kLevels), D3D11_SDK_VERSION, &d3d_, &level, nullptr);
        if (FAILED(hr) || !d3d_) {
            hr = D3D11CreateDevice(
                nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                kLevels, ARRAYSIZE(kLevels), D3D11_SDK_VERSION, &d3d_, &level, nullptr);
        }
        if (FAILED(hr) || !d3d_) {
            failed_ = true;
            return false;
        }

        if (FAILED(d3d_->QueryInterface(kIidIDXGIDevice,
                                        reinterpret_cast<void**>(&dxgi_))) ||
            !dxgi_) {
            Shutdown();
            failed_ = true;
            return false;
        }

        D2D1_FACTORY_OPTIONS options = {};
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, kIidD2D1Factory,
                                     &options,
                                     reinterpret_cast<void**>(&d2dFactory_))) ||
            !d2dFactory_) {
            Shutdown();
            failed_ = true;
            return false;
        }
        if (FAILED(d2dFactory_->QueryInterface(kIidD2D1Factory1,
                                               reinterpret_cast<void**>(&d2dFactory1_))) ||
            !d2dFactory1_) {
            Shutdown();
            failed_ = true;
            return false;
        }
        if (FAILED(d2dFactory1_->CreateDevice(dxgi_, &d2dDevice_)) || !d2dDevice_) {
            Shutdown();
            failed_ = true;
            return false;
        }

        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, kIidDWriteFactory,
                                       reinterpret_cast<IUnknown**>(&dwrite_))) ||
            !dwrite_) {
            Shutdown();
            failed_ = true;
            return false;
        }

        if (FAILED(DCompositionCreateDevice(dxgi_, kIidDCompositionDevice,
                                            reinterpret_cast<void**>(&comp_))) ||
            !comp_) {
            Shutdown();
            failed_ = true;
            return false;
        }

        ready_ = true;
        return true;
    }

    bool IsReady() const { return ready_; }

    void HandleDeviceLost() {
        Shutdown();
        failed_ = false;
    }

    void Shutdown() {
        ready_ = false;
        if (comp_) {
            comp_->Release();
            comp_ = nullptr;
        }
        if (dwrite_) {
            dwrite_->Release();
            dwrite_ = nullptr;
        }
        if (d2dDevice_) {
            d2dDevice_->Release();
            d2dDevice_ = nullptr;
        }
        if (d2dFactory1_) {
            d2dFactory1_->Release();
            d2dFactory1_ = nullptr;
        }
        if (d2dFactory_) {
            d2dFactory_->Release();
            d2dFactory_ = nullptr;
        }
        if (dxgi_) {
            dxgi_->Release();
            dxgi_ = nullptr;
        }
        if (d3d_) {
            d3d_->Release();
            d3d_ = nullptr;
        }
    }

    ID3D11Device* D3DDevice() const { return d3d_; }
    IDXGIDevice* DxgiDevice() const { return dxgi_; }
    ID2D1Factory* D2DFactory() const { return d2dFactory_; }
    ID2D1Factory1* D2DFactory1() const { return d2dFactory1_; }
    ID2D1Device* D2DDevice() const { return d2dDevice_; }
    IDWriteFactory* DWriteFactory() const { return dwrite_; }
    IDCompositionDevice* CompDevice() const { return comp_; }

private:
    bool ready_ = false;
    bool failed_ = false;
    ID3D11Device* d3d_ = nullptr;
    IDXGIDevice* dxgi_ = nullptr;
    ID2D1Factory* d2dFactory_ = nullptr;
    ID2D1Factory1* d2dFactory1_ = nullptr;
    ID2D1Device* d2dDevice_ = nullptr;
    IDWriteFactory* dwrite_ = nullptr;
    IDCompositionDevice* comp_ = nullptr;
};

inline RenderDevice g_renderDevice;

// ===========================================================================
// [CMO:MenuWindow] DirectComposition-backed popup windows.
// ===========================================================================

const GUID kIidIDXGIFactory2 = {
    0x50c83a1c, 0xe072, 0x4c48, {0x87, 0xb0, 0x36, 0x30, 0xfa, 0x36, 0xa6, 0xd0}};

const wchar_t kMenuWindowClass[] = L"ContextMenuOverhaulV2Window";

DWORD MenuWindowStyle() { return WS_POPUP; }

DWORD MenuWindowExStyle() {
    return WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP;
}

class MenuWindow;

using MenuWindowMessageFn = LRESULT (*)(MenuWindow*, HWND, UINT, WPARAM, LPARAM);
inline MenuWindowMessageFn g_menuWindowMessageHook = nullptr;

class MenuWindow {
public:
    ~MenuWindow() { Destroy(); }

    bool Create(HWND owner, const LayoutPanel* panel, bool isRoot,
                int margin = 0) {
        // Pooled windows may carry a previous window/surface; release it so
        // reuse never leaks an HWND, swap chain, target, or visual.
        Destroy();

        owner_ = owner;
        panel_ = panel;
        isRoot_ = isRoot;
        margin_ = margin > 0 ? margin : 0;

        RegisterClassOnce();

        const int width = (panel && panel->size.cx > 0 ? panel->size.cx : 100) +
                          2 * margin_;
        const int height = (panel && panel->size.cy > 0 ? panel->size.cy : 100) +
                           2 * margin_;
        hwnd_ = CreateWindowExW(MenuWindowExStyle(), kMenuWindowClass, L"",
                                MenuWindowStyle(), 0, 0, width, height, owner, nullptr,
                                GetModuleHandleW(nullptr), this);
        if (!hwnd_) {
            return false;
        }
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

        if (!CreateSurface(width, height)) {
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
            return false;
        }
        return true;
    }

    void Move(POINT screenPos) {
        if (!hwnd_) {
            return;
        }
        SetWindowPos(hwnd_, HWND_TOPMOST, screenPos.x, screenPos.y, 0, 0,
                     SWP_NOSIZE | SWP_NOACTIVATE);
    }

    void Show() {
        if (!hwnd_) {
            return;
        }
        ::ShowWindow(hwnd_, isRoot_ ? SW_SHOW : SW_SHOWNOACTIVATE);
    }

    void Hide() {
        if (hwnd_) {
            ::ShowWindow(hwnd_, SW_HIDE);
        }
    }

    void Destroy() {
        if (visual_) {
            visual_->Release();
            visual_ = nullptr;
        }
        if (target_) {
            target_->Release();
            target_ = nullptr;
        }
        if (swapChain_) {
            swapChain_->Release();
            swapChain_ = nullptr;
        }
        if (hwnd_) {
            SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
        }
    }

    HWND Handle() const { return hwnd_; }
    const LayoutPanel* Panel() const { return panel_; }
    bool IsRoot() const { return isRoot_; }
    int Margin() const { return margin_; }
    IDXGISwapChain1* SwapChain() const { return swapChain_; }
    IDCompositionTarget* CompTarget() const { return target_; }
    IDCompositionVisual* CompVisual() const { return visual_; }

    void Present() {
        if (swapChain_) {
            swapChain_->Present(1, 0);
        }
    }

    void Resize(int width, int height) {
        if (!swapChain_ || width <= 0 || height <= 0) {
            return;
        }
        swapChain_->ResizeBuffers(0, static_cast<UINT>(width),
                                  static_cast<UINT>(height), DXGI_FORMAT_UNKNOWN, 0);
    }

    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        if (g_menuWindowMessageHook) {
            return g_menuWindowMessageHook(this, hwnd, msg, wParam, lParam);
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

private:
    static void RegisterClassOnce() {
        static bool registered = false;
        if (registered) {
            return;
        }
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = &MenuWindow::WindowProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        wc.lpszClassName = kMenuWindowClass;
        RegisterClassExW(&wc);
        registered = true;
    }

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam,
                                       LPARAM lParam) {
        auto* window =
            reinterpret_cast<MenuWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (window) {
            return window->HandleMessage(hwnd, msg, wParam, lParam);
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    bool CreateSurface(int width, int height) {
        if (!g_renderDevice.IsReady() || width <= 0 || height <= 0) {
            return false;
        }

        IDXGIDevice* dxgi = g_renderDevice.DxgiDevice();
        IDXGIAdapter* adapter = nullptr;
        if (FAILED(dxgi->GetAdapter(&adapter)) || !adapter) {
            return false;
        }
        IDXGIFactory2* factory = nullptr;
        const HRESULT factoryHr =
            adapter->GetParent(kIidIDXGIFactory2, reinterpret_cast<void**>(&factory));
        adapter->Release();
        if (FAILED(factoryHr) || !factory) {
            return false;
        }

        DXGI_SWAP_CHAIN_DESC1 desc = {};
        desc.Width = static_cast<UINT>(width);
        desc.Height = static_cast<UINT>(height);
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        const HRESULT chainHr = factory->CreateSwapChainForComposition(
            g_renderDevice.D3DDevice(), &desc, nullptr, &swapChain_);
        factory->Release();
        if (FAILED(chainHr) || !swapChain_) {
            return false;
        }

        IDCompositionDevice* comp = g_renderDevice.CompDevice();
        if (FAILED(comp->CreateTargetForHwnd(hwnd_, TRUE, &target_)) || !target_) {
            return false;
        }
        if (FAILED(comp->CreateVisual(&visual_)) || !visual_) {
            return false;
        }
        if (FAILED(visual_->SetContent(swapChain_))) {
            return false;
        }
        if (FAILED(target_->SetRoot(visual_))) {
            return false;
        }
        comp->Commit();
        return true;
    }

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    const LayoutPanel* panel_ = nullptr;
    bool isRoot_ = false;
    int margin_ = 0;
    IDXGISwapChain1* swapChain_ = nullptr;
    IDCompositionTarget* target_ = nullptr;
    IDCompositionVisual* visual_ = nullptr;
};

class MenuWindowPool {
public:
    static constexpr size_t kMaxWindows = 8;

    MenuWindow* Acquire() {
        if (!free_.empty()) {
            MenuWindow* window = free_.back();
            free_.pop_back();
            return window;
        }
        if (all_.size() >= kMaxWindows) {
            return nullptr;
        }
        auto window = std::make_unique<MenuWindow>();
        MenuWindow* raw = window.get();
        all_.push_back(std::move(window));
        return raw;
    }

    void Release(MenuWindow* window) {
        if (!window) {
            return;
        }
        window->Hide();
        free_.push_back(window);
    }

    void DestroyAll() {
        for (auto& window : all_) {
            window->Destroy();
        }
        all_.clear();
        free_.clear();
    }

    size_t Size() const { return all_.size(); }

private:
    std::vector<std::unique_ptr<MenuWindow>> all_;
    std::vector<MenuWindow*> free_;
};

inline MenuWindowPool g_menuWindowPool;

// ===========================================================================
// [CMO:MenuWindow] Input state, content caches, and Direct2D drawing.
// ===========================================================================

struct MenuInputState {
    int hoverIndex = -1;
    int keyboardIndex = -1;
    int scrollOffset = 0;
    int openSubmenu = -1;
};

struct BackdropBitmap {
    std::vector<uint32_t> pixels;
    int width = 0;
    int height = 0;
};

void DownscaleAndBlur(const uint32_t* src, int srcW, int srcH, int factor,
                      int passes, std::vector<uint32_t>& out, int& outW,
                      int& outH) {
    out.clear();
    outW = 0;
    outH = 0;
    if (!src || srcW <= 0 || srcH <= 0 || factor <= 0) {
        return;
    }

    outW = (srcW + factor - 1) / factor;
    outH = (srcH + factor - 1) / factor;
    out.assign(static_cast<size_t>(outW) * static_cast<size_t>(outH), 0);

    for (int y = 0; y < outH; ++y) {
        for (int x = 0; x < outW; ++x) {
            uint32_t a = 0;
            uint32_t r = 0;
            uint32_t g = 0;
            uint32_t b = 0;
            uint32_t count = 0;
            for (int dy = 0; dy < factor && y * factor + dy < srcH; ++dy) {
                for (int dx = 0; dx < factor && x * factor + dx < srcW; ++dx) {
                    const uint32_t pixel =
                        src[static_cast<size_t>(y * factor + dy) * srcW +
                            (x * factor + dx)];
                    a += (pixel >> 24) & 0xFF;
                    r += (pixel >> 16) & 0xFF;
                    g += (pixel >> 8) & 0xFF;
                    b += pixel & 0xFF;
                    ++count;
                }
            }
            if (count == 0) {
                continue;
            }
            out[static_cast<size_t>(y) * outW + x] =
                ((a / count) << 24) | ((r / count) << 16) | ((g / count) << 8) |
                (b / count);
        }
    }

    for (int pass = 0; pass < passes; ++pass) {
        std::vector<uint32_t> blurred = out;
        for (int y = 0; y < outH; ++y) {
            for (int x = 0; x < outW; ++x) {
                uint32_t a = 0;
                uint32_t r = 0;
                uint32_t g = 0;
                uint32_t b = 0;
                uint32_t count = 0;
                for (int dy = -1; dy <= 1; ++dy) {
                    const int sy = y + dy;
                    if (sy < 0 || sy >= outH) {
                        continue;
                    }
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int sx = x + dx;
                        if (sx < 0 || sx >= outW) {
                            continue;
                        }
                        const uint32_t pixel =
                            out[static_cast<size_t>(sy) * outW + sx];
                        a += (pixel >> 24) & 0xFF;
                        r += (pixel >> 16) & 0xFF;
                        g += (pixel >> 8) & 0xFF;
                        b += pixel & 0xFF;
                        ++count;
                    }
                }
                if (count == 0) {
                    continue;
                }
                blurred[static_cast<size_t>(y) * outW + x] =
                    ((a / count) << 24) | ((r / count) << 16) |
                    ((g / count) << 8) | (b / count);
            }
        }
        out.swap(blurred);
    }
}

void BuildRoundedRectMask(int width, int height, int radius,
                          std::vector<uint8_t>& alpha) {
    if (width <= 0 || height <= 0) {
        alpha.clear();
        return;
    }
    alpha.assign(static_cast<size_t>(width) * static_cast<size_t>(height), 0);
    if (radius <= 0) {
        std::fill(alpha.begin(), alpha.end(), 255);
        return;
    }

    const float r = static_cast<float>(radius);
    const float w = static_cast<float>(width);
    const float h = static_cast<float>(height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float px = static_cast<float>(x) + 0.5f;
            const float py = static_cast<float>(y) + 0.5f;
            const float cx = std::clamp(px, r, std::max(r, w - r));
            const float cy = std::clamp(py, r, std::max(r, h - r));
            const float dx = px - cx;
            const float dy = py - cy;
            const float distance = std::sqrt(dx * dx + dy * dy);
            const float coverage = std::clamp(r - distance + 0.5f, 0.0f, 1.0f);
            alpha[static_cast<size_t>(y) * width + x] =
                static_cast<uint8_t>(coverage * 255.0f + 0.5f);
        }
    }
}

void BuildRoundedRectMaskRadii(int width, int height, int topLeft, int topRight,
                               int bottomRight, int bottomLeft,
                               std::vector<uint8_t>& alpha) {
    if (width <= 0 || height <= 0) {
        alpha.clear();
        return;
    }
    alpha.assign(static_cast<size_t>(width) * static_cast<size_t>(height), 255);

    const int maxRadius = std::min(width, height);
    auto clampRadius = [maxRadius](int radius) {
        return std::clamp(radius, 0, maxRadius);
    };
    const int tl = clampRadius(topLeft);
    const int tr = clampRadius(topRight);
    const int br = clampRadius(bottomRight);
    const int bl = clampRadius(bottomLeft);
    if (tl == 0 && tr == 0 && br == 0 && bl == 0) {
        return;
    }

    const float w = static_cast<float>(width);
    const float h = static_cast<float>(height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float px = static_cast<float>(x) + 0.5f;
            const float py = static_cast<float>(y) + 0.5f;
            float radius = 0.0f;
            float cx = px;
            float cy = py;
            if (px < static_cast<float>(tl) && py < static_cast<float>(tl)) {
                radius = static_cast<float>(tl);
                cx = static_cast<float>(tl);
                cy = static_cast<float>(tl);
            } else if (px > w - static_cast<float>(tr) &&
                       py < static_cast<float>(tr)) {
                radius = static_cast<float>(tr);
                cx = w - static_cast<float>(tr);
                cy = static_cast<float>(tr);
            } else if (px > w - static_cast<float>(br) &&
                       py > h - static_cast<float>(br)) {
                radius = static_cast<float>(br);
                cx = w - static_cast<float>(br);
                cy = h - static_cast<float>(br);
            } else if (px < static_cast<float>(bl) &&
                       py > h - static_cast<float>(bl)) {
                radius = static_cast<float>(bl);
                cx = static_cast<float>(bl);
                cy = h - static_cast<float>(bl);
            }
            if (radius <= 0.0f) {
                continue;
            }
            const float dx = px - cx;
            const float dy = py - cy;
            const float distance = std::sqrt(dx * dx + dy * dy);
            const float coverage = std::clamp(radius - distance + 0.5f, 0.0f, 1.0f);
            alpha[static_cast<size_t>(y) * width + x] =
                static_cast<uint8_t>(coverage * 255.0f + 0.5f);
        }
    }
}

bool BuildShadowBitmap(int width, int height, const CornerRadii& radii, int blur,
                       int opacity, int downscale, std::vector<uint32_t>& pixels,
                       int& outW, int& outH) {
    pixels.clear();
    outW = 0;
    outH = 0;
    if (width <= 0 || height <= 0 || blur < 0 || downscale <= 0) {
        return false;
    }

    const int maskW = width + 2 * blur;
    const int maskH = height + 2 * blur;
    if (maskW <= 0 || maskH <= 0 ||
        static_cast<int64_t>(maskW) * maskH > 16 * 1024 * 1024) {
        return false;
    }
    std::vector<uint8_t> mask;
    BuildRoundedRectMaskRadii(maskW, maskH, radii.topLeft + blur,
                              radii.topRight + blur, radii.bottomRight + blur,
                              radii.bottomLeft + blur, mask);
    std::vector<uint32_t> argb(static_cast<size_t>(maskW) * maskH);
    for (size_t i = 0; i < argb.size(); ++i) {
        const uint32_t alpha =
            (static_cast<uint32_t>(mask[i]) * static_cast<uint32_t>(opacity)) / 255;
        argb[i] = alpha << 24;  // black, premultiplied
    }

    std::vector<uint32_t> blurred;
    DownscaleAndBlur(argb.data(), maskW, maskH, downscale, 3, blurred, outW, outH);
    if (outW <= 0 || outH <= 0) {
        return false;
    }
    pixels = std::move(blurred);
    return true;
}

constexpr int kBackdropDownscaleFactor = 4;
constexpr int kBackdropBlurPasses = 2;

bool CaptureBackdrop(const RECT& screenRect, int factor, BackdropBitmap& out) {
    const int width = screenRect.right - screenRect.left;
    const int height = screenRect.bottom - screenRect.top;
    if (width <= 0 || height <= 0) {
        return false;
    }

    HDC screen = GetDC(nullptr);
    if (!screen) {
        return false;
    }
    HDC memory = CreateCompatibleDC(screen);
    if (!memory) {
        ReleaseDC(nullptr, screen);
        return false;
    }

    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib || !bits) {
        if (dib) {
            DeleteObject(dib);
        }
        DeleteDC(memory);
        ReleaseDC(nullptr, screen);
        return false;
    }

    HGDIOBJ oldBitmap = SelectObject(memory, dib);
    const BOOL copied =
        BitBlt(memory, 0, 0, width, height, screen, screenRect.left,
               screenRect.top, SRCCOPY);
    SelectObject(memory, oldBitmap);

    bool result = false;
    if (copied) {
        std::vector<uint32_t> src(static_cast<size_t>(width) *
                                  static_cast<size_t>(height));
        memcpy(src.data(), bits, src.size() * sizeof(uint32_t));
        int outW = 0;
        int outH = 0;
        std::vector<uint32_t> blurred;
        DownscaleAndBlur(src.data(), width, height, factor > 0 ? factor : 1,
                         kBackdropBlurPasses, blurred, outW, outH);
        if (outW > 0 && outH > 0) {
            out.pixels = std::move(blurred);
            out.width = outW;
            out.height = outH;
            result = true;
        }
    }

    DeleteObject(dib);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    return result;
}

D2D1_COLOR_F ColorFromArgb(uint32_t argb) {
    D2D1_COLOR_F color = {};
    color.a = static_cast<float>((argb >> 24) & 0xFF) / 255.0f;
    color.r = static_cast<float>((argb >> 16) & 0xFF) / 255.0f;
    color.g = static_cast<float>((argb >> 8) & 0xFF) / 255.0f;
    color.b = static_cast<float>(argb & 0xFF) / 255.0f;
    return color;
}

// Defined after IconCache below (declaration order); content caches call it.
HBITMAP GetIconBitmapForMenu(const std::wstring& iconRef,
                             const std::vector<uint8_t>& iconPixels, int sizePx);

class ContentCaches {
public:
    ContentCaches() {
        text_.SetMaxEntries(256);
        icons_.SetMaxEntries(256);
    }

    void SetDevice(ID2D1DeviceContext* dc, IDWriteFactory* dwrite) {
        dc_ = dc;
        dwrite_ = dwrite;
    }

    void Bind(LayoutPanel& panel, const LayoutMetrics& metrics,
              ID2D1DeviceContext* dc) {
        SetDevice(dc, g_renderDevice.DWriteFactory());
        BindPanel(panel, metrics);
    }

    void Clear() {
        text_.Clear([](IDWriteTextLayout* layout) {
            if (layout) {
                layout->Release();
            }
        });
        icons_.Clear([](ID2D1Bitmap* bitmap) {
            if (bitmap) {
                bitmap->Release();
            }
        });
        dc_ = nullptr;
    }

    size_t TextCount() const { return text_.Size(); }
    size_t IconCount() const { return icons_.Size(); }

private:
    void BindPanel(LayoutPanel& panel, const LayoutMetrics& metrics) {
        for (LayoutItem& item : panel.items) {
            if (item.kind == ItemKind::Separator) {
                continue;
            }
            auto resources = std::make_shared<LayoutItemResources>();
            resources->text = GetOrCreateText(item, metrics);
            resources->icon = GetOrCreateIcon(item, metrics);
            // The cache owns one reference; each item's resources own their own.
            if (resources->text) {
                resources->text->AddRef();
            }
            if (resources->icon) {
                resources->icon->AddRef();
            }
            item.resources = std::move(resources);
        }
        for (LayoutPanel& child : panel.children) {
            BindPanel(child, metrics);
        }
    }

    IDWriteTextLayout* GetOrCreateText(const LayoutItem& item,
                                       const LayoutMetrics& metrics) {
        const int width = static_cast<int>(item.textRect.right - item.textRect.left);
        if (width <= 0) {
            return nullptr;
        }
        const std::wstring key =
            item.label + L'\x1f' + metrics.fontFace + L'\x1f' +
            std::to_wstring(static_cast<int>(metrics.fontSize * 4.0f)) + L'\x1f' +
            std::to_wstring(width) + L'\x1f' +
            std::to_wstring(static_cast<int>(metrics.acceleratorMode)) + L'\x1f' +
            std::to_wstring(static_cast<int>(metrics.fontWeight)) + L'\x1f' +
            std::to_wstring(static_cast<int>(metrics.fontStyle));
        if (IDWriteTextLayout** cached = text_.Find(key)) {
            return *cached;
        }
        if (!dc_ || !dwrite_) {
            return nullptr;
        }

        IDWriteTextFormat* format = nullptr;
        if (FAILED(dwrite_->CreateTextFormat(
                metrics.fontFace.c_str(), nullptr,
                FontWeightToDwrite(metrics.fontWeight),
                FontStyleToDwrite(metrics.fontStyle),
                DWRITE_FONT_STRETCH_NORMAL, metrics.fontSize, L"", &format)) ||
            !format) {
            return nullptr;
        }

        const AcceleratorText accelerated = StripAccelerators(item.label);
        const std::wstring& display =
            metrics.acceleratorMode == AcceleratorMode::Raw ? item.label
                                                            : accelerated.text;
        IDWriteTextLayout* layout = nullptr;
        dwrite_->CreateTextLayout(display.c_str(),
                                  static_cast<UINT32>(display.size()), format,
                                  static_cast<float>(width),
                                  static_cast<float>(metrics.itemHeight), &layout);
        if (layout) {
            if (metrics.acceleratorMode == AcceleratorMode::Underline) {
                for (const auto& range : accelerated.underlineRanges) {
                    const DWRITE_TEXT_RANGE textRange = {
                        static_cast<UINT32>(range.first),
                        static_cast<UINT32>(range.second - range.first)};
                    layout->SetUnderline(TRUE, textRange);
                }
            }
            layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            DWRITE_TRIMMING trimming = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            IDWriteInlineObject* ellipsis = nullptr;
            dwrite_->CreateEllipsisTrimmingSign(format, &ellipsis);
            if (ellipsis) {
                layout->SetTrimming(&trimming, ellipsis);
                ellipsis->Release();
            }
            text_.Insert(key, layout, [](IDWriteTextLayout* value) {
                if (value) {
                    value->Release();
                }
            });
        }
        format->Release();
        return layout;
    }

    ID2D1Bitmap* GetOrCreateIcon(const LayoutItem& item,
                                 const LayoutMetrics& metrics) {
        if (item.iconRef.empty() && item.iconPixels.empty()) {
            return nullptr;
        }
        std::wstring key =
            item.iconRef + L'#' + std::to_wstring(metrics.iconSize);
        if (!item.iconPixels.empty()) {
            uint64_t hash = 1469598103934665603ull;
            for (uint8_t byte : item.iconPixels) {
                hash ^= byte;
                hash *= 1099511628211ull;
            }
            key += L'#' + std::to_wstring(hash);
        }
        if (ID2D1Bitmap** cached = icons_.Find(key)) {
            return *cached;
        }
        if (!dc_) {
            return nullptr;
        }
        HBITMAP hbitmap = GetIconBitmapForMenu(item.iconRef, item.iconPixels,
                                               metrics.iconSize);
        if (!hbitmap) {
            return nullptr;
        }
        ID2D1Bitmap* bitmap = BitmapFromHBITMAP(hbitmap);
        if (bitmap) {
            icons_.Insert(key, bitmap, [](ID2D1Bitmap* value) {
                if (value) {
                    value->Release();
                }
            });
        }
        return bitmap;
    }

    ID2D1Bitmap* BitmapFromHBITMAP(HBITMAP hbitmap) {
        BITMAP bm = {};
        if (!GetObjectW(hbitmap, sizeof(bm), &bm) || bm.bmBitsPixel != 32) {
            return nullptr;
        }
        const int width = bm.bmWidth;
        const int height = bm.bmHeight;
        if (width <= 0 || height <= 0) {
            return nullptr;
        }
        std::vector<uint32_t> pixels(static_cast<size_t>(width) *
                                     static_cast<size_t>(height));
        BITMAPINFO info = {};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        HDC screen = GetDC(nullptr);
        const int lines = GetDIBits(screen, hbitmap, 0, static_cast<UINT>(height),
                                    pixels.data(), &info, DIB_RGB_COLORS);
        ReleaseDC(nullptr, screen);
        if (lines == 0) {
            return nullptr;
        }

        ID2D1Bitmap* bitmap = nullptr;
        const D2D1_SIZE_U size = {static_cast<UINT32>(width),
                                  static_cast<UINT32>(height)};
        const D2D1_BITMAP_PROPERTIES props = {
            {DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE}, 96.0f, 96.0f};
        if (FAILED(dc_->CreateBitmap(size, pixels.data(),
                                     static_cast<UINT32>(width * sizeof(uint32_t)),
                                     props, &bitmap))) {
            return nullptr;
        }
        return bitmap;
    }

    ID2D1DeviceContext* dc_ = nullptr;
    IDWriteFactory* dwrite_ = nullptr;
    LruMap<IDWriteTextLayout*> text_;
    LruMap<ID2D1Bitmap*> icons_;
};

inline ContentCaches g_contentCaches;

void DrawCheckmark(ID2D1DeviceContext* dc, const RECT& gutterRect,
                   uint32_t color) {
    ID2D1Factory* factory = g_renderDevice.D2DFactory();
    if (!factory) {
        return;
    }
    ID2D1PathGeometry* geometry = nullptr;
    if (FAILED(factory->CreatePathGeometry(&geometry)) || !geometry) {
        return;
    }
    ID2D1GeometrySink* sink = nullptr;
    if (SUCCEEDED(geometry->Open(&sink)) && sink) {
        const float left = static_cast<float>(gutterRect.left);
        const float top = static_cast<float>(gutterRect.top);
        const float width = static_cast<float>(gutterRect.right - gutterRect.left);
        const float height = static_cast<float>(gutterRect.bottom - gutterRect.top);
        sink->BeginFigure(
            D2D1_POINT_2F{left + width * 0.22f, top + height * 0.52f},
            D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLine(D2D1_POINT_2F{left + width * 0.42f, top + height * 0.72f});
        sink->AddLine(D2D1_POINT_2F{left + width * 0.78f, top + height * 0.30f});
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        sink->Close();
        sink->Release();

        ID2D1SolidColorBrush* brush = nullptr;
        if (SUCCEEDED(dc->CreateSolidColorBrush(ColorFromArgb(color), &brush)) &&
            brush) {
            dc->DrawGeometry(geometry, brush, 1.5f);
            brush->Release();
        }
    }
    geometry->Release();
}

void DrawMarkerDot(ID2D1DeviceContext* dc, const RECT& markerRect,
                   uint32_t color) {
    ID2D1SolidColorBrush* brush = nullptr;
    if (FAILED(dc->CreateSolidColorBrush(ColorFromArgb(color), &brush)) || !brush) {
        return;
    }
    const float cx = (static_cast<float>(markerRect.left) +
                      static_cast<float>(markerRect.right)) / 2.0f;
    const float cy = (static_cast<float>(markerRect.top) +
                      static_cast<float>(markerRect.bottom)) / 2.0f;
    const float radius = std::max(
        2.0f, (static_cast<float>(markerRect.right) -
               static_cast<float>(markerRect.left)) * 0.18f);
    const D2D1_ELLIPSE ellipse = {D2D1_POINT_2F{cx, cy}, radius, radius};
    dc->FillEllipse(&ellipse, brush);
    brush->Release();
}

void DrawMarkerBar(ID2D1DeviceContext* dc, const RECT& markerRect,
                   uint32_t color) {
    ID2D1SolidColorBrush* brush = nullptr;
    if (FAILED(dc->CreateSolidColorBrush(ColorFromArgb(color), &brush)) || !brush) {
        return;
    }
    const float width = std::max(
        2.0f, (static_cast<float>(markerRect.right) -
               static_cast<float>(markerRect.left)) * 0.2f);
    const float cx = (static_cast<float>(markerRect.left) +
                      static_cast<float>(markerRect.right)) / 2.0f;
    const float cy = (static_cast<float>(markerRect.top) +
                      static_cast<float>(markerRect.bottom)) / 2.0f;
    const float half =
        (static_cast<float>(markerRect.bottom) -
         static_cast<float>(markerRect.top)) * 0.25f;
    const D2D1_ROUNDED_RECT bar = {{cx - width / 2, cy - half, cx + width / 2,
                                     cy + half},
                                    width / 2, width / 2};
    dc->FillRoundedRectangle(&bar, brush);
    brush->Release();
}

// Per-corner rounded panel geometry.
ID2D1PathGeometry* BuildPanelGeometry(ID2D1Factory* factory, float width,
                                      float height, const CornerRadii& radii) {
    if (!factory) {
        return nullptr;
    }
    ID2D1PathGeometry* geometry = nullptr;
    if (FAILED(factory->CreatePathGeometry(&geometry)) || !geometry) {
        return nullptr;
    }
    ID2D1GeometrySink* sink = nullptr;
    if (FAILED(geometry->Open(&sink)) || !sink) {
        geometry->Release();
        return nullptr;
    }
    const float tl = static_cast<float>(radii.topLeft);
    const float tr = static_cast<float>(radii.topRight);
    const float br = static_cast<float>(radii.bottomRight);
    const float bl = static_cast<float>(radii.bottomLeft);
    sink->BeginFigure(D2D1_POINT_2F{tl, 0.0f}, D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLine(D2D1_POINT_2F{width - tr, 0.0f});
    if (tr > 0) {
        const D2D1_ARC_SEGMENT arc = {D2D1_POINT_2F{width, tr}, {tr, tr}, 0.0f,
                                      D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                      D2D1_ARC_SIZE_SMALL};
        sink->AddArc(&arc);
    }
    sink->AddLine(D2D1_POINT_2F{width, height - br});
    if (br > 0) {
        const D2D1_ARC_SEGMENT arc = {D2D1_POINT_2F{width - br, height}, {br, br},
                                      0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                      D2D1_ARC_SIZE_SMALL};
        sink->AddArc(&arc);
    }
    sink->AddLine(D2D1_POINT_2F{bl, height});
    if (bl > 0) {
        const D2D1_ARC_SEGMENT arc = {D2D1_POINT_2F{0.0f, height - bl}, {bl, bl},
                                      0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                      D2D1_ARC_SIZE_SMALL};
        sink->AddArc(&arc);
    }
    sink->AddLine(D2D1_POINT_2F{0.0f, tl});
    if (tl > 0) {
        const D2D1_ARC_SEGMENT arc = {D2D1_POINT_2F{tl, 0.0f}, {tl, tl}, 0.0f,
                                      D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                      D2D1_ARC_SIZE_SMALL};
        sink->AddArc(&arc);
    }
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();
    sink->Release();
    return geometry;
}

void DrawSubmenuArrow(ID2D1DeviceContext* dc, const LayoutItem& item,
                      int panelWidth, const LayoutMetrics& metrics,
                      uint32_t color) {
    ID2D1SolidColorBrush* brush = nullptr;
    if (FAILED(dc->CreateSolidColorBrush(ColorFromArgb(color), &brush)) || !brush) {
        return;
    }
    const float right = static_cast<float>(panelWidth - metrics.padding);
    const float left = right - static_cast<float>(metrics.submenuArrowWidth) * 0.4f;
    const float midY = (static_cast<float>(item.rect.top) +
                        static_cast<float>(item.rect.bottom)) / 2.0f;
    const float half = static_cast<float>(metrics.submenuArrowWidth) * 0.22f;
    dc->DrawLine(D2D1_POINT_2F{left, midY - half}, D2D1_POINT_2F{right, midY},
                 brush, 1.5f);
    dc->DrawLine(D2D1_POINT_2F{right, midY}, D2D1_POINT_2F{left, midY + half},
                 brush, 1.5f);
    brush->Release();
}

void DrawPanel(ID2D1DeviceContext* dc, const LayoutPanel& panel,
               const MenuInputState& state, const LayoutMetrics& metrics,
               const Appearance& appearance, const BackdropBitmap* backdrop,
               int margin = 0) {
    if (!dc) {
        return;
    }

    if (margin > 0) {
        const D2D1_MATRIX_3X2_F transform = {
            1.0f, 0.0f, 0.0f, 1.0f, static_cast<float>(margin),
            static_cast<float>(margin)};
        dc->SetTransform(&transform);
    }

    const D2D1_RECT_F rect = {0.0f, 0.0f, static_cast<float>(panel.size.cx),
                              static_cast<float>(panel.size.cy)};
    const float radius = static_cast<float>(metrics.cornerRadius);
    const D2D1_ROUNDED_RECT rounded = {rect, radius, radius};

    // Blurred drop shadow drawn in the margin outside the panel.
    if (appearance.shadow && margin > 0 &&
        (metrics.shadowSize > 0 || metrics.shadowBlur > 0)) {
        const int blur = metrics.shadowBlur > 0 ? metrics.shadowBlur
                                                : metrics.shadowSize;
        CornerRadii radii;
        if (metrics.hasCornerRadii) {
            radii = metrics.cornerRadii;
        } else {
            radii.topLeft = radii.topRight = radii.bottomRight =
                radii.bottomLeft = metrics.cornerRadius;
        }
        std::vector<uint32_t> shadowPixels;
        int shadowW = 0;
        int shadowH = 0;
        if (BuildShadowBitmap(panel.size.cx, panel.size.cy, radii, blur,
                              metrics.shadowOpacity, 4, shadowPixels, shadowW,
                              shadowH)) {
            ID2D1Bitmap* bitmap = nullptr;
            const D2D1_SIZE_U size = {static_cast<UINT32>(shadowW),
                                      static_cast<UINT32>(shadowH)};
            const D2D1_BITMAP_PROPERTIES props = {
                {DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED}, 96.0f,
                96.0f};
            if (SUCCEEDED(dc->CreateBitmap(
                    size, shadowPixels.data(),
                    static_cast<UINT32>(shadowW * sizeof(uint32_t)), props,
                    &bitmap)) &&
                bitmap) {
                const float maskW =
                    static_cast<float>(panel.size.cx + 2 * blur);
                const float maskH =
                    static_cast<float>(panel.size.cy + 2 * blur);
                const D2D1_RECT_F dest = {
                    static_cast<float>(-blur), static_cast<float>(-blur + 2),
                    static_cast<float>(-blur) + maskW,
                    static_cast<float>(-blur + 2) + maskH};
                dc->DrawBitmap(bitmap, dest, 1.0f, D2D1_INTERPOLATION_MODE_LINEAR);
                bitmap->Release();
            }
        }
    }

    if (backdrop && backdrop->width > 0 && backdrop->height > 0 &&
        !backdrop->pixels.empty()) {
        const float scaleX = static_cast<float>(backdrop->width) /
                             static_cast<float>(std::max(1L, panel.size.cx));
        const int maskRadius = static_cast<int>(
            static_cast<float>(metrics.cornerRadius) * scaleX);
        std::vector<uint8_t> mask;
        if (metrics.hasCornerRadii) {
            BuildRoundedRectMaskRadii(
                backdrop->width, backdrop->height,
                static_cast<int>(metrics.cornerRadii.topLeft * scaleX),
                static_cast<int>(metrics.cornerRadii.topRight * scaleX),
                static_cast<int>(metrics.cornerRadii.bottomRight * scaleX),
                static_cast<int>(metrics.cornerRadii.bottomLeft * scaleX), mask);
        } else {
            BuildRoundedRectMask(backdrop->width, backdrop->height, maskRadius, mask);
        }
        std::vector<uint32_t> pixels = backdrop->pixels;
        for (size_t i = 0; i < pixels.size() && i < mask.size(); ++i) {
            const uint32_t alpha = (((pixels[i] >> 24) & 0xFF) * mask[i]) / 255;
            const uint32_t r = ((((pixels[i] >> 16) & 0xFF) * alpha) / 255) << 16;
            const uint32_t g = ((((pixels[i] >> 8) & 0xFF) * alpha) / 255) << 8;
            const uint32_t b = ((pixels[i] & 0xFF) * alpha) / 255;
            pixels[i] = (alpha << 24) | r | g | b;
        }

        ID2D1Bitmap* bitmap = nullptr;
        const D2D1_SIZE_U size = {static_cast<UINT32>(backdrop->width),
                                  static_cast<UINT32>(backdrop->height)};
        const D2D1_BITMAP_PROPERTIES props = {
            {DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED}, 96.0f,
            96.0f};
        if (SUCCEEDED(dc->CreateBitmap(
                size, pixels.data(),
                static_cast<UINT32>(backdrop->width * sizeof(uint32_t)), props,
                &bitmap)) &&
            bitmap) {
            dc->DrawBitmap(bitmap, rect, 1.0f, D2D1_INTERPOLATION_MODE_LINEAR);
            bitmap->Release();
        }
    }

    ID2D1PathGeometry* panelGeometry = nullptr;
    if (metrics.hasCornerRadii) {
        panelGeometry = BuildPanelGeometry(g_renderDevice.D2DFactory(),
                                           static_cast<float>(panel.size.cx),
                                           static_cast<float>(panel.size.cy),
                                           metrics.cornerRadii);
    }

    if ((appearance.background >> 24) != 0) {
        ID2D1SolidColorBrush* brush = nullptr;
        if (SUCCEEDED(dc->CreateSolidColorBrush(ColorFromArgb(appearance.background),
                                                &brush)) &&
            brush) {
            if (panelGeometry) {
                dc->FillGeometry(panelGeometry, brush);
            } else {
                dc->FillRoundedRectangle(&rounded, brush);
            }
            brush->Release();
        }
    }

    for (size_t i = 0; i < panel.items.size(); ++i) {
        const LayoutItem& item = panel.items[i];
        const bool hovered = static_cast<int>(i) == state.hoverIndex ||
                             static_cast<int>(i) == state.keyboardIndex;

        if (item.kind == ItemKind::Separator) {
            ID2D1SolidColorBrush* brush = nullptr;
            if (SUCCEEDED(dc->CreateSolidColorBrush(ColorFromArgb(metrics.separator),
                                                    &brush)) &&
                brush) {
                const float y = (static_cast<float>(item.rect.top) +
                                 static_cast<float>(item.rect.bottom)) /
                                2.0f;
                const float inset =
                    static_cast<float>(metrics.padding + metrics.gutterWidth / 2);
                dc->DrawLine(D2D1_POINT_2F{inset, y},
                             D2D1_POINT_2F{static_cast<float>(panel.size.cx) - inset, y},
                             brush, 1.0f);
                brush->Release();
            }
            continue;
        }

        if (item.kind == ItemKind::Header) {
            ID2D1SolidColorBrush* headerBrush = nullptr;
            if (item.resources && item.resources->text &&
                SUCCEEDED(dc->CreateSolidColorBrush(
                    ColorFromArgb(metrics.headerColor), &headerBrush)) &&
                headerBrush) {
                dc->DrawTextLayout(
                    D2D1_POINT_2F{static_cast<float>(item.rect.left) +
                                      static_cast<float>(metrics.padding),
                                  static_cast<float>(item.textRect.top)},
                    item.resources->text, headerBrush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
                headerBrush->Release();
            }
            continue;
        }

        if (hovered && (appearance.hoverBackground >> 24) != 0) {
            ID2D1SolidColorBrush* brush = nullptr;
            if (SUCCEEDED(dc->CreateSolidColorBrush(
                    ColorFromArgb(appearance.hoverBackground), &brush)) &&
                brush) {
                const float inset = static_cast<float>(metrics.itemPadding) / 2.0f;
                const D2D1_RECT_F hoverRect = {
                    static_cast<float>(item.rect.left) + inset,
                    static_cast<float>(item.rect.top) + 1.0f,
                    static_cast<float>(item.rect.right) - inset,
                    static_cast<float>(item.rect.bottom) - 1.0f};
                dc->FillRectangle(hoverRect, brush);
                brush->Release();
            }
        }

        if ((item.flags & kModelChecked) != 0 &&
            item.markerStyle != static_cast<int>(MarkerStyle::None)) {
            const MarkerStyle markerStyle =
                static_cast<MarkerStyle>(item.markerStyle);
            if (markerStyle == MarkerStyle::Dot) {
                DrawMarkerDot(dc, item.markerRect, metrics.markerColor);
            } else if (markerStyle == MarkerStyle::Bar) {
                DrawMarkerBar(dc, item.markerRect, metrics.markerColor);
            } else {
                DrawCheckmark(dc, item.markerRect, metrics.markerColor);
            }
        }

        if (item.resources && item.resources->icon) {
            const D2D1_RECT_F iconRect = {
                static_cast<float>(item.iconRect.left),
                static_cast<float>(item.iconRect.top),
                static_cast<float>(item.iconRect.right),
                static_cast<float>(item.iconRect.bottom)};
            dc->DrawBitmap(item.resources->icon, iconRect, 1.0f,
                           D2D1_INTERPOLATION_MODE_LINEAR);
        }

        if (item.resources && item.resources->text) {
            ID2D1SolidColorBrush* brush = nullptr;
            const uint32_t color = hovered ? item.hoverTextColor : item.textColor;
            if (SUCCEEDED(dc->CreateSolidColorBrush(ColorFromArgb(color), &brush)) &&
                brush) {
                dc->DrawTextLayout(
                    D2D1_POINT_2F{static_cast<float>(item.textRect.left),
                                  static_cast<float>(item.textRect.top)},
                    item.resources->text, brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
                brush->Release();
            }
        }

        if (item.kind == ItemKind::Submenu) {
            DrawSubmenuArrow(dc, item, panel.size.cx, metrics, metrics.submenuArrow);
        }
    }

    if (metrics.borderWidth > 0 && (appearance.border >> 24) != 0) {
        ID2D1SolidColorBrush* brush = nullptr;
        if (SUCCEEDED(dc->CreateSolidColorBrush(ColorFromArgb(appearance.border),
                                                &brush)) &&
            brush) {
            if (panelGeometry) {
                dc->DrawGeometry(panelGeometry, brush,
                                 static_cast<float>(metrics.borderWidth));
            } else {
                dc->DrawRoundedRectangle(&rounded, brush,
                                         static_cast<float>(metrics.borderWidth));
            }
            brush->Release();
        }
    }
    if (panelGeometry) {
        panelGeometry->Release();
    }
}

// ===========================================================================
// [CMO:MenuInput] Keyboard and mouse state machine.
// ===========================================================================

enum class MenuInputEvent : uint8_t {
    MouseMove,
    MouseLeave,
    WheelUp,
    WheelDown,
    KeyUp,
    KeyDown,
    KeyLeft,
    KeyRight,
    KeyEnter,
    KeyEscape,
    KeyHome,
    KeyEnd,
};

bool MenuItemIsSelectable(const LayoutItem& item) {
    return item.kind != ItemKind::Separator && item.kind != ItemKind::Header &&
           (item.flags & kModelDisabled) == 0;
}

void MenuStateMouseMove(MenuInputState& state, const LayoutPanel& panel,
                        int itemIndex) {
    state.keyboardIndex = -1;
    if (itemIndex < 0 || itemIndex >= static_cast<int>(panel.items.size()) ||
        !MenuItemIsSelectable(panel.items[itemIndex])) {
        state.hoverIndex = -1;
        return;
    }
    state.hoverIndex = itemIndex;
}

void MenuStateMouseLeave(MenuInputState& state) {
    state.hoverIndex = -1;
}

void MenuStateWheel(MenuInputState& state, const LayoutPanel& panel, int delta,
                    int maxHeight = 0) {
    const int itemCount = static_cast<int>(panel.items.size());
    if (itemCount == 0) {
        state.scrollOffset = 0;
        return;
    }

    int visibleCount = itemCount;
    if (maxHeight > 0) {
        visibleCount = 0;
        int y = 0;
        for (const LayoutItem& item : panel.items) {
            const int height = static_cast<int>(item.rect.bottom - item.rect.top);
            if (y + height > maxHeight) {
                break;
            }
            y += height;
            ++visibleCount;
        }
    }

    const int maxOffset = std::max(0, itemCount - visibleCount);
    const int step = delta > 0 ? 1 : -1;
    state.scrollOffset = std::clamp(state.scrollOffset + step, 0, maxOffset);

    if (state.hoverIndex >= 0) {
        if (state.hoverIndex < state.scrollOffset) {
            state.hoverIndex = state.scrollOffset;
        } else if (state.hoverIndex >= state.scrollOffset + visibleCount) {
            state.hoverIndex = state.scrollOffset + visibleCount - 1;
        }
    }
    if (state.keyboardIndex >= 0) {
        if (state.keyboardIndex < state.scrollOffset) {
            state.keyboardIndex = state.scrollOffset;
        } else if (state.keyboardIndex >= state.scrollOffset + visibleCount) {
            state.keyboardIndex = state.scrollOffset + visibleCount - 1;
        }
    }
}

void MenuStateKey(MenuInputState& state, const LayoutPanel& panel,
                  MenuInputEvent event) {
    const int count = static_cast<int>(panel.items.size());
    if (count == 0) {
        state.keyboardIndex = -1;
        return;
    }

    auto findNext = [&](int from, int direction) {
        for (int step = 1; step <= count; ++step) {
            const int index =
                ((from + direction * step) % count + count) % count;
            if (MenuItemIsSelectable(panel.items[index])) {
                return index;
            }
        }
        return -1;
    };
    auto findEdge = [&](int direction) {
        for (int i = 0; i < count; ++i) {
            const int candidate = direction > 0 ? i : count - 1 - i;
            if (MenuItemIsSelectable(panel.items[candidate])) {
                return candidate;
            }
        }
        return -1;
    };

    switch (event) {
        case MenuInputEvent::KeyDown: {
            const int from = state.keyboardIndex >= 0 ? state.keyboardIndex : -1;
            state.keyboardIndex = findNext(from, +1);
            state.hoverIndex = -1;
            break;
        }
        case MenuInputEvent::KeyUp: {
            const int from = state.keyboardIndex >= 0 ? state.keyboardIndex : count;
            state.keyboardIndex = findNext(from, -1);
            state.hoverIndex = -1;
            break;
        }
        case MenuInputEvent::KeyHome:
            state.keyboardIndex = findEdge(+1);
            state.hoverIndex = -1;
            break;
        case MenuInputEvent::KeyEnd:
            state.keyboardIndex = findEdge(-1);
            state.hoverIndex = -1;
            break;
        case MenuInputEvent::KeyRight: {
            const int index = state.keyboardIndex >= 0 ? state.keyboardIndex
                                                       : state.hoverIndex;
            if (index >= 0 && index < count &&
                panel.items[index].kind == ItemKind::Submenu) {
                state.openSubmenu = panel.items[index].submenuIndex;
            }
            break;
        }
        case MenuInputEvent::KeyLeft:
            state.openSubmenu = -1;
            break;
        default:
            break;
    }
}

const LayoutItem* MenuStateActiveItem(const LayoutPanel& panel,
                                      const MenuInputState& state) {
    const int index = state.hoverIndex >= 0 ? state.hoverIndex : state.keyboardIndex;
    if (index < 0 || index >= static_cast<int>(panel.items.size())) {
        return nullptr;
    }
    return &panel.items[index];
}

int MenuStateVisibleItems(const LayoutPanel& panel, const MenuInputState& state,
                          int maxHeight) {
    int y = 0;
    int count = 0;
    for (int i = state.scrollOffset; i < static_cast<int>(panel.items.size()); ++i) {
        const int height =
            static_cast<int>(panel.items[i].rect.bottom - panel.items[i].rect.top);
        if (y + height > maxHeight) {
            break;
        }
        y += height;
        ++count;
    }
    return count;
}

int MenuStateItemAt(const LayoutPanel& panel, const MenuInputState& state,
                    POINT clientPoint) {
    if (clientPoint.x < 0 || clientPoint.x >= panel.size.cx) {
        return -1;
    }
    for (size_t i = 0; i < panel.items.size(); ++i) {
        const LayoutItem& item = panel.items[i];
        if (clientPoint.y >= item.rect.top && clientPoint.y < item.rect.bottom) {
            if (!MenuItemIsSelectable(item)) {
                return -1;
            }
            return static_cast<int>(i);
        }
    }
    return -1;
}

// Highest (deepest) open level whose screen rect contains the point, or -1.
int SessionLevelAtPoint(const std::vector<RECT>& windowScreenRects,
                        POINT screenPoint) {
    for (size_t i = windowScreenRects.size(); i > 0; --i) {
        const RECT& rect = windowScreenRects[i - 1];
        if (screenPoint.x >= rect.left && screenPoint.x < rect.right &&
            screenPoint.y >= rect.top && screenPoint.y < rect.bottom) {
            return static_cast<int>(i - 1);
        }
    }
    return -1;
}

POINT PanelPointForWindow(const RECT& windowScreenRect, int margin,
                          POINT screenPoint) {
    return POINT{screenPoint.x - windowScreenRect.left - margin,
                 screenPoint.y - windowScreenRect.top - margin};
}

// ===========================================================================
// [CMO:MenuWindow] Custom menu session: window wiring and invocation.
// ===========================================================================

const GUID kIidIDXGISurface = {
    0xcafcb56c, 0x6ac3, 0x4889, {0xbf, 0x47, 0x9e, 0x23, 0xbb, 0xd2, 0x60, 0xec}};

// Defined later in [CMO:Theme]; declared here for ShowCustomMenu.
bool IsDarkThemeActive();

uint32_t DpiForWindow(HWND window) {
    if (window) {
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        if (user32) {
            using GetDpiForWindow_t = UINT(WINAPI*)(HWND);
            auto getDpiForWindow = reinterpret_cast<GetDpiForWindow_t>(
                GetProcAddress(user32, "GetDpiForWindow"));
            if (getDpiForWindow) {
                const UINT dpi = getDpiForWindow(window);
                if (dpi >= 48) {
                    return dpi;
                }
            }
        }
    }
    return 96;
}

RECT WorkAreaForPoint(POINT pt) {
    RECT workArea = {0, 0, GetSystemMetrics(SM_CXSCREEN),
                     GetSystemMetrics(SM_CYSCREEN)};
    MONITORINFO info = {};
    info.cbSize = sizeof(info);
    const HMONITOR monitor = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    if (monitor && GetMonitorInfoW(monitor, &info)) {
        workArea = info.rcWork;
    }
    return workArea;
}

struct CustomMenuResult {
    std::optional<uint32_t> chosenItemId;
    bool handled = false;
    bool failed = false;
};

bool FindMenuItemById(const std::vector<MenuItem>& items, uint32_t id,
                      MenuItem& out) {
    for (const MenuItem& item : items) {
        if (item.id == id) {
            out = item;
            return true;
        }
        if (FindMenuItemById(item.children, id, out)) {
            return true;
        }
    }
    return false;
}

bool BuildMenuItemForInvocation(const MenuModel& model,
                                const InvocationDescriptor& descriptor,
                                MenuItem& out) {
    if (FindMenuItemById(model.items, descriptor.id, out)) {
        out.id = descriptor.id;
        out.action = descriptor.action;
        out.customCommandIndex = descriptor.customCommandIndex;
        out.verbOffset = descriptor.verbOffset;
        if (descriptor.hasOffset) {
            out.flags |= kModelHasOffset;
        }
        return true;
    }

    MenuItem item{};
    item.id = descriptor.id;
    item.kind = ItemKind::Command;
    item.action = descriptor.action;
    item.viewAction = descriptor.viewAction;
    item.verbOffset = descriptor.verbOffset;
    item.sortIndex = descriptor.sortIndex;
    item.customCommandIndex = descriptor.customCommandIndex;
    item.newIndex = descriptor.newIndex;
    item.canonicalVerb = descriptor.canonicalVerb;
    item.targetPath = descriptor.targetPath;
    if (descriptor.hasOffset) {
        item.flags |= kModelHasOffset;
    }
    out = std::move(item);
    return true;
}

struct MenuSession {
    CustomMenuResult result;
    std::shared_ptr<const RulesConfig> config;
    const MenuModel* model = nullptr;
    std::vector<MenuWindow*> windows;
    std::vector<MenuInputState> states;
    std::vector<BackdropBitmap> backdrops;
    int active = 0;
    int maxHeight = 0;
    int margin = 0;
    int submenuDelayMs = 150;
    int hoverCandidate = -1;
    int hoverLevel = -1;
    bool submenuTimerActive = false;
    bool done = false;
    LayoutMetrics metrics;
    Appearance appearance;
};

inline MenuSession* g_menuSession = nullptr;

void OnDeviceLost();

int MeasureTextWidthDirectWrite(const wchar_t* label, size_t length,
                                const LayoutMetrics& metrics) {
    const std::wstring display = StripAccelerators(std::wstring(label, length)).text;
    label = display.c_str();
    length = display.size();
    IDWriteFactory* dwrite = g_renderDevice.DWriteFactory();
    if (!dwrite || length == 0) {
        return EstimateTextWidth(label, length, metrics);
    }
    IDWriteTextFormat* format = nullptr;
    if (FAILED(dwrite->CreateTextFormat(
            metrics.fontFace.c_str(), nullptr,
            FontWeightToDwrite(metrics.fontWeight),
            FontStyleToDwrite(metrics.fontStyle), DWRITE_FONT_STRETCH_NORMAL,
            metrics.fontSize, L"", &format)) ||
        !format) {
        return EstimateTextWidth(label, length, metrics);
    }
    int width = EstimateTextWidth(label, length, metrics);
    IDWriteTextLayout* layout = nullptr;
    if (SUCCEEDED(dwrite->CreateTextLayout(label, static_cast<UINT32>(length),
                                           format, 4096.0f,
                                           static_cast<float>(metrics.itemHeight),
                                           &layout)) &&
        layout) {
        DWRITE_TEXT_METRICS textMetrics = {};
        if (SUCCEEDED(layout->GetMetrics(&textMetrics))) {
            width = static_cast<int>(textMetrics.widthIncludingTrailingWhitespace) + 2;
        }
        layout->Release();
    }
    format->Release();
    return width;
}

struct AnimationSpec {
    bool animate = false;
    bool slide = false;
    int durationMs = 0;
};

// MinGW's dcomp.h lists overload pairs in the wrong order (and omits
// SetOpacity, which lives on IDCompositionVisual3). Mirror the real SDK vtable
// order so animation calls hit the intended slots.
struct IDCompositionVisualCorrect : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SetOffsetX(float offsetX) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetOffsetX(IDCompositionAnimation* animation) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetOffsetY(float offsetY) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetOffsetY(IDCompositionAnimation* animation) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetTransform(
        const D2D_MATRIX_3X2_F& matrix) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetTransform(
        IDCompositionTransform* transform) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetTransformParent(
        IDCompositionVisual* visual) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEffect(IDCompositionEffect* effect) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetBitmapInterpolationMode(
        DCOMPOSITION_BITMAP_INTERPOLATION_MODE mode) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetBorderMode(
        DCOMPOSITION_BORDER_MODE mode) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetClip(const D2D_RECT_F& rect) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetClip(IDCompositionClip* clip) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetContent(IUnknown* content) = 0;
    virtual HRESULT STDMETHODCALLTYPE AddVisual(IDCompositionVisual* visual,
                                                BOOL insertAbove,
                                                IDCompositionVisual* referenceVisual) = 0;
    virtual HRESULT STDMETHODCALLTYPE RemoveVisual(IDCompositionVisual* visual) = 0;
    virtual HRESULT STDMETHODCALLTYPE RemoveAllVisuals() = 0;
    virtual HRESULT STDMETHODCALLTYPE SetCompositeMode(
        DCOMPOSITION_COMPOSITE_MODE mode) = 0;
};

struct IDCompositionEffectGroupCorrect : public IDCompositionEffect {
    virtual HRESULT STDMETHODCALLTYPE SetOpacity(float opacity) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetOpacity(
        IDCompositionAnimation* animation) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetTransform3D(
        IDCompositionTransform3D* transform) = 0;
};

AnimationSpec ResolveAnimationSpec(const Appearance& appearance) {
    AnimationSpec spec;
    switch (appearance.animation) {
        case AnimationKind::None:
            break;
        case AnimationKind::Fade:
            spec.animate = true;
            spec.durationMs = appearance.animationDuration;
            break;
        case AnimationKind::Slide:
            spec.animate = true;
            spec.slide = true;
            spec.durationMs = appearance.animationDuration;
            break;
    }
    if (spec.durationMs < 0) {
        spec.durationMs = 0;
    }
    return spec;
}

void ApplyWindowAnimation(IDCompositionVisual* visual, const AnimationSpec& spec,
                          bool opening) {
    if (!visual) {
        return;
    }
    IDCompositionDevice* comp = g_renderDevice.CompDevice();
    if (!comp) {
        return;
    }

    if (!spec.animate || spec.durationMs <= 0) {
        // Default opacity is 1; non-animated closes destroy the window.
        return;
    }

    IDCompositionEffectGroup* group = nullptr;
    if (FAILED(comp->CreateEffectGroup(&group)) || !group) {
        return;
    }
    auto* groupCorrect = reinterpret_cast<IDCompositionEffectGroupCorrect*>(group);
    auto* visualCorrect = reinterpret_cast<IDCompositionVisualCorrect*>(visual);

    const double duration = static_cast<double>(spec.durationMs) / 1000.0;
    IDCompositionAnimation* opacity = nullptr;
    if (SUCCEEDED(comp->CreateAnimation(&opacity)) && opacity) {
        opacity->AddCubic(0.0, opening ? 0.0 : 1.0,
                          (opening ? 1.0 : -1.0) / duration, 0.0, 0.0);
        opacity->End(duration, opening ? 1.0 : 0.0);
        groupCorrect->SetOpacity(opacity);
        opacity->Release();
    }
    visualCorrect->SetEffect(group);
    group->Release();

    if (spec.slide) {
        IDCompositionAnimation* slide = nullptr;
        if (SUCCEEDED(comp->CreateAnimation(&slide)) && slide) {
            slide->AddCubic(0.0, opening ? 12.0 : 0.0,
                            (opening ? -12.0 : 12.0) / duration, 0.0, 0.0);
            slide->End(duration, opening ? 0.0 : 12.0);
            visualCorrect->SetOffsetX(slide);
            slide->Release();
        }
    }
    comp->Commit();
}

void RenderMenuWindow(MenuWindow* window, const LayoutPanel& panel,
                      const MenuInputState& state, const LayoutMetrics& metrics,
                      const Appearance& appearance,
                      const BackdropBitmap* backdrop, int margin) {
    if (!window || !window->SwapChain() || !g_renderDevice.D2DDevice()) {
        return;
    }    IDXGISurface* surface = nullptr;
    if (FAILED(window->SwapChain()->GetBuffer(
            0, kIidIDXGISurface, reinterpret_cast<void**>(&surface))) ||
        !surface) {
        return;
    }
    ID2D1DeviceContext* dc = nullptr;
    if (FAILED(g_renderDevice.D2DDevice()->CreateDeviceContext(
            D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc)) ||
        !dc) {
        surface->Release();
        return;
    }
    const D2D1_BITMAP_PROPERTIES1 props = {
        {DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED}, 96.0f, 96.0f,
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW};
    ID2D1Bitmap1* target = nullptr;
    if (SUCCEEDED(dc->CreateBitmapFromDxgiSurface(surface, &props, &target)) &&
        target) {
        dc->SetTarget(target);
        dc->BeginDraw();
        dc->Clear(nullptr);
        DrawPanel(dc, panel, state, metrics, appearance, backdrop, margin);
        const HRESULT drawResult = dc->EndDraw();
        target->Release();
        if (drawResult == D2DERR_RECREATE_TARGET) {
            OnDeviceLost();
        }
    }
    dc->Release();
    surface->Release();
    window->Present();
}

void RepaintMenuWindow(MenuSession* session, int index) {
    if (!session || index < 0 ||
        index >= static_cast<int>(session->windows.size())) {
        return;
    }
    MenuWindow* window = session->windows[index];
    if (!window || !window->Panel()) {
        return;
    }
    const BackdropBitmap* backdrop = nullptr;
    if (index < static_cast<int>(session->backdrops.size()) &&
        session->backdrops[index].width > 0) {
        backdrop = &session->backdrops[index];
    }
    RenderMenuWindow(window, *window->Panel(), session->states[index],
                     session->metrics, session->appearance, backdrop,
                     session->margin);
}

void CloseSubmenusBelow(MenuSession* session, int index) {
    while (static_cast<int>(session->windows.size()) > index + 1) {
        MenuWindow* window = session->windows.back();
        session->windows.pop_back();
        session->states.pop_back();
        if (session->backdrops.size() >= session->windows.size() + 1) {
            session->backdrops.pop_back();
        }
        ApplyWindowAnimation(window->CompVisual(),
                             ResolveAnimationSpec(session->appearance), false);
        g_menuWindowPool.Release(window);
    }
    session->active = index;
    if (index >= 0 && index < static_cast<int>(session->states.size())) {
        session->states[index].openSubmenu = -1;
    }
}

void OpenSubmenu(MenuSession* session, int index, int itemIndex) {
    if (!session || index < 0 ||
        index >= static_cast<int>(session->windows.size())) {
        return;
    }
    const LayoutPanel* parent = session->windows[index]->Panel();
    if (!parent || itemIndex < 0 ||
        itemIndex >= static_cast<int>(parent->items.size())) {
        return;
    }
    const LayoutItem& item = parent->items[itemIndex];
    if (item.kind != ItemKind::Submenu || item.submenuIndex < 0 ||
        item.submenuIndex >= static_cast<int>(parent->children.size())) {
        return;
    }

    CloseSubmenusBelow(session, index);
    const LayoutPanel& childPanel = parent->children[item.submenuIndex];

    MenuWindow* child = g_menuWindowPool.Acquire();
    if (!child ||
        !child->Create(session->windows[index]->Handle(), &childPanel, false,
                       session->margin)) {
        if (child) {
            g_menuWindowPool.Release(child);
        }
        return;
    }

    POINT topLeft = {item.rect.left + session->margin,
                     item.rect.top + session->margin};
    ClientToScreen(session->windows[index]->Handle(), &topLeft);
    const RECT itemScreen = {topLeft.x, topLeft.y,
                             topLeft.x + (item.rect.right - item.rect.left),
                             topLeft.y + (item.rect.bottom - item.rect.top)};
    const RECT workArea = WorkAreaForPoint(topLeft);
    POINT childPos =
        SubmenuPosition(itemScreen, childPanel.size, workArea, 4);
    childPos.x -= session->margin;
    childPos.y -= session->margin;
    child->Move(childPos);

    BackdropBitmap backdrop;
    const POINT panelTopLeft = {childPos.x + session->margin,
                                childPos.y + session->margin};
    const RECT captureRect = {panelTopLeft.x, panelTopLeft.y,
                              panelTopLeft.x + childPanel.size.cx,
                              panelTopLeft.y + childPanel.size.cy};
    const bool hasBackdrop =
        session->appearance.blur &&
        CaptureBackdrop(captureRect, kBackdropDownscaleFactor, backdrop);

    session->windows.push_back(child);
    session->states.push_back(MenuInputState{});
    session->backdrops.push_back(hasBackdrop ? std::move(backdrop)
                                             : BackdropBitmap{});
    session->active = static_cast<int>(session->windows.size()) - 1;
    session->states[index].openSubmenu = item.submenuIndex;
    session->states[index].hoverIndex = itemIndex;

    RepaintMenuWindow(session, index);
    RenderMenuWindow(child, childPanel, session->states.back(), session->metrics,
                     session->appearance,
                     hasBackdrop ? &session->backdrops.back() : nullptr,
                     session->margin);
    ApplyWindowAnimation(child->CompVisual(),
                         ResolveAnimationSpec(session->appearance), true);
    child->Show();
    SetFocus(child->Handle());
}

constexpr UINT_PTR kMenuSubmenuTimerId = 1;

std::vector<RECT> SessionWindowRects(const MenuSession* session) {
    std::vector<RECT> rects;
    rects.reserve(session->windows.size());
    for (MenuWindow* menuWindow : session->windows) {
        RECT rect = {};
        GetWindowRect(menuWindow->Handle(), &rect);
        rects.push_back(rect);
    }
    return rects;
}

LRESULT CustomMenuWindowProc(MenuWindow* window, HWND hwnd, UINT msg,
                             WPARAM wParam, LPARAM lParam) {
    MenuSession* session = g_menuSession;
    if (!session) {
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    int index = -1;
    for (size_t i = 0; i < session->windows.size(); ++i) {
        if (session->windows[i] == window) {
            index = static_cast<int>(i);
            break;
        }
    }
    if (index < 0 || !window->Panel()) {
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    const LayoutPanel& panel = *window->Panel();
    MenuInputState& state = session->states[index];
    const POINT clientPoint = {static_cast<short>(LOWORD(lParam)),
                               static_cast<short>(HIWORD(lParam))};
    const POINT panelPoint = {clientPoint.x - session->margin,
                              clientPoint.y - session->margin};

    switch (msg) {
        case WM_MOUSEMOVE: {
            const std::vector<RECT> rects = SessionWindowRects(session);
            POINT screen = clientPoint;
            ClientToScreen(hwnd, &screen);
            const int level = SessionLevelAtPoint(rects, screen);
            if (level < 0) {
                if (session->active >= 0 &&
                    session->active < static_cast<int>(session->states.size())) {
                    MenuStateMouseLeave(session->states[session->active]);
                    RepaintMenuWindow(session, session->active);
                }
                CloseSubmenusBelow(session, 0);
                if (session->submenuTimerActive) {
                    KillTimer(hwnd, kMenuSubmenuTimerId);
                    session->submenuTimerActive = false;
                    session->hoverCandidate = -1;
                }
                return 0;
            }

            session->active = level;
            const LayoutPanel& levelPanel = *session->windows[level]->Panel();
            MenuInputState& levelState = session->states[level];
            const POINT levelPoint =
                PanelPointForWindow(rects[level], session->margin, screen);

            const int hit = MenuStateItemAt(levelPanel, levelState, levelPoint);
            const bool isSubmenu =
                hit >= 0 && levelPanel.items[hit].kind == ItemKind::Submenu;
            const bool ownsOpenChild =
                levelState.openSubmenu >= 0 && hit >= 0 &&
                levelPanel.items[hit].submenuIndex == levelState.openSubmenu;
            if (!ownsOpenChild) {
                CloseSubmenusBelow(session, level);
            }

            if (hit != levelState.hoverIndex) {
                MenuStateMouseMove(levelState, levelPanel, hit);
                RepaintMenuWindow(session, level);
            }

            if (isSubmenu) {
                if (!ownsOpenChild &&
                    (session->hoverCandidate != hit ||
                     !session->submenuTimerActive)) {
                    session->hoverCandidate = hit;
                    session->hoverLevel = level;
                    session->submenuTimerActive = true;
                    SetTimer(hwnd, kMenuSubmenuTimerId,
                             static_cast<UINT>(session->submenuDelayMs <= 0
                                                   ? 1
                                                   : session->submenuDelayMs),
                             nullptr);
                }
            } else if (session->submenuTimerActive) {
                KillTimer(hwnd, kMenuSubmenuTimerId);
                session->submenuTimerActive = false;
                session->hoverCandidate = -1;
            }
            return 0;
        }
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN: {
            const std::vector<RECT> rects = SessionWindowRects(session);
            POINT screen = clientPoint;
            ClientToScreen(hwnd, &screen);
            if (SessionLevelAtPoint(rects, screen) < 0) {
                if (g_settings.debugLogging) {
                    Wh_Log(L"Outside %s closes the session",
                           msg == WM_RBUTTONDOWN ? L"right-click" : L"click");
                }
                if (msg == WM_RBUTTONDOWN) {
                    const HWND target = WindowFromPoint(screen);
                    bool ours = false;
                    for (MenuWindow* menuWindow : session->windows) {
                        if (menuWindow->Handle() == target) {
                            ours = true;
                            break;
                        }
                    }
                    if (target && !ours) {
                        POINT client = screen;
                        ScreenToClient(target, &client);
                        if (g_settings.debugLogging) {
                            Wh_Log(L"Forwarding WM_RBUTTONUP to %p", target);
                        }
                        PostMessageW(target, WM_RBUTTONUP, MK_RBUTTON,
                                     MAKELPARAM(client.x, client.y));
                    }
                }
                session->done = true;
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            const std::vector<RECT> rects = SessionWindowRects(session);
            POINT screen = clientPoint;
            ClientToScreen(hwnd, &screen);
            const int level = SessionLevelAtPoint(rects, screen);
            if (level >= 0) {
                const LayoutPanel& levelPanel = *session->windows[level]->Panel();
                MenuInputState& levelState = session->states[level];
                const POINT levelPoint =
                    PanelPointForWindow(rects[level], session->margin, screen);
                const int hit = MenuStateItemAt(levelPanel, levelState, levelPoint);
                if (hit >= 0) {
                    const LayoutItem& item = levelPanel.items[hit];
                    if (item.kind == ItemKind::Submenu) {
                        OpenSubmenu(session, level, hit);
                    } else {
                        session->result.chosenItemId = item.invocation.id;
                        session->done = true;
                    }
                }
            }
            return 0;
        }
        case WM_RBUTTONUP:
            return 0;
        case WM_MOUSEWHEEL: {
            const std::vector<RECT> rects = SessionWindowRects(session);
            const POINT screen = {static_cast<short>(LOWORD(lParam)),
                                  static_cast<short>(HIWORD(lParam))};
            const int level = SessionLevelAtPoint(rects, screen);
            if (level >= 0) {
                const LayoutPanel& levelPanel = *session->windows[level]->Panel();
                MenuStateWheel(session->states[level], levelPanel,
                               static_cast<short>(HIWORD(wParam)),
                               session->maxHeight);
                RepaintMenuWindow(session, level);
            }
            return 0;
        }
        case WM_TIMER: {
            if (wParam == kMenuSubmenuTimerId) {
                session->submenuTimerActive = false;
                KillTimer(hwnd, kMenuSubmenuTimerId);
                const int level = session->hoverLevel;
                if (level >= 0 && level == session->active &&
                    level < static_cast<int>(session->windows.size()) &&
                    session->hoverCandidate >= 0 &&
                    session->hoverCandidate ==
                        session->states[level].hoverIndex) {
                    OpenSubmenu(session, level, session->hoverCandidate);
                }
            }
            return 0;
        }
        case WM_KEYDOWN: {
            if (index != session->active) {
                return 0;
            }
            switch (wParam) {
                case VK_ESCAPE: {
                    if (index > 0) {
                        CloseSubmenusBelow(session, index - 1);
                    } else {
                        session->done = true;
                    }
                    return 0;
                }
                case VK_LEFT: {
                    if (index > 0) {
                        CloseSubmenusBelow(session, index - 1);
                    } else {
                        MenuStateKey(state, panel, MenuInputEvent::KeyLeft);
                    }
                    return 0;
                }
                case VK_RIGHT: {
                    const int activeIndex =
                        state.hoverIndex >= 0 ? state.hoverIndex : state.keyboardIndex;
                    if (activeIndex >= 0 &&
                        activeIndex < static_cast<int>(panel.items.size()) &&
                        panel.items[activeIndex].kind == ItemKind::Submenu) {
                        OpenSubmenu(session, index, activeIndex);
                    }
                    return 0;
                }
                case VK_RETURN: {
                    const LayoutItem* activeItem =
                        MenuStateActiveItem(panel, state);
                    if (activeItem) {
                        const int activeIndex = static_cast<int>(
                            activeItem - &panel.items[0]);
                        if (activeItem->kind == ItemKind::Submenu) {
                            OpenSubmenu(session, index, activeIndex);
                        } else {
                            session->result.chosenItemId =
                                activeItem->invocation.id;
                            session->done = true;
                        }
                    }
                    return 0;
                }
                default:
                    break;
            }

            MenuInputEvent event;
            switch (wParam) {
                case VK_UP:
                    event = MenuInputEvent::KeyUp;
                    break;
                case VK_DOWN:
                    event = MenuInputEvent::KeyDown;
                    break;
                case VK_HOME:
                    event = MenuInputEvent::KeyHome;
                    break;
                case VK_END:
                    event = MenuInputEvent::KeyEnd;
                    break;
                default:
                    return 0;
            }
            MenuStateKey(state, panel, event);
            RepaintMenuWindow(session, index);
            return 0;
        }
        case WM_ACTIVATE: {
            if (LOWORD(wParam) == WA_INACTIVE && index == 0) {
                const HWND newActive = reinterpret_cast<HWND>(lParam);
                bool ours = false;
                for (MenuWindow* menuWindow : session->windows) {
                    if (menuWindow->Handle() == newActive) {
                        ours = true;
                        break;
                    }
                }
                if (!ours) {
                    session->done = true;
                }
            }
            return 0;
        }
        case WM_ACTIVATEAPP: {
            if (!wParam) {
                session->done = true;
            }
            return 0;
        }
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

CustomMenuResult ShowCustomMenu(const MenuModel& model, const LayoutKey& key,
                                HWND owner, POINT pt) {
    CustomMenuResult result;

    if (!g_renderDevice.IsReady() && !g_renderDevice.Initialize()) {
        result.failed = true;
        return result;
    }

    std::shared_ptr<const RulesConfig> config = g_configStore.Snapshot();
    const RulesConfig emptyConfig;
    const RulesConfig& effective = config ? *config : emptyConfig;
    const Appearance appearance = ResolveAppearance(effective, key.darkTheme);
    const LayoutMetrics metrics =
        ResolveLayoutMetrics(appearance, key.dpi, key.darkTheme);

    std::shared_ptr<const LayoutPanel> panel = g_layoutCache.Find(key);
    if (!panel) {
        auto built = std::make_shared<LayoutPanel>(
            BuildLayoutPanel(model.items, metrics, &MeasureTextWidthDirectWrite));
        bool bound = false;
        ID2D1DeviceContext* bindDc = nullptr;
        if (SUCCEEDED(g_renderDevice.D2DDevice()->CreateDeviceContext(
                D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &bindDc)) &&
            bindDc) {
            g_contentCaches.Bind(*built, metrics, bindDc);
            bindDc->Release();
            bound = true;
        }
        if (bound) {
            g_layoutCache.Put(key, built);
        }
        panel = built;
    }
    if (!panel) {
        result.failed = true;
        return result;
    }

    MenuWindow* root = g_menuWindowPool.Acquire();
    const int margin =
        appearance.shadow && (metrics.shadowSize > 0 || metrics.shadowBlur > 0)
            ? std::min(std::max(metrics.shadowSize, metrics.shadowBlur), 24)
            : 0;
    if (!root || !root->Create(owner, panel.get(), true, margin)) {
        if (root) {
            g_menuWindowPool.Release(root);
        }
        result.failed = true;
        return result;
    }

    const RECT workArea = WorkAreaForPoint(pt);
    const POINT panelPos = ClampPanelPosition(pt, panel->size, workArea);

    MenuSession session;
    session.config = config;
    session.model = &model;
    session.metrics = metrics;
    session.appearance = appearance;
    session.submenuDelayMs = g_settings.submenuDelayMs;
    if (session.submenuDelayMs < 0) {
        DWORD systemDelay = 400;
        session.submenuDelayMs =
            SystemParametersInfoW(SPI_GETMENUSHOWDELAY, 0, &systemDelay, 0)
                ? static_cast<int>(systemDelay)
                : 400;
    }
    session.maxHeight = workArea.bottom - workArea.top;
    session.margin = margin;
    session.windows.push_back(root);
    session.states.push_back(MenuInputState{});

    BackdropBitmap backdrop;
    const RECT captureRect = {panelPos.x, panelPos.y,
                              panelPos.x + panel->size.cx,
                              panelPos.y + panel->size.cy};
    const bool hasBackdrop =
        appearance.blur &&
        CaptureBackdrop(captureRect, kBackdropDownscaleFactor, backdrop);
    session.backdrops.push_back(hasBackdrop ? std::move(backdrop)
                                            : BackdropBitmap{});

    g_menuSession = &session;
    g_menuWindowMessageHook = &CustomMenuWindowProc;
    if (g_settings.debugLogging) {
        Wh_Log(L"Custom menu session start (margin=%d dpi=%u)", margin, key.dpi);
    }

    root->Move(POINT{panelPos.x - margin, panelPos.y - margin});
    RenderMenuWindow(root, *panel, session.states[0], metrics, appearance,
                     hasBackdrop ? &session.backdrops[0] : nullptr, margin);
    ApplyWindowAnimation(root->CompVisual(), ResolveAnimationSpec(appearance), true);
    root->Show();
    SetForegroundWindow(root->Handle());
    SetFocus(root->Handle());
    SetCapture(root->Handle());

    MSG msg = {};
    while (!session.done) {
        const BOOL got = GetMessageW(&msg, nullptr, 0, 0);
        if (got <= 0) {
            if (got == 0) {
                PostQuitMessage(static_cast<int>(msg.wParam));
            }
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (session.submenuTimerActive) {
        KillTimer(root->Handle(), kMenuSubmenuTimerId);
        session.submenuTimerActive = false;
    }
    const AnimationSpec closing = ResolveAnimationSpec(appearance);
    if (closing.animate && closing.durationMs > 0) {
        for (MenuWindow* window : session.windows) {
            ApplyWindowAnimation(window->CompVisual(), closing, false);
        }
        Sleep(static_cast<DWORD>(closing.durationMs));
    }
    if (GetCapture() == root->Handle()) {
        ReleaseCapture();
    }
    for (size_t i = session.windows.size(); i > 1; --i) {
        g_menuWindowPool.Release(session.windows[i - 1]);
    }
    g_menuWindowPool.Release(root);
    g_menuWindowMessageHook = nullptr;
    g_menuSession = nullptr;
    if (g_settings.debugLogging) {
        Wh_Log(L"Custom menu session end (chosen=%d)",
               session.result.chosenItemId.has_value() ? 1 : 0);
    }

    result = session.result;
    result.handled = true;
    return result;
}

void PrebuildLayoutsForWarmup(const std::vector<MenuModel>& models, uint32_t dpi,
                              bool darkTheme) {
    if (!g_renderDevice.IsReady() && !g_renderDevice.Initialize()) {
        return;
    }
    std::shared_ptr<const RulesConfig> config = g_configStore.Snapshot();
    const RulesConfig emptyConfig;
    const RulesConfig& effective = config ? *config : emptyConfig;
    const Appearance appearance = ResolveAppearance(effective, darkTheme);
    const LayoutMetrics metrics =
        ResolveLayoutMetrics(appearance, dpi, darkTheme);

    ID2D1DeviceContext* dc = nullptr;
    if (FAILED(g_renderDevice.D2DDevice()->CreateDeviceContext(
            D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc)) ||
        !dc) {
        return;
    }
    for (const MenuModel& model : models) {
        const LayoutKey key =
            MakeLayoutKey(model.sig, effective, dpi, darkTheme, model);
        if (g_layoutCache.Find(key)) {
            continue;
        }
        auto panel =
            std::make_shared<LayoutPanel>(BuildLayoutPanel(
                model.items, metrics, &MeasureTextWidthDirectWrite));
        g_contentCaches.Bind(*panel, metrics, dc);
        g_layoutCache.Put(key, panel);
    }
    dc->Release();
}

void OnDeviceLost() {
    g_layoutCache.InvalidateDevice();
    g_contentCaches.Clear();
    g_renderDevice.HandleDeviceLost();
}

// Shell property keys used by the Sort by and Group by submenus (all in the
// shell's System property set, defined here so no SDK propkey.h is needed).
const PROPERTYKEY kShellPropertyKeys[] = {
    {{0xB725F130, 0x47EF, 0x101A, {0xA5, 0xF1, 0x02, 0x60, 0x8C, 0x9E, 0xEB, 0xAC}},
     10},  // Name
    {{0xB725F130, 0x47EF, 0x101A, {0xA5, 0xF1, 0x02, 0x60, 0x8C, 0x9E, 0xEB, 0xAC}},
     14},  // Date modified
    {{0xB725F130, 0x47EF, 0x101A, {0xA5, 0xF1, 0x02, 0x60, 0x8C, 0x9E, 0xEB, 0xAC}},
     4},  // Type
    {{0xB725F130, 0x47EF, 0x101A, {0xA5, 0xF1, 0x02, 0x60, 0x8C, 0x9E, 0xEB, 0xAC}},
     12},  // Size
};
const wchar_t* const kShellPropertyLabels[] = {L"Name", L"Date modified", L"Type",
                                               L"Size"};
constexpr uint32_t kShellPropertyKeyCount = ARRAYSIZE(kShellPropertyKeys);
constexpr uint32_t kGroupNoneIndex = 0xFFFFFFFF;

// Core model for a context. The common commands come first, cached extension
// items are merged in later, and the native fallback stays last.
MenuModel BuildCoreModel(Scope scope, const std::vector<std::wstring>& paths, Shape shape) {
    MenuModel model{};
    const std::wstring typeKey =
        scope == Scope::Files ? MakeTypeKey(paths) : std::wstring(L"*");
    model.sig = ContextSignature{scope, typeKey, shape, Variant::Normal};

    uint32_t nextId = 1;
    auto makeCommand = [&](std::wstring label, std::wstring verb, uint32_t flags,
                           std::wstring iconRef = L"") {
        MenuItem item{};
        item.id = nextId++;
        item.kind = ItemKind::Command;
        item.label = std::move(label);
        item.canonicalVerb = std::move(verb);
        item.action = ActionKind::ShellVerb;
        item.flags = flags;
        item.iconRef = std::move(iconRef);
        return item;
    };
    auto makeViewAction = [&](std::wstring label, std::wstring dedupVerb,
                              ViewAction action, uint32_t flags,
                              std::wstring iconRef = L"", int32_t iconSize = -1) {
        MenuItem item{};
        item.id = nextId++;
        item.kind = ItemKind::Command;
        item.label = std::move(label);
        item.canonicalVerb = std::move(dedupVerb);
        item.action = ActionKind::ViewAction;
        item.viewAction = static_cast<uint32_t>(action);
        item.flags = flags;
        item.iconRef = std::move(iconRef);
        item.iconSize = iconSize;
        return item;
    };
    auto addCommand = [&](std::wstring label, std::wstring verb,
                          uint32_t flags = kModelNone, std::wstring iconRef = L"") {
        model.items.push_back(
            makeCommand(std::move(label), std::move(verb), flags, std::move(iconRef)));
    };
    auto addViewAction = [&](std::wstring label, std::wstring dedupVerb,
                             ViewAction action, uint32_t flags = kModelNone,
                             std::wstring iconRef = L"") {
        model.items.push_back(makeViewAction(std::move(label), std::move(dedupVerb),
                                             action, flags, std::move(iconRef)));
    };
    auto addSeparator = [&]() {
        MenuItem item{};
        item.id = nextId++;
        item.kind = ItemKind::Separator;
        model.items.push_back(std::move(item));
    };
    auto addSubmenu = [&](std::wstring label) -> MenuItem& {
        MenuItem item{};
        item.id = nextId++;
        item.kind = ItemKind::Submenu;
        item.action = ActionKind::Submenu;
        item.label = std::move(label);
        model.items.push_back(std::move(item));
        return model.items.back();
    };
    auto addFallback = [&]() {
        MenuItem item{};
        item.id = nextId++;
        item.kind = ItemKind::Command;
        item.action = ActionKind::Fallback;
        item.label = L"Show classic menu";
        model.items.push_back(std::move(item));
    };

    const bool multi = shape == Shape::Multi;
    const uint32_t multiDisabled = multi ? kModelDisabled : kModelNone;
    const std::wstring openLabel = FormatMultiLabel(L"Open", paths.size());

    if (scope == Scope::Background || scope == Scope::Desktop) {
        MenuItem& viewMenu = addSubmenu(L"View");
        viewMenu.iconRef = L"@glyph:E890";
        viewMenu.children.push_back(makeViewAction(L"Extra large icons", L"viewxlarge",
                                                   ViewAction::ViewExtraLargeIcons,
                                                   kModelNone, L"@glyph:F0E2", 256));
        viewMenu.children.push_back(makeViewAction(L"Large icons", L"viewlarge",
                                                   ViewAction::ViewLargeIcons, kModelNone,
                                                   L"@glyph:F0E2", 96));
        viewMenu.children.push_back(makeViewAction(L"Medium icons", L"viewmedium",
                                                   ViewAction::ViewMediumIcons, kModelNone,
                                                   L"@glyph:E8A9", 48));
        viewMenu.children.push_back(makeViewAction(L"Small icons", L"viewsmall",
                                                   ViewAction::ViewSmallIcons, kModelNone,
                                                   L"@glyph:E8A9"));
        viewMenu.children.push_back(makeViewAction(L"List", L"viewlist",
                                                   ViewAction::ViewList, kModelNone,
                                                   L"@glyph:EA37"));
        viewMenu.children.push_back(makeViewAction(L"Details", L"viewdetails",
                                                   ViewAction::ViewDetails, kModelNone,
                                                   L"@glyph:E9D5"));
        viewMenu.children.push_back(makeViewAction(L"Tiles", L"viewtiles",
                                                   ViewAction::ViewTiles, kModelNone,
                                                   L"@glyph:ECA5"));
        viewMenu.children.push_back(makeViewAction(L"Content", L"viewcontent",
                                                   ViewAction::ViewContent, kModelNone,
                                                   L"@glyph:E8FD"));
        {
            MenuItem separator{};
            separator.id = nextId++;
            separator.kind = ItemKind::Separator;
            viewMenu.children.push_back(std::move(separator));
        }
        viewMenu.children.push_back(makeViewAction(L"Auto arrange icons", L"autoarrange",
                                                   ViewAction::AutoArrange, kModelNone));
        viewMenu.children.push_back(makeViewAction(L"Align icons to grid", L"aligngrid",
                                                   ViewAction::AlignToGrid, kModelNone));

        MenuItem& sortMenu = addSubmenu(L"Sort by");
        sortMenu.iconRef = L"@glyph:E8CB";
        for (uint32_t i = 0; i < kShellPropertyKeyCount; ++i) {
            MenuItem sortItem{};
            sortItem.id = nextId++;
            sortItem.kind = ItemKind::Command;
            sortItem.label = kShellPropertyLabels[i];
            sortItem.action = ActionKind::SortBy;
            sortItem.sortIndex = i;
            sortItem.sortAscending = true;
            sortMenu.children.push_back(std::move(sortItem));
        }
        {
            MenuItem separator{};
            separator.id = nextId++;
            separator.kind = ItemKind::Separator;
            sortMenu.children.push_back(std::move(separator));
        }
        for (bool ascending : {true, false}) {
            MenuItem directionItem{};
            directionItem.id = nextId++;
            directionItem.kind = ItemKind::Command;
            directionItem.label = ascending ? L"Ascending" : L"Descending";
            directionItem.action = ActionKind::SortDirection;
            directionItem.sortAscending = ascending;
            sortMenu.children.push_back(std::move(directionItem));
        }

        MenuItem& groupMenu = addSubmenu(L"Group by");
        groupMenu.iconRef = L"@glyph:E902";
        {
            MenuItem noneItem{};
            noneItem.id = nextId++;
            noneItem.kind = ItemKind::Command;
            noneItem.label = L"(None)";
            noneItem.action = ActionKind::GroupBy;
            noneItem.sortIndex = kGroupNoneIndex;
            groupMenu.children.push_back(std::move(noneItem));
        }
        for (uint32_t i = 0; i < kShellPropertyKeyCount; ++i) {
            MenuItem groupItem{};
            groupItem.id = nextId++;
            groupItem.kind = ItemKind::Command;
            groupItem.label = kShellPropertyLabels[i];
            groupItem.action = ActionKind::GroupBy;
            groupItem.sortIndex = i;
            groupMenu.children.push_back(std::move(groupItem));
        }
        {
            MenuItem separator{};
            separator.id = nextId++;
            separator.kind = ItemKind::Separator;
            groupMenu.children.push_back(std::move(separator));
        }
        for (bool ascending : {true, false}) {
            MenuItem directionItem{};
            directionItem.id = nextId++;
            directionItem.kind = ItemKind::Command;
            directionItem.label = ascending ? L"Ascending" : L"Descending";
            directionItem.action = ActionKind::GroupDirection;
            directionItem.sortAscending = ascending;
            groupMenu.children.push_back(std::move(directionItem));
        }

        addViewAction(L"Refresh", L"refresh", ViewAction::Refresh, kModelNone,
                      L"@glyph:E72C");
        addSeparator();
        addCommand(L"Paste", L"paste", kModelNone, L"@glyph:E77F");
        addCommand(L"Paste shortcut", L"pastelink");
        addSeparator();
        MenuItem& newMenu = addSubmenu(L"New");
        BuildNewMenuChildren(newMenu.children, nextId);
        if (scope == Scope::Desktop) {
            addSeparator();
            addCommand(L"Display settings", L"display", kModelNone, L"@glyph:E7F4");
            addCommand(L"Personalize", L"personalize", kModelNone, L"@glyph:E790");
        }
        addSeparator();
        addFallback();
        return model;
    }

    if (scope == Scope::Drive) {
        addCommand(openLabel, L"open", kModelDefault);
        addCommand(L"Open in new window", L"opennew");
        addCommand(L"Pin to Quick access", L"pintohome");
        addSeparator();
        addCommand(L"Properties", L"properties", kModelNone, L"@glyph:E713");
        addSeparator();
        addFallback();
        return model;
    }

    if (scope == Scope::Folders) {
        addCommand(openLabel, L"open", kModelDefault);
        addCommand(L"Open in new window", L"opennew");
        addCommand(L"Pin to Quick access", L"pintohome");
        addSeparator();
    } else {
        addCommand(openLabel, L"open", kModelDefault);
        addCommand(L"Open with", L"openwith", kModelNone, L"@glyph:E8E5");
        addSeparator();
    }

    addCommand(L"Cut", L"cut", kModelNone, L"@glyph:E8C6");
    addCommand(L"Copy", L"copy", kModelNone, L"@glyph:E8C8");
    addViewAction(L"Rename", L"rename", ViewAction::Rename, multiDisabled,
                  L"@glyph:E8AC");
    addCommand(L"Delete", L"delete", kModelNone, L"@glyph:E74D");
    addSeparator();
    addCommand(L"Create shortcut", L"createshortcut", multiDisabled);
    addSubmenu(L"Send to");
    addCommand(L"Copy as path", L"copyaspath");
    addSeparator();
    addCommand(L"Properties", L"properties", kModelNone, L"@glyph:E713");
    addSeparator();
    addFallback();

    return model;
}

MenuModel BuildCoreFileModel(const std::vector<std::wstring>& paths, Shape shape) {
    return BuildCoreModel(Scope::Files, paths, shape);
}

// Merges cached extension items into a freshly built core model: core items
// keep their order, native invocation descriptors are adopted for matching
// items, duplicates are dropped, cached separators are skipped, and the
// fallback item stays last.
MenuModel MergeCoreWithCached(const MenuModel& core, const MenuModel& cached) {
    MenuModel result = core;

    MenuItem fallback{};
    bool hadFallback = false;
    if (!result.items.empty() && result.items.back().action == ActionKind::Fallback) {
        fallback = result.items.back();
        result.items.pop_back();
        hadFallback = true;
    }

    auto matches = [](const MenuItem& left, const MenuItem& right) {
        if (LabelsMatchIgnoreCase(left.label, right.label)) {
            return true;
        }
        return !left.canonicalVerb.empty() && left.canonicalVerb == right.canonicalVerb;
    };

    std::unordered_set<std::wstring> coreLabels;
    std::unordered_set<std::wstring> coreVerbs;
    for (const MenuItem& item : result.items) {
        if (!item.label.empty()) {
            const std::wstring normalized = NormalizeMenuLabel(item.label);
            if (!normalized.empty()) {
                coreLabels.insert(normalized);
            }
        }
        if (!item.canonicalVerb.empty()) {
            coreVerbs.insert(item.canonicalVerb);
        }
    }

    std::vector<MenuItem> added;
    for (const MenuItem& item : cached.items) {
        if (item.kind == ItemKind::Separator) {
            continue;
        }

        bool matchedCore = false;
        for (MenuItem& coreItem : result.items) {
            // Only invokable commands donate a descriptor; a matching submenu
            // has none and would otherwise clear the core verb and point the
            // item at offset 0.
            if (coreItem.action != ActionKind::ShellVerb ||
                item.action != ActionKind::ShellVerb ||
                item.kind != ItemKind::Command || !matches(coreItem, item)) {
                continue;
            }
            // Adopt the native invocation descriptor: the shell rejects some
            // canonical verb strings but its own offsets always dispatch.
            coreItem.canonicalVerb = item.canonicalVerb;
            coreItem.verbOffset = item.verbOffset;
            coreItem.flags |= kModelHasOffset;
            matchedCore = true;
            break;
        }
        if (matchedCore) {
            continue;
        }

        const std::wstring normalizedLabel = NormalizeMenuLabel(item.label);
        if ((!normalizedLabel.empty() && coreLabels.count(normalizedLabel)) ||
            (!item.canonicalVerb.empty() && coreVerbs.count(item.canonicalVerb))) {
            continue;
        }
        added.push_back(item);
    }

    if (!added.empty()) {
        result.items.insert(result.items.end(), added.begin(), added.end());
    }
    if (hadFallback) {
        if (!added.empty()) {
            MenuItem separator{};
            separator.id = 9999;
            separator.kind = ItemKind::Separator;
            result.items.push_back(std::move(separator));
        }
        result.items.push_back(std::move(fallback));
    }
    return result;
}

}  // namespace cmo

// ===========================================================================
// [CMO:Cache] In-memory and persistent menu model cache.
// ===========================================================================
namespace cmo {

constexpr uint32_t kCacheMagic = 0x434F4D4F;  // "COMO"
constexpr uint32_t kCacheVersion = 15;
constexpr uint32_t kMaxCacheEntries = 1024;
constexpr uint32_t kMaxModelItems = 4096;
constexpr uint32_t kMaxMenuDepth = 16;
constexpr uint32_t kMaxStringChars = 65536;

std::wstring CacheFilePath() {
    wchar_t storagePath[MAX_PATH] = {};
    if (!Wh_GetModStoragePath(storagePath, ARRAYSIZE(storagePath))) {
        return L"";
    }
    return std::wstring(storagePath) + L"\\menu-cache.bin";
}

namespace {

void WriteU8(std::vector<uint8_t>& out, uint8_t value) {
    out.push_back(value);
}

void WriteU32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
}

void WriteU64(std::vector<uint8_t>& out, uint64_t value) {
    WriteU32(out, static_cast<uint32_t>(value & 0xFFFFFFFFu));
    WriteU32(out, static_cast<uint32_t>(value >> 32));
}

void WriteString(std::vector<uint8_t>& out, const std::wstring& text) {
    WriteU32(out, static_cast<uint32_t>(text.size()));
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(text.data());
    out.insert(out.end(), bytes, bytes + text.size() * sizeof(wchar_t));
}

void WriteBytes(std::vector<uint8_t>& out, const std::vector<uint8_t>& bytes) {
    WriteU32(out, static_cast<uint32_t>(bytes.size()));
    out.insert(out.end(), bytes.begin(), bytes.end());
}

void WriteItem(std::vector<uint8_t>& out, const MenuItem& item) {
    WriteU32(out, item.id);
    WriteU8(out, static_cast<uint8_t>(item.kind));
    WriteU8(out, static_cast<uint8_t>(item.action));
    WriteU32(out, item.flags);
    WriteU32(out, static_cast<uint32_t>(item.viewAction));
    WriteU32(out, item.verbOffset);
    WriteString(out, item.label);
    WriteString(out, item.canonicalVerb);
    WriteString(out, item.iconRef);
    WriteString(out, item.targetPath);
    WriteBytes(out, item.iconPixels);
    WriteU32(out, static_cast<uint32_t>(item.children.size()));
    for (const MenuItem& child : item.children) {
        WriteItem(out, child);
    }
}

void WriteSignature(std::vector<uint8_t>& out, const ContextSignature& sig) {
    WriteU8(out, static_cast<uint8_t>(sig.scope));
    WriteString(out, sig.typeKey);
    WriteU8(out, static_cast<uint8_t>(sig.shape));
    WriteU8(out, static_cast<uint8_t>(sig.variant));
}

void WriteModel(std::vector<uint8_t>& out, const MenuModel& model) {
    WriteSignature(out, model.sig);
    WriteU32(out, model.flags);
    WriteU64(out, model.sourceStamp);
    WriteU32(out, static_cast<uint32_t>(model.handlerModules.size()));
    for (const std::wstring& module : model.handlerModules) {
        WriteString(out, module);
    }
    WriteU32(out, static_cast<uint32_t>(model.items.size()));
    for (const MenuItem& item : model.items) {
        WriteItem(out, item);
    }
}

uint32_t ReadU32At(std::span<const uint8_t> data, size_t offset) {
    return static_cast<uint32_t>(data[offset]) |
           (static_cast<uint32_t>(data[offset + 1]) << 8) |
           (static_cast<uint32_t>(data[offset + 2]) << 16) |
           (static_cast<uint32_t>(data[offset + 3]) << 24);
}

uint32_t Crc32(std::span<const uint8_t> data) {
    uint32_t crc = 0xFFFFFFFFu;
    for (uint8_t byte : data) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

uint64_t ComputeModuleStamp(const std::vector<std::wstring>& modules) {
    uint64_t stamp = 1469598103934665603ULL;
    for (const std::wstring& path : modules) {
        WIN32_FILE_ATTRIBUTE_DATA data = {};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
            continue;
        }
        const uint64_t size =
            (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
        const uint64_t modified =
            (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) |
            data.ftLastWriteTime.dwLowDateTime;
        stamp = HashCombine(stamp, HashString(path));
        stamp = HashCombine(stamp, size);
        stamp = HashCombine(stamp, modified);
    }
    return stamp;
}

class Reader {
public:
    explicit Reader(std::span<const uint8_t> data) : data_(data) {}

    bool ReadU8(uint8_t& value) {
        if (pos_ + 1 > data_.size()) {
            return false;
        }
        value = data_[pos_++];
        return true;
    }

    bool ReadU32(uint32_t& value) {
        if (pos_ + 4 > data_.size()) {
            return false;
        }
        value = ReadU32At(data_, pos_);
        pos_ += 4;
        return true;
    }

    bool ReadU64(uint64_t& value) {
        uint32_t low = 0;
        uint32_t high = 0;
        if (!ReadU32(low) || !ReadU32(high)) {
            return false;
        }
        value = (static_cast<uint64_t>(high) << 32) | low;
        return true;
    }

    bool ReadString(std::wstring& text) {
        uint32_t length = 0;
        if (!ReadU32(length) || length > kMaxStringChars) {
            return false;
        }
        const size_t bytes = static_cast<size_t>(length) * sizeof(wchar_t);
        if (pos_ + bytes > data_.size()) {
            return false;
        }
        // Copy into aligned storage; the serialized offset may be unaligned.
        text.resize(length);
        if (bytes > 0) {
            memcpy(text.data(), data_.data() + pos_, bytes);
        }
        pos_ += bytes;
        return true;
    }

    bool ReadBytes(std::vector<uint8_t>& bytes, uint32_t maxBytes) {
        uint32_t length = 0;
        if (!ReadU32(length) || length > maxBytes) {
            return false;
        }
        if (pos_ + length > data_.size()) {
            return false;
        }
        bytes.assign(data_.begin() + pos_, data_.begin() + pos_ + length);
        pos_ += length;
        return true;
    }

    bool ReadSignature(ContextSignature& sig) {
        uint8_t scope = 0;
        uint8_t shape = 0;
        uint8_t variant = 0;
        if (!ReadU8(scope) || !ReadString(sig.typeKey) || !ReadU8(shape) ||
            !ReadU8(variant)) {
            return false;
        }
        sig.scope = static_cast<Scope>(scope);
        sig.shape = static_cast<Shape>(shape);
        sig.variant = static_cast<Variant>(variant);
        return true;
    }

    bool ReadItem(MenuItem& item, uint32_t depth = 0) {
        if (depth > kMaxMenuDepth) {
            return false;
        }
        uint8_t kind = 0;
        uint8_t action = 0;
        uint32_t viewAction = 0;
        uint32_t childCount = 0;
        if (!ReadU32(item.id) || !ReadU8(kind) || !ReadU8(action) ||
            !ReadU32(item.flags) || !ReadU32(viewAction) ||
            !ReadU32(item.verbOffset) || !ReadString(item.label) ||
            !ReadString(item.canonicalVerb) || !ReadString(item.iconRef) ||
            !ReadString(item.targetPath) || !ReadBytes(item.iconPixels, 4096) ||
            !ReadU32(childCount)) {
            return false;
        }
        item.kind = static_cast<ItemKind>(kind);
        item.action = static_cast<ActionKind>(action);
        item.viewAction = viewAction;
        for (uint32_t i = 0; i < childCount; ++i) {
            MenuItem child{};
            if (!ReadItem(child, depth + 1)) {
                return false;
            }
            item.children.push_back(std::move(child));
        }
        return true;
    }

    bool ReadModel(MenuModel& model) {
        uint32_t modelFlags = 0;
        uint32_t moduleCount = 0;
        uint32_t itemCount = 0;
        if (!ReadSignature(model.sig) || !ReadU32(modelFlags) ||
            !ReadU64(model.sourceStamp) || !ReadU32(moduleCount) ||
            moduleCount > kMaxCacheEntries) {
            return false;
        }
        model.flags = modelFlags;
        for (uint32_t i = 0; i < moduleCount; ++i) {
            std::wstring module;
            if (!ReadString(module)) {
                return false;
            }
            model.handlerModules.push_back(std::move(module));
        }
        if (!ReadU32(itemCount) || itemCount > kMaxModelItems) {
            return false;
        }
        for (uint32_t i = 0; i < itemCount; ++i) {
            MenuItem item{};
            if (!ReadItem(item)) {
                return false;
            }
            model.items.push_back(std::move(item));
        }
        return true;
    }

private:
    std::span<const uint8_t> data_;
    size_t pos_ = 0;
};

}  // namespace

class Cache {
public:
    std::optional<MenuModel> Find(const ContextSignature& signature) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = entries_.find(signature.Hash());
        if (it == entries_.end()) {
            return std::nullopt;
        }
        it->second.lastUsed = GetTickCount64();
        return it->second.model;
    }

    bool Has(const ContextSignature& signature) {
        std::lock_guard<std::mutex> lock(mutex_);
        return entries_.find(signature.Hash()) != entries_.end();
    }

    void Put(MenuModel model) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = entries_.find(model.sig.Hash());
        if (it != entries_.end() && (model.flags & kModelWarmup) &&
            !(it->second.model.flags & kModelWarmup)) {
            // A live model is authoritative; never let warm-up overwrite it.
            return;
        }
        PutLocked(std::move(model));
    }

    void Clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.clear();
        dirty_ = true;
    }

    void SetMaxEntries(size_t maxEntries) {
        std::lock_guard<std::mutex> lock(mutex_);
        maxEntries_ = maxEntries;
        EvictIfNeededLocked();
    }

    size_t Size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return entries_.size();
    }

    std::vector<uint8_t> Serialize() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return SerializeLocked();
    }

    static bool Deserialize(std::span<const uint8_t> data, Cache& out) {
        std::vector<MenuModel> models;
        if (!DeserializeModels(data, models)) {
            return false;
        }
        out.ReplaceAll(std::move(models));
        return true;
    }

    bool Load(const std::wstring& path) {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            return false;
        }

        LARGE_INTEGER size = {};
        if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 ||
            size.QuadPart > 16 * 1024 * 1024) {
            CloseHandle(file);
            return false;
        }

        std::vector<uint8_t> data(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        BOOL ok = ReadFile(file, data.data(), static_cast<DWORD>(data.size()), &read,
                           nullptr);
        CloseHandle(file);
        if (!ok || read != data.size()) {
            return false;
        }

        std::vector<MenuModel> models;
        if (!DeserializeModels(data, models)) {
            return false;
        }
        ReplaceAll(std::move(models));
        return true;
    }

    bool Save(const std::wstring& path) {
        std::vector<uint8_t> data = Serialize();
        const std::wstring tempPath = path + L".tmp";

        HANDLE file = CreateFileW(tempPath.c_str(), GENERIC_WRITE, 0, nullptr,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            return false;
        }

        DWORD written = 0;
        BOOL ok = WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &written,
                            nullptr);
        CloseHandle(file);
        if (!ok || written != data.size()) {
            DeleteFileW(tempPath.c_str());
            return false;
        }

        if (!MoveFileExW(tempPath.c_str(), path.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            DeleteFileW(tempPath.c_str());
            return false;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        dirty_ = false;
        lastSaveTick_ = GetTickCount64();
        return true;
    }

    bool MaybeSave(const std::wstring& path) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!dirty_ || GetTickCount64() - lastSaveTick_ < 5000) {
                return true;
            }
        }
        return Save(path);
    }

    // Clears the cache when any recorded handler module changed on disk.
    bool RevalidateStamps() {
        struct Check {
            uint64_t stamp;
            std::vector<std::wstring> modules;
        };
        std::vector<Check> checks;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& pair : entries_) {
                if (pair.second.model.handlerModules.empty()) {
                    continue;
                }
                checks.push_back(
                    {pair.second.model.sourceStamp, pair.second.model.handlerModules});
            }
        }

        bool stale = false;
        for (const Check& check : checks) {
            if (ComputeModuleStamp(check.modules) != check.stamp) {
                stale = true;
                break;
            }
        }
        if (stale) {
            std::lock_guard<std::mutex> lock(mutex_);
            entries_.clear();
            dirty_ = true;
        }
        return stale;
    }

private:
    struct Entry {
        MenuModel model;
        uint64_t lastUsed = 0;
    };

    void PutLocked(MenuModel model) {
        Entry entry{};
        entry.model = std::move(model);
        entry.lastUsed = GetTickCount64();
        entries_[entry.model.sig.Hash()] = std::move(entry);
        EvictIfNeededLocked();
        dirty_ = true;
    }

    void ReplaceAll(std::vector<MenuModel> models) {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.clear();
        for (MenuModel& model : models) {
            Entry entry{};
            entry.model = std::move(model);
            entry.lastUsed = GetTickCount64();
            entries_[entry.model.sig.Hash()] = std::move(entry);
        }
        EvictIfNeededLocked();
        dirty_ = false;
    }

    void EvictIfNeededLocked() {
        while (entries_.size() > maxEntries_ && !entries_.empty()) {
            auto oldest = entries_.begin();
            for (auto it = entries_.begin(); it != entries_.end(); ++it) {
                if (it->second.lastUsed < oldest->second.lastUsed) {
                    oldest = it;
                }
            }
            entries_.erase(oldest);
        }
    }

    std::vector<uint8_t> SerializeLocked() const {
        std::vector<uint8_t> out;
        WriteU32(out, kCacheMagic);
        WriteU32(out, kCacheVersion);
        WriteU32(out, static_cast<uint32_t>(entries_.size()));
        for (const auto& pair : entries_) {
            WriteModel(out, pair.second.model);
        }
        WriteU32(out, Crc32(out));
        return out;
    }

    static bool DeserializeModels(std::span<const uint8_t> data,
                                  std::vector<MenuModel>& out) {
        if (data.size() < 12) {
            return false;
        }
        if (ReadU32At(data, 0) != kCacheMagic) {
            return false;
        }
        if (ReadU32At(data, 4) != kCacheVersion) {
            return false;
        }

        const size_t payloadSize = data.size() - 4;
        if (ReadU32At(data, payloadSize) != Crc32(data.subspan(0, payloadSize))) {
            return false;
        }

        Reader reader(data.subspan(0, payloadSize));
        uint32_t magic = 0;
        uint32_t version = 0;
        uint32_t entryCount = 0;
        if (!reader.ReadU32(magic) || !reader.ReadU32(version) ||
            !reader.ReadU32(entryCount) || entryCount > kMaxCacheEntries) {
            return false;
        }

        std::vector<MenuModel> models;
        for (uint32_t i = 0; i < entryCount; ++i) {
            MenuModel model{};
            if (!reader.ReadModel(model)) {
                return false;
            }
            models.push_back(std::move(model));
        }

        out = std::move(models);
        return true;
    }

    mutable std::mutex mutex_;
    std::unordered_map<uint64_t, Entry> entries_;
    size_t maxEntries_ = 256;
    bool dirty_ = false;
    uint64_t lastSaveTick_ = 0;
};

inline Cache g_cache;

}  // namespace cmo

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
    return kind == ShellViewKind::Desktop || kind == ShellViewKind::ShellDefView ||
           kind == ShellViewKind::NavPane;
}

// Maps the popup owner to a scope. A replaceable owner with an empty
// selection is a background menu; Desktop background and desktop icons share
// the same owner kind.
inline Scope ScopeFromKind(ShellViewKind kind, bool background) {
    switch (kind) {
        case ShellViewKind::Desktop:
            return background ? Scope::Desktop : Scope::Files;
        case ShellViewKind::ShellDefView:
            return background ? Scope::Background : Scope::Files;
        case ShellViewKind::NavPane:
            return Scope::NavPane;
        default:
            return Scope::Other;
    }
}

inline bool AllPathsAreDrives(const std::vector<std::wstring>& paths) {
    if (paths.empty()) {
        return false;
    }
    for (const std::wstring& path : paths) {
        if (path.size() != 3 || path[1] != L':' ||
            (path[2] != L'\\' && path[2] != L'/')) {
            return false;
        }
    }
    return true;
}

inline bool IsFilesystemContext(bool folderIsFilesystem, bool allItemsAreFilesystem) {
    return folderIsFilesystem && allItemsAreFilesystem;
}

// Refines a files-scope selection using shell attributes: drive roots first,
// then folders, otherwise files.
inline Scope RefineScope(Scope scope, bool allFolders, bool allDrives) {
    if (scope != Scope::Files) {
        return scope;
    }
    if (allDrives) {
        return Scope::Drive;
    }
    if (allFolders) {
        return Scope::Folders;
    }
    return Scope::Files;
}

// True when two path sets contain the same items in any order.
inline bool PathSetsEqual(std::vector<std::wstring> left,
                          std::vector<std::wstring> right) {
    if (left.size() != right.size()) {
        return false;
    }
    std::sort(left.begin(), left.end());
    std::sort(right.begin(), right.end());
    return left == right;
}

inline bool ShouldShowNativeReplay(uint32_t modelFlags) {
    return (modelFlags & kModelOwnerDraw) != 0;
}

// True when the user's apps use the dark theme.
inline bool IsDarkThemeActive() {
    DWORD lightTheme = 1;
    DWORD size = sizeof(lightTheme);
    HKEY key = nullptr;
    if (RegOpenKeyExW(
            HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0,
            KEY_READ, &key) == ERROR_SUCCESS) {
        RegQueryValueExW(key, L"AppsUseLightTheme", nullptr, nullptr,
                         reinterpret_cast<LPBYTE>(&lightTheme), &size);
        RegCloseKey(key);
    }
    return lightTheme == 0;
}

// The actual menu background color for the owner window's theme. The theme
// lookup can return light colors even in dark mode, so the result is sanity
// checked against the registry theme setting.
inline COLORREF MenuBackgroundColor(HWND owner) {
    COLORREF color = 0;
    bool haveColor = false;
    HTHEME theme = OpenThemeData(owner, L"Menu");
    if (theme) {
        if (SUCCEEDED(GetThemeColor(theme, MENU_POPUPBACKGROUND, 0, TMT_FILLCOLOR,
                                    &color))) {
            haveColor = true;
        }
        CloseThemeData(theme);
    }
    if (!haveColor) {
        color = GetSysColor(COLOR_MENU);
    }

    const bool darkTheme = IsDarkThemeActive();
    const int luminance =
        (GetRValue(color) * 30 + GetGValue(color) * 59 + GetBValue(color) * 11) / 100;
    if (darkTheme && luminance > 128) {
        return RGB(44, 44, 44);
    }
    if (!darkTheme && luminance < 128) {
        return GetSysColor(COLOR_MENU);
    }
    return color;
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

    // Populated once per open; reused for discovery and the native fallback.
    HMENU populatedMenu = nullptr;
    bool populated = false;
    IContextMenu3* contextMenu3 = nullptr;
    IContextMenu2* contextMenu2 = nullptr;
    std::vector<std::wstring> handlerModules;
    uint64_t sourceStamp = 0;
    bool discoveryDone = false;
    bool reopenRequested = false;
    bool menuInitialized = false;
};

// Releases every resource a capture owns.
void ReleaseCapture(PendingCapture& capture) {
    if (capture.populatedMenu) {
        DestroyMenu(capture.populatedMenu);
        capture.populatedMenu = nullptr;
    }
    if (capture.contextMenu3) {
        capture.contextMenu3->Release();
        capture.contextMenu3 = nullptr;
    }
    if (capture.contextMenu2) {
        capture.contextMenu2->Release();
        capture.contextMenu2 = nullptr;
    }
    if (capture.obj) {
        capture.obj->Release();
        capture.obj = nullptr;
    }
    capture.populated = false;
}

class PendingQueue {
public:
    void Push(PendingCapture capture) {
        Clear();
        capture_ = capture;
        valid_ = true;
    }

    // Moves the pending capture out and transfers ownership of its COM
    // reference to the caller. Returns false when nothing is pending.
    bool Take(PendingCapture& out) {
        if (!valid_) {
            return false;
        }
        out = capture_;
        capture_ = {};
        valid_ = false;
        return true;
    }

    void Clear() {
        if (valid_) {
            ReleaseCapture(capture_);
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

// Undocumented shell32 bit set for every menu that is actually shown as a
// popup. Desktop menus (icons and background) pass the same flags as Explorer
// views but omit CMF_EXPLORE; they still carry this bit, while verb-state
// queries and submenu builds do not.
constexpr UINT kCmfPopupMenu = 0x00020000;

// Only main popup menus are deferred. CMF_DEFAULTONLY is the default-verb
// resolution used by double-click/open; CMF_NOVERBS builds submenus such as
// Send to; CMF_VERBSONLY builds verb-only menus. Those never show a popup we
// could replace and must reach the shell untouched.
bool ShouldDeferContextMenu(UINT flags) {
    if (flags & (CMF_DEFAULTONLY | CMF_NOVERBS | CMF_VERBSONLY)) {
        return false;
    }
    return (flags & (CMF_EXPLORE | kCmfPopupMenu)) != 0;
}

using QueryContextMenu_t =
    HRESULT(STDMETHODCALLTYPE*)(IContextMenu*, HMENU, UINT, UINT, UINT, UINT);
inline QueryContextMenu_t QueryContextMenu_Original = nullptr;

HRESULT STDMETHODCALLTYPE QueryContextMenu_Hook(IContextMenu* pThis, HMENU hmenu,
                                                UINT indexMenu, UINT idCmdFirst,
                                                UINT idCmdLast, UINT uFlags) {
    if (!ShouldDeferContextMenu(uFlags)) {
        Wh_Log(L"QueryContextMenu pass-through: flags=%08X", uFlags);
        return QueryContextMenu_Original(pThis, hmenu, indexMenu, idCmdFirst, idCmdLast,
                                         uFlags);
    }

    // A capture that never reached TrackPopupMenu* is stale; release it
    // before capturing the new one.
    PendingCapture previous{};
    if (g_pending.Take(previous)) {
        ReleaseCapture(previous);
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
    PendingCapture capture{};
    if (!g_pending.Take(capture)) {
        return false;
    }
    ReplayInto(capture.obj, hMenu, capture.indexMenu, capture.idCmdFirst,
               capture.idCmdLast, capture.flags);
    ReleaseCapture(capture);
    Wh_Log(L"Replayed native population for owner=%p", owner);
    return true;
}

uint32_t MapMenuState(UINT state) {
    uint32_t flags = kModelNone;
    if (state & MFS_DISABLED) {
        flags |= kModelDisabled;
    }
    if (state & MFS_CHECKED) {
        flags |= kModelChecked;
    }
    if (state & MFS_DEFAULT) {
        flags |= kModelDefault;
    }
    return flags;
}

// Bitmaps attached with SetMenuItemBitmaps cannot be read back from the
// menu, so record them while the shell populates it. Thread-local because
// population happens on the thread that owns the menu.
thread_local std::unordered_map<uintptr_t, std::unordered_map<UINT, HBITMAP>>
    g_recordedItemBitmaps;

void ClearRecordedItemBitmaps() {
    g_recordedItemBitmaps.clear();
}

HBITMAP LookupRecordedItemBitmap(HMENU menu, UINT id) {
    auto menuIt = g_recordedItemBitmaps.find(reinterpret_cast<uintptr_t>(menu));
    if (menuIt == g_recordedItemBitmaps.end()) {
        return nullptr;
    }
    auto itemIt = menuIt->second.find(id);
    return itemIt == menuIt->second.end() ? nullptr : itemIt->second;
}

using SetMenuItemBitmaps_t = decltype(&SetMenuItemBitmaps);
inline SetMenuItemBitmaps_t SetMenuItemBitmaps_Original = nullptr;

BOOL WINAPI SetMenuItemBitmaps_Hook(HMENU hMenu, UINT uPosition, UINT uFlags,
                                    HBITMAP hBitmapUnchecked, HBITMAP hBitmapChecked) {
    if (hMenu && (hBitmapUnchecked || hBitmapChecked)) {
        UINT id = uPosition;
        if ((uFlags & MF_BYPOSITION) != 0) {
            id = GetMenuItemID(hMenu, uPosition);
        }
        if (id != static_cast<UINT>(-1)) {
            // Bound the recording; menus are normally far smaller than this.
            if (g_recordedItemBitmaps.size() > 64) {
                g_recordedItemBitmaps.clear();
            }
            g_recordedItemBitmaps[reinterpret_cast<uintptr_t>(hMenu)][id] =
                hBitmapUnchecked ? hBitmapUnchecked : hBitmapChecked;
        }
    }
    return SetMenuItemBitmaps_Original
               ? SetMenuItemBitmaps_Original(hMenu, uPosition, uFlags,
                                             hBitmapUnchecked, hBitmapChecked)
               : FALSE;
}

// HBMMENU_* sentinels are -1 or 1..13; real GDI bitmap handles are arbitrary
// 32-bit values (sign-extended on 64-bit Windows) and must be captured.
bool IsSentinelMenuBitmap(HBITMAP bitmap) {
    const INT_PTR value = reinterpret_cast<INT_PTR>(bitmap);
    return value == -1 || (value > 0 && value <= 13);
}

// Copies a shell menu bitmap into BGRA pixels (any bit depth, top-down).
void CaptureBitmapPixels(HBITMAP bitmap, std::vector<uint8_t>& out) {
    out.clear();
    BITMAP bitmapInfo = {};
    if (!GetObjectW(bitmap, sizeof(bitmapInfo), &bitmapInfo) ||
        bitmapInfo.bmWidth <= 0 || bitmapInfo.bmHeight <= 0 ||
        bitmapInfo.bmWidth > 64 || bitmapInfo.bmHeight > 64) {
        return;
    }

    const int width = bitmapInfo.bmWidth;
    const int height = bitmapInfo.bmHeight;

    BITMAPINFO dibInfo = {};
    dibInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    dibInfo.bmiHeader.biWidth = width;
    dibInfo.bmiHeader.biHeight = height;  // bottom-up
    dibInfo.bmiHeader.biPlanes = 1;
    dibInfo.bmiHeader.biBitCount = 32;
    dibInfo.bmiHeader.biCompression = BI_RGB;

    std::vector<uint8_t> buffer(static_cast<size_t>(width) * height * 4);
    HDC screen = GetDC(nullptr);
    const int lines =
        GetDIBits(screen, bitmap, 0, height, buffer.data(), &dibInfo, DIB_RGB_COLORS);
    ReleaseDC(nullptr, screen);
    if (lines != height) {
        Wh_Log(L"Bitmap capture failed: %dx%d %dbpp", width, height,
               bitmapInfo.bmBitsPixel);
        return;
    }

    // Flip to top-down for the rest of the pipeline.
    out.resize(buffer.size());
    for (int y = 0; y < height; ++y) {
        memcpy(out.data() + static_cast<size_t>(y) * width * 4,
               buffer.data() + static_cast<size_t>(height - 1 - y) * width * 4,
               static_cast<size_t>(width) * 4);
    }
}

// Reads a string value from a Classes subkey; HKCU takes precedence over
// HKLM, mirroring the HKEY_CLASSES_ROOT merge order.
std::wstring ReadClassesString(const std::wstring& subKey, const wchar_t* valueName) {
    const std::wstring fullKey = L"Software\\Classes\\" + subKey;
    for (HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
        HKEY key = nullptr;
        if (RegOpenKeyExW(root, fullKey.c_str(), 0, KEY_READ, &key) !=
            ERROR_SUCCESS) {
            continue;
        }

        wchar_t buffer[512] = {};
        DWORD size = sizeof(buffer);
        DWORD type = 0;
        std::wstring result;
        if (RegQueryValueExW(key, valueName, nullptr, &type,
                             reinterpret_cast<LPBYTE>(buffer),
                             &size) == ERROR_SUCCESS &&
            (type == REG_SZ || type == REG_EXPAND_SZ) && buffer[0]) {
            result.assign(buffer);
        }
        RegCloseKey(key);
        if (!result.empty()) {
            return result;
        }
    }
    return L"";
}

// Candidate shell keys for a context, most specific first.
std::vector<std::wstring> ShellIconBases(const ContextSignature& signature) {
    std::vector<std::wstring> bases;
    if (signature.scope == Scope::Folders || signature.scope == Scope::Drive) {
        bases.push_back(L"Directory");
        bases.push_back(L"Folder");
        bases.push_back(L"AllFilesystemObjects");
        bases.push_back(L"*");
    } else if (signature.scope == Scope::Background ||
               signature.scope == Scope::Desktop) {
        bases.push_back(L"Directory\\Background");
        bases.push_back(L"DesktopBackground");
    } else {
        const std::wstring& typeKey = signature.typeKey;
        if (!typeKey.empty() && typeKey != L"*" && typeKey != L"mixed" &&
            typeKey[0] == L'.') {
            const std::wstring progId = ReadClassesString(typeKey, nullptr);
            if (!progId.empty()) {
                bases.push_back(progId);
            }
            bases.push_back(typeKey);
            bases.push_back(L"SystemFileAssociations\\" + typeKey);
        }
        bases.push_back(L"*");
        bases.push_back(L"AllFilesystemObjects");
    }
    return bases;
}

// Resolves the icon a static verb registers under HKCR for this context.
std::wstring ResolveRegistryIcon(const ContextSignature& signature,
                                 const std::wstring& verb) {
    if (verb.empty()) {
        return L"";
    }

    for (const std::wstring& base : ShellIconBases(signature)) {
        const std::wstring icon =
            ReadClassesString(base + L"\\shell\\" + verb, L"Icon");
        if (!icon.empty()) {
            return icon;
        }
    }
    return L"";
}

// Resolves strings like "@C:\path\file.dll,-123" to their display text.
std::wstring ResolveIndirectString(const std::wstring& value) {
    if (value.empty() || value[0] != L'@') {
        return value;
    }
    wchar_t buffer[512] = {};
    if (SUCCEEDED(SHLoadIndirectString(value.c_str(), buffer, ARRAYSIZE(buffer),
                                       nullptr))) {
        return buffer;
    }
    return value;
}

// Finds a shell verb key whose display text matches the item label, for
// handlers whose key name differs from the canonical verb.
std::wstring ResolveRegistryIconByLabel(const ContextSignature& signature,
                                        const std::wstring& label) {
    std::wstring normalized = label;
    normalized.erase(std::remove(normalized.begin(), normalized.end(), L'&'),
                     normalized.end());
    if (normalized.empty()) {
        return L"";
    }

    for (const std::wstring& base : ShellIconBases(signature)) {
        const std::wstring shellKey = base + L"\\shell";
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER,
                          (L"Software\\Classes\\" + shellKey).c_str(), 0, KEY_READ,
                          &key) != ERROR_SUCCESS &&
            RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                          (L"Software\\Classes\\" + shellKey).c_str(), 0, KEY_READ,
                          &key) != ERROR_SUCCESS) {
            continue;
        }

        for (DWORD i = 0;; ++i) {
            wchar_t name[256] = {};
            DWORD nameLength = ARRAYSIZE(name);
            if (RegEnumKeyExW(key, i, name, &nameLength, nullptr, nullptr, nullptr,
                              nullptr) != ERROR_SUCCESS) {
                break;
            }

            const std::wstring verbKey = shellKey + L"\\" + name;
            std::wstring display = ReadClassesString(verbKey, L"MUIVerb");
            if (display.empty()) {
                display = ReadClassesString(verbKey, nullptr);
            }
            display = ResolveIndirectString(display);
            display.erase(std::remove(display.begin(), display.end(), L'&'),
                          display.end());
            if (!display.empty() &&
                _wcsicmp(display.c_str(), normalized.c_str()) == 0) {
                const std::wstring icon = ReadClassesString(verbKey, L"Icon");
                if (!icon.empty()) {
                    RegCloseKey(key);
                    return icon;
                }
            }
        }
        RegCloseKey(key);
    }
    return L"";
}

// Generic vendor/platform words carry no signal when matching version info
// against menu labels; without this, labels containing "Windows" or
// "Microsoft" would match almost every handler.
bool IsGenericVersionWord(const std::wstring& word) {
    static const wchar_t* kStopWords[] = {
        L"Microsoft", L"Windows",     L"Corporation", L"Corp",
        L"Inc",       L"Ltd",         L"LLC",         L"Software",
        L"System",    L"Operating",   L"Company",     L"Product",
        L"Version",   L"Common",      L"File",        L"Files",
        L"Shell",     L"Application", L"Program",     L"Service",
        L"Services",  L"Technologies", L"Technology", L"International",
        L"Limited",   L"Group",       L"Global",
    };
    for (const wchar_t* stop : kStopWords) {
        if (_wcsicmp(word.c_str(), stop) == 0) {
            return true;
        }
    }
    return false;
}

// True when any significant word (5+ characters) of `text` appears in the
// label. Used to match handler DLL version info against menu item labels.
bool LabelMatchesWords(const std::wstring& label, const std::wstring& text) {
    std::wstring word;
    for (size_t i = 0; i <= text.size(); ++i) {
        const wchar_t c = (i < text.size()) ? text[i] : L' ';
        if (iswalnum(c)) {
            word += c;
            continue;
        }
        if (word.size() >= 5 && !IsGenericVersionWord(word) &&
            StrStrIW(label.c_str(), word.c_str()) != nullptr) {
            return true;
        }
        word.clear();
    }
    return false;
}

// Matches a handler DLL by its version-info company/product/description.
bool VersionInfoMatchesLabel(const std::wstring& path, const std::wstring& label) {
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (size == 0) {
        return false;
    }

    std::vector<uint8_t> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) {
        return false;
    }

    struct Translation {
        WORD language;
        WORD codePage;
    };
    Translation fallback = {0x0409, 0x04B0};
    Translation* translations = nullptr;
    UINT translationBytes = 0;
    if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation",
                        reinterpret_cast<LPVOID*>(&translations),
                        &translationBytes) ||
        translationBytes < sizeof(Translation)) {
        translations = &fallback;
        translationBytes = sizeof(fallback);
    }

    static const wchar_t* kFields[] = {L"CompanyName", L"ProductName",
                                       L"FileDescription"};
    const size_t translationCount = translationBytes / sizeof(Translation);
    for (size_t t = 0; t < translationCount; ++t) {
        for (const wchar_t* field : kFields) {
            wchar_t query[128] = {};
            swprintf(query, ARRAYSIZE(query),
                     L"\\StringFileInfo\\%04x%04x\\%s", translations[t].language,
                     translations[t].codePage, field);
            wchar_t* value = nullptr;
            UINT valueChars = 0;
            if (VerQueryValueW(data.data(), query, reinterpret_cast<LPVOID*>(&value),
                               &valueChars) &&
                value && valueChars > 0 && LabelMatchesWords(label, value)) {
                return true;
            }
        }
    }
    return false;
}

// Last resort: find a ContextMenuHandlers key that matches the item label
// (by key name, handler DLL name, or DLL version info) and extract icon 0
// from its DLL.
// True when the label matches a registered ContextMenuHandlers entry, by key
// name, handler DLL name, or DLL version info. Fills `dllOut` with the
// handler DLL path when matched. Used for icons and for classifying
// third-party items.
bool LabelMatchesRegisteredHandler(const ContextSignature& signature,
                                   const std::wstring& label,
                                   std::wstring* dllOut) {
    if (label.empty()) {
        return false;
    }

    for (const std::wstring& base : ShellIconBases(signature)) {
        const std::wstring handlersKey = base + L"\\shellex\\ContextMenuHandlers";
        for (HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
            HKEY key = nullptr;
            if (RegOpenKeyExW(root, (L"Software\\Classes\\" + handlersKey).c_str(), 0,
                              KEY_READ, &key) != ERROR_SUCCESS) {
                continue;
            }

            for (DWORD i = 0;; ++i) {
                wchar_t name[256] = {};
                DWORD nameLength = ARRAYSIZE(name);
                if (RegEnumKeyExW(key, i, name, &nameLength, nullptr, nullptr, nullptr,
                                  nullptr) != ERROR_SUCCESS) {
                    break;
                }

                const std::wstring clsid =
                    ReadClassesString(handlersKey + L"\\" + name, nullptr);
                if (clsid.empty()) {
                    continue;
                }
                std::wstring dll = ReadClassesString(
                    L"CLSID\\" + clsid + L"\\InprocServer32", nullptr);
                if (dll.empty()) {
                    continue;
                }
                if (dll.size() >= 2 && dll.front() == L'"' && dll.back() == L'"') {
                    dll = dll.substr(1, dll.size() - 2);
                }

                wchar_t expanded[MAX_PATH] = {};
                if (!ExpandEnvironmentStringsW(dll.c_str(), expanded,
                                              ARRAYSIZE(expanded)) ||
                    GetFileAttributesW(expanded) == INVALID_FILE_ATTRIBUTES) {
                    continue;
                }

                bool matches =
                    nameLength >= 5 && StrStrIW(label.c_str(), name) != nullptr;
                if (!matches) {
                    // Some handlers are registered under a generic key name;
                    // match the DLL file name instead.
                    std::wstring stem = PathFindFileNameW(expanded);
                    const size_t dot = stem.find_last_of(L'.');
                    if (dot != std::wstring::npos) {
                        stem.resize(dot);
                    }
                    matches = stem.size() >= 5 &&
                              StrStrIW(label.c_str(), stem.c_str()) != nullptr;
                }
                if (!matches) {
                    // Or match the DLL's version info (company/product name).
                    matches = VersionInfoMatchesLabel(expanded, label);
                }
                if (!matches) {
                    continue;
                }

                if (dllOut) {
                    *dllOut = expanded;
                }
                RegCloseKey(key);
                return true;
            }
            RegCloseKey(key);
        }
    }
    return false;
}

std::wstring ResolveHandlerDllIconByLabel(const ContextSignature& signature,
                                          const std::wstring& label) {
    std::wstring dll;
    if (LabelMatchesRegisteredHandler(signature, label, &dll)) {
        return dll + L",0";
    }
    return L"";
}

// Logs the registered context menu handlers so an unresolved item can be
// traced to its registration.
void LogHandlerCandidates(const ContextSignature& signature,
                          const std::wstring& label) {
    for (const std::wstring& base : ShellIconBases(signature)) {
        const std::wstring handlersKey = base + L"\\shellex\\ContextMenuHandlers";
        for (HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
            HKEY key = nullptr;
            if (RegOpenKeyExW(root, (L"Software\\Classes\\" + handlersKey).c_str(), 0,
                              KEY_READ, &key) != ERROR_SUCCESS) {
                continue;
            }

            std::wstring names;
            for (DWORD i = 0; i < 12; ++i) {
                wchar_t name[256] = {};
                DWORD nameLength = ARRAYSIZE(name);
                if (RegEnumKeyExW(key, i, name, &nameLength, nullptr, nullptr, nullptr,
                                  nullptr) != ERROR_SUCCESS) {
                    break;
                }
                if (!names.empty()) {
                    names += L", ";
                }
                names += name;
            }
            RegCloseKey(key);

            if (!names.empty()) {
                Wh_Log(L"Handler candidates for '%s' (%s): %s", label.c_str(),
                       base.c_str(), names.c_str());
            }
        }
    }
}

// Fills in registry icons for static verbs that provide no menu bitmap and
// marks items that belong to registered shell extensions (used by the
// advanced submenu).
void ApplyRegistryIcons(std::vector<MenuItem>& items,
                        const ContextSignature& signature) {
    for (MenuItem& item : items) {
        ApplyRegistryIcons(item.children, signature);
        if (item.kind == ItemKind::Separator) {
            continue;
        }

        std::wstring handlerDll;
        const bool registered =
            LabelMatchesRegisteredHandler(signature, item.label, &handlerDll);
        if (registered) {
            item.flags |= kModelThirdParty;
        }

        if (item.action != ActionKind::ShellVerb || !item.iconPixels.empty() ||
            !item.iconRef.empty()) {
            continue;
        }

        std::wstring icon = ResolveRegistryIcon(signature, item.canonicalVerb);
        if (icon.empty()) {
            icon = ResolveRegistryIconByLabel(signature, item.label);
        }
        if (icon.empty() && registered) {
            icon = handlerDll + L",0";
        }
        if (!icon.empty()) {
            item.iconRef = icon;
            Wh_Log(L"Registry icon for '%s' (verb '%s'): %s", item.label.c_str(),
                   item.canonicalVerb.c_str(), icon.c_str());
        } else if (g_settings.debugLogging) {
            LogHandlerCandidates(signature, item.label);
            Wh_Log(L"No registry icon for '%s' (verb '%s')", item.label.c_str(),
                   item.canonicalVerb.c_str());
        }
    }
}

void BuildItemsFromHMenu(HMENU menu, UINT idCmdFirst, IContextMenu* context,
                         uint32_t& nextId, std::vector<MenuItem>& out, bool& ownerDraw) {
    const int count = GetMenuItemCount(menu);
    for (int index = 0; index < count; ++index) {
        MENUITEMINFOW info = {};
        info.cbSize = sizeof(info);
        info.fMask = MIIM_ID | MIIM_STATE | MIIM_FTYPE | MIIM_SUBMENU | MIIM_BITMAP;
        if (!GetMenuItemInfoW(menu, static_cast<UINT>(index), TRUE, &info)) {
            continue;
        }

        MenuItem item{};
        item.id = nextId++;
        item.flags = MapMenuState(info.fState) | kModelExtension;

        if (info.fType & MFT_SEPARATOR) {
            item.kind = ItemKind::Separator;
            out.push_back(std::move(item));
            continue;
        }

        wchar_t labelBuffer[512] = {};
        const int labelLength = GetMenuStringW(menu, static_cast<UINT>(index), labelBuffer,
                                               ARRAYSIZE(labelBuffer), MF_BYPOSITION);
        if (labelLength > 0) {
            item.label.assign(labelBuffer, static_cast<size_t>(labelLength));
        }

        if (info.hbmpItem && IsSentinelMenuBitmap(info.hbmpItem)) {
            Wh_Log(L"Sentinel menu bitmap %lld for '%s'",
                   static_cast<long long>(reinterpret_cast<INT_PTR>(info.hbmpItem)),
                   item.label.c_str());
        }

        if (info.fType & MFT_OWNERDRAW) {
            item.flags |= kModelOwnerDraw;
            ownerDraw = true;
        }

        // Capture icons the shell itself provides, for commands and submenu
        // parents alike.
        if (info.hbmpItem && !IsSentinelMenuBitmap(info.hbmpItem)) {
            CaptureBitmapPixels(info.hbmpItem, item.iconPixels);
        }
        if (item.iconPixels.empty()) {
            if (HBITMAP recorded = LookupRecordedItemBitmap(menu, info.wID)) {
                CaptureBitmapPixels(recorded, item.iconPixels);
            }
        }

        if (info.hSubMenu) {
            item.kind = ItemKind::Submenu;
            item.action = ActionKind::Submenu;
            BuildItemsFromHMenu(info.hSubMenu, idCmdFirst, context, nextId, item.children,
                                ownerDraw);
        } else {
            item.kind = ItemKind::Command;
            item.action = ActionKind::ShellVerb;
            if (info.wID >= idCmdFirst) {
                item.verbOffset = info.wID - idCmdFirst;
                item.flags |= kModelHasOffset;
            }
            if (context) {
                wchar_t verbBuffer[256] = {};
                if (SUCCEEDED(context->GetCommandString(
                        static_cast<UINT_PTR>(item.verbOffset), GCS_VERBW, nullptr,
                        reinterpret_cast<LPSTR>(verbBuffer), ARRAYSIZE(verbBuffer)))) {
                    item.canonicalVerb = verbBuffer;
                }
            }
        }
        out.push_back(std::move(item));
    }
}

MenuModel BuildModelFromHMenu(HMENU menu, UINT idCmdFirst,
                              const ContextSignature& signature, IContextMenu* context) {
    MenuModel model{};
    model.sig = signature;
    uint32_t nextId = 10000;
    bool ownerDraw = false;
    BuildItemsFromHMenu(menu, idCmdFirst, context, nextId, model.items, ownerDraw);
    if (ownerDraw) {
        model.flags |= kModelOwnerDraw;
    }
    return model;
}

// Walks the retained populated menu into the cache. Always called after the
// interactive menu has closed, on the same UI thread. Persistence happens
// later on the invalidation thread.
std::vector<std::wstring> SnapshotLoadedModules();
std::vector<std::wstring> DiffModules(const std::vector<std::wstring>& before,
                                      const std::vector<std::wstring>& after);
bool EnsureContextPopulated(PendingCapture& capture);

// Mimics a menu host: asks the context object to initialize each menu and
// submenu before its items are read. Extensions populate dynamic labels and
// submenu children on WM_INITMENUPOPUP; without this, discovery captures
// empty labels and empty submenus.
void InitializeMenuRecursive(PendingCapture& capture, HMENU menu, UINT position) {
    if (capture.contextMenu3) {
        LRESULT result = 0;
        capture.contextMenu3->HandleMenuMsg2(
            WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(menu),
            MAKELPARAM(position, 0), &result);
    } else if (capture.contextMenu2) {
        capture.contextMenu2->HandleMenuMsg(
            WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(menu), MAKELPARAM(position, 0));
    }

    const int count = GetMenuItemCount(menu);
    for (int i = 0; i < count; ++i) {
        MENUITEMINFOW info = {};
        info.cbSize = sizeof(info);
        info.fMask = MIIM_SUBMENU;
        if (GetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &info) &&
            info.hSubMenu) {
            InitializeMenuRecursive(capture, info.hSubMenu, static_cast<UINT>(i));
        }
    }
}

// Debug dump of a native menu: labels, ids, offsets and bitmap sentinels.
// Run with Debug logging after the menu closes, when dynamic submenus have
// been populated.
void DumpMenuTree(HMENU menu, UINT idCmdFirst, int depth) {
    if (!g_settings.debugLogging || !menu) {
        return;
    }
    const int count = GetMenuItemCount(menu);
    for (int i = 0; i < count; ++i) {
        MENUITEMINFOW info = {};
        info.cbSize = sizeof(info);
        info.fMask = MIIM_ID | MIIM_FTYPE | MIIM_SUBMENU | MIIM_STATE | MIIM_BITMAP;
        if (!GetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &info)) {
            continue;
        }
        wchar_t label[256] = {};
        GetMenuStringW(menu, static_cast<UINT>(i), label, ARRAYSIZE(label),
                       MF_BYPOSITION);
        const long long bitmap = static_cast<long long>(reinterpret_cast<INT_PTR>(
            info.hbmpItem));
        if (info.hSubMenu) {
            Wh_Log(L"[menu d%d] submenu '%s' id=%u state=%04X bmp=%lld children=%d",
                   depth, label, info.wID, info.fState, bitmap,
                   GetMenuItemCount(info.hSubMenu));
            DumpMenuTree(info.hSubMenu, idCmdFirst, depth + 1);
        } else if (!(info.fType & MFT_SEPARATOR)) {
            const long offset = info.wID >= idCmdFirst
                                    ? static_cast<long>(info.wID - idCmdFirst)
                                    : -1;
            Wh_Log(L"[menu d%d] item '%s' id=%u offset=%ld state=%04X bmp=%lld",
                   depth, label, info.wID, offset, info.fState, bitmap);
        }
    }
}

// Debug dump of a discovered model; helps identify unlabeled or unusual items.
void DumpModelItems(const std::vector<MenuItem>& items, int depth) {    for (const MenuItem& item : items) {
        Wh_Log(L"[d%d] kind=%d action=%d flags=%04X offset=%u verb='%s' label='%s' "
               L"children=%zu icons=%zu",
               depth, static_cast<int>(item.kind), static_cast<int>(item.action),
               item.flags, item.verbOffset, item.canonicalVerb.c_str(),
               item.label.c_str(), item.children.size(), item.iconPixels.size());
        DumpModelItems(item.children, depth + 1);
    }
}

// Copies icons captured before host initialization onto the post-init model,
// matching by canonical verb first and label second.
void MergePreInitIcons(std::vector<MenuItem>& post, const std::vector<MenuItem>& pre) {
    for (MenuItem& item : post) {
        const MenuItem* match = nullptr;
        for (const MenuItem& candidate : pre) {
            if (!item.canonicalVerb.empty() && !candidate.canonicalVerb.empty() &&
                candidate.canonicalVerb == item.canonicalVerb) {
                match = &candidate;
                break;
            }
        }
        if (!match) {
            for (const MenuItem& candidate : pre) {
                if (!item.label.empty() && !candidate.label.empty() &&
                    candidate.label == item.label) {
                    match = &candidate;
                    break;
                }
            }
        }

        if (item.iconPixels.empty() && item.action == ActionKind::ShellVerb && match &&
            !match->iconPixels.empty()) {
            item.iconPixels = match->iconPixels;
        }
        if (match) {
            MergePreInitIcons(item.children, match->children);
        }
    }
}

// Asks the extension to draw an item into an offscreen bitmap (the owner-draw
// path) and crops the icon gutter. Covers items that expose no MIM_BITMAP,
// including HBMMENU_CALLBACK items.
bool CaptureOwnerDrawIcon(PendingCapture& capture, const MenuItem& item,
                          std::vector<uint8_t>& out) {
    if (!capture.contextMenu2 && !capture.contextMenu3) {
        return false;
    }

    const int height =
        std::max(24, static_cast<int>(GetSystemMetrics(SM_CYMENU)));
    const int width = height * 4;
    const UINT commandId = capture.idCmdFirst + item.verbOffset;

    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);

    BITMAPINFO dibInfo = {};
    dibInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    dibInfo.bmiHeader.biWidth = width;
    dibInfo.bmiHeader.biHeight = -height;  // top-down
    dibInfo.bmiHeader.biPlanes = 1;
    dibInfo.bmiHeader.biBitCount = 32;
    dibInfo.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bitmap =
        CreateDIBSection(screen, &dibInfo, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap || !bits) {
        if (bitmap) {
            DeleteObject(bitmap);
        }
        DeleteDC(memory);
        ReleaseDC(nullptr, screen);
        return false;
    }

    HGDIOBJ oldBitmap = SelectObject(memory, bitmap);
    const COLORREF background = MenuBackgroundColor(capture.owner);
    RECT rect = {0, 0, width, height};
    HBRUSH backgroundBrush = CreateSolidBrush(background);
    FillRect(memory, &rect, backgroundBrush);
    DeleteObject(backgroundBrush);

    DRAWITEMSTRUCT drawInfo = {};
    drawInfo.CtlType = ODT_MENU;
    drawInfo.CtlID = commandId;
    drawInfo.itemID = commandId;
    drawInfo.itemAction = ODA_DRAWENTIRE;
    if (item.flags & kModelDisabled) {
        drawInfo.itemState |= ODS_DISABLED;
    }
    if (item.flags & kModelChecked) {
        drawInfo.itemState |= ODS_CHECKED;
    }
    if (item.flags & kModelDefault) {
        drawInfo.itemState |= ODS_DEFAULT;
    }
    drawInfo.hwndItem = reinterpret_cast<HWND>(capture.populatedMenu);
    drawInfo.hDC = memory;
    drawInfo.rcItem = rect;

    bool handled = false;
    if (capture.contextMenu3) {
        LRESULT result = 0;
        handled = SUCCEEDED(capture.contextMenu3->HandleMenuMsg2(
            WM_DRAWITEM, 0, reinterpret_cast<LPARAM>(&drawInfo), &result));
    } else if (capture.contextMenu2) {
        handled = SUCCEEDED(capture.contextMenu2->HandleMenuMsg(
            WM_DRAWITEM, 0, reinterpret_cast<LPARAM>(&drawInfo)));
    }

    if (handled) {
        // Crop the icon gutter: the bounding box of pixels that differ from
        // the menu background in the left half of the item.
        const uint8_t backgroundBlue = GetBValue(background);
        const uint8_t backgroundGreen = GetGValue(background);
        const uint8_t backgroundRed = GetRValue(background);
        const uint8_t* pixels = static_cast<const uint8_t*>(bits);

        int minX = width;
        int minY = height;
        int maxX = -1;
        int maxY = -1;
        const int scanWidth = width / 2;
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < scanWidth; ++x) {
                const uint8_t* pixel = pixels + (static_cast<size_t>(y) * width + x) * 4;
                if (pixel[0] != backgroundBlue || pixel[1] != backgroundGreen ||
                    pixel[2] != backgroundRed) {
                    if (x < minX) minX = x;
                    if (x > maxX) maxX = x;
                    if (y < minY) minY = y;
                    if (y > maxY) maxY = y;
                }
            }
        }
        if (maxX >= minX && maxY >= minY) {
            const int cropWidth = maxX - minX + 1;
            const int cropHeight = maxY - minY + 1;
            out.resize(static_cast<size_t>(cropWidth) * cropHeight * 4);
            for (int y = 0; y < cropHeight; ++y) {
                memcpy(out.data() + static_cast<size_t>(y) * cropWidth * 4,
                       pixels + (static_cast<size_t>(minY + y) * width + minX) * 4,
                       static_cast<size_t>(cropWidth) * 4);
            }
        }
    }

    SelectObject(memory, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    return handled && !out.empty();
}

void CaptureOwnerDrawIcons(PendingCapture& capture, std::vector<MenuItem>& items) {
    for (MenuItem& item : items) {
        CaptureOwnerDrawIcons(capture, item.children);
        if (!item.iconPixels.empty() || !item.iconRef.empty() ||
            item.kind != ItemKind::Command || item.action != ActionKind::ShellVerb ||
            !(item.flags & kModelHasOffset)) {
            continue;
        }

        std::vector<uint8_t> pixels;
        if (CaptureOwnerDrawIcon(capture, item, pixels)) {
            item.iconPixels = std::move(pixels);
            Wh_Log(L"Owner-draw icon captured for '%s'", item.label.c_str());
        } else {
            Wh_Log(L"Owner-draw draw not handled for '%s' (verb '%s')",
                   item.label.c_str(), item.canonicalVerb.c_str());
        }
    }
}

void DiscoverIntoCache(PendingCapture& capture, const ContextSignature& signature) {
    if (capture.discoveryDone || !capture.obj) {
        return;
    }
    if (!EnsureContextPopulated(capture)) {
        return;
    }

    const ULONGLONG start = GetTickCount64();

    // Capture the menu as populated, before host initialization: extensions
    // often switch their bitmaps to callback-drawn icons on
    // WM_INITMENUPOPUP, which would lose the real images.
    MenuModel preInit = BuildModelFromHMenu(capture.populatedMenu, capture.idCmdFirst,
                                            signature, capture.obj);

    // Initialize like a real host, then rebuild for dynamic labels/children.
    if (!capture.menuInitialized) {
        InitializeMenuRecursive(capture, capture.populatedMenu, 0);
        capture.menuInitialized = true;
    }
    MenuModel model = BuildModelFromHMenu(capture.populatedMenu, capture.idCmdFirst,
                                          signature, capture.obj);
    MergePreInitIcons(model.items, preInit.items);
    ClearRecordedItemBitmaps();

    ApplyRegistryIcons(model.items, signature);
    CaptureOwnerDrawIcons(capture, model.items);
    model.handlerModules = capture.handlerModules;
    model.sourceStamp = capture.sourceStamp;
    capture.discoveryDone = true;

    Wh_Log(L"Discovered %zu menu items in %llu ms", model.items.size(),
           static_cast<unsigned long long>(GetTickCount64() - start));
    if (g_settings.debugLogging) {
        DumpModelItems(model.items, 0);
    }
    // Warm the render-ready layout for the custom menu while we are already
    // paying discovery cost, so the next open is a cache hit. Only the UI
    // thread may touch the render device and content caches.
    if (GetCurrentThreadId() == g_uiThreadId) {
        PrebuildLayoutsForWarmup({model}, DpiForWindow(GetDesktopWindow()),
                                 IsDarkThemeActive());
    }
    g_cache.Put(std::move(model));
}

// SendTo entries are built once (at mod init) and copied into the model at
// open time, keeping the interactive path free of filesystem I/O.
std::vector<MenuItem> g_sendToChildren;
std::mutex g_sendToMutex;

void RebuildSendToChildren() {
    std::vector<MenuItem> children;

    PWSTR sendToPath = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_SendTo, 0, nullptr, &sendToPath)) &&
        sendToPath) {
        const std::wstring directory(sendToPath);
        const std::wstring pattern = directory + L"\\*.lnk";
        WIN32_FIND_DATAW findData = {};
        HANDLE find = FindFirstFileW(pattern.c_str(), &findData);
        if (find != INVALID_HANDLE_VALUE) {
            uint32_t nextChildId = 20000;
            do {
                if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    continue;
                }
                std::wstring label = findData.cFileName;
                const size_t dot = label.find_last_of(L'.');
                if (dot != std::wstring::npos) {
                    label.resize(dot);
                }

                MenuItem child{};
                child.id = nextChildId++;
                child.kind = ItemKind::Command;
                child.action = ActionKind::ShellVerb;
                child.canonicalVerb = L"sendto";
                child.label = std::move(label);
                child.targetPath = directory + L"\\" + findData.cFileName;
                children.push_back(std::move(child));
            } while (FindNextFileW(find, &findData));
            FindClose(find);
        }
        CoTaskMemFree(sendToPath);
    }

    std::lock_guard<std::mutex> lock(g_sendToMutex);
    g_sendToChildren = std::move(children);
}

std::vector<MenuItem> GetSendToChildren() {
    std::lock_guard<std::mutex> lock(g_sendToMutex);
    return g_sendToChildren;
}

void InvalidateSendToChildren() {
    std::lock_guard<std::mutex> lock(g_sendToMutex);
    g_sendToChildren.clear();
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

// --- Selection context ------------------------------------------------------
// Reads the current selection for the menu owner. Adapted from the
// remove-context-menu-items mod by Armaninyow (MIT-licensed).

IShellBrowser* GetShellBrowserForWindow(HWND hwnd) {
    struct ShellWindowClass {
        const wchar_t* name;
        bool isFrame;
    };
    static const ShellWindowClass kClasses[] = {
        {L"ShellTabWindowClass", false},
        {L"CabinetWClass", true},
        {L"ExploreWClass", true},
    };

    for (HWND window = hwnd; window; window = GetAncestor(window, GA_PARENT)) {
        wchar_t className[256] = {};
        GetClassNameW(window, className, ARRAYSIZE(className));
        for (const ShellWindowClass& cls : kClasses) {
            if (wcscmp(className, cls.name) != 0) {
                continue;
            }
            LRESULT result =
                SendMessageW(window, WM_USER + 7 /* CWM_GETISHELLBROWSER */, 0, 0);
            IShellBrowser* browser = reinterpret_cast<IShellBrowser*>(result);
            if (browser) {
                return browser;
            }
            if (cls.isFrame) {
                return nullptr;
            }
            break;
        }
    }
    return nullptr;
}

IShellBrowser* GetDesktopShellBrowser() {
    IShellWindows* shellWindows = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL,
                                IID_IShellWindows, (void**)&shellWindows)) ||
        !shellWindows) {
        return nullptr;
    }

    IShellBrowser* browser = nullptr;
    VARIANT empty = {};
    long hwnd = 0;
    IDispatch* dispatch = nullptr;
    if (SUCCEEDED(shellWindows->FindWindowSW(&empty, &empty, SWC_DESKTOP, &hwnd,
                                             SWFO_NEEDDISPATCH, &dispatch)) &&
        dispatch) {
        IServiceProvider* provider = nullptr;
        if (SUCCEEDED(dispatch->QueryInterface(IID_IServiceProvider,
                                               (void**)&provider)) &&
            provider) {
            provider->QueryService(SID_STopLevelBrowser, IID_IShellBrowser,
                                   (void**)&browser);
            provider->Release();
        }
        dispatch->Release();
    }

    shellWindows->Release();
    return browser;
}

struct SelectionInfo {
    std::vector<std::wstring> paths;
    bool folderIsFilesystem = false;
    bool allItemsAreFilesystem = true;
    bool allItemsAreFolders = false;
};

SelectionInfo GetSelectionFromShellBrowser(IShellBrowser* browser) {
    SelectionInfo info;
    if (!browser) {
        return info;
    }

    IShellView* view = nullptr;
    if (SUCCEEDED(browser->QueryActiveShellView(&view)) && view) {
        IFolderView* folderView = nullptr;
        if (SUCCEEDED(view->QueryInterface(IID_IFolderView, (void**)&folderView)) &&
            folderView) {
            IShellFolder* folder = nullptr;
            if (SUCCEEDED(folderView->GetFolder(IID_IShellFolder, (void**)&folder)) &&
                folder) {
                SFGAOF folderAttributes = SFGAO_FILESYSTEM;
                info.folderIsFilesystem =
                    SUCCEEDED(folder->GetAttributesOf(0, nullptr, &folderAttributes)) &&
                    (folderAttributes & SFGAO_FILESYSTEM) != 0;

                bool allFolders = true;
                IEnumIDList* enumIds = nullptr;
                if (SUCCEEDED(folderView->Items(SVGIO_SELECTION, IID_IEnumIDList,
                                                (void**)&enumIds)) &&
                    enumIds) {
                    LPITEMIDLIST pidl = nullptr;
                    while (enumIds->Next(1, &pidl, nullptr) == S_OK) {
                        SFGAOF itemAttributes = SFGAO_FILESYSTEM | SFGAO_FOLDER;
                        PCUITEMID_CHILD child = pidl;
                        if (SUCCEEDED(
                                folder->GetAttributesOf(1, &child, &itemAttributes))) {
                            if (!(itemAttributes & SFGAO_FILESYSTEM)) {
                                info.allItemsAreFilesystem = false;
                            }
                            if (!(itemAttributes & SFGAO_FOLDER)) {
                                allFolders = false;
                            }
                        } else {
                            info.allItemsAreFilesystem = false;
                            allFolders = false;
                        }

                        STRRET strret = {};
                        if (SUCCEEDED(folder->GetDisplayNameOf(pidl, SHGDN_FORPARSING,
                                                               &strret))) {
                            LPWSTR path = nullptr;
                            if (SUCCEEDED(StrRetToStrW(&strret, pidl, &path)) && path) {
                                if (path[0]) {
                                    info.paths.emplace_back(path);
                                }
                                CoTaskMemFree(path);
                            }
                        }
                        CoTaskMemFree(pidl);
                    }
                    enumIds->Release();
                }
                info.allItemsAreFolders = allFolders && !info.paths.empty();
                folder->Release();
            }
            folderView->Release();
        }
        view->Release();
    }
    return info;
}

SelectionInfo GetSelection(HWND owner, ShellViewKind kind) {
    if (kind == ShellViewKind::Desktop) {
        IShellBrowser* browser = GetDesktopShellBrowser();
        SelectionInfo info = GetSelectionFromShellBrowser(browser);
        if (browser) {
            browser->Release();
        }
        return info;
    }

    if (kind == ShellViewKind::ShellDefView) {
        IShellBrowser* browser = GetShellBrowserForWindow(owner);
        if (!browser) {
            return {};
        }
        // CWM_GETISHELLBROWSER returns a borrowed pointer; hold a reference
        // for the duration of the lookup.
        browser->AddRef();
        SelectionInfo info = GetSelectionFromShellBrowser(browser);
        browser->Release();
        return info;
    }

    return {};
}

std::vector<std::wstring> SnapshotLoadedModules() {
    std::vector<std::wstring> modules;
    HANDLE snapshot =
        CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE) {
        return modules;
    }

    MODULEENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    if (Module32FirstW(snapshot, &entry)) {
        do {
            modules.emplace_back(entry.szExePath);
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return modules;
}

std::vector<std::wstring> DiffModules(const std::vector<std::wstring>& before,
                                      const std::vector<std::wstring>& after) {
    std::vector<std::wstring> added;
    for (const std::wstring& path : after) {
        if (std::find(before.begin(), before.end(), path) == before.end()) {
            added.push_back(path);
        }
    }
    return added;
}

}  // namespace cmo

// ===========================================================================
// [CMO:Invoker] Executing a chosen menu item.
// ===========================================================================
namespace cmo {

enum class InvokeResult : uint8_t { Handled, FallbackNative, Failed };

struct InvocationContext {
    HWND owner = nullptr;
    POINT pt = {};
    std::vector<std::wstring> paths;
    std::wstring directory;
    IContextMenu* liveContext = nullptr;
    UINT idCmdFirst = 0;
    ShellViewKind kind = ShellViewKind::None;
    DWORD clipboardSequence = 0;
    bool clipboardHadData = false;
    std::shared_ptr<const RulesConfig> config;
};

int ShowWindowToShowCmd(ShowWindow showWindow) {
    switch (showWindow) {
        case ShowWindow::Maximized:
            return SW_SHOWMAXIMIZED;
        case ShowWindow::Minimized:
            return SW_SHOWMINIMIZED;
        case ShowWindow::Hidden:
            return SW_HIDE;
        default:
            return SW_SHOWNORMAL;
    }
}

void ReplaceAll(std::wstring& text, const std::wstring& from,
                const std::wstring& to) {
    if (from.empty()) {
        return;
    }
    size_t pos = 0;
    while ((pos = text.find(from, pos)) != std::wstring::npos) {
        text.replace(pos, from.size(), to);
        pos += to.size();
    }
}

std::wstring QuotePathIfNeeded(const std::wstring& path) {
    if (path.find(L' ') == std::wstring::npos &&
        path.find(L'\t') == std::wstring::npos) {
        return path;
    }
    return L'"' + path + L'"';
}

// Splits an expanded command line into the executable and its parameters for
// ShellExecuteEx (which does not parse a single command-line string).
bool SplitCommandLine(const std::wstring& command, std::wstring& file,
                      std::wstring& parameters) {
    bool inQuotes = false;
    bool split = false;
    for (size_t i = 0; i < command.size(); ++i) {
        const wchar_t c = command[i];
        if (c == L'"') {
            inQuotes = !inQuotes;
        } else if (!inQuotes && (c == L' ' || c == L'\t')) {
            file = command.substr(0, i);
            parameters = TrimWhitespace(command.substr(i + 1));
            split = true;
            break;
        }
    }
    if (!split) {
        file = command;
        parameters.clear();
    }
    if (file.size() >= 2 && file.front() == L'"' && file.back() == L'"') {
        file = file.substr(1, file.size() - 2);
    }
    return !file.empty();
}

std::wstring ExpandCommandPlaceholders(const std::wstring& command,
                                       const InvocationContext& ctx) {
    std::wstring expanded = ExpandEnv(command);

    ReplaceAll(expanded, L"%dir%", ctx.directory);

    std::wstring allPaths;
    for (const std::wstring& path : ctx.paths) {
        if (!allPaths.empty()) {
            allPaths += L' ';
        }
        allPaths += L'"' + path + L'"';
    }
    ReplaceAll(expanded, L"%*", allPaths);

    if (!ctx.paths.empty()) {
        ReplaceAll(expanded, L"%1", QuotePathIfNeeded(ctx.paths.front()));
    }
    return expanded;
}

bool InvokeCustomCommand(const CustomCommand& command,
                         const InvocationContext& ctx) {
    const std::wstring expanded = ExpandCommandPlaceholders(command.command, ctx);
    if (expanded.empty()) {
        return false;
    }
    std::wstring workingDir = ExpandCommandPlaceholders(command.workingDir, ctx);
    if (workingDir.empty()) {
        workingDir = ctx.directory;
    }

    if (command.runAs == RunAs::Admin) {
        std::wstring file;
        std::wstring parameters;
        if (!SplitCommandLine(expanded, file, parameters)) {
            return false;
        }
        SHELLEXECUTEINFOW info = {};
        info.cbSize = sizeof(info);
        info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
        info.hwnd = ctx.owner;
        info.lpVerb = L"runas";
        info.lpFile = file.c_str();
        info.lpParameters = parameters.empty() ? nullptr : parameters.c_str();
        info.lpDirectory = workingDir.empty() ? nullptr : workingDir.c_str();
        info.nShow = ShowWindowToShowCmd(command.showWindow);
        if (ShellExecuteExW(&info)) {
            if (info.hProcess) {
                CloseHandle(info.hProcess);
            }
            return true;
        }
        return false;
    }

    std::vector<wchar_t> mutableCommand(expanded.begin(), expanded.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = static_cast<WORD>(ShowWindowToShowCmd(command.showWindow));
    PROCESS_INFORMATION pi = {};
    const BOOL ok = CreateProcessW(
        nullptr, mutableCommand.data(), nullptr, nullptr, FALSE, 0, nullptr,
        workingDir.empty() ? nullptr : workingDir.c_str(), &si, &pi);
    if (ok) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return true;
    }

    std::wstring file;
    std::wstring parameters;
    if (!SplitCommandLine(expanded, file, parameters)) {
        return false;
    }
    SHELLEXECUTEINFOW info = {};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    info.hwnd = ctx.owner;
    info.lpFile = file.c_str();
    info.lpParameters = parameters.empty() ? nullptr : parameters.c_str();
    info.lpDirectory = workingDir.empty() ? nullptr : workingDir.c_str();
    info.nShow = ShowWindowToShowCmd(command.showWindow);
    if (ShellExecuteExW(&info)) {
        if (info.hProcess) {
            CloseHandle(info.hProcess);
        }
        return true;
    }
    return false;
}

// Invokes a menu item through the live context object using its descriptor
// (canonical verb or command offset).
// Fills a CMINVOKECOMMANDINFOEX for the item. Both the ANSI lpVerb and the
// wide lpVerbW are set: the shell reads lpVerb to tell offsets from verbs,
// and wide-only descriptors are ignored (observed on real Windows).
void FillInvokeCommandInfo(const MenuItem& item, const InvocationContext& ctx,
                           std::string& ansiStorage, CMINVOKECOMMANDINFOEX& info);

CMINVOKECOMMANDINFOEX BuildInvokeCommandInfo(const MenuItem& item,
                                             const InvocationContext& ctx);

// Maps a view action to its documented folder view mode.
FOLDERVIEWMODE FolderViewModeFor(ViewAction action) {
    switch (action) {
        case ViewAction::ViewExtraLargeIcons:
        case ViewAction::ViewLargeIcons:
        case ViewAction::ViewMediumIcons:
            return FVM_ICON;
        case ViewAction::ViewSmallIcons:
            return FVM_SMALLICON;
        case ViewAction::ViewList:
            return FVM_LIST;
        case ViewAction::ViewDetails:
            return FVM_DETAILS;
        case ViewAction::ViewTiles:
            return FVM_TILE;
        case ViewAction::ViewContent:
            return FVM_CONTENT;
        default:
            return FVM_ICON;
    }
}

bool IsIconViewMode(ViewAction action) {
    return action == ViewAction::ViewExtraLargeIcons ||
           action == ViewAction::ViewLargeIcons ||
           action == ViewAction::ViewMediumIcons ||
           action == ViewAction::ViewSmallIcons;
}

bool EqualPropertyKey(const PROPERTYKEY& left, const PROPERTYKEY& right) {
    return IsEqualGUID(left.fmtid, right.fmtid) && left.pid == right.pid;
}

const PROPERTYKEY kNullPropertyKey = {};

bool InvokeContextItem(IContextMenu* context, const MenuItem& item,
                       const InvocationContext& ctx) {
    if (!context) {
        return false;
    }
    std::string ansiStorage;
    CMINVOKECOMMANDINFOEX info = {};
    FillInvokeCommandInfo(item, ctx, ansiStorage, info);
    return SUCCEEDED(context->InvokeCommand(reinterpret_cast<CMINVOKECOMMANDINFO*>(&info)));
}

// Active shell view for the menu owner, or nullptr.
IShellView* GetActiveShellView(HWND owner, ShellViewKind kind) {
    IShellBrowser* browser = nullptr;
    if (kind == ShellViewKind::Desktop) {
        browser = GetDesktopShellBrowser();
    } else if (kind == ShellViewKind::ShellDefView) {
        browser = GetShellBrowserForWindow(owner);
        if (browser) {
            browser->AddRef();  // borrowed pointer from CWM_GETISHELLBROWSER
        }
    }
    if (!browser) {
        return nullptr;
    }

    IShellView* view = nullptr;
    browser->QueryActiveShellView(&view);
    browser->Release();
    return view;
}

// Current folder shown by the view: the target directory for New items.
std::wstring GetCurrentFolderPath(HWND owner, ShellViewKind kind) {
    IShellView* view = GetActiveShellView(owner, kind);
    if (!view) {
        return L"";
    }

    std::wstring path;
    IFolderView* folderView = nullptr;
    if (SUCCEEDED(view->QueryInterface(IID_IFolderView, (void**)&folderView)) &&
        folderView) {
        IShellFolder* folder = nullptr;
        if (SUCCEEDED(folderView->GetFolder(IID_IShellFolder, (void**)&folder)) &&
            folder) {
            IPersistFolder2* persist = nullptr;
            if (SUCCEEDED(folder->QueryInterface(IID_IPersistFolder2, (void**)&persist)) &&
                persist) {
                PIDLIST_ABSOLUTE pidl = nullptr;
                if (SUCCEEDED(persist->GetCurFolder(&pidl)) && pidl) {
                    wchar_t buffer[MAX_PATH] = {};
                    if (SHGetPathFromIDListW(pidl, buffer)) {
                        path = buffer;
                    }
                    CoTaskMemFree(pidl);
                }
                persist->Release();
            }
            folder->Release();
        }
        folderView->Release();
    }
    view->Release();
    return path;
}

// Selects a newly created item in the view, optionally in rename mode.
void SelectCreatedItem(HWND owner, ShellViewKind kind, const std::wstring& path,
                       bool edit) {
    IShellView* view = GetActiveShellView(owner, kind);
    if (!view) {
        return;
    }
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (SUCCEEDED(SHILCreateFromPath(path.c_str(), &pidl, nullptr)) && pidl) {
        SVSIF flags =
            SVSI_DESELECTOTHERS | SVSI_ENSUREVISIBLE | SVSI_FOCUSED | SVSI_SELECT;
        if (edit) {
            flags |= SVSI_EDIT;
        }
        view->SelectItem(ILFindLastID(pidl), flags);
        CoTaskMemFree(pidl);
    }
    view->Release();
}

// Creates the New submenu item chosen from the core model.
bool CreateNewItemFromTemplate(const MenuItem& item, const InvocationContext& ctx) {
    if (item.newIndex >= g_newTemplates.size()) {
        return false;
    }
    const NewTemplate& tmpl = g_newTemplates[item.newIndex];
    const std::wstring directory = GetCurrentFolderPath(ctx.owner, ctx.kind);
    if (directory.empty()) {
        return false;
    }

    std::wstring createdPath;
    if (!CreateNewItemInFolder(tmpl, directory, createdPath)) {
        return false;
    }

    if (!createdPath.empty()) {
        const DWORD event =
            tmpl.kind == NewTemplate::Kind::Folder ? SHCNE_MKDIR : SHCNE_CREATE;
        SHChangeNotify(event, SHCNF_PATHW | SHCNF_FLUSH, createdPath.c_str(), nullptr);
        const bool edit = tmpl.kind != NewTemplate::Kind::Shortcut &&
                          tmpl.kind != NewTemplate::Kind::Command;
        SelectCreatedItem(ctx.owner, ctx.kind, createdPath, edit);
    }
    return true;
}

bool CreateShortcutForPaths(const std::vector<std::wstring>& paths) {
    if (paths.empty()) {
        return false;
    }

    bool ok = false;
    for (const std::wstring& path : paths) {
        std::wstring directory = path;
        const size_t slash = directory.find_last_of(L"\\/");
        if (slash != std::wstring::npos) {
            directory.resize(slash);
        } else {
            directory.clear();
        }
        std::wstring name = path.substr(slash == std::wstring::npos ? 0 : slash + 1);
        const size_t dot = name.find_last_of(L'.');
        if (dot != std::wstring::npos && dot > 0) {
            name.resize(dot);
        }

        std::wstring target;
        if (!directory.empty()) {
            target = directory + L"\\";
        }
        target += name + L" - Shortcut.lnk";

        IShellLinkW* link = nullptr;
        if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_IShellLinkW, (void**)&link)) ||
            !link) {
            continue;
        }
        link->SetPath(path.c_str());
        if (!directory.empty()) {
            link->SetWorkingDirectory(directory.c_str());
        }

        IPersistFile* file = nullptr;
        if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, (void**)&file)) && file) {
            if (SUCCEEDED(file->Save(target.c_str(), TRUE))) {
                ok = true;
            }
            file->Release();
        }
        link->Release();
    }
    return ok;
}

// The active view as IFolderView2, used for sorting and grouping.
IFolderView2* GetFolderView2(HWND owner, ShellViewKind kind) {
    IShellView* view = GetActiveShellView(owner, kind);
    if (!view) {
        return nullptr;
    }
    IFolderView2* folderView = nullptr;
    view->QueryInterface(IID_IFolderView2, (void**)&folderView);
    view->Release();
    return folderView;
}

bool InvokeSortBy(const MenuItem& item, const InvocationContext& ctx) {
    if (item.sortIndex >= kShellPropertyKeyCount) {
        return false;
    }
    IFolderView2* view = GetFolderView2(ctx.owner, ctx.kind);
    if (!view) {
        return false;
    }
    SORTCOLUMN column = {};
    column.propkey = kShellPropertyKeys[item.sortIndex];
    column.direction = item.sortAscending ? 1 : -1;
    const bool ok = SUCCEEDED(view->SetSortColumns(&column, 1));
    view->Release();
    return ok;
}

bool InvokeSortDirection(const MenuItem& item, const InvocationContext& ctx) {
    IFolderView2* view = GetFolderView2(ctx.owner, ctx.kind);
    if (!view) {
        return false;
    }

    SORTCOLUMN column = {};
    column.propkey = kShellPropertyKeys[0];  // default to Name
    int count = 0;
    if (SUCCEEDED(view->GetSortColumnCount(&count)) && count > 0) {
        SORTCOLUMN current = {};
        if (SUCCEEDED(view->GetSortColumns(&current, 1))) {
            column.propkey = current.propkey;
        }
    }
    column.direction = item.sortAscending ? 1 : -1;
    const bool ok = SUCCEEDED(view->SetSortColumns(&column, 1));
    view->Release();
    return ok;
}

bool InvokeGroupBy(const MenuItem& item, const InvocationContext& ctx) {
    IFolderView2* view = GetFolderView2(ctx.owner, ctx.kind);
    if (!view) {
        return false;
    }
    const PROPERTYKEY key = item.sortIndex == kGroupNoneIndex
                                ? kNullPropertyKey
                                : kShellPropertyKeys[item.sortIndex];
    const bool ok = SUCCEEDED(view->SetGroupBy(key, TRUE));
    view->Release();
    return ok;
}

bool InvokeGroupDirection(const MenuItem& item, const InvocationContext& ctx) {
    IFolderView2* view = GetFolderView2(ctx.owner, ctx.kind);
    if (!view) {
        return false;
    }
    PROPERTYKEY key = {};
    WINBOOL ascending = TRUE;
    bool ok = false;
    if (SUCCEEDED(view->GetGroupBy(&key, &ascending)) &&
        !EqualPropertyKey(key, kNullPropertyKey)) {
        ok = SUCCEEDED(view->SetGroupBy(key, item.sortAscending ? TRUE : FALSE));
    }
    view->Release();
    return ok;
}

// Marks the View / Sort by / Group by entries that match the view's current
// state, so the menu shows the same dots as the shell's own submenus.
void ApplyViewStateChecks(std::vector<MenuItem>& items, HWND owner,
                          ShellViewKind kind) {
    IFolderView2* view = GetFolderView2(owner, kind);
    if (!view) {
        return;
    }

    FOLDERVIEWMODE mode = FVM_AUTO;
    int iconSize = -1;
    const bool haveMode = SUCCEEDED(view->GetViewModeAndIconSize(&mode, &iconSize));
    DWORD folderFlags = 0;
    const bool haveFlags = SUCCEEDED(view->GetCurrentFolderFlags(&folderFlags));

    SORTCOLUMN sortColumn = {};
    int sortCount = 0;
    const bool haveSort =
        SUCCEEDED(view->GetSortColumnCount(&sortCount)) && sortCount > 0 &&
        SUCCEEDED(view->GetSortColumns(&sortColumn, 1));

    PROPERTYKEY groupKey = {};
    WINBOOL groupAscending = TRUE;
    const bool haveGroup = SUCCEEDED(view->GetGroupBy(&groupKey, &groupAscending));
    const bool groupActive = haveGroup && !EqualPropertyKey(groupKey, kNullPropertyKey);

    for (MenuItem& submenu : items) {
        if (submenu.kind != ItemKind::Submenu) {
            continue;
        }
        for (MenuItem& child : submenu.children) {
            if (child.action == ActionKind::ViewAction) {
                const ViewAction action = static_cast<ViewAction>(child.viewAction);
                if (!haveMode) {
                    continue;
                }
                if (IsIconViewMode(action) && child.iconSize >= 0) {
                    if (mode == FVM_ICON && iconSize == child.iconSize) {
                        child.flags |= kModelChecked;
                    }
                } else if (action == ViewAction::ViewSmallIcons) {
                    if (mode == FVM_SMALLICON || (mode == FVM_ICON && iconSize == 16)) {
                        child.flags |= kModelChecked;
                    }
                } else if (action == ViewAction::ViewList) {
                    if (mode == FVM_LIST) {
                        child.flags |= kModelChecked;
                    }
                } else if (action == ViewAction::ViewDetails) {
                    if (mode == FVM_DETAILS) {
                        child.flags |= kModelChecked;
                    }
                } else if (action == ViewAction::ViewTiles) {
                    if (mode == FVM_TILE) {
                        child.flags |= kModelChecked;
                    }
                } else if (action == ViewAction::ViewContent) {
                    if (mode == FVM_CONTENT) {
                        child.flags |= kModelChecked;
                    }
                } else if (haveFlags && action == ViewAction::AutoArrange) {
                    if (folderFlags & FWF_AUTOARRANGE) {
                        child.flags |= kModelChecked;
                    }
                } else if (haveFlags && action == ViewAction::AlignToGrid) {
                    if (folderFlags & FWF_SNAPTOGRID) {
                        child.flags |= kModelChecked;
                    }
                }
            } else if (child.action == ActionKind::SortBy) {
                if (haveSort && child.sortIndex < kShellPropertyKeyCount &&
                    EqualPropertyKey(kShellPropertyKeys[child.sortIndex],
                                     sortColumn.propkey)) {
                    child.flags |= kModelChecked;
                }
            } else if (child.action == ActionKind::SortDirection) {
                if (haveSort &&
                    (sortColumn.direction > 0) == child.sortAscending) {
                    child.flags |= kModelChecked;
                }
            } else if (child.action == ActionKind::GroupBy) {
                if (child.sortIndex == kGroupNoneIndex) {
                    if (haveGroup && !groupActive) {
                        child.flags |= kModelChecked;
                    }
                } else if (groupActive && child.sortIndex < kShellPropertyKeyCount &&
                           EqualPropertyKey(kShellPropertyKeys[child.sortIndex],
                                            groupKey)) {
                    child.flags |= kModelChecked;
                }
            } else if (child.action == ActionKind::GroupDirection) {
                if (groupActive && (groupAscending != FALSE) == child.sortAscending) {
                    child.flags |= kModelChecked;
                }
            }
        }
    }
    view->Release();
}

// Dispatches a documented view operation through IFolderView2 / IShellView.
InvokeResult InvokeViewAction(const MenuItem& item, const InvocationContext& ctx) {
    IShellView* view = GetActiveShellView(ctx.owner, ctx.kind);
    if (!view) {
        return InvokeResult::FallbackNative;
    }

    InvokeResult result = InvokeResult::FallbackNative;
    const ViewAction action = static_cast<ViewAction>(item.viewAction);

    if (action == ViewAction::Refresh) {
        result = SUCCEEDED(view->Refresh()) ? InvokeResult::Handled
                                            : InvokeResult::FallbackNative;
    } else {
        IFolderView2* folderView = nullptr;
        if (SUCCEEDED(view->QueryInterface(IID_IFolderView2, (void**)&folderView)) &&
            folderView) {
            switch (action) {
                case ViewAction::Rename:
                    result = SUCCEEDED(folderView->DoRename())
                                 ? InvokeResult::Handled
                                 : InvokeResult::FallbackNative;
                    break;
                case ViewAction::ViewExtraLargeIcons:
                case ViewAction::ViewLargeIcons:
                case ViewAction::ViewMediumIcons:
                case ViewAction::ViewSmallIcons:
                case ViewAction::ViewList:
                case ViewAction::ViewDetails:
                case ViewAction::ViewTiles:
                case ViewAction::ViewContent: {
                    const FOLDERVIEWMODE mode = FolderViewModeFor(action);
                    const int iconSize = IsIconViewMode(action) ? item.iconSize : -1;
                    result = SUCCEEDED(folderView->SetViewModeAndIconSize(mode, iconSize))
                                 ? InvokeResult::Handled
                                 : InvokeResult::FallbackNative;
                    break;
                }
                case ViewAction::AutoArrange:
                case ViewAction::AlignToGrid: {
                    DWORD flags = 0;
                    const DWORD mask = action == ViewAction::AutoArrange
                                           ? FWF_AUTOARRANGE
                                           : FWF_SNAPTOGRID;
                    if (SUCCEEDED(folderView->GetCurrentFolderFlags(&flags))) {
                        result = SUCCEEDED(
                                     folderView->SetCurrentFolderFlags(mask, flags ^ mask))
                                     ? InvokeResult::Handled
                                     : InvokeResult::FallbackNative;
                    }
                    break;
                }
                default:
                    break;
            }
            folderView->Release();
        }
    }

    view->Release();
    return result;
}

bool CopyAsPath(const std::vector<std::wstring>& paths) {
    std::wstring text;
    for (const std::wstring& path : paths) {
        if (!text.empty()) {
            text += L"\r\n";
        }
        text += L'"';
        text += path;
        text += L'"';
    }
    if (text.empty()) {
        return false;
    }

    if (!OpenClipboard(nullptr)) {
        return false;
    }
    bool ok = false;
    if (EmptyClipboard()) {
        const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        if (HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
            if (void* data = GlobalLock(memory)) {
                memcpy(data, text.c_str(), bytes);
                GlobalUnlock(memory);
                if (SetClipboardData(CF_UNICODETEXT, memory)) {
                    ok = true;
                } else {
                    GlobalFree(memory);
                }
            } else {
                GlobalFree(memory);
            }
        }
    }
    CloseClipboard();
    return ok;
}

bool ClipboardHasFileData() {
    return IsClipboardFormatAvailable(CF_HDROP) != FALSE;
}

// Paste availability: cached state while the clipboard sequence is unchanged,
// otherwise the freshly checked state.
bool ComputePasteEnabled(DWORD sequenceAtOpen, DWORD currentSequence,
                         bool cachedHadData, bool currentHasData) {
    return sequenceAtOpen == currentSequence ? cachedHadData : currentHasData;
}

// Executes a SendTo shortcut with the selected paths as arguments.
bool InvokeSendTo(const std::wstring& target, const std::vector<std::wstring>& paths) {
    if (target.empty() || paths.empty()) {
        return false;
    }

    std::wstring parameters;
    for (const std::wstring& path : paths) {
        if (!parameters.empty()) {
            parameters += L' ';
        }
        parameters += L'"';
        parameters += path;
        parameters += L'"';
    }

    SHELLEXECUTEINFOW info = {};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_INVOKEIDLIST | SEE_MASK_FLAG_NO_UI;
    info.lpFile = target.c_str();
    info.lpParameters = parameters.c_str();
    info.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&info) != FALSE;
}

// Fills an invocation descriptor. Both fields are populated deliberately:
// the shell's default context menu reads lpVerb (ANSI) to decide between an
// offset (MAKEINTRESOURCE) and a canonical verb, and ignores wide-only input.
void FillInvokeCommandInfo(const MenuItem& item, const InvocationContext& ctx,
                           std::string& ansiStorage, CMINVOKECOMMANDINFOEX& info) {
    info = {};
    info.cbSize = sizeof(info);
    info.fMask = CMIC_MASK_UNICODE;
    info.hwnd = ctx.owner;
    info.nShow = SW_SHOWNORMAL;
    info.ptInvoke = ctx.pt;

    auto descriptor = ChooseInvokeDescriptor(item);
    if (!descriptor.first.empty()) {
        const int length = WideCharToMultiByte(CP_ACP, 0, descriptor.first.c_str(), -1,
                                               nullptr, 0, nullptr, nullptr);
        if (length > 0) {
            ansiStorage.resize(static_cast<size_t>(length));
            WideCharToMultiByte(CP_ACP, 0, descriptor.first.c_str(), -1,
                                ansiStorage.data(), length, nullptr, nullptr);
            ansiStorage.resize(static_cast<size_t>(length - 1));
            info.lpVerb = ansiStorage.c_str();
        }
        info.lpVerbW = descriptor.first.c_str();
    } else {
        info.lpVerb = MAKEINTRESOURCEA(descriptor.second);
        info.lpVerbW = MAKEINTRESOURCEW(descriptor.second);
    }
}

CMINVOKECOMMANDINFOEX BuildInvokeCommandInfo(const MenuItem& item,
                                             const InvocationContext& ctx) {
    static thread_local std::string ansiStorage;
    CMINVOKECOMMANDINFOEX info = {};
    FillInvokeCommandInfo(item, ctx, ansiStorage, info);
    return info;
}

// Runs the real population once per open on the live object, retaining the
// populated menu for discovery and the native fallback. Idempotent.
bool EnsureContextPopulated(PendingCapture& capture) {
    if (capture.populated) {
        return true;
    }
    if (!capture.obj) {
        return false;
    }

    const std::vector<std::wstring> modulesBefore = SnapshotLoadedModules();
    const ULONGLONG start = GetTickCount64();

    HMENU menu = CreatePopupMenu();
    if (!menu) {
        return false;
    }
    // Extensions may attach bitmaps with SetMenuItemBitmaps during population;
    // start a fresh recording so lookups match this menu.
    ClearRecordedItemBitmaps();
    ReplayInto(capture.obj, menu, capture.indexMenu, capture.idCmdFirst, capture.idCmdLast,
               capture.flags);

    capture.populatedMenu = menu;
    capture.populated = true;
    capture.used = true;
    capture.handlerModules = DiffModules(modulesBefore, SnapshotLoadedModules());
    capture.sourceStamp = ComputeModuleStamp(capture.handlerModules);

    capture.obj->QueryInterface(IID_IContextMenu3, (void**)&capture.contextMenu3);
    capture.obj->QueryInterface(IID_IContextMenu2, (void**)&capture.contextMenu2);

    Wh_Log(L"Population: %llu ms, %d items",
           static_cast<unsigned long long>(GetTickCount64() - start),
           GetMenuItemCount(menu));
    return true;
}

// Invokes a cached extension item through the live context object, preferring
// its native offset (verb strings are rejected by the shell's own menu).
InvokeResult InvokeExtensionItem(const MenuItem& item, const InvocationContext& ctx,
                                 PendingCapture& capture) {
    if (item.flags & kModelOwnerDraw) {
        return InvokeResult::FallbackNative;
    }
    if (!ctx.liveContext || !EnsureContextPopulated(capture)) {
        return InvokeResult::FallbackNative;
    }

    if (item.flags & kModelHasOffset) {
        MenuItem offsetItem = item;
        offsetItem.canonicalVerb.clear();
        if (InvokeContextItem(ctx.liveContext, offsetItem, ctx)) {
            return InvokeResult::Handled;
        }
    }
    if (InvokeContextItem(ctx.liveContext, item, ctx)) {
        return InvokeResult::Handled;
    }
    return InvokeResult::FallbackNative;
}

// Finds the offset of the native item matching a core item, by canonical
// verb first and label second. Recurses into submenus.
std::optional<uint32_t> FindNativeOffsetInMenu(HMENU menu, UINT idCmdFirst,
                                               IContextMenu* context,
                                               const MenuItem& target) {
    const int count = GetMenuItemCount(menu);
    for (int i = 0; i < count; ++i) {
        MENUITEMINFOW info = {};
        info.cbSize = sizeof(info);
        info.fMask = MIIM_ID | MIIM_FTYPE | MIIM_SUBMENU;
        if (!GetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &info)) {
            continue;
        }
        if (info.fType & MFT_SEPARATOR) {
            continue;
        }
        if (info.hSubMenu) {
            if (auto found = FindNativeOffsetInMenu(info.hSubMenu, idCmdFirst, context,
                                                    target)) {
                return found;
            }
            continue;
        }
        if (info.wID < idCmdFirst) {
            continue;
        }
        const uint32_t offset = info.wID - idCmdFirst;

        if (context && !target.canonicalVerb.empty()) {
            wchar_t verb[128] = {};
            if (SUCCEEDED(context->GetCommandString(
                    static_cast<UINT_PTR>(offset), GCS_VERBW, nullptr,
                    reinterpret_cast<LPSTR>(verb), ARRAYSIZE(verb))) &&
                _wcsicmp(verb, target.canonicalVerb.c_str()) == 0) {
                return offset;
            }
        }
        if (!target.label.empty()) {
            wchar_t label[512] = {};
            const int length = GetMenuStringW(menu, static_cast<UINT>(i), label,
                                              ARRAYSIZE(label), MF_BYPOSITION);
            if (length > 0 && _wcsicmp(label, target.label.c_str()) == 0) {
                return offset;
            }
        }
    }
    return std::nullopt;
}

std::vector<std::wstring> BuiltinActionTargets(BuiltinAction action,
                                               const InvocationContext& ctx) {
    std::vector<std::wstring> targets = ctx.paths;
    if (targets.empty() && !ctx.directory.empty()) {
        targets.push_back(ctx.directory);
    }
    if (action == BuiltinAction::Properties && targets.size() > 1) {
        targets.resize(1);
    }
    return targets;
}

bool InvokeBuiltinAction(const MenuItem& item, const InvocationContext& ctx) {
    const std::vector<std::wstring> targets =
        BuiltinActionTargets(item.builtinAction, ctx);
    if (targets.empty()) {
        return false;
    }
    switch (item.builtinAction) {
        case BuiltinAction::CopyPath:
            return CopyAsPath(targets);
        case BuiltinAction::OpenNewWindow: {
            bool any = false;
            for (const std::wstring& folder : targets) {
                SHELLEXECUTEINFOW info = {};
                info.cbSize = sizeof(info);
                info.fMask = SEE_MASK_FLAG_NO_UI;
                info.hwnd = ctx.owner;
                info.lpVerb = L"explore";
                info.lpFile = folder.c_str();
                info.nShow = SW_SHOWNORMAL;
                any = ShellExecuteExW(&info) != FALSE || any;
            }
            return any;
        }
        case BuiltinAction::Properties: {
            if (SHObjectProperties(ctx.owner, SHOP_FILEPATH, targets.front().c_str(),
                                   nullptr)) {
                return true;
            }
            SHELLEXECUTEINFOW info = {};
            info.cbSize = sizeof(info);
            info.fMask = SEE_MASK_FLAG_NO_UI;
            info.hwnd = ctx.owner;
            info.lpVerb = L"properties";
            info.lpFile = targets.front().c_str();
            info.nShow = SW_SHOWNORMAL;
            return ShellExecuteExW(&info) != FALSE;
        }
        case BuiltinAction::None:
            return false;
    }
    return false;
}

InvokeResult InvokeItem(const MenuItem& item, const InvocationContext& ctx,
                        PendingCapture& capture) {
    if (item.kind == ItemKind::Separator || (item.flags & kModelDisabled)) {
        return InvokeResult::Handled;
    }

    switch (item.action) {
        case ActionKind::Fallback:
            return InvokeResult::FallbackNative;
        case ActionKind::Submenu:
            return InvokeResult::Handled;
        case ActionKind::NewItem:
            return CreateNewItemFromTemplate(item, ctx) ? InvokeResult::Handled
                                                        : InvokeResult::Failed;
        case ActionKind::SortBy:
            return InvokeSortBy(item, ctx) ? InvokeResult::Handled
                                           : InvokeResult::Failed;
        case ActionKind::SortDirection:
            return InvokeSortDirection(item, ctx) ? InvokeResult::Handled
                                                  : InvokeResult::Failed;
        case ActionKind::GroupBy:
            return InvokeGroupBy(item, ctx) ? InvokeResult::Handled
                                            : InvokeResult::Failed;
        case ActionKind::GroupDirection:
            return InvokeGroupDirection(item, ctx) ? InvokeResult::Handled
                                                   : InvokeResult::Failed;
        case ActionKind::Builtin:
            return InvokeBuiltinAction(item, ctx) ? InvokeResult::Handled
                                                  : InvokeResult::Failed;
        case ActionKind::CustomCommand:
            if (ctx.config &&
                item.customCommandIndex < ctx.config->commands.size() &&
                InvokeCustomCommand(ctx.config->commands[item.customCommandIndex],
                                    ctx)) {
                return InvokeResult::Handled;
            }
            return InvokeResult::Failed;
        case ActionKind::ViewAction:
            return InvokeViewAction(item, ctx);
        case ActionKind::ShellVerb:
            if (item.canonicalVerb == L"copyaspath") {
                return CopyAsPath(ctx.paths) ? InvokeResult::Handled
                                             : InvokeResult::Failed;
            }
            if (item.canonicalVerb == L"sendto") {
                return InvokeSendTo(item.targetPath, ctx.paths)
                           ? InvokeResult::Handled
                           : InvokeResult::FallbackNative;
            }
            if (item.canonicalVerb == L"createshortcut") {
                return CreateShortcutForPaths(ctx.paths) ? InvokeResult::Handled
                                                         : InvokeResult::FallbackNative;
            }
            if (item.canonicalVerb == L"paste") {
                const DWORD currentSequence = GetClipboardSequenceNumber();
                const bool currentHasData =
                    currentSequence == ctx.clipboardSequence ? ctx.clipboardHadData
                                                             : ClipboardHasFileData();
                if (!ComputePasteEnabled(ctx.clipboardSequence, currentSequence,
                                         ctx.clipboardHadData, currentHasData)) {
                    return InvokeResult::Handled;
                }
            }
            // The shell only knows its own commands once the object has been
            // populated. Verb strings are rejected by the default context
            // menu on real Windows, while its own offsets always dispatch,
            // so prefer the offset and fall back to the verb last.
            if (!EnsureContextPopulated(capture)) {
                return InvokeResult::FallbackNative;
            }
            if (item.flags & kModelHasOffset) {
                MenuItem offsetItem = item;
                offsetItem.canonicalVerb.clear();
                if (InvokeContextItem(ctx.liveContext, offsetItem, ctx)) {
                    return InvokeResult::Handled;
                }
            } else if (auto nativeOffset = FindNativeOffsetInMenu(
                           capture.populatedMenu, capture.idCmdFirst, ctx.liveContext,
                           item)) {
                MenuItem offsetItem = item;
                offsetItem.canonicalVerb.clear();
                offsetItem.verbOffset = *nativeOffset;
                if (InvokeContextItem(ctx.liveContext, offsetItem, ctx)) {
                    return InvokeResult::Handled;
                }
            }
            if (!item.canonicalVerb.empty() &&
                InvokeContextItem(ctx.liveContext, item, ctx)) {
                return InvokeResult::Handled;
            }
            return InvokeResult::FallbackNative;
    }
    return InvokeResult::Failed;
}

}  // namespace cmo

// ===========================================================================
// [CMO:View] Rendering abstraction and the native implementation.
// ===========================================================================
namespace cmo {

using TrackPopupMenuEx_t = decltype(&TrackPopupMenuEx);
inline TrackPopupMenuEx_t TrackPopupMenuEx_Original = nullptr;

using TrackPopupMenu_t = decltype(&TrackPopupMenu);
inline TrackPopupMenu_t TrackPopupMenu_Original = nullptr;

// Temporarily disables menu animation (the master switch) while a menu we
// display is opening, so it appears instantly. Disabling only the fade is not
// enough: per the SPI_SETMENUFADE contract, menus then fall back to the slide
// animation. The previous value is restored on scope exit; the change is
// session-only (never written to disk, no broadcast).
std::atomic<bool> g_animationSuppressed{false};

class MenuAnimationSuppressor {
public:
    MenuAnimationSuppressor() {
        if (!g_settings.instantMenuFade) {
            return;
        }
        BOOL enabled = FALSE;
        if (!SystemParametersInfoW(SPI_GETMENUANIMATION, 0, &enabled, 0) || !enabled) {
            return;
        }
        if (SystemParametersInfoW(
                SPI_SETMENUANIMATION, 0,
                reinterpret_cast<PVOID>(static_cast<ULONG_PTR>(FALSE)), 0)) {
            active_ = true;
            g_animationSuppressed.store(true);
        }
    }

    ~MenuAnimationSuppressor() {
        if (!active_) {
            return;
        }
        SystemParametersInfoW(
            SPI_SETMENUANIMATION, 0,
            reinterpret_cast<PVOID>(static_cast<ULONG_PTR>(TRUE)), 0);
        g_animationSuppressed.store(false);
    }

private:
    bool active_ = false;
};

// Defensive restore in case a suppressor was active when the mod unloaded.
void RestoreMenuAnimation() {
    if (g_animationSuppressed.exchange(false)) {
        SystemParametersInfoW(
            SPI_SETMENUANIMATION, 0,
            reinterpret_cast<PVOID>(static_cast<ULONG_PTR>(TRUE)), 0);
    }
}

// Temporarily lowers the system submenu show delay (MenuShowDelay, default
// 400 ms) while the replacement menu is open; the previous value is restored
// on scope exit. The delay is never lengthened, and a configured value of -1
// leaves the system setting alone.
std::atomic<bool> g_menuDelaySuppressed{false};
std::atomic<DWORD> g_menuDelayOriginal{400};

class MenuDelaySuppressor {
public:
    MenuDelaySuppressor() {
        if (g_settings.submenuDelayMs < 0) {
            return;
        }
        DWORD current = 0;
        if (!SystemParametersInfoW(SPI_GETMENUSHOWDELAY, 0, &current, 0)) {
            return;
        }
        if (current <= static_cast<DWORD>(g_settings.submenuDelayMs)) {
            return;
        }
        g_menuDelayOriginal.store(current);
        if (SystemParametersInfoW(SPI_SETMENUSHOWDELAY,
                                  static_cast<UINT>(g_settings.submenuDelayMs),
                                  nullptr, 0)) {
            active_ = true;
            g_menuDelaySuppressed.store(true);
        }
    }

    ~MenuDelaySuppressor() {
        if (!active_) {
            return;
        }
        SystemParametersInfoW(SPI_SETMENUSHOWDELAY,
                              static_cast<UINT>(g_menuDelayOriginal.load()), nullptr,
                              0);
        g_menuDelaySuppressed.store(false);
    }

private:
    bool active_ = false;
};

// Defensive restore in case a suppressor was active when the mod unloaded.
void RestoreMenuDelay() {
    if (g_menuDelaySuppressed.exchange(false)) {
        SystemParametersInfoW(SPI_SETMENUSHOWDELAY,
                              static_cast<UINT>(g_menuDelayOriginal.load()), nullptr,
                              0);
    }
}

// Parses a shell icon reference: "file,index", "file", or a quoted path with
// an optional trailing ",index". Environment expansion happens at load time.
bool ParseIconRef(std::wstring_view ref, std::wstring& path, int& index) {
    path.clear();
    index = 0;

    std::wstring_view text = ref;
    while (!text.empty() && (text.front() == L' ' || text.front() == L'\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == L' ' || text.back() == L'\t')) {
        text.remove_suffix(1);
    }
    if (text.empty()) {
        return false;
    }

    if (text.front() == L'"') {
        text.remove_prefix(1);
        const size_t quote = text.find(L'"');
        if (quote == std::wstring_view::npos) {
            return false;
        }
        path.assign(text.substr(0, quote));
        text.remove_prefix(quote + 1);
        if (!text.empty() && text.front() == L',') {
            text.remove_prefix(1);
            index = _wtoi(std::wstring(text).c_str());
        }
        return !path.empty();
    }

    const size_t comma = text.find_last_of(L',');
    if (comma != std::wstring_view::npos) {
        const std::wstring_view suffix = text.substr(comma + 1);
        bool numeric = !suffix.empty();
        for (wchar_t c : suffix) {
            if (c < L'0' || c > L'9') {
                numeric = false;
                break;
            }
        }
        if (numeric) {
            path.assign(text.substr(0, comma));
            index = _wtoi(std::wstring(suffix).c_str());
            return !path.empty();
        }
    }

    path.assign(text);
    return true;
}

// Cached menu bitmaps and icons. Bitmaps are owned by this cache and must not
// be destroyed by callers; they are released in Clear() at mod unload.
// Icon references understood here:
//   "@file"       - the selected item's type icon
//   "@folder"     - the stock folder icon
//   "@glyph:XXXX" - an icon-font glyph rendered to a bitmap
//   anything else - a shell icon reference for ParseIconRef
class IconCache {
public:
    ~IconCache() { Clear(); }

    // Returns a menu bitmap for the item's captured icon, or nullptr. The
    // bitmap is composited over the menu background so classic (alpha-less)
    // menu drawing shows it correctly.
    HBITMAP GetBitmap(const MenuItem& item, int sizePx) {
        return GetBitmapFor(item.iconRef, item.iconPixels, sizePx);
    }

    // Custom-renderer entry point: no MenuItem required.
    HBITMAP GetBitmapFor(const std::wstring& iconRef,
                         const std::vector<uint8_t>& iconPixels, int sizePx) {
        if (!iconPixels.empty()) {
            return BitmapFromPixels(iconPixels, sizePx);
        }
        if (iconRef.empty()) {
            return nullptr;
        }
        return BitmapFromRef(iconRef, sizePx);
    }

    // The owner window whose theme determines the menu background color.
    void SetThemeOwner(HWND owner) { themeOwner_ = owner; }

    // Pre-renders the core action glyphs, so the open path only attaches
    // already-cached bitmaps.
    void PreloadCoreIcons(int sizePx) {
        static const wchar_t* kGlyphRefs[] = {
            L"@glyph:E8C6", L"@glyph:E8C8", L"@glyph:E8AC", L"@glyph:E74D",
            L"@glyph:E713", L"@glyph:E8E5", L"@glyph:E72C", L"@glyph:E77F",
        };
        for (const wchar_t* ref : kGlyphRefs) {
            BitmapFromRef(ref, sizePx);
        }
    }

    void Clear() {
        for (auto& pair : bitmaps_) {
            if (pair.second) {
                DeleteObject(pair.second);
            }
        }
        bitmaps_.clear();
        for (auto& pair : icons_) {
            if (pair.second) {
                DestroyIcon(pair.second);
            }
        }
        icons_.clear();
    }

private:
    HBITMAP BitmapFromRef(const std::wstring& ref, int sizePx) {
        const std::wstring key = ref + L"#" + std::to_wstring(sizePx);
        auto it = bitmaps_.find(key);
        if (it != bitmaps_.end()) {
            return it->second;
        }

        HBITMAP bitmap = nullptr;
        if (ref.rfind(L"@ext:", 0) == 0) {
            const std::wstring spec = ref.substr(5);
            const bool isFolder = _wcsicmp(spec.c_str(), L"folder") == 0;
            SHFILEINFOW info = {};
            if (SHGetFileInfoW(isFolder ? L"folder" : spec.c_str(),
                               isFolder ? FILE_ATTRIBUTE_DIRECTORY
                                        : FILE_ATTRIBUTE_NORMAL,
                               &info, sizeof(info),
                               SHGFI_USEFILEATTRIBUTES | SHGFI_ICON | SHGFI_SMALLICON) &&
                info.hIcon) {
                bitmap = BitmapFromIcon(info.hIcon, sizePx);
                DestroyIcon(info.hIcon);
            }
        } else if (ref.rfind(L"@glyph:", 0) == 0) {
            const wchar_t codepoint =
                static_cast<wchar_t>(wcstoul(ref.c_str() + 7, nullptr, 16));
            std::vector<uint8_t> pixels;
            if (GlyphPixels(codepoint, sizePx, pixels)) {
                bitmap = OpaqueBitmapFromBgra(pixels, sizePx, sizePx, sizePx);
            }
        } else {
            HICON icon = GetIcon(ref, sizePx);
            if (icon) {
                bitmap = BitmapFromIcon(icon, sizePx);
            }
        }
        bitmaps_[key] = bitmap;
        return bitmap;
    }

    HBITMAP BitmapFromPixels(const std::vector<uint8_t>& pixels, int sizePx) {
        if (pixels.size() < 4) {
            return nullptr;
        }
        const int side =
            static_cast<int>(std::sqrt(static_cast<double>(pixels.size()) / 4.0));
        if (side <= 0 ||
            static_cast<size_t>(side) * side * 4 != pixels.size()) {
            return nullptr;
        }

        const std::wstring key = L"px#" + std::to_wstring(HashBytes(pixels)) + L"#" +
                                 std::to_wstring(sizePx);
        auto it = bitmaps_.find(key);
        if (it != bitmaps_.end()) {
            return it->second;
        }
        HBITMAP bitmap = OpaqueBitmapFromBgra(pixels, side, side, sizePx);
        bitmaps_[key] = bitmap;
        return bitmap;
    }

    HICON GetIcon(std::wstring_view ref, int sizePx) {
        const std::wstring key = std::wstring(ref) + L"#" + std::to_wstring(sizePx);
        auto it = icons_.find(key);
        if (it != icons_.end()) {
            return it->second;
        }

        HICON icon = nullptr;
        std::wstring path;
        int index = 0;
        if (ParseIconRef(ref, path, index)) {
            wchar_t expanded[MAX_PATH] = {};
            if (ExpandEnvironmentStringsW(path.c_str(), expanded,
                                          ARRAYSIZE(expanded))) {
                HICON large = nullptr;
                if (SUCCEEDED(SHDefExtractIconW(expanded, index, 0, &large, nullptr,
                                                MAKELONG(sizePx, sizePx)))) {
                    icon = large;
                }
            }
        }
        icons_[key] = icon;
        return icon;
    }

    static uint64_t HashBytes(const std::vector<uint8_t>& bytes) {
        uint64_t hash = 1469598103934665603ULL;
        for (uint8_t byte : bytes) {
            hash ^= byte;
            hash *= 1099511628211ULL;
        }
        return hash;
    }

    static void FillBitmapHeader(BITMAPV5HEADER& header, int width, int height) {
        header = {};
        header.bV5Size = sizeof(header);
        header.bV5Width = width;
        header.bV5Height = -height;  // top-down
        header.bV5Planes = 1;
        header.bV5BitCount = 32;
        header.bV5Compression = BI_BITFIELDS;
        header.bV5RedMask = 0x00FF0000;
        header.bV5GreenMask = 0x0000FF00;
        header.bV5BlueMask = 0x000000FF;
        header.bV5AlphaMask = 0xFF000000;
    }

    HBITMAP BitmapFromIcon(HICON icon, int sizePx) {
        HDC screen = GetDC(nullptr);
        HDC memory = CreateCompatibleDC(screen);
        BITMAPV5HEADER header = {};
        FillBitmapHeader(header, sizePx, sizePx);
        void* bits = nullptr;
        HBITMAP bitmap = CreateDIBSection(
            screen, reinterpret_cast<BITMAPINFO*>(&header), DIB_RGB_COLORS, &bits,
            nullptr, 0);
        if (bitmap && bits) {
            HGDIOBJ old = SelectObject(memory, bitmap);
            // Fill with the menu background first: menus draw bitmaps without
            // alpha, so the icon must be composited here.
            RECT rect = {0, 0, sizePx, sizePx};
            HBRUSH backgroundBrush = CreateSolidBrush(MenuBackgroundColor(themeOwner_));
            FillRect(memory, &rect, backgroundBrush);
            DeleteObject(backgroundBrush);
            DrawIconEx(memory, 0, 0, icon, sizePx, sizePx, 0, nullptr, DI_NORMAL);
            SelectObject(memory, old);
        }
        DeleteDC(memory);
        ReleaseDC(nullptr, screen);
        return bitmap;
    }

    // Composites captured BGRA pixels over the actual themed menu background
    // into an opaque 32bpp bitmap. Classic menus draw MIM_BITMAP bitmaps
    // without alpha blending, so transparency must be baked in here.
    HBITMAP OpaqueBitmapFromBgra(const std::vector<uint8_t>& pixels, int width,
                                 int height, int sizePx) {
        HDC screen = GetDC(nullptr);
        HDC sourceDc = CreateCompatibleDC(screen);
        HDC targetDc = CreateCompatibleDC(screen);

        BITMAPV5HEADER sourceHeader = {};
        FillBitmapHeader(sourceHeader, width, height);
        void* sourceBits = nullptr;
        HBITMAP source = CreateDIBSection(
            screen, reinterpret_cast<BITMAPINFO*>(&sourceHeader), DIB_RGB_COLORS,
            &sourceBits, nullptr, 0);
        if (source && sourceBits) {
            memcpy(sourceBits, pixels.data(), pixels.size());
        }

        BITMAPV5HEADER targetHeader = {};
        FillBitmapHeader(targetHeader, sizePx, sizePx);
        void* targetBits = nullptr;
        HBITMAP target = CreateDIBSection(
            screen, reinterpret_cast<BITMAPINFO*>(&targetHeader), DIB_RGB_COLORS,
            &targetBits, nullptr, 0);

        if (source && target && targetBits) {
            HGDIOBJ oldSource = SelectObject(sourceDc, source);
            HGDIOBJ oldTarget = SelectObject(targetDc, target);

            RECT rect = {0, 0, sizePx, sizePx};
            HBRUSH backgroundBrush = CreateSolidBrush(MenuBackgroundColor(themeOwner_));
            FillRect(targetDc, &rect, backgroundBrush);
            DeleteObject(backgroundBrush);

            if (width == sizePx && height == sizePx) {
                // Composite in software: menus draw bitmaps without alpha, so
                // transparency has to be resolved here. Zero-alpha pixels keep
                // the background; when the bitmap has no alpha channel at all,
                // black is treated as the classic color key.
                bool allZeroAlpha = true;
                for (size_t i = 3; i < pixels.size(); i += 4) {
                    if (pixels[i] != 0) {
                        allZeroAlpha = false;
                        break;
                    }
                }

                uint8_t* target = static_cast<uint8_t*>(targetBits);
                for (int i = 0; i < sizePx * sizePx; ++i) {
                    const uint8_t blue = pixels[i * 4];
                    const uint8_t green = pixels[i * 4 + 1];
                    const uint8_t red = pixels[i * 4 + 2];
                    const uint8_t alpha = pixels[i * 4 + 3];

                    if (allZeroAlpha) {
                        if (blue == 0 && green == 0 && red == 0) {
                            continue;  // color key
                        }
                        target[i * 4] = blue;
                        target[i * 4 + 1] = green;
                        target[i * 4 + 2] = red;
                        continue;
                    }
                    if (alpha == 0) {
                        continue;
                    }
                    if (alpha == 255) {
                        target[i * 4] = blue;
                        target[i * 4 + 1] = green;
                        target[i * 4 + 2] = red;
                        continue;
                    }
                    const uint32_t inverse = 255 - alpha;
                    target[i * 4] = static_cast<uint8_t>(
                        (blue * alpha + target[i * 4] * inverse) / 255);
                    target[i * 4 + 1] = static_cast<uint8_t>(
                        (green * alpha + target[i * 4 + 1] * inverse) / 255);
                    target[i * 4 + 2] = static_cast<uint8_t>(
                        (red * alpha + target[i * 4 + 2] * inverse) / 255);
                }
            } else {
                bool hasAlpha = false;
                for (size_t i = 3; i < pixels.size(); i += 4) {
                    if (pixels[i] != 0 && pixels[i] != 255) {
                        hasAlpha = true;
                        break;
                    }
                }

                if (hasAlpha) {
                    // Premultiply for GdiAlphaBlend with AC_SRC_ALPHA.
                    std::vector<uint8_t> premultiplied = pixels;
                    for (size_t i = 0; i < premultiplied.size(); i += 4) {
                        const uint32_t alpha = premultiplied[i + 3];
                        premultiplied[i] = static_cast<uint8_t>(
                            premultiplied[i] * alpha / 255);
                        premultiplied[i + 1] = static_cast<uint8_t>(
                            premultiplied[i + 1] * alpha / 255);
                        premultiplied[i + 2] = static_cast<uint8_t>(
                            premultiplied[i + 2] * alpha / 255);
                    }
                    memcpy(sourceBits, premultiplied.data(), premultiplied.size());

                    BLENDFUNCTION blend = {};
                    blend.BlendOp = AC_SRC_OVER;
                    blend.SourceConstantAlpha = 255;
                    blend.AlphaFormat = AC_SRC_ALPHA;
                    GdiAlphaBlend(targetDc, 0, 0, sizePx, sizePx, sourceDc, 0, 0,
                                  width, height, blend);
                } else {
                    SetStretchBltMode(targetDc, HALFTONE);
                    StretchBlt(targetDc, 0, 0, sizePx, sizePx, sourceDc, 0, 0, width,
                               height, SRCCOPY);
                }
            }

            SelectObject(targetDc, oldTarget);
            SelectObject(sourceDc, oldSource);
        } else if (target) {
            DeleteObject(target);
            target = nullptr;
        }

        if (source) {
            DeleteObject(source);
        }
        DeleteDC(sourceDc);
        DeleteDC(targetDc);
        ReleaseDC(nullptr, screen);
        return target;
    }

    static bool FontExists(const wchar_t* faceName) {
        HDC dc = GetDC(nullptr);
        LOGFONTW logFont = {};
        logFont.lfCharSet = DEFAULT_CHARSET;
        wcsncpy(logFont.lfFaceName, faceName, LF_FACESIZE - 1);
        bool found = false;
        EnumFontFamiliesExW(
            dc, &logFont,
            [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM lParam) -> int {
                *reinterpret_cast<bool*>(lParam) = true;
                return 0;
            },
            reinterpret_cast<LPARAM>(&found), 0);
        ReleaseDC(nullptr, dc);
        return found;
    }

    // Renders an icon-font glyph into straight BGRA pixels (alpha =
    // coverage) in the menu text color. The caller composites it over the
    // menu background.
    static bool GlyphPixels(wchar_t codepoint, int sizePx,
                            std::vector<uint8_t>& out) {
        static const wchar_t* kFonts[] = {L"Segoe Fluent Icons",
                                          L"Segoe MDL2 Assets"};
        for (const wchar_t* font : kFonts) {
            if (!FontExists(font)) {
                continue;
            }

            HDC screen = GetDC(nullptr);
            HDC memory = CreateCompatibleDC(screen);
            BITMAPV5HEADER header = {};
            FillBitmapHeader(header, sizePx, sizePx);
            void* bits = nullptr;
            HBITMAP bitmap = CreateDIBSection(
                screen, reinterpret_cast<BITMAPINFO*>(&header), DIB_RGB_COLORS,
                &bits, nullptr, 0);
            if (!bitmap || !bits) {
                if (bitmap) {
                    DeleteObject(bitmap);
                }
                DeleteDC(memory);
                ReleaseDC(nullptr, screen);
                continue;
            }

            memset(bits, 0, static_cast<size_t>(sizePx) * sizePx * 4);
            HGDIOBJ oldBitmap = SelectObject(memory, bitmap);
            HFONT fontHandle = CreateFontW(
                -sizePx, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_DONTCARE, font);
            HGDIOBJ oldFont = SelectObject(memory, fontHandle);
            SetBkMode(memory, TRANSPARENT);
            SetTextColor(memory, RGB(255, 255, 255));
            RECT rect = {0, 0, sizePx, sizePx};
            DrawTextW(memory, &codepoint, 1, &rect,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            SelectObject(memory, oldFont);
            DeleteObject(fontHandle);
            SelectObject(memory, oldBitmap);
            DeleteDC(memory);
            ReleaseDC(nullptr, screen);

            const bool darkTheme = IsDarkThemeActive();
            const COLORREF textColor = GetSysColor(COLOR_MENUTEXT);
            const uint8_t red = darkTheme ? 255 : GetRValue(textColor);
            const uint8_t green = darkTheme ? 255 : GetGValue(textColor);
            const uint8_t blue = darkTheme ? 255 : GetBValue(textColor);

            const uint8_t* pixels = static_cast<const uint8_t*>(bits);
            out.assign(static_cast<size_t>(sizePx) * sizePx * 4, 0);
            bool anyCoverage = false;
            for (int i = 0; i < sizePx * sizePx; ++i) {
                uint8_t coverage = pixels[i * 4];
                if (pixels[i * 4 + 1] > coverage) {
                    coverage = pixels[i * 4 + 1];
                }
                if (pixels[i * 4 + 2] > coverage) {
                    coverage = pixels[i * 4 + 2];
                }
                if (coverage > 0) {
                    anyCoverage = true;
                }
                out[i * 4] = blue;
                out[i * 4 + 1] = green;
                out[i * 4 + 2] = red;
                out[i * 4 + 3] = coverage;
            }
            DeleteObject(bitmap);
            if (anyCoverage) {
                return true;
            }
        }
        return false;
    }

    HWND themeOwner_ = nullptr;
    std::unordered_map<std::wstring, HBITMAP> bitmaps_;
    std::unordered_map<std::wstring, HICON> icons_;
};

inline IconCache g_iconCache;

HBITMAP GetIconBitmapForMenu(const std::wstring& iconRef,
                             const std::vector<uint8_t>& iconPixels, int sizePx) {
    return g_iconCache.GetBitmapFor(iconRef, iconPixels, sizePx);
}

class NativeMenuView {
public:
    static std::optional<uint32_t> Show(const MenuModel& model, HWND owner, POINT pt,
                                        bool* creationFailed = nullptr) {
        if (creationFailed) {
            *creationFailed = false;
        }

        HMENU menu = CreatePopupMenu();
        if (!menu) {
            if (creationFailed) {
                *creationFailed = true;
            }
            return std::nullopt;
        }
        const int iconSize = GetSystemMetrics(SM_CXSMICON);
        g_iconCache.SetThemeOwner(owner);
        AppendItems(menu, model.items, iconSize);

        if (!TrackPopupMenuEx_Original) {
            if (creationFailed) {
                *creationFailed = true;
            }
            DestroyMenu(menu);
            return std::nullopt;
        }

        const UINT flags = TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN;
        MenuAnimationSuppressor animationSuppressor;
        MenuDelaySuppressor delaySuppressor;
        int command = TrackPopupMenuEx_Original(menu, flags, pt.x, pt.y, owner, nullptr);
        DestroyMenu(menu);

        if (command == 0) {
            return std::nullopt;
        }
        return static_cast<uint32_t>(command);
    }

private:
    static void AppendItems(HMENU menu, const std::vector<MenuItem>& items,
                            int iconSize) {
        int index = 0;
        for (const MenuItem& item : items) {
            if (item.kind == ItemKind::Separator) {
                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                ++index;
                continue;
            }

            UINT flags = MF_STRING;
            if (item.flags & kModelDisabled) {
                flags |= MF_GRAYED;
            }
            if (item.flags & kModelChecked) {
                flags |= MF_CHECKED;
            }
            if (item.flags & kModelDefault) {
                flags |= MF_DEFAULT;
            }

            if (item.kind == ItemKind::Submenu) {
                HMENU submenu = CreatePopupMenu();
                if (!submenu) {
                    ++index;
                    continue;
                }
                AppendItems(submenu, item.children, iconSize);
                AppendMenuW(menu, flags | MF_POPUP,
                            reinterpret_cast<UINT_PTR>(submenu), item.label.c_str());
            } else {
                AppendMenuW(menu, flags, item.id, item.label.c_str());
            }

            // Checked items keep the checkmark gutter; everything else may
            // carry a cached icon bitmap.
            if (!(item.flags & kModelChecked)) {
                HBITMAP bitmap = g_iconCache.GetBitmap(item, iconSize);
                if (bitmap) {
                    MENUITEMINFOW bitmapInfo = {};
                    bitmapInfo.cbSize = sizeof(bitmapInfo);
                    bitmapInfo.fMask = MIIM_BITMAP;
                    bitmapInfo.hbmpItem = bitmap;
                    SetMenuItemInfoW(menu, static_cast<UINT>(index), TRUE, &bitmapInfo);
                }
            }
            ++index;
        }
    }
};

// Subclasses the menu owner while a menu is displayed. Optionally forwards
// menu messages to the shell context object (required for dynamic submenus
// and owner-draw items on the native menu), and optionally warms the cache
// after a short delay while the menu is still open.
constexpr UINT kDiscoveryTimerId = 0xC0DE;
constexpr UINT_PTR kOwnerSubclassId = 0xC0DE;

class OwnerSubclass {
public:
    OwnerSubclass(HWND owner, PendingCapture* capture, bool forwardMenuMessages)
        : owner_(owner), capture_(capture), forward_(forwardMenuMessages) {
        if (SetWindowSubclass(owner, &OwnerSubclass::Proc, kOwnerSubclassId,
                              reinterpret_cast<DWORD_PTR>(this))) {
            subclassed_ = true;
        }
    }

    ~OwnerSubclass() {
        StopDiscoveryTimer();
        if (subclassed_) {
            RemoveWindowSubclass(owner_, &OwnerSubclass::Proc, kOwnerSubclassId);
        }
    }

    // Runs discovery after the menu has been painted and is interactive. The
    // delay must outlast menu construction (icon resolution included):
    // population blocks the UI thread, and starting it before the first paint
    // leaves the menu blank until the reopen.
    void StartDiscoveryTimer(const ContextSignature& signature) {
        discoverySignature_ = signature;
        SetTimer(owner_, kDiscoveryTimerId, 600, nullptr);
        timerSet_ = true;
    }

    void StopDiscoveryTimer() {
        if (timerSet_) {
            KillTimer(owner_, kDiscoveryTimerId);
            timerSet_ = false;
        }
    }

private:
    static LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                 UINT_PTR idSubclass, DWORD_PTR refData) {
        auto* self = reinterpret_cast<OwnerSubclass*>(refData);
        return self->Handle(hwnd, msg, wParam, lParam, idSubclass);
    }

    LRESULT Handle(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                   UINT_PTR idSubclass) {
        if (msg == WM_TIMER && wParam == kDiscoveryTimerId) {
            StopDiscoveryTimer();
            if (capture_ && !capture_->discoveryDone && !capture_->reopenRequested) {
                // Close the menu before population: it blocks the UI thread
                // and would leave visible items blank until the reopen.
                capture_->reopenRequested = true;
                EndMenu();
            }
            return 0;
        }

        if (forward_ && capture_) {
            switch (msg) {
                case WM_INITMENUPOPUP:
                case WM_DRAWITEM:
                case WM_MEASUREITEM:
                case WM_MENUCHAR: {
                    LRESULT result = 0;
                    bool handled = false;
                    if (capture_->contextMenu3) {
                        handled = SUCCEEDED(capture_->contextMenu3->HandleMenuMsg2(
                            msg, wParam, lParam, &result));
                    } else if (capture_->contextMenu2) {
                        handled = SUCCEEDED(capture_->contextMenu2->HandleMenuMsg(
                            msg, wParam, lParam));
                    }
                    if (msg == WM_MENUCHAR && handled) {
                        return result;
                    }
                    break;
                }
                default:
                    break;
            }
        }

        return DefSubclassProc(hwnd, msg, wParam, lParam);
    }

    HWND owner_ = nullptr;
    PendingCapture* capture_ = nullptr;
    bool forward_ = false;
    bool subclassed_ = false;
    bool timerSet_ = false;
    ContextSignature discoverySignature_{};
};

// Shows the retained, really-populated native menu with menu-message
// forwarding and invokes the selection through the live object. Used by the
// fallback item and the Shift bypass.
std::optional<uint32_t> ShowNativeReplay(PendingCapture& capture, HWND owner, POINT pt) {
    if (!EnsureContextPopulated(capture) || !capture.populatedMenu ||
        !TrackPopupMenuEx_Original) {
        return std::nullopt;
    }

    // Submenus are populated on WM_INITMENUPOPUP; initialize the retained menu
    // before showing it or placeholders (such as the New submenu's) would be
    // displayed and their invocation would fail.
    if (!capture.menuInitialized) {
        InitializeMenuRecursive(capture, capture.populatedMenu, 0);
        capture.menuInitialized = true;
    }
    DumpMenuTree(capture.populatedMenu, capture.idCmdFirst, 0);

    OwnerSubclass subclass(owner, &capture, /*forwardMenuMessages=*/true);

    const UINT flags = TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN;
    MenuAnimationSuppressor animationSuppressor;
    int command = TrackPopupMenuEx_Original(capture.populatedMenu, flags, pt.x, pt.y, owner,
                                            nullptr);
    if (command == 0 || !capture.obj) {
        return std::nullopt;
    }

    CMINVOKECOMMANDINFOEX info = {};
    info.cbSize = sizeof(info);
    info.fMask = CMIC_MASK_UNICODE;
    info.hwnd = owner;
    const UINT offset = static_cast<UINT>(command) - capture.idCmdFirst;
    info.lpVerb = MAKEINTRESOURCEA(offset);
    info.lpVerbW = MAKEINTRESOURCEW(offset);
    info.nShow = SW_SHOWNORMAL;
    if (FAILED(capture.obj->InvokeCommand(
            reinterpret_cast<CMINVOKECOMMANDINFO*>(&info)))) {
        Wh_Log(L"Native menu invocation failed for offset %u", offset);
    }

    return static_cast<uint32_t>(command);
}

// Open-path timing, active only with debugLogging.
class Perf {
public:
    void MarkOpenPathStart() {
        if (g_settings.debugLogging) {
            startTick_ = GetTickCount64();
        }
    }

    uint64_t OpenPathElapsedMs() {
        if (!g_settings.debugLogging || startTick_ == 0) {
            return 0;
        }
        return GetTickCount64() - startTick_;
    }

private:
    uint64_t startTick_ = 0;
};

inline Perf g_perf;

}  // namespace cmo

// ===========================================================================
// [CMO:Warmup] Background cache warm-up.
// ===========================================================================
namespace cmo {

std::vector<std::wstring> BuildWarmupTypes(
    std::span<const std::wstring> configuredExtensions) {
    std::vector<std::wstring> types;

    auto addUnique = [&](std::wstring value) {
        if (std::find(types.begin(), types.end(), value) == types.end()) {
            types.push_back(std::move(value));
        }
    };

    addUnique(L"*");
    addUnique(L"Directory");
    addUnique(L"Directory\\Background");
    addUnique(L"Desktop");
    addUnique(L"Drive");

    for (const std::wstring& raw : configuredExtensions) {
        const size_t begin = raw.find_first_not_of(L" \t");
        if (begin == std::wstring::npos) {
            continue;
        }
        const size_t end = raw.find_last_not_of(L" \t");
        std::wstring extension = raw.substr(begin, end - begin + 1);
        if (extension.empty()) {
            continue;
        }
        if (extension[0] != L'.') {
            extension.insert(extension.begin(), L'.');
        }
        CharLowerBuffW(extension.data(), static_cast<DWORD>(extension.size()));
        addUnique(std::move(extension));
    }
    return types;
}

// Creates a shell context menu object for a path on the warm-up thread.
IContextMenu* CreateContextMenuForPath(const std::wstring& path, bool background) {
    PIDLIST_ABSOLUTE absolute = nullptr;
    if (FAILED(SHParseDisplayName(path.c_str(), nullptr, &absolute, 0, nullptr)) ||
        !absolute) {
        return nullptr;
    }

    IContextMenu* menu = nullptr;
    if (background) {
        DEFCONTEXTMENU dcm = {};
        dcm.pidlFolder = absolute;
        dcm.cidl = 0;
        SHCreateDefaultContextMenu(&dcm, IID_IContextMenu, (void**)&menu);
    } else {
        IShellFolder* parent = nullptr;
        PCUITEMID_CHILD child = ILFindLastID(absolute);
        if (SUCCEEDED(SHBindToParent(absolute, IID_IShellFolder, (void**)&parent, &child)) &&
            parent) {
            parent->GetUIObjectOf(nullptr, 1, &child, IID_IContextMenu, nullptr,
                                  (void**)&menu);
            parent->Release();
        }
    }

    CoTaskMemFree(absolute);
    return menu;
}

const wchar_t* WarmupMutexName() {
    return L"Local\\ContextMenuOverhaulWarmup";
}

// Small per-process jitter so a cold start with many Explorer processes does
// not have them all populate at the same instant.
int WarmupJitterMs(uint32_t seed) {
    return static_cast<int>(seed % 3001);
}

class Warmup {
public:
    void Start() {
        if (thread_) {
            return;
        }
        stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        resumeEvent_ = CreateEventW(nullptr, TRUE, TRUE, nullptr);
        if (!stopEvent_ || !resumeEvent_) {
            if (stopEvent_) CloseHandle(stopEvent_);
            if (resumeEvent_) CloseHandle(resumeEvent_);
            stopEvent_ = nullptr;
            resumeEvent_ = nullptr;
            return;
        }

        thread_ = CreateThread(nullptr, 0, &Warmup::ThreadProc, this, 0, nullptr);
        if (thread_) {
            SetThreadPriority(thread_, THREAD_PRIORITY_BELOW_NORMAL);
        } else {
            CloseHandle(stopEvent_);
            CloseHandle(resumeEvent_);
            stopEvent_ = nullptr;
            resumeEvent_ = nullptr;
        }
    }

    void Stop() {
        if (stopEvent_) {
            SetEvent(stopEvent_);
        }
        // Wake a paused worker so it can observe the stop event.
        if (resumeEvent_) {
            SetEvent(resumeEvent_);
        }
        paused_.store(false);

        bool exited = true;
        if (thread_) {
            exited = WaitForSingleObject(thread_, 30000) == WAIT_OBJECT_0;
            if (exited) {
                CloseHandle(thread_);
                thread_ = nullptr;
            } else {
                // A handler is wedged in the worker. Leaking the handles is
                // safer than closing them under a running thread.
                Wh_Log(L"Warm-up thread did not exit; leaking its handles");
            }
        }
        if (exited) {
            if (stopEvent_) {
                CloseHandle(stopEvent_);
                stopEvent_ = nullptr;
            }
            if (resumeEvent_) {
                CloseHandle(resumeEvent_);
                resumeEvent_ = nullptr;
            }
        }
    }

    // Pauses warm-up between contexts while a menu is open.
    void SetMenuOpen(bool open) {
        paused_.store(open);
        if (resumeEvent_) {
            if (open) {
                ResetEvent(resumeEvent_);
            } else {
                SetEvent(resumeEvent_);
            }
        }
    }

    bool IsPaused() const { return paused_.load(); }

private:
    static DWORD WINAPI ThreadProc(LPVOID param) {
        static_cast<Warmup*>(param)->Run();
        return 0;
    }

    void Run() {
        // The New submenu is built from these templates; prebuild them so the
        // first right-click does not pay for the registry walk.
        EnsureNewTemplates();

        const int delaySeconds =
            g_settings.warmupDelaySeconds > 0 ? g_settings.warmupDelaySeconds : 0;
        if (WaitForSingleObject(stopEvent_, static_cast<DWORD>(delaySeconds) * 1000) ==
            WAIT_OBJECT_0) {
            LogEarlyStop();
            return;
        }

        const uint32_t seed = static_cast<uint32_t>(GetTickCount64()) ^
                              (GetCurrentProcessId() * 2654435761u);
        const int jitter = WarmupJitterMs(seed);
        if (jitter > 0 &&
            WaitForSingleObject(stopEvent_, static_cast<DWORD>(jitter)) ==
                WAIT_OBJECT_0) {
            LogEarlyStop();
            return;
        }

        std::vector<std::wstring> configured;
        for (int i = 0;; ++i) {
            PCWSTR value = Wh_GetStringSetting(L"warmupExtensions[%d]", i);
            const bool empty = !value || !value[0];
            if (!empty) {
                configured.emplace_back(value);
            }
            Wh_FreeStringSetting(value);
            if (empty) {
                break;
            }
        }
        const std::vector<std::wstring> types = BuildWarmupTypes(configured);

        wchar_t storagePath[MAX_PATH] = {};
        if (!Wh_GetModStoragePath(storagePath, ARRAYSIZE(storagePath))) {
            return;
        }
        const std::wstring warmupDir = std::wstring(storagePath) + L"\\warmup";
        CreateDirectoryW(storagePath, nullptr);
        CreateDirectoryW(warmupDir.c_str(), nullptr);

        // The shared cache may already hold everything another process warmed.
        if (g_cache.Size() == 0) {
            g_cache.Load(CacheFilePath());
        }

        HANDLE mutex = CreateMutexW(nullptr, FALSE, WarmupMutexName());
        const bool haveMutex =
            mutex && WaitForSingleObject(mutex, 10000) == WAIT_OBJECT_0;
        if (!haveMutex && g_cache.Size() == 0) {
            // Another process is warming; take whatever it has saved.
            g_cache.Load(CacheFilePath());
        }

        // Rebuild the SendTo entries off the UI thread (also after handler
        // registry invalidation).
        RebuildSendToChildren();

        const bool comInitialized =
            SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));

        int warmed = 0;
        int skipped = 0;
        bool completed = true;
        for (const std::wstring& type : types) {
            if (WaitForSingleObject(stopEvent_, 0) == WAIT_OBJECT_0) {
                completed = false;
                break;
            }
            WaitForSingleObject(resumeEvent_, INFINITE);
            if (WaitForSingleObject(stopEvent_, 0) == WAIT_OBJECT_0) {
                completed = false;
                break;
            }
            if (WarmOneType(type, warmupDir)) {
                ++warmed;
            } else {
                ++skipped;
            }
            Sleep(50);
        }

        if (comInitialized) {
            CoUninitialize();
        }
        if (completed && warmed > 0) {
            g_cache.Save(CacheFilePath());
        }
        if (g_settings.debugLogging) {
            if (completed) {
                Wh_Log(L"Warm-up finished (%d warmed, %d already cached)", warmed,
                       skipped);
            } else {
                Wh_Log(L"Warm-up stopped early (%d warmed)", warmed);
            }
        }
        if (haveMutex) {
            ReleaseMutex(mutex);
        }
        if (mutex) {
            CloseHandle(mutex);
        }
    }

    static void LogEarlyStop() {
        if (g_settings.debugLogging) {
            Wh_Log(L"Warm-up stopped early (0 warmed)");
        }
    }

    bool WarmOneType(const std::wstring& type, const std::wstring& warmupDir) {
        if (type == L"*") {
            const std::wstring path = warmupDir + L"\\warmup";
            EnsureScratchFile(path);
            return WarmPathIfMissing(
                path, false,
                ContextSignature{Scope::Files, L"*", Shape::Single, Variant::Normal});
        }
        if (type == L"Directory") {
            const std::wstring path = warmupDir + L"\\warmup-folder";
            CreateDirectoryW(path.c_str(), nullptr);
            return WarmPathIfMissing(
                path, false,
                ContextSignature{Scope::Folders, L"*", Shape::Single, Variant::Normal});
        }
        if (type == L"Directory\\Background") {
            const std::wstring path = warmupDir + L"\\warmup-folder";
            CreateDirectoryW(path.c_str(), nullptr);
            return WarmPathIfMissing(
                path, true, ContextSignature{Scope::Background, L"*", Shape::Single,
                                             Variant::Normal});
        }
        if (type == L"Drive") {
            wchar_t windowsDir[MAX_PATH] = {};
            if (GetWindowsDirectoryW(windowsDir, ARRAYSIZE(windowsDir))) {
                const std::wstring drive(windowsDir, 3);  // "C:\"
                return WarmPathIfMissing(
                    drive, false,
                    ContextSignature{Scope::Drive, L"*", Shape::Single,
                                     Variant::Normal});
            }
            return false;
        }
        if (type == L"Desktop") {
            wchar_t desktopPath[MAX_PATH] = {};
            if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_DESKTOP, nullptr, 0,
                                           desktopPath))) {
                return WarmPathIfMissing(
                    desktopPath, false,
                    ContextSignature{Scope::Desktop, L"*", Shape::Single,
                                     Variant::Normal});
            }
            return false;
        }
        const std::wstring path = warmupDir + L"\\warmup" + type;
        EnsureScratchFile(path);
        return WarmPathIfMissing(
            path, false,
            ContextSignature{Scope::Files, type, Shape::Single, Variant::Normal});
    }

    bool WarmPathIfMissing(const std::wstring& path, bool background,
                           const ContextSignature& signature) {
        if (g_cache.Has(signature)) {
            return false;
        }
        WarmPath(path, background, signature);
        return true;
    }

    static void EnsureScratchFile(const std::wstring& path) {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            CloseHandle(file);
        }
    }

    void WarmPath(const std::wstring& path, bool background,
                  const ContextSignature& signature) {
        IContextMenu* menu = CreateContextMenuForPath(path, background);
        if (!menu) {
            if (g_settings.debugLogging) {
                Wh_Log(L"Warm-up: no context menu for %s", path.c_str());
            }
            return;
        }

        const std::vector<std::wstring> modulesBefore = SnapshotLoadedModules();
        HMENU offscreen = CreatePopupMenu();
        if (offscreen) {
            ClearRecordedItemBitmaps();
            ReplayInto(menu, offscreen, 0, 1, 0x7FFF, CMF_NORMAL);
            MenuModel model = BuildModelFromHMenu(offscreen, 1, signature, menu);
            ClearRecordedItemBitmaps();
            DestroyMenu(offscreen);
            if (!model.items.empty()) {
                ApplyRegistryIcons(model.items, signature);
                model.handlerModules =
                    DiffModules(modulesBefore, SnapshotLoadedModules());
                model.sourceStamp = ComputeModuleStamp(model.handlerModules);
                model.flags |= kModelWarmup;
                g_cache.Put(std::move(model));
            }
        }
        menu->Release();
    }

    HANDLE thread_ = nullptr;
    HANDLE stopEvent_ = nullptr;
    HANDLE resumeEvent_ = nullptr;
    std::atomic<bool> paused_{false};
};

inline Warmup g_warmup;

}  // namespace cmo

// ===========================================================================
// [CMO:Invalidation] Cache invalidation on handler registration changes.
// ===========================================================================
namespace cmo {

struct SourceStamp {
    uint64_t registryStamp = 0;
    uint64_t dllStamp = 0;
};

bool StampMatches(const SourceStamp& cached, const SourceStamp& current) {
    return cached.registryStamp == current.registryStamp &&
           cached.dllStamp == current.dllStamp;
}

class Invalidation {
public:
    void Start(HWND notifyWnd = nullptr) {
        (void)notifyWnd;
        if (thread_) {
            return;
        }
        stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        checkEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!stopEvent_ || !checkEvent_) {
            if (stopEvent_) {
                CloseHandle(stopEvent_);
            }
            if (checkEvent_) {
                CloseHandle(checkEvent_);
            }
            stopEvent_ = nullptr;
            checkEvent_ = nullptr;
            return;
        }
        thread_ = CreateThread(nullptr, 0, &Invalidation::ThreadProc, this, 0, nullptr);
        if (!thread_) {
            CloseHandle(stopEvent_);
            CloseHandle(checkEvent_);
            stopEvent_ = nullptr;
            checkEvent_ = nullptr;
        }
    }

    // Asks for a handler check. Called when a menu is opened: the registry is
    // only inspected while menus are actually being used, not on a timer.
    // Cheap (SetEvent) and safe from any thread.
    void RequestCheck() {
        if (checkEvent_) {
            SetEvent(checkEvent_);
        }
    }

    void Stop() {
        if (stopEvent_) {
            SetEvent(stopEvent_);
        }
        if (thread_) {
            WaitForSingleObject(thread_, 5000);
            CloseHandle(thread_);
            thread_ = nullptr;
        }
        if (stopEvent_) {
            CloseHandle(stopEvent_);
            stopEvent_ = nullptr;
        }
        if (checkEvent_) {
            CloseHandle(checkEvent_);
            checkEvent_ = nullptr;
        }
    }

    uint64_t Generation() const { return generation_.load(); }

private:
    static DWORD WINAPI ThreadProc(LPVOID param) {
        static_cast<Invalidation*>(param)->Run();
        return 0;
    }

    // Fingerprints the watched handler keys using their last-write times and
    // subkey stamps. Cheap registry reads; unlike change notifications, this
    // cannot storm when unrelated shell activity touches these keys.
    static uint64_t ComputeRegistryFingerprint() {
        static const wchar_t* kKeys[] = {
            L"*\\shellex\\ContextMenuHandlers",
            L"AllFilesystemObjects\\shellex\\ContextMenuHandlers",
            L"Directory\\shellex\\ContextMenuHandlers",
            L"Directory\\Background\\shellex\\ContextMenuHandlers",
            L"Folder\\shellex\\ContextMenuHandlers",
            L"Drive\\shellex\\ContextMenuHandlers",
            L"DesktopBackground\\shellex\\ContextMenuHandlers",
        };
        constexpr DWORD kKeyCount = ARRAYSIZE(kKeys);

        uint64_t fingerprint = 1469598103934665603ULL;
        for (DWORD i = 0; i < kKeyCount; ++i) {
            fingerprint = HashCombine(fingerprint, HashString(kKeys[i]));

            HKEY key = nullptr;
            if (RegOpenKeyExW(HKEY_CLASSES_ROOT, kKeys[i], 0, KEY_READ, &key) !=
                ERROR_SUCCESS) {
                fingerprint = HashCombine(fingerprint, 0);
                continue;
            }

            DWORD subKeys = 0;
            DWORD values = 0;
            FILETIME lastWrite = {};
            RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, &subKeys, nullptr, nullptr,
                             &values, nullptr, nullptr, nullptr, &lastWrite);
            fingerprint = HashCombine(fingerprint, subKeys);
            fingerprint = HashCombine(fingerprint, values);
            fingerprint = HashCombine(
                fingerprint,
                (static_cast<uint64_t>(lastWrite.dwHighDateTime) << 32) |
                    lastWrite.dwLowDateTime);

            // A change inside a handler's own subkey does not update the
            // parent's last-write time; stamp each subkey as well.
            for (DWORD j = 0; j < subKeys; ++j) {
                wchar_t name[256] = {};
                DWORD nameLength = ARRAYSIZE(name);
                FILETIME subLastWrite = {};
                if (RegEnumKeyExW(key, j, name, &nameLength, nullptr, nullptr, nullptr,
                                  &subLastWrite) != ERROR_SUCCESS) {
                    continue;
                }
                fingerprint = HashCombine(fingerprint, HashString(name));
                fingerprint = HashCombine(
                    fingerprint,
                    (static_cast<uint64_t>(subLastWrite.dwHighDateTime) << 32) |
                        subLastWrite.dwLowDateTime);
            }
            RegCloseKey(key);
        }
        return fingerprint;
    }

    void InvalidateNow(const wchar_t* reason) {
        ++generation_;
        g_cache.Clear();
        InvalidateSendToChildren();
        // The warm-up thread rebuilds the SendTo entries and pre-built models.
        g_warmup.Stop();
        g_warmup.Start();
        Wh_Log(L"%s; cache invalidated", reason);
    }

    void Run() {
        uint64_t registryFingerprint = ComputeRegistryFingerprint();
        ULONGLONG lastCheck = GetTickCount64();

        for (;;) {
            HANDLE events[] = {stopEvent_, checkEvent_};
            const DWORD wait = WaitForMultipleObjects(2, events, FALSE, INFINITE);
            if (wait == WAIT_OBJECT_0) {
                break;
            }
            if (wait != WAIT_OBJECT_0 + 1) {
                continue;
            }

            // Menus can open in bursts; never fingerprint more than once per
            // debounce window.
            const ULONGLONG now = GetTickCount64();
            if (now - lastCheck < 5000) {
                continue;
            }
            lastCheck = now;

            const uint64_t current = ComputeRegistryFingerprint();
            if (current != registryFingerprint) {
                registryFingerprint = current;
                InvalidateNow(L"Context menu handlers changed");
            }

            if (now >= nextRevalidationTick_) {
                nextRevalidationTick_ = now + 3600000;
                if (g_cache.RevalidateStamps()) {
                    Wh_Log(L"Handler modules changed; cache invalidated");
                    g_warmup.Stop();
                    g_warmup.Start();
                }
            }

            g_cache.MaybeSave(CacheFilePath());
        }
    }

    HANDLE thread_ = nullptr;
    HANDLE stopEvent_ = nullptr;
    HANDLE checkEvent_ = nullptr;
    std::atomic<uint64_t> generation_{0};
    uint64_t nextRevalidationTick_ = 0;
};

inline Invalidation g_invalidation;

}  // namespace cmo

// ===========================================================================
// [CMO:Hooks] Hook functions and interception state.
// ===========================================================================
namespace cmo {

enum class MenuPath : uint8_t { Ours, NativeBypass, Passthrough };

// Decides which menu a popup owner gets. Shift bypasses the replacement and
// shows the untouched native menu (stock behavior, extended verbs included).
MenuPath DecidePath(bool shiftHeld, ShellViewKind kind, bool hasPendingCapture,
                    bool enableShiftBypass) {
    if (!hasPendingCapture || !IsReplaceableKind(kind)) {
        return MenuPath::Passthrough;
    }
    if (shiftHeld && enableShiftBypass) {
        return MenuPath::NativeBypass;
    }
    return MenuPath::Ours;
}

std::wstring DirectoryOfPath(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return L"";
    }
    return path.substr(0, slash);
}

std::wstring SelectionDirectory(const std::vector<std::wstring>& paths,
                                Scope scope) {
    if (paths.empty()) {
        return L"";
    }
    if (scope == Scope::Folders || scope == Scope::Drive) {
        return paths.front();
    }
    return DirectoryOfPath(paths.front());
}

bool ShowReplacementMenu(PendingCapture& capture, ShellViewKind kind, HWND owner, POINT pt) {
    g_perf.MarkOpenPathStart();
    // Handler changes are checked while menus are used, not on a timer.
    g_invalidation.RequestCheck();

    SelectionInfo info = GetSelection(owner, kind);
    std::vector<std::wstring>& paths = info.paths;
    Shape shape = paths.size() > 1 ? Shape::Multi : Shape::Single;

    // The navigation pane has no resolvable selection path; its model comes
    // from the captured shell menu instead (see the v2.1 spec, section 6.4).
    const bool capturedMenuContext = kind == ShellViewKind::NavPane;

    if (!capturedMenuContext &&
        !IsFilesystemContext(info.folderIsFilesystem, info.allItemsAreFilesystem)) {
        Wh_Log(L"Non-filesystem namespace: using the native menu");
        ShowNativeReplay(capture, owner, pt);
        g_warmup.SetMenuOpen(false);
        return true;
    }

    Scope scope = capturedMenuContext
                      ? Scope::NavPane
                      : RefineScope(ScopeFromKind(kind, paths.empty()),
                                    info.allItemsAreFolders, AllPathsAreDrives(paths));
    const std::wstring typeKey =
        scope == Scope::Files ? MakeTypeKey(paths) : std::wstring(L"*");
    ContextSignature signature{scope, typeKey, shape, Variant::Normal};

    const DWORD clipboardSequence = GetClipboardSequenceNumber();
    const bool clipboardHadData = ClipboardHasFileData();

    g_warmup.SetMenuOpen(true);

    while (true) {
        std::optional<MenuModel> cached;
        bool needsDiscovery = false;
        MenuModel model;
        if (capturedMenuContext) {
            HMENU capturedMenu = CreatePopupMenu();
            if (capturedMenu) {
                ReplayInto(capture.obj, capturedMenu, capture.indexMenu,
                           capture.idCmdFirst, capture.idCmdLast, capture.flags);
                model = BuildModelFromHMenu(capturedMenu, capture.idCmdFirst,
                                            signature, capture.obj);
                DestroyMenu(capturedMenu);
            }
        } else {
            cached = g_cache.Find(signature);
            needsDiscovery = !cached || (cached->flags & kModelWarmup);
            Wh_Log(L"Cache %s: scope=%d key=%s shape=%d paths=%zu",
                   cached ? (needsDiscovery ? L"warm" : L"hit") : L"miss",
                   static_cast<int>(scope), typeKey.c_str(),
                   static_cast<int>(shape), paths.size());

            // A true cache miss has no menu to show. Populate first: the shell's
            // population blocks the UI thread, so a placeholder menu would sit
            // blank until the real menu replaced it. The native menu pays the
            // same first-open cost. Warm entries are shown provisionally and
            // refreshed after the menu closes.
            if (!cached && !capture.discoveryDone) {
                Wh_Log(L"Populating before showing the menu");
                DiscoverIntoCache(capture, signature);
                cached = g_cache.Find(signature);
                needsDiscovery = !cached || (cached->flags & kModelWarmup);
            }

            model = cached
                        ? MergeCoreWithCached(BuildCoreModel(scope, paths, shape),
                                              *cached)
                        : BuildCoreModel(scope, paths, shape);
        }
        if (!g_settings.showMoreOptionsItem) {
            std::erase_if(model.items, [](const MenuItem& item) {
                return item.action == ActionKind::Fallback;
            });
        }

        g_configStore.EnsureLoaded();
        g_configStore.RefreshIfChanged();
        std::shared_ptr<const RulesConfig> rules = g_configStore.Snapshot();
        bool hasMoveRules = false;
        if (rules) {
            ItemContext itemCtx{};
            itemCtx.scope = scope;
            itemCtx.shape = shape;
            itemCtx.paths = paths;
            hasMoveRules =
                ApplyRulesConfigToModel(model, *rules, itemCtx).hasMoveRules;
        }

        DumpSuspiciousItems(model.items, 0);
        PruneMenuItems(model.items);

        if (ShouldShowNativeReplay(model.flags)) {
            Wh_Log(L"Owner-draw context: using the native menu");
            ShowNativeReplay(capture, owner, pt);
            break;
        }

        for (MenuItem& item : model.items) {
            if (item.canonicalVerb == L"paste" && !clipboardHadData) {
                item.flags |= kModelDisabled;
            }
            if (item.kind == ItemKind::Submenu && item.label == L"Send to" &&
                item.children.empty()) {
                item.children = GetSendToChildren();
            }
        }

        if (!hasMoveRules) {
            ReorganizeAdvancedItems(model.items);
        }

        if (scope == Scope::Background || scope == Scope::Desktop) {
            ApplyViewStateChecks(model.items, owner, kind);
        }
        Wh_Log(L"Menu prep: %llu ms",
               static_cast<unsigned long long>(g_perf.OpenPathElapsedMs()));

        bool creationFailed = false;
        std::optional<uint32_t> chosen;
        bool customShown = false;
        const MenuMode mode = ResolveMenuMode(
            g_settings.menuMode, g_modeController.ConsecutiveFailures());
        if (mode == MenuMode::Custom) {
            if (g_menuSession != nullptr) {
                // A session already owns the mouse; never clobber it.
                if (g_settings.debugLogging) {
                    Wh_Log(L"Refusing a second custom menu session");
                }
                g_modeController.RecordSuccess();
                customShown = true;
            } else {
                const RulesConfig emptyConfig;
                const RulesConfig& effectiveRules = rules ? *rules : emptyConfig;
                const LayoutKey layoutKey =
                    MakeLayoutKey(signature, effectiveRules, DpiForWindow(owner),
                                  IsDarkThemeActive(), model);

                const CustomMenuResult custom =
                    ShowCustomMenu(model, layoutKey, owner, pt);
                if (custom.failed) {
                    Wh_Log(L"Custom menu failed; using the HMENU path");
                    g_modeController.RecordFailure();
                } else {
                    g_modeController.RecordSuccess();
                    customShown = true;
                    chosen = custom.chosenItemId;
                }
            }
        }
        if (!customShown) {
            chosen = NativeMenuView::Show(model, owner, pt, &creationFailed);
        }

        if (creationFailed) {
            Wh_Log(L"Menu creation failed; using the native menu");
            ShowNativeReplay(capture, owner, pt);
            if (needsDiscovery && !capture.discoveryDone) {
                DiscoverIntoCache(capture, signature);
            }
            break;
        }

        if (chosen) {
            const MenuItem* item = FindById(model, *chosen);
            if (item) {
                InvocationContext ctx{};
                ctx.owner = owner;
                ctx.pt = pt;
                ctx.paths = paths;
                ctx.liveContext = capture.obj;
                ctx.idCmdFirst = capture.idCmdFirst;
                ctx.kind = kind;
                ctx.clipboardSequence = clipboardSequence;
                ctx.clipboardHadData = clipboardHadData;
                ctx.config = rules;
                ctx.directory = SelectionDirectory(paths, scope);
                if (ctx.directory.empty()) {
                    ctx.directory = GetCurrentFolderPath(owner, kind);
                }

                InvokeResult result = InvokeResult::Failed;
                if (item->flags & kModelExtension) {
                    result = (model.flags & kModelOwnerDraw)
                                 ? InvokeResult::FallbackNative
                                 : InvokeExtensionItem(*item, ctx, capture);
                } else if (item->action == ActionKind::ViewAction) {
                    // View actions act on the view's current selection; never
                    // act on a different selection than the one captured.
                    SelectionInfo current = GetSelection(owner, kind);
                    result = PathSetsEqual(current.paths, paths)
                                 ? InvokeItem(*item, ctx, capture)
                                 : InvokeResult::FallbackNative;
                } else {
                    result = InvokeItem(*item, ctx, capture);
                }

                Wh_Log(L"Invoke '%s' -> %d", item->label.c_str(),
                       static_cast<int>(result));

                if (result == InvokeResult::FallbackNative) {
                    ShowNativeReplay(capture, owner, pt);
                }
            }
            break;
        }

        // Warm-up entries are provisional: refresh them from the real context
        // now that the menu has closed. Live entries never populate again.
        if (needsDiscovery && !capture.discoveryDone) {
            DiscoverIntoCache(capture, signature);
        }
        break;
    }

    g_warmup.SetMenuOpen(false);
    return true;
}

BOOL WINAPI TrackPopupMenuEx_Hook(HMENU hMenu, UINT uFlags, int x, int y, HWND hWnd,
                                  LPTPMPARAMS lptpm) {
    ShellViewKind kind = ClassifyOwner(hWnd);
    g_pending.ExpireOlderThan(GetTickCount64(), 60000);
    PendingCapture pending{};
    const bool hasPending = g_pending.Take(pending);
    const bool shiftHeld = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    const MenuPath path =
        DecidePath(shiftHeld, kind, hasPending, g_settings.enableShiftBypass);

    if (path == MenuPath::Ours && hasPending) {
        Wh_Log(L"Replacing context menu: kind=%d", static_cast<int>(kind));
        pending.owner = hWnd;
        ShowReplacementMenu(pending, kind, hWnd, POINT{x, y});
        ReleaseCapture(pending);
        return 0;
    }

    if (path == MenuPath::NativeBypass && hasPending) {
        Wh_Log(L"Shift bypass: showing the native menu");
        ShowNativeReplay(pending, hWnd, POINT{x, y});
        ReleaseCapture(pending);
        return 0;
    }

    if (hasPending) {
        Wh_Log(L"Passing through: kind=%d", static_cast<int>(kind));
        ReplayInto(pending.obj, hMenu, pending.indexMenu, pending.idCmdFirst,
                   pending.idCmdLast, pending.flags);
        ReleaseCapture(pending);
    }
    return TrackPopupMenuEx_Original(hMenu, uFlags, x, y, hWnd, lptpm);
}

BOOL WINAPI TrackPopupMenu_Hook(HMENU hMenu, UINT uFlags, int x, int y, int nReserved,
                                HWND hWnd, const RECT* prcRect) {
    ShellViewKind kind = ClassifyOwner(hWnd);
    g_pending.ExpireOlderThan(GetTickCount64(), 60000);
    PendingCapture pending{};
    const bool hasPending = g_pending.Take(pending);
    const bool shiftHeld = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    const MenuPath path =
        DecidePath(shiftHeld, kind, hasPending, g_settings.enableShiftBypass);

    if (path == MenuPath::Ours && hasPending) {
        Wh_Log(L"Replacing context menu: kind=%d", static_cast<int>(kind));
        pending.owner = hWnd;
        ShowReplacementMenu(pending, kind, hWnd, POINT{x, y});
        ReleaseCapture(pending);
        return 0;
    }

    if (path == MenuPath::NativeBypass && hasPending) {
        Wh_Log(L"Shift bypass: showing the native menu");
        ShowNativeReplay(pending, hWnd, POINT{x, y});
        ReleaseCapture(pending);
        return 0;
    }

    if (hasPending) {
        Wh_Log(L"Passing through: kind=%d", static_cast<int>(kind));
        ReplayInto(pending.obj, hMenu, pending.indexMenu, pending.idCmdFirst,
                   pending.idCmdLast, pending.flags);
        ReleaseCapture(pending);
    }
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
        // Suppressed unconditionally so Shift+right-click reaches our bypass
        // and shows the classic native menu, matching stock Windows 11.
        Wh_Log(L"Blocking modern context menu presenter");
        if (ppvOut) {
            *ppvOut = nullptr;
        }
        return E_FAIL;
    }
    return IUnknown_QueryService_Original(punk, guidService, riid, ppvOut);
}

using ShouldShowMiniMenu_t = bool(WINAPI*)(void*, void*);
inline ShouldShowMiniMenu_t ShouldShowMiniMenu_Original = nullptr;

bool WINAPI ShouldShowMiniMenu_Hook(void* pThis, void* param) {
    (void)pThis;
    (void)param;
    // Suppressed unconditionally so the classic path (and our replacement)
    // always handles desktop menus; Shift bypass is handled by the popup hooks.
    Wh_Log(L"Blocking modern desktop mini menu");
    return false;
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
    cmo::g_uiThreadId = GetCurrentThreadId();

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

    // Record bitmaps extensions attach with SetMenuItemBitmaps; the API has
    // no getter, and this is how the native menu gets their icons.
    if (!Wh_SetFunctionHook((void*)SetMenuItemBitmaps,
                            (void*)cmo::SetMenuItemBitmaps_Hook,
                            (void**)&cmo::SetMenuItemBitmaps_Original)) {
        Wh_Log(L"Failed to hook SetMenuItemBitmaps");
    }

    cmo::LoadSettings();
    cmo::g_iconCache.PreloadCoreIcons(GetSystemMetrics(SM_CXSMICON));

    const std::wstring cachePath = cmo::CacheFilePath();
    if (!cachePath.empty()) {
        if (cmo::g_settings.clearCache) {
            DeleteFileW(cachePath.c_str());
        } else if (cmo::g_cache.Load(cachePath)) {
            Wh_Log(L"Loaded menu cache");
        } else {
            // Corrupt or unreadable cache: remove it and rebuild.
            DeleteFileW(cachePath.c_str());
        }
    }
    cmo::g_invalidation.Start();
    cmo::g_warmup.Start();
    cmo::RebuildSendToChildren();

    if (cmo::InstallPopulationHook()) {
        Wh_Log(L"Population hook installed");
        // Only hide the modern menu once the replacement can actually run.
        cmo::InstallWin11Suppression();
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
        cmo::InstallWin11Suppression();
        Wh_Log(L"Population hook installed after init");
    }
}

void Wh_ModUninit() {
    Wh_Log(L"Context Menu Overhaul uninit");
    cmo::g_menuWindowPool.DestroyAll();
    cmo::g_contentCaches.Clear();
    cmo::g_layoutCache.InvalidateAll();
    cmo::g_renderDevice.Shutdown();
    cmo::RestoreMenuAnimation();
    cmo::RestoreMenuDelay();
    cmo::g_warmup.Stop();
    cmo::g_invalidation.Stop();
    cmo::g_iconCache.Clear();

    const std::wstring cachePath = cmo::CacheFilePath();
    if (!cachePath.empty()) {
        cmo::g_cache.Save(cachePath);
    }
}

void Wh_ModSettingsChanged() {
    Wh_Log(L"Context Menu Overhaul settings changed");
    cmo::LoadSettings();

    if (cmo::g_settings.clearCache) {
        cmo::g_cache.Clear();
        const std::wstring cachePath = cmo::CacheFilePath();
        if (!cachePath.empty()) {
            DeleteFileW(cachePath.c_str());
        }
    }
}
