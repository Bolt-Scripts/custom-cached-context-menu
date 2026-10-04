// ==WindhawkMod==
// @id              context-menu-overhaul
// @name            Context Menu Overhaul
// @description     Replaces the Explorer context menu with an instantly-opening cached menu, then discovers and caches shell extension items asynchronously.
// @version         0.3.25
// @include         explorer.exe
// @architecture    x86-64
// @compilerOptions -lole32 -lshlwapi -luuid -lcomctl32 -ladvapi32 -lgdi32 -luxtheme -lversion
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

enum class ItemKind : uint8_t { Command, Submenu, Separator };
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
};

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
enum class SubmenuPositionKind : uint8_t { Top, Bottom, After, Before };

struct CustomCommand {
    std::wstring label;
    std::wstring command;
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

struct RulesConfig {
    Appearance appearance;
    Appearance lightAppearance;
    Appearance darkAppearance;
    bool hasLightAppearance = false;
    bool hasDarkAppearance = false;
    std::vector<Rule> rules;
    std::vector<CustomCommand> commands;
    std::vector<CustomSubmenu> submenus;
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

// Applies one appearance key/value; false means invalid key or value.
bool ApplyAppearanceValue(Appearance& appearance, const std::wstring& key,
                          const std::wstring& value) {
    if (key == L"background") return ParseColor(value, appearance.background);
    if (key == L"blur") return ParseBool(value, appearance.blur);
    if (key == L"blurstrength") {
        return ParseIntValue(value, appearance.blurStrength) && appearance.blurStrength >= 0;
    }
    if (key == L"cornerradius") {
        return ParseIntValue(value, appearance.cornerRadius) && appearance.cornerRadius >= 0;
    }
    if (key == L"border") return ParseColor(value, appearance.border);
    if (key == L"borderwidth") {
        return ParseIntValue(value, appearance.borderWidth) && appearance.borderWidth >= 0;
    }
    if (key == L"shadow") return ParseBool(value, appearance.shadow);
    if (key == L"shadowsize") {
        return ParseIntValue(value, appearance.shadowSize) && appearance.shadowSize >= 0;
    }
    if (key == L"font") return ParseFont(value, appearance.fontFace, appearance.fontSize);
    if (key == L"itemheight") {
        return ParseIntValue(value, appearance.itemHeight) && appearance.itemHeight > 0;
    }
    if (key == L"iconsize") {
        return ParseIntValue(value, appearance.iconSize) && appearance.iconSize > 0;
    }
    if (key == L"padding") {
        return ParseIntValue(value, appearance.padding) && appearance.padding >= 0;
    }
    if (key == L"separator") return ParseColor(value, appearance.separator);
    if (key == L"hoverbackground") return ParseColor(value, appearance.hoverBackground);
    if (key == L"pressedbackground") return ParseColor(value, appearance.pressedBackground);
    if (key == L"textcolor") return ParseColor(value, appearance.textColor);
    if (key == L"disabledtextcolor") return ParseColor(value, appearance.disabledTextColor);
    if (key == L"submenuarrow") return ParseColor(value, appearance.submenuArrow);
    if (key == L"animation") return ParseAnimationKind(value, appearance.animation);
    if (key == L"animationduration") {
        return ParseIntValue(value, appearance.animationDuration) &&
               appearance.animationDuration >= 0;
    }
    return false;
}

std::wstring DefaultRulesConfigText() {
    return L"; Context Menu Overhaul v2 configuration\n"
           L"; Colors are #RRGGBB or #AARRGGBB. Comments start with ';'.\n"
           L"\n"
           L"[appearance]\n"
           L"; background = #1E1E1EF0\n"
           L"; blur = true\n"
           L"; cornerRadius = 8\n"
           L"; border = #FFFFFF22\n"
           L"; shadow = true\n"
           L"; font = Segoe UI, 9\n"
           L"; itemHeight = 28\n"
           L"; iconSize = 16\n"
           L"; padding = 6\n"
           L"; hoverBackground = #FFFFFF14\n"
           L"; textColor = #FFFFFF\n"
           L"; animation = none\n"
           L"\n"
           L"; [appearance.light]\n"
           L"; background = #F5F5F5F2\n"
           L"; textColor = #202020\n"
           L"\n"
           L"; [rules]\n"
           L"; hide = label:\"Cast to Device\"\n"
           L"; move = thirdParty -> \"More options\"\n";
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
        Ignored,
    };
    Section section = Section::None;

