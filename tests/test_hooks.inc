// Test-only helpers for the repository's test suite (tests/logic_test.cpp).
// Included by the mod only when CMO_TESTING is defined; the Windhawk build
// never opens this file, so none of it is part of the submitted mod.
namespace cmo {

std::vector<uint32_t> FlattenIds(const MenuModel& model) {
    std::vector<uint32_t> ids;
    FlattenIdsInto(model.items, ids);
    return ids;
}

void ResetNewTemplatesForTesting() {
    std::lock_guard<std::mutex> lock(g_newTemplatesMutex);
    g_newTemplates.clear();
    g_newTemplatesReady.store(false, std::memory_order_relaxed);
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

void ModeController::SetMode(MenuMode mode) {
    mode_ = mode;
}

bool Warmup::IsPaused() const {
    return paused_.load();
}

}  // namespace cmo
