// ==WindhawkMod==
// @id              context-menu-overhaul
// @name            Context Menu Overhaul
// @description     Replaces the Explorer context menu with an instantly-opening cached menu, then discovers and caches shell extension items asynchronously.
// @version         0.3.0
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
- instantMenuFade: true
  $name: Instant menu open
  $description: Temporarily disables system menu animation (fade and slide) while this mod's menu opens, so it appears instantly. Session-only; the previous setting is restored immediately.
*/
// ==/WindhawkModSettings==

#include <windows.h>
#include <exdisp.h>
#include <servprov.h>
#include <shlguid.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>

#include <commctrl.h>
#include <tlhelp32.h>
#include <windhawk_utils.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
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
    int warmupDelaySeconds = 5;
    bool clearCache = false;
    bool debugLogging = false;
    bool instantMenuFade = true;
};

inline Settings g_settings;

void LoadSettings() {
    g_settings.enableShiftBypass = Wh_GetIntSetting(L"enableShiftBypass") != 0;
    g_settings.showMoreOptionsItem = Wh_GetIntSetting(L"showMoreOptionsItem") != 0;
    g_settings.warmupDelaySeconds = Wh_GetIntSetting(L"warmupDelaySeconds");
    g_settings.clearCache = Wh_GetIntSetting(L"clearCache") != 0;
    g_settings.debugLogging = Wh_GetIntSetting(L"debugLogging") != 0;
    g_settings.instantMenuFade = Wh_GetIntSetting(L"instantMenuFade") != 0;
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
enum class ActionKind : uint8_t { ViewAction, ShellVerb, Fallback, Submenu };

// Documented view operations, dispatched through IFolderView2 / IShellView.
// The old FCIDM_* view command IDs are not defined by the Windows SDK and
// must not be guessed.
enum class ViewAction : uint32_t {
    None = 0,
    Rename,
    Refresh,
    ViewLargeIcons,
    ViewSmallIcons,
    ViewList,
    ViewDetails,
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
                              std::wstring iconRef = L"") {
        MenuItem item{};
        item.id = nextId++;
        item.kind = ItemKind::Command;
        item.label = std::move(label);
        item.canonicalVerb = std::move(dedupVerb);
        item.action = ActionKind::ViewAction;
        item.viewAction = static_cast<uint32_t>(action);
        item.flags = flags;
        item.iconRef = std::move(iconRef);
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
        item.label = L"Show more options";
        model.items.push_back(std::move(item));
    };

    const bool multi = shape == Shape::Multi;
    const uint32_t multiDisabled = multi ? kModelDisabled : kModelNone;
    const std::wstring openLabel = FormatMultiLabel(L"Open", paths.size());

    if (scope == Scope::Background || scope == Scope::Desktop) {
        MenuItem& viewMenu = addSubmenu(L"View");
        viewMenu.children.push_back(makeViewAction(L"Large icons", L"viewlarge",
                                                   ViewAction::ViewLargeIcons, kModelNone));
        viewMenu.children.push_back(makeViewAction(L"Small icons", L"viewsmall",
                                                   ViewAction::ViewSmallIcons, kModelNone));
        viewMenu.children.push_back(makeViewAction(L"List", L"viewlist",
                                                   ViewAction::ViewList, kModelNone));
        viewMenu.children.push_back(makeViewAction(L"Details", L"viewdetails",
                                                   ViewAction::ViewDetails, kModelNone));

        addCommand(L"Sort by", L"sortby");
        addViewAction(L"Refresh", L"refresh", ViewAction::Refresh, kModelNone,
                      L"@glyph:E72C");
        addSeparator();
        addCommand(L"Paste", L"paste", kModelNone, L"@glyph:E77F");
        addCommand(L"Paste shortcut", L"pastelink");
        addSeparator();
        addCommand(L"New", L"new");
        if (scope == Scope::Desktop) {
            addSeparator();
            addCommand(L"Display settings", L"display");
            addCommand(L"Personalize", L"personalize");
        }
        addSeparator();
        addFallback();
        return model;
    }

    if (scope == Scope::Drive) {
        addCommand(openLabel, L"open", kModelDefault, L"@folder");
        addCommand(L"Open in new window", L"opennew");
        addCommand(L"Pin to Quick access", L"pintohome");
        addSeparator();
        addCommand(L"Properties", L"properties", kModelNone, L"@glyph:E713");
        addSeparator();
        addFallback();
        return model;
    }

    if (scope == Scope::Folders) {
        addCommand(openLabel, L"open", kModelDefault, L"@folder");
        addCommand(L"Open in new window", L"opennew");
        addCommand(L"Pin to Quick access", L"pintohome");
        addSeparator();
    } else {
        addCommand(openLabel, L"open", kModelDefault, L"@file");
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
        if (!left.label.empty() && left.label == right.label) {
            return true;
        }
        return !left.canonicalVerb.empty() && left.canonicalVerb == right.canonicalVerb;
    };

    std::unordered_set<std::wstring> coreLabels;
    std::unordered_set<std::wstring> coreVerbs;
    for (const MenuItem& item : result.items) {
        if (!item.label.empty()) {
            coreLabels.insert(item.label);
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
            if (coreItem.action != ActionKind::ShellVerb || !matches(coreItem, item)) {
                continue;
            }
            // Adopt the native descriptor: the shell rejects some canonical
            // verb strings, but its own offsets always dispatch. Remember
            // the offset so cached opens invoke exactly like the native menu.
            coreItem.canonicalVerb = item.canonicalVerb;
            coreItem.verbOffset = item.verbOffset;
            coreItem.flags |= kModelHasOffset;
            matchedCore = true;
            break;
        }
        if (matchedCore) {
            continue;
        }

        if ((!item.label.empty() && coreLabels.count(item.label)) ||
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
constexpr uint32_t kCacheVersion = 6;
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

// Only the main file/folder menu is deferred. CMF_DEFAULTONLY is the
// default-verb resolution used by double-click/open; CMF_NOVERBS builds
// submenus such as Send to; CMF_VERBSONLY builds verb-only menus. Those
// never show a popup we could replace and must reach the shell untouched.
bool ShouldDeferContextMenu(UINT flags) {
    if (flags & (CMF_DEFAULTONLY | CMF_NOVERBS | CMF_VERBSONLY)) {
        return false;
    }
    return (flags & CMF_EXPLORE) != 0;
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

// Copies a shell menu bitmap (32bpp, small-icon sized) into BGRA pixels.
void CaptureBitmapPixels(HBITMAP bitmap, std::vector<uint8_t>& out) {
    out.clear();
    BITMAP bitmapInfo = {};
    if (!GetObjectW(bitmap, sizeof(bitmapInfo), &bitmapInfo)) {
        return;
    }

    const int expected = GetSystemMetrics(SM_CXSMICON);
    if (bitmapInfo.bmWidth != expected || bitmapInfo.bmHeight != expected ||
        bitmapInfo.bmBitsPixel != 32) {
        return;
    }

    BITMAPINFO dibInfo = {};
    dibInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    dibInfo.bmiHeader.biWidth = expected;
    dibInfo.bmiHeader.biHeight = -expected;
    dibInfo.bmiHeader.biPlanes = 1;
    dibInfo.bmiHeader.biBitCount = 32;
    dibInfo.bmiHeader.biCompression = BI_RGB;

    std::vector<uint8_t> pixels(static_cast<size_t>(expected) * expected * 4);
    HDC screen = GetDC(nullptr);
    const int lines = GetDIBits(screen, bitmap, 0, expected, pixels.data(), &dibInfo,
                                DIB_RGB_COLORS);
    ReleaseDC(nullptr, screen);
    if (lines == expected) {
        out = std::move(pixels);
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

        if (info.fType & MFT_OWNERDRAW) {
            item.flags |= kModelOwnerDraw;
            ownerDraw = true;
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
            // Capture icons the shell itself provides. HBMMENU_* sentinels
            // are small integers; real bitmaps are pointers.
            if (info.hbmpItem &&
                reinterpret_cast<INT_PTR>(info.hbmpItem) > 16) {
                CaptureBitmapPixels(info.hbmpItem, item.iconPixels);
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

// Debug dump of a discovered model; helps identify unlabeled or unusual items.
void DumpModelItems(const std::vector<MenuItem>& items, int depth) {
    for (const MenuItem& item : items) {
        Wh_Log(L"[d%d] kind=%d action=%d flags=%04X offset=%u verb='%s' label='%s' "
               L"children=%zu",
               depth, static_cast<int>(item.kind), static_cast<int>(item.action),
               item.flags, item.verbOffset, item.canonicalVerb.c_str(),
               item.label.c_str(), item.children.size());
        DumpModelItems(item.children, depth + 1);
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
    InitializeMenuRecursive(capture, capture.populatedMenu, 0);
    MenuModel model = BuildModelFromHMenu(capture.populatedMenu, capture.idCmdFirst,
                                          signature, capture.obj);
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
    IContextMenu* liveContext = nullptr;
    UINT idCmdFirst = 0;
    ShellViewKind kind = ShellViewKind::None;
    DWORD clipboardSequence = 0;
    bool clipboardHadData = false;
};

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
        case ViewAction::ViewSmallIcons:
            return FVM_SMALLICON;
        case ViewAction::ViewList:
            return FVM_LIST;
        case ViewAction::ViewDetails:
            return FVM_DETAILS;
        default:
            return FVM_ICON;
    }
}

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
                case ViewAction::ViewLargeIcons:
                case ViewAction::ViewSmallIcons:
                case ViewAction::ViewList:
                case ViewAction::ViewDetails: {
                    const FOLDERVIEWMODE mode = FolderViewModeFor(action);
                    result = SUCCEEDED(folderView->SetViewModeAndIconSize(mode, -1))
                                 ? InvokeResult::Handled
                                 : InvokeResult::FallbackNative;
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

    // Returns a menu bitmap for the item's icon, or nullptr. `paths` is the
    // captured selection, used to resolve "@file".
    HBITMAP GetBitmap(const MenuItem& item, const std::vector<std::wstring>& paths,
                      int sizePx) {
        if (!item.iconPixels.empty()) {
            return BitmapFromPixels(item.iconPixels, sizePx);
        }
        if (item.iconRef.empty()) {
            return nullptr;
        }
        return BitmapFromRef(item.iconRef, paths, sizePx);
    }

    // Pre-renders the core glyphs and the stock folder icon, so the open path
    // only ever attaches already-cached bitmaps.
    void PreloadCoreIcons(int sizePx) {
        static const wchar_t* kGlyphRefs[] = {
            L"@glyph:E8C6", L"@glyph:E8C8", L"@glyph:E8AC", L"@glyph:E74D",
            L"@glyph:E713", L"@glyph:E8E5", L"@glyph:E72C", L"@glyph:E77F",
        };
        for (const wchar_t* ref : kGlyphRefs) {
            BitmapFromRef(ref, {}, sizePx);
        }
        BitmapFromRef(L"@folder", {}, sizePx);
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
    HBITMAP BitmapFromRef(const std::wstring& ref,
                          const std::vector<std::wstring>& paths, int sizePx) {
        // "@file" resolves to a type icon, so key it by extension rather than
        // sharing one bitmap across every file type.
        const std::wstring cacheRef =
            ref == L"@file"
                ? (L"@file:" +
                   (paths.empty() ? std::wstring(L"")
                                  : MakeExtensionKey(paths.front())))
                : ref;
        const std::wstring key = cacheRef + L"#" + std::to_wstring(sizePx);
        auto it = bitmaps_.find(key);
        if (it != bitmaps_.end()) {
            return it->second;
        }

        HBITMAP bitmap = nullptr;
        if (ref == L"@file") {
            if (!paths.empty()) {
                SHFILEINFOW fileInfo = {};
                if (SHGetFileInfoW(paths.front().c_str(), FILE_ATTRIBUTE_NORMAL,
                                   &fileInfo, sizeof(fileInfo),
                                   SHGFI_ICON | SHGFI_SMALLICON |
                                       SHGFI_USEFILEATTRIBUTES) &&
                    fileInfo.hIcon) {
                    bitmap = BitmapFromIcon(fileInfo.hIcon, sizePx);
                    DestroyIcon(fileInfo.hIcon);
                }
            }
        } else if (ref == L"@folder") {
            SHSTOCKICONINFO stockInfo = {};
            stockInfo.cbSize = sizeof(stockInfo);
            if (SUCCEEDED(SHGetStockIconInfo(SIID_FOLDER,
                                             SHGSI_ICON | SHGSI_SMALLICON,
                                             &stockInfo)) &&
                stockInfo.hIcon) {
                bitmap = BitmapFromIcon(stockInfo.hIcon, sizePx);
                DestroyIcon(stockInfo.hIcon);
            }
        } else if (ref.rfind(L"@glyph:", 0) == 0) {
            const wchar_t codepoint =
                static_cast<wchar_t>(wcstoul(ref.c_str() + 7, nullptr, 16));
            bitmap = GlyphBitmap(codepoint, sizePx);
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
        HBITMAP bitmap = ScaledBitmapFromBgra(pixels, side, side, sizePx);
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

    static HBITMAP BitmapFromIcon(HICON icon, int sizePx) {
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
            DrawIconEx(memory, 0, 0, icon, sizePx, sizePx, 0, nullptr, DI_NORMAL);
            SelectObject(memory, old);
        }
        DeleteDC(memory);
        ReleaseDC(nullptr, screen);
        return bitmap;
    }

    static HBITMAP ScaledBitmapFromBgra(const std::vector<uint8_t>& pixels, int width,
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
            SetStretchBltMode(targetDc, HALFTONE);
            StretchBlt(targetDc, 0, 0, sizePx, sizePx, sourceDc, 0, 0, width, height,
                       SRCCOPY);
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

    static bool IsDarkThemeActive() {
        DWORD lightTheme = 1;
        DWORD size = sizeof(lightTheme);
        HKEY key = nullptr;
        if (RegOpenKeyExW(
                HKEY_CURRENT_USER,
                L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                0, KEY_READ, &key) == ERROR_SUCCESS) {
            RegQueryValueExW(key, L"AppsUseLightTheme", nullptr, nullptr,
                             reinterpret_cast<LPBYTE>(&lightTheme), &size);
            RegCloseKey(key);
        }
        return lightTheme == 0;
    }

    static HBITMAP GlyphBitmap(wchar_t codepoint, int sizePx) {
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

            // Derive coverage from the white rendering and store a
            // premultiplied monochrome glyph in the menu text color.
            const bool darkTheme = IsDarkThemeActive();
            const COLORREF textColor = GetSysColor(COLOR_MENUTEXT);
            const uint8_t red = darkTheme ? 255 : GetRValue(textColor);
            const uint8_t green = darkTheme ? 255 : GetGValue(textColor);
            const uint8_t blue = darkTheme ? 255 : GetBValue(textColor);

            uint8_t* pixels = static_cast<uint8_t*>(bits);
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
                pixels[i * 4] = static_cast<uint8_t>(blue * coverage / 255);
                pixels[i * 4 + 1] = static_cast<uint8_t>(green * coverage / 255);
                pixels[i * 4 + 2] = static_cast<uint8_t>(red * coverage / 255);
                pixels[i * 4 + 3] = coverage;
            }
            if (anyCoverage) {
                return bitmap;
            }
            DeleteObject(bitmap);
        }
        return nullptr;
    }

    std::unordered_map<std::wstring, HBITMAP> bitmaps_;
    std::unordered_map<std::wstring, HICON> icons_;
};

inline IconCache g_iconCache;

class NativeMenuView {
public:
    static std::optional<uint32_t> Show(const MenuModel& model, HWND owner, POINT pt,
                                        const std::vector<std::wstring>& paths,
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
        AppendItems(menu, model.items, paths, iconSize);

        if (!TrackPopupMenuEx_Original) {
            if (creationFailed) {
                *creationFailed = true;
            }
            DestroyMenu(menu);
            return std::nullopt;
        }

        const UINT flags = TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN;
        MenuAnimationSuppressor animationSuppressor;
        int command = TrackPopupMenuEx_Original(menu, flags, pt.x, pt.y, owner, nullptr);
        DestroyMenu(menu);

        if (command == 0) {
            return std::nullopt;
        }
        return static_cast<uint32_t>(command);
    }

private:
    static void AppendItems(HMENU menu, const std::vector<MenuItem>& items,
                            const std::vector<std::wstring>& paths, int iconSize) {
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
                AppendItems(submenu, item.children, paths, iconSize);
                AppendMenuW(menu, flags | MF_POPUP,
                            reinterpret_cast<UINT_PTR>(submenu), item.label.c_str());
            } else {
                AppendMenuW(menu, flags, item.id, item.label.c_str());
            }

            // Checked items keep the checkmark gutter; everything else may
            // carry a cached icon bitmap.
            if (!(item.flags & kModelChecked)) {
                HBITMAP bitmap = g_iconCache.GetBitmap(item, paths, iconSize);
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

    // Runs discovery after the menu has been painted and is interactive.
    void StartDiscoveryTimer(const ContextSignature& signature) {
        discoverySignature_ = signature;
        SetTimer(owner_, kDiscoveryTimerId, 150, nullptr);
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
            if (capture_ && !capture_->discoveryDone) {
                DiscoverIntoCache(*capture_, discoverySignature_);
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
            ReplayInto(menu, offscreen, 0, 1, 0x7FFF, CMF_NORMAL);
            MenuModel model = BuildModelFromHMenu(offscreen, 1, signature, menu);
            DestroyMenu(offscreen);
            if (!model.items.empty()) {
                model.handlerModules =
                    DiffModules(modulesBefore, SnapshotLoadedModules());
                model.sourceStamp = ComputeModuleStamp(model.handlerModules);
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
        if (!stopEvent_) {
            return;
        }
        thread_ = CreateThread(nullptr, 0, &Invalidation::ThreadProc, this, 0, nullptr);
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

        for (;;) {
            if (WaitForSingleObject(stopEvent_, 5000) == WAIT_OBJECT_0) {
                break;
            }

            const uint64_t current = ComputeRegistryFingerprint();
            if (current != registryFingerprint) {
                registryFingerprint = current;
                InvalidateNow(L"Context menu handlers changed");
            }

            if (GetTickCount64() >= nextRevalidationTick_) {
                nextRevalidationTick_ = GetTickCount64() + 3600000;
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

    std::optional<MenuModel> cached = g_cache.Find(signature);
    Wh_Log(L"Cache %s: scope=%d key=%s shape=%d paths=%zu",
           cached ? L"hit" : L"miss", static_cast<int>(scope), typeKey.c_str(),
           static_cast<int>(shape), paths.size());

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
        g_warmup.SetMenuOpen(false);
        return true;
    }

    const DWORD clipboardSequence = GetClipboardSequenceNumber();
    const bool clipboardHadData = ClipboardHasFileData();
    for (MenuItem& item : model.items) {
        if (item.canonicalVerb == L"paste" && !clipboardHadData) {
            item.flags |= kModelDisabled;
        }
        if (item.kind == ItemKind::Submenu && item.label == L"Send to" &&
            item.children.empty()) {
            item.children = GetSendToChildren();
        }
    }

    Wh_Log(L"Menu prep: %llu ms",
           static_cast<unsigned long long>(g_perf.OpenPathElapsedMs()));

    g_warmup.SetMenuOpen(true);
    bool creationFailed = false;
    std::optional<uint32_t> chosen;
    {
        // On a cache miss the menu paints immediately and discovery runs from
        // the timer while the menu is interactive, so closing stays instant.
        // Cache hits skip population entirely; extension items populate
        // lazily if they are clicked.
        if (cached) {
            chosen = NativeMenuView::Show(model, owner, pt, paths, &creationFailed);
        } else {
            OwnerSubclass subclass(owner, &capture, /*forwardMenuMessages=*/false);
            subclass.StartDiscoveryTimer(signature);
            chosen = NativeMenuView::Show(model, owner, pt, paths, &creationFailed);
        }
    }

    if (creationFailed) {
        Wh_Log(L"Menu creation failed; using the native menu");
        ShowNativeReplay(capture, owner, pt);
        if (!cached && !capture.discoveryDone) {
            DiscoverIntoCache(capture, signature);
        }
        g_warmup.SetMenuOpen(false);
        return true;
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
                // View actions act on the view's current selection; never act
                // on a different selection than the one captured.
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
    }

    // If the timer did not run while the menu was open (fast dismissal),
    // warm the cache now. Cache hits never populate.
    if (!cached && !capture.discoveryDone) {
        DiscoverIntoCache(capture, signature);
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
    cmo::RestoreMenuAnimation();
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
