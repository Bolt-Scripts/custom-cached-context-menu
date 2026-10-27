// Test-only helpers for the repository's test suite (tests/logic_test.cpp).
// Included by the mod only when CMO_TESTING is defined; the Windhawk build
// never opens this file, so none of it is part of the submitted mod.
#include <type_traits>

namespace cmo {

bool LabelMatchesRegisteredHandler(const ContextSignature& signature,
                                   const std::wstring& label,
                                   std::wstring* dllOut);

// Publishes a single settings change through the production publisher.
template <typename T>
void SetSettingForTesting(T Settings::* member, std::type_identity_t<T> value) {
    Settings next = g_settings.Snapshot();
    next.*member = std::move(value);
    g_settings.Publish(std::move(next));
}

std::vector<uint32_t> FlattenIds(const MenuModel& model) {
    std::vector<uint32_t> ids;
    FlattenIdsInto(model.items, ids);
    return ids;
}

void ResetNewTemplatesForTesting() {
    ResetNewTemplates();
}

size_t CountSubstring(const std::wstring& text, const std::wstring& needle) {
    if (needle.empty()) {
        return 0;
    }
    size_t count = 0;
    size_t pos = 0;
    while ((pos = text.find(needle, pos)) != std::wstring::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

// Reset restores inheritance: remove the key when something can be inherited
// (theme preset, light/dark base, derived unset defaults), else write the
// schema default.
ConfigOverride ResetOverrideForTarget(const SettingsTarget& target,
                                      const ConfigSchemaEntry& entry) {
    ConfigOverride change;
    change.section = target.section;
    change.key = entry.key;
    const bool inherits = target.kind == SettingsTargetKind::ThemeFile ||
                          target.kind == SettingsTargetKind::MenuIniLight ||
                          target.kind == SettingsTargetKind::MenuIniDark ||
                          entry.unset;
    if (inherits) {
        change.remove = true;
        return change;
    }
    change.value = NormalizeAppearanceValue(entry, entry.defaultValue);
    return change;
}

void SettingsRequestFlush(SettingsWriteState& state) {
    if (state.pending) {
        state.deadlineMs = 0;
    }
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

MenuModel BuildCoreFileModel(const std::vector<std::wstring>& paths, Shape shape) {
    return BuildCoreModel(Scope::Files, paths, shape);
}

MenuModel BuildCoreMenuForTesting(Scope scope,
                                  const std::vector<std::wstring>& paths,
                                  Shape shape) {
    return BuildCoreModel(scope, paths, shape);
}

std::vector<MenuItem> BuildSettingsTreeForTesting(
    const SettingsModelInputs& inputs) {
    return BuildSettingsTree(inputs);
}

MenuItem MakeCommandForTesting(const wchar_t* label) {
    MenuItem item{};
    item.id = 9000;
    item.kind = ItemKind::Command;
    item.action = ActionKind::ShellVerb;
    item.label = label;
    item.canonicalLabel = label;
    return item;
}

MenuItem MakeSubmenuForTesting(const wchar_t* label) {
    MenuItem item{};
    item.id = 9001;
    item.kind = ItemKind::Submenu;
    item.action = ActionKind::Submenu;
    item.label = label;
    item.canonicalLabel = label;
    return item;
}

bool DeserializeCacheWithSystemLanguageForTesting(
    std::span<const uint8_t> data, const wchar_t* systemTag) {
    return Cache::DeserializeWithLanguageForTesting(data, systemTag);
}

MenuItem* FindItemByLabelInItem(MenuItem& item, const wchar_t* label) {
    if (_wcsicmp(item.label.c_str(), label) == 0) {
        return &item;
    }
    for (MenuItem& child : item.children) {
        if (MenuItem* found = FindItemByLabelInItem(child, label)) {
            return found;
        }
    }
    return nullptr;
}

MenuItem* FindItemByLabel(const std::vector<MenuItem>& items,
                          const wchar_t* label) {
    for (const MenuItem& item : items) {
        if (_wcsicmp(item.label.c_str(), label) == 0) {
            return const_cast<MenuItem*>(&item);
        }
        if (MenuItem* found = FindItemByLabel(item.children, label)) {
            return found;
        }
    }
    return nullptr;
}

MenuItem* FindItemByLabel(const MenuModel& model, const wchar_t* label) {
    return FindItemByLabel(model.items, label);
}

MenuItem* FindItemByRole(const std::vector<MenuItem>& items, BuiltinRole role) {
    for (const MenuItem& item : items) {
        if (item.builtinRole == role) {
            return const_cast<MenuItem*>(&item);
        }
        if (MenuItem* found = FindItemByRole(item.children, role)) {
            return found;
        }
    }
    return nullptr;
}

MenuItem* FindItemByRole(const MenuModel& model, BuiltinRole role) {
    return FindItemByRole(model.items, role);
}

size_t CountItemsByLabel(const std::vector<MenuItem>& items,
                         const wchar_t* label) {
    size_t count = 0;
    for (const MenuItem& item : items) {
        if (_wcsicmp(item.label.c_str(), label) == 0) {
            ++count;
        }
        count += CountItemsByLabel(item.children, label);
    }
    return count;
}

size_t CountItemsByLabel(const MenuModel& model, const wchar_t* label) {
    return CountItemsByLabel(model.items, label);
}

size_t CountItemsByRole(const std::vector<MenuItem>& items, BuiltinRole role) {
    size_t count = 0;
    for (const MenuItem& item : items) {
        if (item.builtinRole == role) {
            ++count;
        }
        count += CountItemsByRole(item.children, role);
    }
    return count;
}

size_t CountItemsByRole(const MenuModel& model, BuiltinRole role) {
    return CountItemsByRole(model.items, role);
}

// Compatibility wrapper; the open path uses the prebuilt list directly.
std::wstring ResolveRegistryIconByLabel(const ContextSignature& signature,
                                        const std::wstring& label) {
    return MatchShellLabelIcon(BuildShellLabelIcons(signature), label);
}

bool VersionInfoMatchesLabel(const std::wstring& path, const std::wstring& label) {
    std::vector<std::wstring> strings;
    ReadVersionStrings(path, strings);
    for (const std::wstring& value : strings) {
        if (LabelMatchesWords(label, value)) {
            return true;
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

CMINVOKECOMMANDINFOEX BuildInvokeCommandInfo(const MenuItem& item,
                                             const InvocationContext& ctx) {
    static thread_local std::string ansiStorage;
    CMINVOKECOMMANDINFOEX info = {};
    FillInvokeCommandInfo(item, ctx, ansiStorage, info);
    return info;
}

bool StampMatches(const SourceStamp& cached, const SourceStamp& current) {
    return cached.registryStamp == current.registryStamp &&
           cached.dllStamp == current.dllStamp;
}

bool ConfigStore::ApplyTextForTesting(const std::wstring& text) {
    return ApplyText(text);
}

bool Warmup::IsPaused() const {
    return paused_.load();
}

bool LabelMatchesRegisteredHandler(const ContextSignature& signature,
                                   const std::wstring& label,
                                   std::wstring* dllOut) {
    return MatchesRegisteredHandler(BuildRegisteredHandlers(signature), label,
                                    dllOut);
}

void LayoutCache::SetMaxEntries(size_t maxEntries) {
    maxEntries_ = maxEntries == 0 ? 1 : maxEntries;
}
void Cache::SetMaxEntries(size_t maxEntries) {
    std::lock_guard<std::mutex> lock(mutex_);
    maxEntries_ = maxEntries;
    EvictIfNeededLocked();
}

}  // namespace cmo
