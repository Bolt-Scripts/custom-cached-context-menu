#define CMO_TESTING 1
#include "wh_api_stub.h"
#include "../mod.wh.cpp"

static int g_failures = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                 \
        }                                                                 \
    } while (0)

#define CHECK_EQ(a, b)                                                          \
    do {                                                                        \
        auto va_ = (a);                                                         \
        auto vb_ = (b);                                                         \
        if (!(va_ == vb_)) {                                                    \
            fprintf(stderr, "FAIL %s:%d: %s != %s\n", __FILE__, __LINE__, #a,   \
                    #b);                                                        \
            ++g_failures;                                                       \
        }                                                                       \
    } while (0)

static cmo::MenuItem MakeDeepItem(int depth) {
    cmo::MenuItem item{};
    item.id = 30000 + static_cast<uint32_t>(depth);
    item.label = L"deep";
    if (depth > 0) {
        item.kind = cmo::ItemKind::Submenu;
        item.action = cmo::ActionKind::Submenu;
        item.children.push_back(MakeDeepItem(depth - 1));
    } else {
        item.kind = cmo::ItemKind::Command;
        item.action = cmo::ActionKind::ShellVerb;
        item.canonicalVerb = L"x";
    }
    return item;
}

// Minimal IContextMenu2 used to verify host-style initialization.
class FakeContextMenu2 : public IContextMenu2 {
public:
    int handleMenuMsgCalls = 0;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** ppvObject) override {
        if (ppvObject) {
            *ppvObject = nullptr;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE QueryContextMenu(HMENU, UINT, UINT, UINT, UINT) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE InvokeCommand(CMINVOKECOMMANDINFO*) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCommandString(UINT_PTR, UINT, UINT*, LPSTR,
                                               UINT) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE HandleMenuMsg(UINT uMsg, WPARAM, LPARAM) override {
        if (uMsg == WM_INITMENUPOPUP) {
            ++handleMenuMsgCalls;
        }
        return S_OK;
    }
};

int main() {
    CHECK_EQ(cmo::MakeExtensionKey(L"file.txt"), std::wstring(L".txt"));
    CHECK_EQ(cmo::MakeExtensionKey(L"FILE.TXT"), std::wstring(L".txt"));
    CHECK_EQ(cmo::MakeExtensionKey(L"archive.tar.gz"), std::wstring(L".gz"));
    CHECK_EQ(cmo::MakeExtensionKey(L".gitignore"), std::wstring(L"*"));
    CHECK_EQ(cmo::MakeExtensionKey(L"trailing."), std::wstring(L"*"));
    CHECK_EQ(cmo::MakeExtensionKey(L"noext"), std::wstring(L"*"));
    CHECK_EQ(cmo::MakeExtensionKey(L""), std::wstring(L"*"));
    CHECK_EQ(cmo::MakeExtensionKey(LR"(C:\dir.with.dot\file)"), std::wstring(L"*"));
    CHECK_EQ(cmo::MakeExtensionKey(LR"(C:\dir\report.PDF)"), std::wstring(L".pdf"));
    CHECK_EQ(cmo::MakeExtensionKey(L"\u5831\u544a\u66f8.xlsx"), std::wstring(L".xlsx"));

    CHECK_EQ(cmo::MakeTypeKey({L"a.txt", L"b.txt"}), std::wstring(L".txt"));
    CHECK_EQ(cmo::MakeTypeKey({L"a.txt", L"b.png"}), std::wstring(L"mixed"));
    CHECK_EQ(cmo::MakeTypeKey({}), std::wstring(L"*"));

    cmo::ContextSignature a{cmo::Scope::Files, L".txt", cmo::Shape::Single,
                            cmo::Variant::Normal};
    cmo::ContextSignature b{cmo::Scope::Files, L".txt", cmo::Shape::Single,
                            cmo::Variant::Normal};
    cmo::ContextSignature c{cmo::Scope::Files, L".png", cmo::Shape::Single,
                            cmo::Variant::Normal};
    CHECK(a == b);
    CHECK(a.Hash() == b.Hash());
    CHECK(!(a == c));

    CHECK_EQ(cmo::ClassifyClassChain({L"SHELLDLL_DefView", L"CabinetWClass"}, false),
             cmo::ShellViewKind::ShellDefView);
    CHECK_EQ(cmo::ClassifyClassChain({L"NamespaceTreeControl", L"ShellTabWindowClass"}, false),
             cmo::ShellViewKind::NavPane);
    CHECK_EQ(cmo::ClassifyClassChain({L"ToolbarWindow32"}, true),
             cmo::ShellViewKind::Desktop);
    CHECK_EQ(cmo::ClassifyClassChain({L"Shell_TrayWnd"}, false),
             cmo::ShellViewKind::None);
    CHECK(cmo::IsReplaceableKind(cmo::ShellViewKind::ShellDefView));
    CHECK(cmo::IsReplaceableKind(cmo::ShellViewKind::Desktop));
    CHECK(!cmo::IsReplaceableKind(cmo::ShellViewKind::NavPane));

    cmo::PendingQueue queue;
    cmo::PendingCapture capture{};
    capture.tick = 1000;
    capture.idCmdFirst = 11;
    queue.Push(capture);
    cmo::PendingCapture taken{};
    CHECK(queue.Take(taken));
    CHECK(taken.idCmdFirst == 11);
    CHECK(!queue.Take(taken));

    // A taken capture stays stable when a new capture is pushed (reentrancy).
    cmo::PendingCapture replacement{};
    replacement.idCmdFirst = 22;
    queue.Push(replacement);
    CHECK(taken.idCmdFirst == 11);

    queue.Push(capture);
    queue.ExpireOlderThan(1200, 500);
    cmo::PendingCapture fresh{};
    CHECK(queue.Take(fresh));

    queue.Push(capture);
    queue.ExpireOlderThan(2000, 500);
    CHECK(!queue.Take(fresh));

    queue.Push(capture);
    queue.Clear();
    CHECK(!queue.Take(fresh));

    std::vector<std::wstring> onePath{L"a.txt"};
    cmo::MenuModel single = cmo::BuildCoreFileModel(onePath, cmo::Shape::Single);
    CHECK(!single.items.empty());
    CHECK_EQ(single.items.front().label, std::wstring(L"Open"));
    CHECK_EQ(single.items.back().label, std::wstring(L"Show more options"));
    CHECK_EQ(single.sig.typeKey, std::wstring(L".txt"));

    std::vector<std::wstring> threePaths{L"a.txt", L"b.txt", L"c.txt"};
    cmo::MenuModel multi = cmo::BuildCoreFileModel(threePaths, cmo::Shape::Multi);
    bool foundOpenItems = false;
    bool multiCutEnabled = false;
    bool multiRenameDisabled = false;
    for (const cmo::MenuItem& item : multi.items) {
        if (item.label == L"Open 3 items") {
            foundOpenItems = true;
        }
        if (item.label == L"Cut" && !(item.flags & cmo::kModelDisabled)) {
            multiCutEnabled = true;
        }
        if (item.label == L"Rename" && (item.flags & cmo::kModelDisabled)) {
            multiRenameDisabled = true;
        }
    }
    CHECK(foundOpenItems);
    CHECK(multiCutEnabled);
    CHECK(multiRenameDisabled);

    std::vector<uint32_t> ids = cmo::FlattenIds(single);
    CHECK(ids.size() >= 10);
    bool idsUnique = true;
    for (size_t i = 0; i < ids.size(); ++i) {
        for (size_t j = i + 1; j < ids.size(); ++j) {
            if (ids[i] == ids[j]) {
                idsUnique = false;
            }
        }
    }
    CHECK(idsUnique);
    const cmo::MenuItem* firstItem = cmo::FindById(single, single.items.front().id);
    CHECK(firstItem && firstItem->label == L"Open");

    cmo::MenuItem verbItem{};
    verbItem.canonicalVerb = L"open";
    verbItem.verbOffset = 7;
    auto verbDescriptor = cmo::ChooseInvokeDescriptor(verbItem);
    CHECK_EQ(verbDescriptor.first, std::wstring(L"open"));
    CHECK(verbDescriptor.second == 0);

    cmo::MenuItem offsetItem{};
    offsetItem.verbOffset = 7;
    auto offsetDescriptor = cmo::ChooseInvokeDescriptor(offsetItem);
    CHECK(offsetDescriptor.first.empty());
    CHECK(offsetDescriptor.second == 7);

    CHECK(cmo::MapMenuState(0) == cmo::kModelNone);
    CHECK((cmo::MapMenuState(MFS_DISABLED) & cmo::kModelDisabled) != 0);
    CHECK((cmo::MapMenuState(MFS_CHECKED) & cmo::kModelChecked) != 0);
    CHECK((cmo::MapMenuState(MFS_DEFAULT) & cmo::kModelDefault) != 0);

    cmo::MenuModel coreModel = cmo::BuildCoreFileModel(onePath, cmo::Shape::Single);
    cmo::MenuModel cachedModel{};
    cachedModel.sig = coreModel.sig;
    cmo::MenuItem duplicateOpen{};
    duplicateOpen.id = 10000;
    duplicateOpen.kind = cmo::ItemKind::Command;
    duplicateOpen.action = cmo::ActionKind::ShellVerb;
    duplicateOpen.label = L"Open";
    duplicateOpen.canonicalVerb = L"open";
    cmo::MenuItem localizedOpen{};
    localizedOpen.id = 10001;
    localizedOpen.kind = cmo::ItemKind::Command;
    localizedOpen.action = cmo::ActionKind::ShellVerb;
    localizedOpen.label = L"\u00d6ffnen";
    localizedOpen.canonicalVerb = L"open";
    cmo::MenuItem cachedSeparator{};
    cachedSeparator.id = 10002;
    cachedSeparator.kind = cmo::ItemKind::Separator;
    cmo::MenuItem winrarItem{};
    winrarItem.id = 10003;
    winrarItem.kind = cmo::ItemKind::Command;
    winrarItem.action = cmo::ActionKind::ShellVerb;
    winrarItem.label = L"Extract to...";
    winrarItem.canonicalVerb = L"WinRAR.Extract";
    cachedModel.items = {duplicateOpen, localizedOpen, cachedSeparator, winrarItem};

    cmo::MenuModel mergedModel = cmo::MergeCoreWithCached(coreModel, cachedModel);
    int openCount = 0;
    int winrarCount = 0;
    int separatorCount = 0;
    for (const cmo::MenuItem& item : mergedModel.items) {
        if (item.label == L"Open") openCount++;
        if (item.label == L"Extract to...") winrarCount++;
        if (item.kind == cmo::ItemKind::Separator) separatorCount++;
    }
    CHECK(openCount == 1);
    CHECK(winrarCount == 1);
    CHECK(separatorCount >= 3);
    CHECK_EQ(mergedModel.items.back().label, std::wstring(L"Show more options"));

    cmo::MenuItem verbInvoke{};
    verbInvoke.canonicalVerb = L"open";
    verbInvoke.verbOffset = 3;
    cmo::InvocationContext invokeCtx{};
    invokeCtx.owner = nullptr;
    invokeCtx.pt = POINT{10, 20};
    CMINVOKECOMMANDINFOEX verbInfo = cmo::BuildInvokeCommandInfo(verbInvoke, invokeCtx);
    CHECK(verbInfo.cbSize == sizeof(CMINVOKECOMMANDINFOEX));
    CHECK((verbInfo.fMask & CMIC_MASK_UNICODE) != 0);
    CHECK(verbInfo.lpVerbW != nullptr);
    CHECK(wcscmp(verbInfo.lpVerbW, L"open") == 0);
    CHECK(verbInfo.hwnd == invokeCtx.owner);
    CHECK(verbInfo.ptInvoke.x == 10 && verbInfo.ptInvoke.y == 20);

    cmo::MenuItem offsetInvoke{};
    offsetInvoke.verbOffset = 7;
    CMINVOKECOMMANDINFOEX offsetInfo = cmo::BuildInvokeCommandInfo(offsetInvoke, invokeCtx);
    CHECK(reinterpret_cast<UINT_PTR>(offsetInfo.lpVerbW) == 7);

    cmo::MenuItem ownerDrawItem{};
    ownerDrawItem.flags = cmo::kModelOwnerDraw;
    cmo::PendingCapture emptyCapture{};
    CHECK(cmo::InvokeExtensionItem(ownerDrawItem, invokeCtx, emptyCapture) ==
          cmo::InvokeResult::FallbackNative);

    cmo::InvocationContext snapshotCtx{};
    std::vector<std::wstring> sourcePaths{L"a.txt"};
    snapshotCtx.paths = sourcePaths;
    sourcePaths.push_back(L"b.txt");
    CHECK(snapshotCtx.paths.size() == 1);

    // --- Task 7: persistence, LRU eviction, source stamps ---
    cmo::Cache serializeCache;
    cmo::MenuModel roundTrip = cmo::BuildCoreFileModel(onePath, cmo::Shape::Single);
    cmo::MenuItem submenuItem{};
    submenuItem.id = 500;
    submenuItem.kind = cmo::ItemKind::Submenu;
    submenuItem.action = cmo::ActionKind::Submenu;
    submenuItem.label = L"WinRAR";
    cmo::MenuItem submenuChild{};
    submenuChild.id = 501;
    submenuChild.kind = cmo::ItemKind::Command;
    submenuChild.action = cmo::ActionKind::ShellVerb;
    submenuChild.label = L"Extract Here";
    submenuChild.canonicalVerb = L"WinRAR.ExtractHere";
    submenuChild.verbOffset = 42;
    submenuItem.children.push_back(submenuChild);
    roundTrip.items.insert(roundTrip.items.end() - 1, submenuItem);
    roundTrip.handlerModules.push_back(L"cmo-test-storage\\fake-handler.dll");
    roundTrip.sourceStamp = 42;
    serializeCache.Put(roundTrip);

    std::vector<uint8_t> serialized = serializeCache.Serialize();
    CHECK(!serialized.empty());

    cmo::Cache restoredCache;
    CHECK(cmo::Cache::Deserialize(serialized, restoredCache));
    std::optional<cmo::MenuModel> restoredModel = restoredCache.Find(roundTrip.sig);
    CHECK(restoredModel.has_value());
    CHECK(restoredModel && restoredModel->items.size() == roundTrip.items.size());
    CHECK(restoredModel && restoredModel->items.back().label == L"Show more options");
    CHECK(restoredModel && restoredModel->handlerModules.size() == 1);
    CHECK(restoredModel && restoredModel->sourceStamp == 42);
    const cmo::MenuItem* restoredChild =
        restoredModel ? cmo::FindById(*restoredModel, 501) : nullptr;
    CHECK(restoredChild != nullptr);
    CHECK(restoredChild && restoredChild->canonicalVerb == L"WinRAR.ExtractHere");
    CHECK(restoredChild && restoredChild->verbOffset == 42);

    std::vector<uint8_t> badVersion = serialized;
    const uint32_t differentVersion = cmo::kCacheVersion + 1;
    badVersion[4] = static_cast<uint8_t>(differentVersion & 0xFF);
    badVersion[5] = static_cast<uint8_t>((differentVersion >> 8) & 0xFF);
    badVersion[6] = static_cast<uint8_t>((differentVersion >> 16) & 0xFF);
    badVersion[7] = static_cast<uint8_t>((differentVersion >> 24) & 0xFF);
    cmo::Cache rejectedCache;
    CHECK(!cmo::Cache::Deserialize(badVersion, rejectedCache));

    std::vector<uint8_t> truncatedData(serialized.begin(),
                                       serialized.begin() + serialized.size() / 2);
    CHECK(!cmo::Cache::Deserialize(truncatedData, rejectedCache));

    std::vector<uint8_t> corruptData = serialized;
    corruptData[10] ^= 0xFF;
    CHECK(!cmo::Cache::Deserialize(corruptData, rejectedCache));

    cmo::Cache lruCache;
    lruCache.SetMaxEntries(2);
    cmo::MenuModel lruA = cmo::BuildCoreFileModel({L"a.txt"}, cmo::Shape::Single);
    cmo::MenuModel lruB = cmo::BuildCoreFileModel({L"b.png"}, cmo::Shape::Single);
    cmo::MenuModel lruC = cmo::BuildCoreFileModel({L"c.pdf"}, cmo::Shape::Single);
    lruCache.Put(lruA);
    Sleep(20);
    lruCache.Put(lruB);
    Sleep(20);
    CHECK(lruCache.Find(lruA.sig).has_value());
    Sleep(20);
    lruCache.Put(lruC);
    CHECK(lruCache.Size() == 2);
    CHECK(!lruCache.Find(lruB.sig).has_value());
    CHECK(lruCache.Find(lruA.sig).has_value());
    CHECK(lruCache.Find(lruC.sig).has_value());

    CreateDirectoryW(L"cmo-test-storage", nullptr);
    std::wstring cachePath = L"cmo-test-storage\\cache-test.bin";
    CHECK(serializeCache.Save(cachePath));
    cmo::Cache diskCache;
    CHECK(diskCache.Load(cachePath));
    CHECK(diskCache.Find(roundTrip.sig).has_value());
    DeleteFileW(cachePath.c_str());

    // Find returns an independent copy: a later Clear must not invalidate it.
    std::optional<cmo::MenuModel> independent = diskCache.Find(roundTrip.sig);
    CHECK(independent.has_value());
    diskCache.Clear();
    CHECK(independent.has_value() &&
          independent->items.size() == roundTrip.items.size());

    // A model nested deeper than the parser limit is rejected.
    cmo::Cache deepCache;
    cmo::MenuModel deepModel{};
    deepModel.sig = cmo::ContextSignature{cmo::Scope::Files, L".deep",
                                          cmo::Shape::Single, cmo::Variant::Normal};
    deepModel.items.push_back(MakeDeepItem(20));
    deepCache.Put(deepModel);
    std::vector<uint8_t> deepBytes = deepCache.Serialize();
    cmo::Cache deepRestored;
    CHECK(!cmo::Cache::Deserialize(deepBytes, deepRestored));

    // Module stamps change when a recorded handler file changes.
    const std::wstring stampFile = L"cmo-test-storage\\stamp-test.bin";
    {
        HANDLE file = CreateFileW(stampFile.c_str(), GENERIC_WRITE, 0, nullptr,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(file, "a", 1, &written, nullptr);
            CloseHandle(file);
        }
    }
    const uint64_t stampBefore = cmo::ComputeModuleStamp({stampFile});
    Sleep(30);
    {
        HANDLE file = CreateFileW(stampFile.c_str(), GENERIC_WRITE, 0, nullptr,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(file, "abcd", 4, &written, nullptr);
            CloseHandle(file);
        }
    }
    const uint64_t stampAfter = cmo::ComputeModuleStamp({stampFile});
    CHECK(stampBefore != stampAfter);
    DeleteFileW(stampFile.c_str());

    cmo::SourceStamp stampA{1, 2};
    cmo::SourceStamp stampB{1, 2};
    cmo::SourceStamp stampC{1, 3};
    CHECK(cmo::StampMatches(stampA, stampB));
    CHECK(!cmo::StampMatches(stampA, stampC));

    std::vector<std::wstring> configuredTypes{L"TXT", L".PNG", L" .Zip ", L"txt", L""};
    std::vector<std::wstring> warmTypes = cmo::BuildWarmupTypes(configuredTypes);
    CHECK(warmTypes.size() == 8);
    CHECK_EQ(warmTypes[0], std::wstring(L"*"));
    CHECK_EQ(warmTypes[1], std::wstring(L"Directory"));
    CHECK_EQ(warmTypes[2], std::wstring(L"Directory\\Background"));
    CHECK_EQ(warmTypes[3], std::wstring(L"Desktop"));
    CHECK_EQ(warmTypes[4], std::wstring(L"Drive"));
    CHECK_EQ(warmTypes[5], std::wstring(L".txt"));
    CHECK_EQ(warmTypes[6], std::wstring(L".png"));
    CHECK_EQ(warmTypes[7], std::wstring(L".zip"));

    cmo::Warmup warmupState;
    CHECK(!warmupState.IsPaused());
    warmupState.SetMenuOpen(true);
    CHECK(warmupState.IsPaused());
    warmupState.SetMenuOpen(false);
    CHECK(!warmupState.IsPaused());

    // Stop must wake a paused worker and finish promptly.
    cmo::Warmup warmupThread;
    warmupThread.Start();
    Sleep(30);
    warmupThread.SetMenuOpen(true);
    warmupThread.Stop();
    CHECK(!warmupThread.IsPaused());

    CHECK(cmo::DecidePath(true, cmo::ShellViewKind::ShellDefView, true, true) ==
          cmo::MenuPath::NativeBypass);
    CHECK(cmo::DecidePath(false, cmo::ShellViewKind::ShellDefView, true, true) ==
          cmo::MenuPath::Ours);
    CHECK(cmo::DecidePath(false, cmo::ShellViewKind::NavPane, true, true) ==
          cmo::MenuPath::Passthrough);
    CHECK(cmo::DecidePath(false, cmo::ShellViewKind::ShellDefView, false, true) ==
          cmo::MenuPath::Passthrough);
    CHECK(cmo::DecidePath(true, cmo::ShellViewKind::Desktop, true, false) ==
          cmo::MenuPath::Ours);
    CHECK(cmo::DecidePath(true, cmo::ShellViewKind::ShellDefView, true, false) ==
          cmo::MenuPath::Ours);

    CHECK(cmo::ScopeFromKind(cmo::ShellViewKind::Desktop, false) == cmo::Scope::Files);
    CHECK(cmo::ScopeFromKind(cmo::ShellViewKind::Desktop, true) == cmo::Scope::Desktop);
    CHECK(cmo::ScopeFromKind(cmo::ShellViewKind::ShellDefView, false) == cmo::Scope::Files);
    CHECK(cmo::ScopeFromKind(cmo::ShellViewKind::ShellDefView, true) ==
          cmo::Scope::Background);
    CHECK(cmo::ScopeFromKind(cmo::ShellViewKind::NavPane, false) == cmo::Scope::NavPane);

    CHECK_EQ(cmo::FormatMultiLabel(L"Open", 3), std::wstring(L"Open 3 items"));
    CHECK_EQ(cmo::FormatMultiLabel(L"Copy", 1), std::wstring(L"Copy"));

    CHECK(cmo::ComputePasteEnabled(5, 5, true, false));
    CHECK(!cmo::ComputePasteEnabled(5, 5, false, true));
    CHECK(cmo::ComputePasteEnabled(5, 6, false, true));
    CHECK(!cmo::ComputePasteEnabled(5, 6, true, false));

    cmo::MenuModel folderModel =
        cmo::BuildCoreModel(cmo::Scope::Folders, onePath, cmo::Shape::Single);
    bool hasOpenInNewWindow = false;
    bool hasPin = false;
    for (const cmo::MenuItem& item : folderModel.items) {
        if (item.label == L"Open in new window") hasOpenInNewWindow = true;
        if (item.label == L"Pin to Quick access") hasPin = true;
    }
    CHECK(hasOpenInNewWindow);
    CHECK(hasPin);

    cmo::MenuModel backgroundModel =
        cmo::BuildCoreModel(cmo::Scope::Background, {}, cmo::Shape::Single);
    bool hasView = false;
    bool hasSort = false;
    bool hasRefresh = false;
    bool hasPasteItem = false;
    bool hasNew = false;
    for (const cmo::MenuItem& item : backgroundModel.items) {
        if (item.label == L"View") hasView = true;
        if (item.label == L"Sort by") hasSort = true;
        if (item.label == L"Refresh") hasRefresh = true;
        if (item.canonicalVerb == L"paste") hasPasteItem = true;
        if (item.label == L"New") hasNew = true;
    }
    CHECK(hasView);
    CHECK(hasSort);
    CHECK(hasRefresh);
    CHECK(hasPasteItem);
    CHECK(hasNew);

    cmo::MenuModel desktopModel =
        cmo::BuildCoreModel(cmo::Scope::Desktop, {}, cmo::Shape::Single);
    bool hasPersonalize = false;
    bool hasDisplaySettings = false;
    for (const cmo::MenuItem& item : desktopModel.items) {
        if (item.label == L"Personalize") hasPersonalize = true;
        if (item.label == L"Display settings") hasDisplaySettings = true;
    }
    CHECK(hasPersonalize);
    CHECK(hasDisplaySettings);

    CHECK(cmo::IsFilesystemContext(true, true));
    CHECK(!cmo::IsFilesystemContext(false, true));
    CHECK(!cmo::IsFilesystemContext(true, false));
    CHECK(cmo::RefineScope(cmo::Scope::Files, true, false) == cmo::Scope::Folders);
    CHECK(cmo::RefineScope(cmo::Scope::Files, false, true) == cmo::Scope::Drive);
    CHECK(cmo::RefineScope(cmo::Scope::Files, false, false) == cmo::Scope::Files);
    CHECK(cmo::RefineScope(cmo::Scope::Background, true, true) ==
          cmo::Scope::Background);
    CHECK(cmo::PathSetsEqual({L"a", L"b"}, {L"b", L"a"}));
    CHECK(!cmo::PathSetsEqual({L"a"}, {L"b"}));
    CHECK(!cmo::PathSetsEqual({L"a"}, {L"a", L"b"}));
    CHECK(cmo::ShouldShowNativeReplay(cmo::kModelOwnerDraw));
    CHECK(!cmo::ShouldShowNativeReplay(cmo::kModelNone));

    cmo::MenuModel dottedFolder =
        cmo::BuildCoreModel(cmo::Scope::Folders, {L"release.v1"}, cmo::Shape::Single);
    CHECK_EQ(dottedFolder.sig.typeKey, std::wstring(L"*"));

    // A failed menu construction is distinguishable from a dismissal.
    bool showFailed = false;
    auto showResult = cmo::NativeMenuView::Show(single, nullptr, POINT{0, 0}, &showFailed);
    CHECK(!showResult.has_value());
    CHECK(showFailed);

    std::wstring iconPath;
    int iconIndex = -1;
    CHECK(cmo::ParseIconRef(L"shell32.dll,3", iconPath, iconIndex));
    CHECK_EQ(iconPath, std::wstring(L"shell32.dll"));
    CHECK(iconIndex == 3);
    CHECK(cmo::ParseIconRef(L"shell32.dll", iconPath, iconIndex));
    CHECK_EQ(iconPath, std::wstring(L"shell32.dll"));
    CHECK(iconIndex == 0);
    CHECK(cmo::ParseIconRef(L"\"C:\\Program Files\\App\\app.exe\",12", iconPath,
                            iconIndex));
    CHECK_EQ(iconPath, std::wstring(L"C:\\Program Files\\App\\app.exe"));
    CHECK(iconIndex == 12);
    CHECK(!cmo::ParseIconRef(L"", iconPath, iconIndex));
    CHECK(cmo::ParseIconRef(L"file,notanumber", iconPath, iconIndex));
    CHECK_EQ(iconPath, std::wstring(L"file,notanumber"));

    cmo::Perf perf;
    perf.MarkOpenPathStart();
    CHECK(perf.OpenPathElapsedMs() == 0);

    // Fix regression tests: documented view actions and native descriptor
    // adoption.
    CHECK(cmo::FolderViewModeFor(cmo::ViewAction::ViewLargeIcons) == FVM_ICON);
    CHECK(cmo::FolderViewModeFor(cmo::ViewAction::ViewSmallIcons) == FVM_SMALLICON);
    CHECK(cmo::FolderViewModeFor(cmo::ViewAction::ViewList) == FVM_LIST);
    CHECK(cmo::FolderViewModeFor(cmo::ViewAction::ViewDetails) == FVM_DETAILS);
    CHECK(cmo::FolderViewModeFor(cmo::ViewAction::Rename) == FVM_ICON);

    bool renameIsViewAction = false;
    bool cutIsVerb = false;
    bool createShortcutIsVerb = false;
    for (const cmo::MenuItem& item : single.items) {
        if (item.label == L"Rename") {
            renameIsViewAction =
                item.action == cmo::ActionKind::ViewAction &&
                item.viewAction == static_cast<uint32_t>(cmo::ViewAction::Rename);
        }
        if (item.label == L"Cut") {
            cutIsVerb = item.action == cmo::ActionKind::ShellVerb &&
                        item.canonicalVerb == L"cut";
        }
        if (item.label == L"Create shortcut") {
            createShortcutIsVerb = item.action == cmo::ActionKind::ShellVerb &&
                                   item.canonicalVerb == L"createshortcut";
        }
    }
    CHECK(renameIsViewAction);
    CHECK(cutIsVerb);
    CHECK(createShortcutIsVerb);

    // Native descriptors are adopted for matching core items, including
    // offset-only ones.
    cmo::MenuModel adoptionCore = cmo::BuildCoreFileModel(onePath, cmo::Shape::Single);
    cmo::MenuModel adoptionCached{};
    adoptionCached.sig = adoptionCore.sig;
    cmo::MenuItem nativeCut{};
    nativeCut.id = 15000;
    nativeCut.kind = cmo::ItemKind::Command;
    nativeCut.action = cmo::ActionKind::ShellVerb;
    nativeCut.label = L"Cut";
    nativeCut.verbOffset = 7;
    adoptionCached.items.push_back(nativeCut);
    cmo::MenuModel adopted = cmo::MergeCoreWithCached(adoptionCore, adoptionCached);
    bool cutAdopted = false;
    for (const cmo::MenuItem& item : adopted.items) {
        if (item.label == L"Cut") {
            cutAdopted = (item.flags & cmo::kModelHasOffset) != 0 &&
                         item.verbOffset == 7;
        }
    }
    CHECK(cutAdopted);

    // Native items carry a valid offset; core items look up the native item's
    // offset before falling back to verb invocation.
    {
        HMENU nativeMenu = CreatePopupMenu();
        AppendMenuW(nativeMenu, MF_STRING, 11, L"Open");
        AppendMenuW(nativeMenu, MF_SEPARATOR, 0, nullptr);
        HMENU nativeSubmenu = CreatePopupMenu();
        AppendMenuW(nativeSubmenu, MF_STRING, 13, L"Copy");
        AppendMenuW(nativeMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(nativeSubmenu),
                    L"More");
        cmo::MenuItem copyTarget{};
        copyTarget.label = L"Copy";
        auto foundCopy = cmo::FindNativeOffsetInMenu(nativeMenu, 10, nullptr, copyTarget);
        CHECK(foundCopy.has_value() && *foundCopy == 3);
        cmo::MenuItem missingTarget{};
        missingTarget.label = L"Nope";
        CHECK(!cmo::FindNativeOffsetInMenu(nativeMenu, 10, nullptr, missingTarget)
                   .has_value());
        DestroyMenu(nativeMenu);

        // Host-style initialization sends WM_INITMENUPOPUP for the menu and
        // every submenu before discovery reads their items.
        HMENU initTop = CreatePopupMenu();
        HMENU initChild = CreatePopupMenu();
        AppendMenuW(initChild, MF_STRING, 41, L"Child");
        AppendMenuW(initTop, MF_POPUP, reinterpret_cast<UINT_PTR>(initChild),
                    L"Parent");
        FakeContextMenu2 fakeMenu;
        cmo::PendingCapture fakeCapture{};
        fakeCapture.contextMenu2 = &fakeMenu;
        cmo::InitializeMenuRecursive(fakeCapture, initTop, 0);
        CHECK(fakeMenu.handleMenuMsgCalls == 2);
        fakeCapture.contextMenu2 = nullptr;
        DestroyMenu(initTop);

        HMENU flagMenu = CreatePopupMenu();
        AppendMenuW(flagMenu, MF_STRING, 21, L"Item");
        cmo::MenuModel flagModel = cmo::BuildModelFromHMenu(
            flagMenu, 20,
            cmo::ContextSignature{cmo::Scope::Files, L".x", cmo::Shape::Single,
                                  cmo::Variant::Normal},
            nullptr);
        CHECK(!flagModel.items.empty());
        CHECK((flagModel.items.front().flags & cmo::kModelHasOffset) != 0);
        CHECK(flagModel.items.front().verbOffset == 1);
        DestroyMenu(flagMenu);
    }

    // Fix: only main file/folder menus are deferred. Flags are the exact
    // values seen in the on-device log.
    CHECK(cmo::ShouldDeferContextMenu(0x00020494));   // file menu
    CHECK(cmo::ShouldDeferContextMenu(0x00020594));   // Shift-extended menu
    CHECK(cmo::ShouldDeferContextMenu(0x00020424));   // background menu
    CHECK(!cmo::ShouldDeferContextMenu(0x00000805));  // CMF_DEFAULTONLY (open)
    CHECK(!cmo::ShouldDeferContextMenu(0x00000008));  // CMF_NOVERBS (Send to)
    CHECK(!cmo::ShouldDeferContextMenu(0x00000002));  // CMF_VERBSONLY

    // Fix: invocation descriptors carry both the ANSI and the wide verb.
    cmo::MenuItem bothItem{};
    bothItem.canonicalVerb = L"open";
    cmo::InvocationContext bothCtx{};
    std::string ansiStorage;
    CMINVOKECOMMANDINFOEX bothInfo = {};
    cmo::FillInvokeCommandInfo(bothItem, bothCtx, ansiStorage, bothInfo);
    CHECK(bothInfo.lpVerb != nullptr);
    CHECK(bothInfo.lpVerbW != nullptr);
    CHECK(strcmp(bothInfo.lpVerb, "open") == 0);
    CHECK(wcscmp(bothInfo.lpVerbW, L"open") == 0);

    cmo::MenuItem offsetOnlyItem{};
    offsetOnlyItem.verbOffset = 9;
    CMINVOKECOMMANDINFOEX offsetOnlyInfo = {};
    cmo::FillInvokeCommandInfo(offsetOnlyItem, bothCtx, ansiStorage, offsetOnlyInfo);
    CHECK(reinterpret_cast<UINT_PTR>(offsetOnlyInfo.lpVerb) == 9);
    CHECK(reinterpret_cast<UINT_PTR>(offsetOnlyInfo.lpVerbW) == 9);

    // Integration against the real shell32 in this Wine environment: the
    // shell reads lpVerb (ANSI) to tell offsets from verbs. A wide-only
    // descriptor is refused; setting both fields dispatches.
    {
        CreateDirectoryW(L"cmo-test-storage", nullptr);
        CreateDirectoryW(L"cmo-test-storage\\integration", nullptr);
        wchar_t currentDirectory[MAX_PATH] = {};
        GetCurrentDirectoryW(ARRAYSIZE(currentDirectory), currentDirectory);
        const std::wstring integrationFile =
            std::wstring(currentDirectory) +
            L"\\cmo-test-storage\\integration\\sample.txt";
        HANDLE file = CreateFileW(integrationFile.c_str(), GENERIC_WRITE, 0, nullptr,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(file, "x", 1, &written, nullptr);
            CloseHandle(file);
        }

        IContextMenu* shellMenu = cmo::CreateContextMenuForPath(integrationFile, false);
        CHECK(shellMenu != nullptr);
        if (shellMenu) {
            void** vtable = *reinterpret_cast<void***>(shellMenu);
            auto shellQueryContextMenu =
                reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(IContextMenu*, HMENU, UINT,
                                                             UINT, UINT, UINT)>(vtable[3]);
            HMENU shellHMenu = CreatePopupMenu();
            CHECK(shellHMenu != nullptr);
            CHECK(SUCCEEDED(shellQueryContextMenu(shellMenu, shellHMenu, 0, 1, 0x7FFF,
                                                  CMF_NORMAL)));
            CHECK(GetMenuItemCount(shellHMenu) > 0);

            CMINVOKECOMMANDINFOEX wideOnly = {};
            wideOnly.cbSize = sizeof(wideOnly);
            wideOnly.fMask = CMIC_MASK_UNICODE;
            wideOnly.lpVerbW = MAKEINTRESOURCEW(0);
            wideOnly.nShow = SW_SHOWNORMAL;
            const HRESULT wideOnlyResult = shellMenu->InvokeCommand(
                reinterpret_cast<CMINVOKECOMMANDINFO*>(&wideOnly));
            CHECK(FAILED(wideOnlyResult));

            // Find the native "copy" command with the production lookup and
            // dispatch it through the production descriptor (a harmless
            // clipboard action).
            cmo::MenuItem copyTarget{};
            copyTarget.canonicalVerb = L"copy";
            auto copyOffset =
                cmo::FindNativeOffsetInMenu(shellHMenu, 1, shellMenu, copyTarget);
            if (copyOffset.has_value()) {
                cmo::MenuItem copyItem{};
                copyItem.verbOffset = *copyOffset;
                cmo::InvocationContext copyCtx{};
                std::string copyAnsi;
                CMINVOKECOMMANDINFOEX copyInfo = {};
                cmo::FillInvokeCommandInfo(copyItem, copyCtx, copyAnsi, copyInfo);
                const HRESULT copyResult = shellMenu->InvokeCommand(
                    reinterpret_cast<CMINVOKECOMMANDINFO*>(&copyInfo));
                CHECK(copyResult != E_INVALIDARG);
            }

            DestroyMenu(shellHMenu);
            shellMenu->Release();
        }
    }

    if (g_failures == 0) {
        wprintf(L"ALL TESTS PASSED\n");
        return 0;
    }
    fprintf(stderr, "%d test(s) failed\n", g_failures);
    return 1;
}