    RulesConfig config;
    int currentCommand = -1;
    int currentSubmenu = -1;

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

        const std::wstring trimmed = TrimWhitespace(line);
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
                errors.push_back({lineNumber, L"invalid value for '" + key + L"'"});
            } else {
                baseValues.emplace_back(key, value);
            }
        } else if (section == Section::AppearanceLight) {
            Appearance scratch = Appearance{};
            if (!ApplyAppearanceValue(scratch, key, value)) {
                errors.push_back({lineNumber, L"invalid value for '" + key + L"'"});
            } else {
                lightValues.emplace_back(key, value);
            }
        } else if (section == Section::AppearanceDark) {
            Appearance scratch = Appearance{};
            if (!ApplyAppearanceValue(scratch, key, value)) {
                errors.push_back({lineNumber, L"invalid value for '" + key + L"'"});
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
        submenu.id = 0xF100 + static_cast<uint32_t>(destinations.size());
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
    auto ensureTopLevel = [&](const std::wstring& name) -> PendingCustomSubmenu& {
        auto it = pending.find(name);
        if (it == pending.end()) {
            PendingCustomSubmenu entry;
            entry.name = name;
            if (const CustomSubmenu* submenu = findSubmenuConfig(name)) {
                entry.iconRef = submenu->iconRef;
                entry.position = submenu->position;
                entry.positionLabel = submenu->positionLabel;
            }
            it = pending.emplace(name, std::move(entry)).first;
            pendingOrder.push_back(name);
        }
        return it->second;
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

        PendingCustomSubmenu& top = ensureTopLevel(segments[0]);
        std::vector<MenuItem>* current = &top.children;
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
                submenu.id = 0xF300 + static_cast<uint32_t>(s);
                submenu.kind = ItemKind::Submenu;
                submenu.action = ActionKind::Submenu;
                submenu.label = segments[s];
                if (const CustomSubmenu* sc = findSubmenuConfig(segments[s])) {
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
        item.kind = ItemKind::Command;
        item.action = ActionKind::CustomCommand;
        item.label = command.label;
        item.iconRef = command.iconRef;
        item.customCommandIndex = static_cast<uint32_t>(i);

        std::vector<MenuItem>* container = nullptr;
        if (!command.menuPath.empty()) {
            container = resolveContainer(command.menuPath);
        }
        if (!container) {
            topLevelCommands.push_back(std::move(item));
            continue;
        }

        if (command.separator == CommandSeparator::Before) {
            MenuItem separator{};
            separator.id = 0xF500 + static_cast<uint32_t>(i);
            separator.kind = ItemKind::Separator;
            container->push_back(std::move(separator));
        }
        container->push_back(std::move(item));
        if (command.separator == CommandSeparator::After) {
            MenuItem separator{};
            separator.id = 0xF501 + static_cast<uint32_t>(i);
            separator.kind = ItemKind::Separator;
            container->push_back(std::move(separator));
        }
    }

    uint32_t syntheticId = 0xF400;
    for (const std::wstring& name : pendingOrder) {
        PendingCustomSubmenu& entry = pending[name];
        MenuItem submenu{};
        submenu.id = syntheticId++;
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

std::wstring ConfigFilePath() {
    wchar_t storagePath[MAX_PATH] = {};
    if (!Wh_GetModStoragePath(storagePath, ARRAYSIZE(storagePath))) {
        return L"";
    }
    return std::wstring(storagePath) + L"\\menu.ini";
}

class ConfigStore {
public:
    void Start() {
        if (thread_) {
            return;
        }
        const std::wstring path = ConfigFilePath();
        if (path.empty()) {
            return;
        }
        const size_t slash = path.find_last_of(L'\\');
        if (slash != std::wstring::npos) {
            CreateDirectoryW(path.substr(0, slash).c_str(), nullptr);
        }
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
            WriteDefaultFile(path);
        }
        std::wstring text;
        if (ReadTextFile(path, text)) {
            ApplyText(text);
        }

        stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!stopEvent_) {
            return;
        }
        thread_ = CreateThread(nullptr, 0, &ConfigStore::ThreadProc, this, 0, nullptr);
        if (!thread_) {
            CloseHandle(stopEvent_);
            stopEvent_ = nullptr;
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
    }

    std::shared_ptr<const RulesConfig> Snapshot() const {
        return config_.load(std::memory_order_acquire);
    }

    uint64_t Revision() const {
        std::shared_ptr<const RulesConfig> snapshot =
            config_.load(std::memory_order_acquire);
        return snapshot ? snapshot->revision : 0;
    }

    bool ApplyTextForTesting(const std::wstring& text) {
        return ApplyText(text);
    }

private:
    bool ApplyText(const std::wstring& text) {
        RulesConfig parsed;
        std::vector<ConfigParseError> errors;
        if (!ParseRulesConfig(text, parsed, errors)) {
            for (const ConfigParseError& error : errors) {
                Wh_Log(L"menu.ini:%d: %s", error.line, error.message.c_str());
            }
            return false;
        }
        std::shared_ptr<const RulesConfig> previous =
            config_.load(std::memory_order_acquire);
        parsed.revision = previous ? previous->revision + 1 : 1;
        config_.store(std::make_shared<const RulesConfig>(std::move(parsed)),
                      std::memory_order_release);
        return true;
    }

    static void WriteDefaultFile(const std::wstring& path) {
        const std::wstring text = DefaultRulesConfigText();
        HANDLE file =
            CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            return;
        }
        DWORD written = 0;
        WriteFile(file, text.c_str(),
                  static_cast<DWORD>(text.size() * sizeof(wchar_t)), &written,
                  nullptr);
        CloseHandle(file);
    }

    static bool ReadTextFile(const std::wstring& path, std::wstring& text) {
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
        text.resize(static_cast<size_t>(size.QuadPart / sizeof(wchar_t)));
        DWORD read = 0;
        const BOOL ok = ReadFile(file, text.data(), static_cast<DWORD>(size.QuadPart),
                                 &read, nullptr);
        CloseHandle(file);
        if (!ok) {
            return false;
        }
        text.resize(read / sizeof(wchar_t));
        return true;
    }

    static DWORD WINAPI ThreadProc(LPVOID param) {
        static_cast<ConfigStore*>(param)->Run();
        return 0;
    }

    void Run() {
        const std::wstring path = ConfigFilePath();
        const size_t slash = path.find_last_of(L'\\');
        if (slash == std::wstring::npos) {
            return;
        }
        const std::wstring dir = path.substr(0, slash);
        const std::wstring fileName = path.substr(slash + 1);

        HANDLE dirHandle = CreateFileW(
            dir.c_str(), FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (dirHandle == INVALID_HANDLE_VALUE) {
            return;
        }

        std::vector<BYTE> buffer(4096);
        for (;;) {
            DWORD bytes = 0;
            if (!ReadDirectoryChangesW(
                    dirHandle, buffer.data(), static_cast<DWORD>(buffer.size()), FALSE,
                    FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE, &bytes,
                    nullptr, nullptr)) {
                break;
            }
            if (WaitForSingleObject(stopEvent_, 0) == WAIT_OBJECT_0) {
                break;
            }

            bool menuChanged = false;
            DWORD offset = 0;
            for (;;) {
                auto* info =
                    reinterpret_cast<FILE_NOTIFY_INFORMATION*>(buffer.data() + offset);
                const std::wstring name(info->FileName,
                                        info->FileNameLength / sizeof(wchar_t));
                if (_wcsicmp(name.c_str(), fileName.c_str()) == 0) {
                    menuChanged = true;
                }
                if (info->NextEntryOffset == 0) {
                    break;
                }
                offset += info->NextEntryOffset;
            }
            if (!menuChanged) {
                continue;
            }

            // Debounce editor write bursts.
            if (WaitForSingleObject(stopEvent_, 300) == WAIT_OBJECT_0) {
                break;
            }
            std::wstring text;
            if (ReadTextFile(path, text)) {
                ApplyText(text);
            }
        }
        CloseHandle(dirHandle);
    }

    std::atomic<std::shared_ptr<const RulesConfig>> config_;
    HANDLE thread_ = nullptr;
    HANDLE stopEvent_ = nullptr;
};

inline ConfigStore g_configStore;

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
    return kind == ShellViewKind::Desktop || kind == ShellViewKind::ShellDefView;
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
        SHELLEXECUTEINFOW info = {};
        info.cbSize = sizeof(info);
        info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
        info.hwnd = ctx.owner;
        info.lpVerb = L"runas";
        info.lpFile = expanded.c_str();
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

    SHELLEXECUTEINFOW info = {};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    info.hwnd = ctx.owner;
    info.lpFile = expanded.c_str();
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
        if (!item.iconPixels.empty()) {
            return BitmapFromPixels(item.iconPixels, sizePx);
        }
        if (item.iconRef.empty()) {
            return nullptr;
        }
        return BitmapFromRef(item.iconRef, sizePx);
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

        // Rebuild the SendTo entries off the UI thread (also after handler
        // registry invalidation).
        RebuildSendToChildren();

        const bool comInitialized =
            SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));

        for (const std::wstring& type : types) {
            if (WaitForSingleObject(stopEvent_, 0) == WAIT_OBJECT_0) {
                break;
            }
            WaitForSingleObject(resumeEvent_, INFINITE);
            if (WaitForSingleObject(stopEvent_, 0) == WAIT_OBJECT_0) {
                break;
            }
            WarmOneType(type, warmupDir);
            Sleep(50);
        }

        if (comInitialized) {
            CoUninitialize();
        }
        Wh_Log(L"Warm-up finished");
    }

    void WarmOneType(const std::wstring& type, const std::wstring& warmupDir) {
        if (type == L"*") {
            const std::wstring path = warmupDir + L"\\warmup";
            EnsureScratchFile(path);
            WarmPath(path, false,
                     ContextSignature{Scope::Files, L"*", Shape::Single, Variant::Normal});
        } else if (type == L"Directory") {
            const std::wstring path = warmupDir + L"\\warmup-folder";
            CreateDirectoryW(path.c_str(), nullptr);
            WarmPath(path, false, ContextSignature{Scope::Folders, L"*", Shape::Single,
                                                   Variant::Normal});
        } else if (type == L"Directory\\Background") {
            const std::wstring path = warmupDir + L"\\warmup-folder";
            CreateDirectoryW(path.c_str(), nullptr);
            WarmPath(path, true, ContextSignature{Scope::Background, L"*", Shape::Single,
                                                  Variant::Normal});
        } else if (type == L"Drive") {
            wchar_t windowsDir[MAX_PATH] = {};
            if (GetWindowsDirectoryW(windowsDir, ARRAYSIZE(windowsDir))) {
                const std::wstring drive(windowsDir, 3);  // "C:\"
                WarmPath(drive, false, ContextSignature{Scope::Drive, L"*", Shape::Single,
                                                        Variant::Normal});
            }
        } else if (type == L"Desktop") {
            wchar_t desktopPath[MAX_PATH] = {};
            if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_DESKTOP, nullptr, 0,
                                           desktopPath))) {
                WarmPath(desktopPath, false,
                         ContextSignature{Scope::Desktop, L"*", Shape::Single,
                                          Variant::Normal});
            }
        } else {
            const std::wstring path = warmupDir + L"\\warmup" + type;
            EnsureScratchFile(path);
            WarmPath(path, false,
                     ContextSignature{Scope::Files, type, Shape::Single, Variant::Normal});
        }
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
            Wh_Log(L"Warm-up: no context menu for %s", path.c_str());
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

bool ShowReplacementMenu(PendingCapture& capture, ShellViewKind kind, HWND owner, POINT pt) {
    g_perf.MarkOpenPathStart();
    // Handler changes are checked while menus are used, not on a timer.
    g_invalidation.RequestCheck();

    SelectionInfo info = GetSelection(owner, kind);
    std::vector<std::wstring>& paths = info.paths;
    Shape shape = paths.size() > 1 ? Shape::Multi : Shape::Single;

    if (!IsFilesystemContext(info.folderIsFilesystem, info.allItemsAreFilesystem)) {
        Wh_Log(L"Non-filesystem namespace: using the native menu");
        ShowNativeReplay(capture, owner, pt);
        g_warmup.SetMenuOpen(false);
        return true;
    }

    Scope scope = RefineScope(ScopeFromKind(kind, paths.empty()), info.allItemsAreFolders,
                              AllPathsAreDrives(paths));
    const std::wstring typeKey =
        scope == Scope::Files ? MakeTypeKey(paths) : std::wstring(L"*");
    ContextSignature signature{scope, typeKey, shape, Variant::Normal};

    const DWORD clipboardSequence = GetClipboardSequenceNumber();
    const bool clipboardHadData = ClipboardHasFileData();

    g_warmup.SetMenuOpen(true);

    while (true) {
        std::optional<MenuModel> cached = g_cache.Find(signature);
        bool needsDiscovery = !cached || (cached->flags & kModelWarmup);
        Wh_Log(L"Cache %s: scope=%d key=%s shape=%d paths=%zu",
               cached ? (needsDiscovery ? L"warm" : L"hit") : L"miss",
               static_cast<int>(scope), typeKey.c_str(), static_cast<int>(shape),
               paths.size());

        // A true cache miss has no menu to show. Populate first: the shell's
        // population blocks the UI thread, so a placeholder menu would sit
        // blank until the real menu replaced it. The native menu pays the same
        // first-open cost. Warm entries are shown provisionally and refreshed
        // after the menu closes.
        if (!cached && !capture.discoveryDone) {
            Wh_Log(L"Populating before showing the menu");
            DiscoverIntoCache(capture, signature);
            cached = g_cache.Find(signature);
            needsDiscovery = !cached || (cached->flags & kModelWarmup);
        }

        MenuModel model =
            cached ? MergeCoreWithCached(BuildCoreModel(scope, paths, shape), *cached)
                   : BuildCoreModel(scope, paths, shape);
        if (!g_settings.showMoreOptionsItem) {
            std::erase_if(model.items, [](const MenuItem& item) {
                return item.action == ActionKind::Fallback;
            });
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

        ReorganizeAdvancedItems(model.items);

        if (scope == Scope::Background || scope == Scope::Desktop) {
            ApplyViewStateChecks(model.items, owner, kind);
        }

        Wh_Log(L"Menu prep: %llu ms",
               static_cast<unsigned long long>(g_perf.OpenPathElapsedMs()));

        bool creationFailed = false;
        std::optional<uint32_t> chosen =
            NativeMenuView::Show(model, owner, pt, &creationFailed);

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
    cmo::g_configStore.Start();
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
    cmo::g_configStore.Stop();
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
