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

// Fake extension that paints a red square when asked to draw a menu item.
class DrawingContextMenu2 : public IContextMenu2 {
public:
    int drawCalls = 0;

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
    HRESULT STDMETHODCALLTYPE HandleMenuMsg(UINT uMsg, WPARAM, LPARAM lParam) override {
        if (uMsg != WM_DRAWITEM) {
            return S_OK;
        }
        ++drawCalls;
        auto* drawInfo = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
        RECT rect = {4, 4, 20, 20};
        HBRUSH brush = CreateSolidBrush(RGB(255, 0, 0));
        FillRect(drawInfo->hDC, &rect, brush);
        DeleteObject(brush);
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
    submenuItem.flags = cmo::kModelThirdParty;
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
    const cmo::MenuItem* restoredSubmenu =
        restoredModel ? cmo::FindById(*restoredModel, 500) : nullptr;
    CHECK(restoredSubmenu != nullptr);
    CHECK(restoredSubmenu &&
          (restoredSubmenu->flags & cmo::kModelThirdParty) != 0);

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
    auto showResult = cmo::NativeMenuView::Show(single, nullptr, POINT{0, 0},
                                                &showFailed);
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

    // Unlabeled items and submenus left empty are pruned: they cannot be
    // rendered faithfully and their cached offsets can dispatch the wrong
    // command.
    {
        cmo::MenuModel unlabeled{};
        cmo::MenuItem labeledItem{};
        labeledItem.id = 1;
        labeledItem.kind = cmo::ItemKind::Command;
        labeledItem.label = L"Open";
        cmo::MenuItem blankItem{};
        blankItem.id = 2;
        blankItem.kind = cmo::ItemKind::Command;
        cmo::MenuItem separatorItem{};
        separatorItem.id = 3;
        separatorItem.kind = cmo::ItemKind::Separator;
        cmo::MenuItem parentItem{};
        parentItem.id = 4;
        parentItem.kind = cmo::ItemKind::Submenu;
        parentItem.label = L"WinRAR";
        cmo::MenuItem blankChild{};
        blankChild.id = 5;
        blankChild.kind = cmo::ItemKind::Command;
        parentItem.children.push_back(blankChild);
        cmo::MenuItem blankParent{};
        blankParent.id = 6;
        blankParent.kind = cmo::ItemKind::Submenu;
        cmo::MenuItem goodParent{};
        goodParent.id = 7;
        goodParent.kind = cmo::ItemKind::Submenu;
        goodParent.label = L"Extras";
        cmo::MenuItem goodChild{};
        goodChild.id = 8;
        goodChild.kind = cmo::ItemKind::Command;
        goodChild.label = L"Extract";
        goodParent.children.push_back(goodChild);
        cmo::MenuItem emptyParent{};
        emptyParent.id = 9;
        emptyParent.kind = cmo::ItemKind::Submenu;
        emptyParent.label = L"Empty";
        unlabeled.items = {labeledItem, blankItem, separatorItem, parentItem,
                           blankParent, goodParent, emptyParent};
        cmo::PruneMenuItems(unlabeled.items);
        CHECK(unlabeled.items.size() == 3);
        CHECK(unlabeled.items[0].label == L"Open");
        CHECK(unlabeled.items[1].kind == cmo::ItemKind::Separator);
        CHECK(unlabeled.items[2].label == L"Extras");
        CHECK(unlabeled.items[2].children.size() == 1);
    }

    // Icons: core actions carry glyph references again.
    bool cutGlyph = false;
    bool renameGlyph = false;
    bool deleteGlyph = false;
    for (const cmo::MenuItem& item : single.items) {
        if (item.label == L"Cut") {
            cutGlyph = item.iconRef == L"@glyph:E8C6";
        }
        if (item.label == L"Rename") {
            renameGlyph = item.iconRef == L"@glyph:E8AC";
        }
        if (item.label == L"Delete") {
            deleteGlyph = item.iconRef == L"@glyph:E74D";
        }
    }
    CHECK(cutGlyph);
    CHECK(renameGlyph);
    CHECK(deleteGlyph);

    // Icons: pixel blobs round-trip through the cache format.
    {
        cmo::Cache pixelCache;
        cmo::MenuModel pixelModel = cmo::BuildCoreFileModel(onePath, cmo::Shape::Single);
        pixelModel.items.front().iconPixels.assign(16 * 16 * 4, 0x5A);
        pixelCache.Put(pixelModel);
        std::vector<uint8_t> pixelBytes = pixelCache.Serialize();
        cmo::Cache pixelRestored;
        CHECK(cmo::Cache::Deserialize(pixelBytes, pixelRestored));
        std::optional<cmo::MenuModel> pixelModelRestored =
            pixelRestored.Find(pixelModel.sig);
        CHECK(pixelModelRestored.has_value());
        CHECK(pixelModelRestored &&
              pixelModelRestored->items.front().iconPixels.size() == 16 * 16 * 4);
    }

    // Icons: pixel blobs produce cached 16x16 bitmaps.
    {
        std::vector<uint8_t> pixels(16 * 16 * 4);
        for (size_t i = 0; i < pixels.size(); i += 4) {
            pixels[i] = 0x20;
            pixels[i + 1] = 0x40;
            pixels[i + 2] = 0x60;
            pixels[i + 3] = 0xFF;
        }
        cmo::MenuItem iconItem{};
        iconItem.iconPixels = pixels;
        HBITMAP bitmap = cmo::g_iconCache.GetBitmap(iconItem, 16);
        CHECK(bitmap != nullptr);
        if (bitmap) {
            BITMAP bitmapInfo = {};
            CHECK(GetObjectW(bitmap, sizeof(bitmapInfo), &bitmapInfo) != 0);
            CHECK(bitmapInfo.bmWidth == 16 && bitmapInfo.bmHeight == 16);
            CHECK(cmo::g_iconCache.GetBitmap(iconItem, 16) == bitmap);
        }
    }

    // Icons: shell menu bitmaps are captured into the model during discovery.
    {
        BITMAPINFO dibInfo = {};
        dibInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        dibInfo.bmiHeader.biWidth = 16;
        dibInfo.bmiHeader.biHeight = -16;
        dibInfo.bmiHeader.biPlanes = 1;
        dibInfo.bmiHeader.biBitCount = 32;
        dibInfo.bmiHeader.biCompression = BI_RGB;

        HDC screen = GetDC(nullptr);
        void* bits = nullptr;
        HBITMAP dib =
            CreateDIBSection(screen, &dibInfo, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (dib && bits) {
            uint8_t* pixelBytes = static_cast<uint8_t*>(bits);
            for (int i = 0; i < 16 * 16; ++i) {
                pixelBytes[i * 4] = 0x11;
                pixelBytes[i * 4 + 1] = 0x22;
                pixelBytes[i * 4 + 2] = 0x33;
                pixelBytes[i * 4 + 3] = 0xFF;
            }

            HMENU captureMenu = CreatePopupMenu();
            AppendMenuW(captureMenu, MF_STRING, 7, L"Captured");
            MENUITEMINFOW menuItemInfo = {};
            menuItemInfo.cbSize = sizeof(menuItemInfo);
            menuItemInfo.fMask = MIIM_BITMAP;
            menuItemInfo.hbmpItem = dib;
            CHECK(SetMenuItemInfoW(captureMenu, 0, TRUE, &menuItemInfo));

            cmo::MenuModel captured = cmo::BuildModelFromHMenu(
                captureMenu, 1,
                cmo::ContextSignature{cmo::Scope::Files, L".x", cmo::Shape::Single,
                                      cmo::Variant::Normal},
                nullptr);
            CHECK(!captured.items.empty());
            CHECK(captured.items.front().iconPixels.size() == 16 * 16 * 4);
            if (captured.items.front().iconPixels.size() == 16 * 16 * 4) {
                CHECK(captured.items.front().iconPixels[0] == 0x11);
                CHECK(captured.items.front().iconPixels[3] == 0xFF);
            }
            DestroyMenu(captureMenu);
            DeleteObject(dib);
        }
        ReleaseDC(nullptr, screen);
    }

    // Icons: bitmaps attached with SetMenuItemBitmaps (no read-back API) are
    // recorded while the menu is populated and captured during discovery.
    {
        BITMAPINFO recordInfo = {};
        recordInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        recordInfo.bmiHeader.biWidth = 16;
        recordInfo.bmiHeader.biHeight = -16;
        recordInfo.bmiHeader.biPlanes = 1;
        recordInfo.bmiHeader.biBitCount = 32;
        recordInfo.bmiHeader.biCompression = BI_RGB;

        HDC screen = GetDC(nullptr);
        void* bits = nullptr;
        HBITMAP recorded =
            CreateDIBSection(screen, &recordInfo, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (recorded && bits) {
            uint8_t* pixelBytes = static_cast<uint8_t*>(bits);
            for (int i = 0; i < 16 * 16; ++i) {
                pixelBytes[i * 4] = 0x44;
                pixelBytes[i * 4 + 1] = 0x55;
                pixelBytes[i * 4 + 2] = 0x66;
                pixelBytes[i * 4 + 3] = 0xFF;
            }

            cmo::ClearRecordedItemBitmaps();
            cmo::SetMenuItemBitmaps_Original = &SetMenuItemBitmaps;
            HMENU recordMenu = CreatePopupMenu();
            AppendMenuW(recordMenu, MF_STRING, 21, L"Recorded");
            cmo::SetMenuItemBitmaps_Hook(recordMenu, 0, MF_BYPOSITION, recorded,
                                         nullptr);

            cmo::MenuModel recordedModel = cmo::BuildModelFromHMenu(
                recordMenu, 1,
                cmo::ContextSignature{cmo::Scope::Files, L".x", cmo::Shape::Single,
                                      cmo::Variant::Normal},
                nullptr);
            CHECK(!recordedModel.items.empty());
            CHECK(recordedModel.items.front().iconPixels.size() == 16 * 16 * 4);
            if (recordedModel.items.front().iconPixels.size() == 16 * 16 * 4) {
                CHECK(recordedModel.items.front().iconPixels[0] == 0x44);
                CHECK(recordedModel.items.front().iconPixels[3] == 0xFF);
            }
            cmo::ClearRecordedItemBitmaps();
            DestroyMenu(recordMenu);
            DeleteObject(recorded);
        }
        ReleaseDC(nullptr, screen);
    }

    // Icons: core items keep their own glyph icons; captured native bitmaps
    // belong to extension items only.
    {
        cmo::MenuModel iconCore = cmo::BuildCoreFileModel(onePath, cmo::Shape::Single);
        cmo::MenuModel iconCached{};
        iconCached.sig = iconCore.sig;
        cmo::MenuItem nativeCutItem{};
        nativeCutItem.id = 16000;
        nativeCutItem.kind = cmo::ItemKind::Command;
        nativeCutItem.action = cmo::ActionKind::ShellVerb;
        nativeCutItem.label = L"Cut";
        nativeCutItem.verbOffset = 4;
        nativeCutItem.iconPixels.assign(16 * 16 * 4, 0x33);
        iconCached.items.push_back(nativeCutItem);
        cmo::MenuModel iconMerged = cmo::MergeCoreWithCached(iconCore, iconCached);
        bool cutKeptGlyph = false;
        for (const cmo::MenuItem& item : iconMerged.items) {
            if (item.label == L"Cut") {
                cutKeptGlyph = item.iconRef == L"@glyph:E8C6" &&
                               item.iconPixels.empty();
            }
        }
        CHECK(cutKeptGlyph);
    }

    // Icons: glyph rendering must not crash when the icon font is absent.
    {
        cmo::MenuItem glyphItem{};
        glyphItem.iconRef = L"@glyph:E8C6";
        HBITMAP glyph = cmo::g_iconCache.GetBitmap(glyphItem, 16);
        (void)glyph;
    }

    // Icons: static verbs without a menu bitmap resolve their registry Icon.
    {
        HKEY verbKey = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER,
                            L"Software\\Classes\\cmotestfile\\shell\\cmoverb", 0,
                            nullptr, 0, KEY_WRITE, nullptr, &verbKey, nullptr) ==
            ERROR_SUCCESS) {
            const wchar_t iconValue[] = L"shell32.dll,3";
            RegSetValueExW(verbKey, L"Icon", 0, REG_SZ,
                           reinterpret_cast<const BYTE*>(iconValue),
                           sizeof(iconValue));
            RegCloseKey(verbKey);

            HKEY extKey = nullptr;
            if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\.cmotest",
                                0, nullptr, 0, KEY_WRITE, nullptr, &extKey,
                                nullptr) == ERROR_SUCCESS) {
                const wchar_t progId[] = L"cmotestfile";
                RegSetValueExW(extKey, nullptr, 0, REG_SZ,
                               reinterpret_cast<const BYTE*>(progId),
                               sizeof(progId));
                RegCloseKey(extKey);
            }

            cmo::ContextSignature sig{cmo::Scope::Files, L".cmotest",
                                      cmo::Shape::Single, cmo::Variant::Normal};
            CHECK_EQ(cmo::ResolveRegistryIcon(sig, L"cmoverb"),
                     std::wstring(L"shell32.dll,3"));

            RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\cmotestfile");
            RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\.cmotest");
        }
    }

    // Icons: alpha-less bitmaps treat black as the transparent color key.
    {
        std::vector<uint8_t> keyedPixels(16 * 16 * 4, 0);
        // One red pixel, black everywhere else, alpha all zero.
        keyedPixels[(5 * 16 + 5) * 4] = 0x00;
        keyedPixels[(5 * 16 + 5) * 4 + 1] = 0x00;
        keyedPixels[(5 * 16 + 5) * 4 + 2] = 0xFF;
        keyedPixels[(5 * 16 + 5) * 4 + 3] = 0x00;

        cmo::MenuItem keyedItem{};
        keyedItem.iconPixels = keyedPixels;
        HBITMAP keyedBitmap = cmo::g_iconCache.GetBitmap(keyedItem, 16);
        CHECK(keyedBitmap != nullptr);
        if (keyedBitmap) {
            BITMAPINFO readInfo = {};
            readInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            readInfo.bmiHeader.biWidth = 16;
            readInfo.bmiHeader.biHeight = 16;
            readInfo.bmiHeader.biPlanes = 1;
            readInfo.bmiHeader.biBitCount = 32;
            readInfo.bmiHeader.biCompression = BI_RGB;
            std::vector<uint8_t> readPixels(16 * 16 * 4);
            HDC readDc = GetDC(nullptr);
            const int readLines = GetDIBits(readDc, keyedBitmap, 0, 16,
                                            readPixels.data(), &readInfo, DIB_RGB_COLORS);
            ReleaseDC(nullptr, readDc);
            CHECK(readLines == 16);
            if (readLines == 16) {
                // Bottom-up rows: the red pixel is at row 16-1-5.
                const size_t redIndex = ((16 - 1 - 5) * 16 + 5) * 4;
                const size_t cornerIndex = 0;
                CHECK(readPixels[redIndex + 2] > 200);
                CHECK(readPixels[cornerIndex] != 0 ||
                      readPixels[cornerIndex + 1] != 0 ||
                      readPixels[cornerIndex + 2] != 0);
            }
        }
    }

    // Icons: label-based registry lookup finds handlers whose key name
    // differs from the canonical verb.
    {
        HKEY labelKey = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER,
                            L"Software\\Classes\\*\\shell\\cmolabel", 0, nullptr, 0,
                            KEY_WRITE, nullptr, &labelKey, nullptr) == ERROR_SUCCESS) {
            const wchar_t muiVerb[] = L"Cmo Label";
            RegSetValueExW(labelKey, L"MUIVerb", 0, REG_SZ,
                           reinterpret_cast<const BYTE*>(muiVerb), sizeof(muiVerb));
            const wchar_t iconValue[] = L"shell32.dll,4";
            RegSetValueExW(labelKey, L"Icon", 0, REG_SZ,
                           reinterpret_cast<const BYTE*>(iconValue),
                           sizeof(iconValue));
            RegCloseKey(labelKey);

            cmo::ContextSignature labelSig{cmo::Scope::Files, L".cmotest2",
                                           cmo::Shape::Single, cmo::Variant::Normal};
            CHECK_EQ(cmo::ResolveRegistryIconByLabel(labelSig, L"Cmo Label"),
                     std::wstring(L"shell32.dll,4"));

            RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\*\\shell\\cmolabel");
        }
    }

    // Owner-draw icons are captured from WM_DRAWITEM.
    {
        DrawingContextMenu2 drawingMenu;
        cmo::PendingCapture drawCapture{};
        drawCapture.contextMenu2 = &drawingMenu;
        drawCapture.idCmdFirst = 1;
        cmo::MenuItem drawItem{};
        drawItem.label = L"Drawn";
        drawItem.verbOffset = 0;
        std::vector<uint8_t> drawnPixels;
        CHECK(cmo::CaptureOwnerDrawIcon(drawCapture, drawItem, drawnPixels));
        CHECK(drawingMenu.drawCalls == 1);
        CHECK(!drawnPixels.empty());
        bool foundRed = false;
        for (size_t i = 0; i + 2 < drawnPixels.size(); i += 4) {
            if (drawnPixels[i] < 64 && drawnPixels[i + 1] < 64 &&
                drawnPixels[i + 2] > 200) {
                foundRed = true;
                break;
            }
        }
        CHECK(foundRed);
        drawCapture.contextMenu2 = nullptr;
    }

    // Icons captured before host initialization are merged onto the
    // post-init model.
    {
        std::vector<cmo::MenuItem> post;
        cmo::MenuItem postItem{};
        postItem.id = 1;
        postItem.kind = cmo::ItemKind::Command;
        postItem.action = cmo::ActionKind::ShellVerb;
        postItem.canonicalVerb = L"tsvn_checkout";
        postItem.label = L"SVN Checkout...";
        post.push_back(postItem);

        std::vector<cmo::MenuItem> pre;
        cmo::MenuItem preItem{};
        preItem.id = 1;
        preItem.kind = cmo::ItemKind::Command;
        preItem.action = cmo::ActionKind::ShellVerb;
        preItem.canonicalVerb = L"tsvn_checkout";
        preItem.label = L"SVN Checkout...";
        preItem.iconPixels.assign(24 * 24 * 4, 0x11);
        pre.push_back(preItem);

        cmo::MergePreInitIcons(post, pre);
        CHECK(post.front().iconPixels.size() == 24 * 24 * 4);
    }

    // Warm-up models never overwrite live ones, and a warm-up entry can be
    // replaced by a live one.
    {
        cmo::Cache warmCache;
        cmo::MenuModel liveModel =
            cmo::BuildCoreFileModel({L"a.txt"}, cmo::Shape::Single);
        warmCache.Put(liveModel);
        cmo::MenuModel warmModel =
            cmo::BuildCoreFileModel({L"a.txt"}, cmo::Shape::Single);
        warmModel.flags = cmo::kModelWarmup;
        warmCache.Put(warmModel);
        std::optional<cmo::MenuModel> kept = warmCache.Find(liveModel.sig);
        CHECK(kept.has_value());
        CHECK(kept && !(kept->flags & cmo::kModelWarmup));

        cmo::Cache warmFirst;
        warmFirst.Put(warmModel);
        warmFirst.Put(liveModel);
        std::optional<cmo::MenuModel> replaced = warmFirst.Find(liveModel.sig);
        CHECK(replaced.has_value());
        CHECK(replaced && !(replaced->flags & cmo::kModelWarmup));
    }

    // Handler DLL fallback resolves an icon for extension-added items whose
    // label matches the handler key name.
    {
        const wchar_t* clsid = L"{11111111-2222-3333-4444-555555555555}";
        HKEY handlerKey = nullptr;
        if (RegCreateKeyExW(
                HKEY_CURRENT_USER,
                L"Software\\Classes\\*\\shellex\\ContextMenuHandlers\\CmoHandler", 0,
                nullptr, 0, KEY_WRITE, nullptr, &handlerKey, nullptr) ==
            ERROR_SUCCESS) {
            RegSetValueExW(handlerKey, nullptr, 0, REG_SZ,
                           reinterpret_cast<const BYTE*>(clsid),
                           static_cast<DWORD>((wcslen(clsid) + 1) * sizeof(wchar_t)));
            RegCloseKey(handlerKey);

            HKEY dllKey = nullptr;
            if (RegCreateKeyExW(
                    HKEY_CURRENT_USER,
                    L"Software\\Classes\\CLSID\\{11111111-2222-3333-4444-555555555555}\\InprocServer32",
                    0, nullptr, 0, KEY_WRITE, nullptr, &dllKey, nullptr) ==
                ERROR_SUCCESS) {
                const wchar_t dllPath[] = L"%SystemRoot%\\System32\\shell32.dll";
                RegSetValueExW(dllKey, nullptr, 0, REG_SZ,
                               reinterpret_cast<const BYTE*>(dllPath),
                               static_cast<DWORD>(sizeof(dllPath)));
                RegCloseKey(dllKey);
            }

            cmo::ContextSignature handlerSig{cmo::Scope::Files, L".cmotest3",
                                             cmo::Shape::Single,
                                             cmo::Variant::Normal};
            const std::wstring handlerIcon =
                cmo::ResolveHandlerDllIconByLabel(handlerSig, L"Scan with CmoHandler");
            CHECK(!handlerIcon.empty());
            CHECK(handlerIcon.find(L",0") != std::wstring::npos);

            // The same matching classifies the item as third-party for the
            // advanced submenu.
            std::wstring matchedDll;
            CHECK(cmo::LabelMatchesRegisteredHandler(handlerSig,
                                                     L"Scan with CmoHandler",
                                                     &matchedDll));
            CHECK(!matchedDll.empty());
            CHECK(!cmo::LabelMatchesRegisteredHandler(handlerSig, L"Nothing Related",
                                                      nullptr));

            std::vector<cmo::MenuItem> classified(1);
            classified[0].kind = cmo::ItemKind::Command;
            classified[0].action = cmo::ActionKind::ShellVerb;
            classified[0].label = L"Scan with CmoHandler";
            cmo::ApplyRegistryIcons(classified, handlerSig);
            CHECK((classified[0].flags & cmo::kModelThirdParty) != 0);

            // Generic key name: match by the handler DLL name instead, with a
            // quoted registered path.
            HKEY genericKey = nullptr;
            if (RegCreateKeyExW(
                    HKEY_CURRENT_USER,
                    L"Software\\Classes\\*\\shellex\\ContextMenuHandlers\\ZZNoMatch",
                    0, nullptr, 0, KEY_WRITE, nullptr, &genericKey, nullptr) ==
                ERROR_SUCCESS) {
                const wchar_t clsid2[] = L"{22222222-3333-4444-5555-666666666666}";
                RegSetValueExW(genericKey, nullptr, 0, REG_SZ,
                               reinterpret_cast<const BYTE*>(clsid2),
                               static_cast<DWORD>(sizeof(clsid2)));
                RegCloseKey(genericKey);

                HKEY dllKey2 = nullptr;
                if (RegCreateKeyExW(
                        HKEY_CURRENT_USER,
                        L"Software\\Classes\\CLSID\\{22222222-3333-4444-5555-666666666666}\\InprocServer32",
                        0, nullptr, 0, KEY_WRITE, nullptr, &dllKey2, nullptr) ==
                    ERROR_SUCCESS) {
                    const wchar_t quotedDll[] =
                        L"\"%SystemRoot%\\System32\\shell32.dll\"";
                    RegSetValueExW(dllKey2, nullptr, 0, REG_SZ,
                                   reinterpret_cast<const BYTE*>(quotedDll),
                                   static_cast<DWORD>(sizeof(quotedDll)));
                    RegCloseKey(dllKey2);
                }

                const std::wstring dllIcon =
                    cmo::ResolveHandlerDllIconByLabel(handlerSig, L"Scan with shell32");
                CHECK(!dllIcon.empty());
                CHECK(dllIcon.find(L",0") != std::wstring::npos);

                RegDeleteTreeW(
                    HKEY_CURRENT_USER,
                    L"Software\\Classes\\*\\shellex\\ContextMenuHandlers\\ZZNoMatch");
                RegDeleteTreeW(
                    HKEY_CURRENT_USER,
                    L"Software\\Classes\\CLSID\\{22222222-3333-4444-5555-666666666666}");
            }

            RegDeleteTreeW(
                HKEY_CURRENT_USER,
                L"Software\\Classes\\*\\shellex\\ContextMenuHandlers\\CmoHandler");
            RegDeleteTreeW(
                HKEY_CURRENT_USER,
                L"Software\\Classes\\CLSID\\{11111111-2222-3333-4444-555555555555}");
        }
    }

    // Handler matching helpers: word matching for version info and the
    // no-crash path for DLLs without version info.
    CHECK(cmo::LabelMatchesWords(L"Scan with Malwarebytes", L"Malwarebytes Inc."));
    CHECK(!cmo::LabelMatchesWords(L"Scan with Malwarebytes", L"Contoso Ltd."));
    CHECK(!cmo::VersionInfoMatchesLabel(L"Z:\\nonexistent\\nope.dll",
                                        L"Anything"));

    // Advanced submenu: configured built-ins and third-party entries move
    // into one submenu placed above the native fallback, with separators
    // collapsed. Disabled or empty cases leave the menu untouched.
    {
        const bool previousEnabled = cmo::g_settings.advancedSubmenu;
        const std::wstring previousLabel = cmo::g_settings.advancedSubmenuLabel;
        const std::vector<std::wstring> previousItems =
            cmo::g_settings.advancedSubmenuItems;

        CHECK(cmo::ParseAdvancedItems(L" Pin to Start ,, Open in Terminal ,").size() ==
              2);
        CHECK(cmo::ParseAdvancedItems(L" ").empty());

        auto makeCommand = [](uint32_t id, const wchar_t* label, const wchar_t* verb,
                              uint32_t flags) {
            cmo::MenuItem item{};
            item.id = id;
            item.kind = cmo::ItemKind::Command;
            item.action = cmo::ActionKind::ShellVerb;
            item.label = label;
            item.canonicalVerb = verb;
            item.flags = flags;
            return item;
        };
        auto makeSeparator = [](uint32_t id) {
            cmo::MenuItem item{};
            item.id = id;
            item.kind = cmo::ItemKind::Separator;
            return item;
        };

        auto buildMenu = [&]() {
            cmo::MenuModel model{};
            model.sig = cmo::ContextSignature{cmo::Scope::Files, L".x",
                                              cmo::Shape::Single,
                                              cmo::Variant::Normal};
            model.items.push_back(makeCommand(1, L"Open", L"open", 0));
            model.items.push_back(makeSeparator(2));
            model.items.push_back(makeCommand(3, L"Scan with Malwarebytes", L"",
                                              cmo::kModelThirdParty));
            model.items.push_back(makeCommand(4, L"Pin to Start", L"", 0));
            model.items.push_back(makeCommand(5, L"Share", L"Windows.ModernShare", 0));
            model.items.push_back(makeSeparator(6));
            cmo::MenuItem fallback = makeCommand(7, L"Show more options", L"", 0);
            fallback.action = cmo::ActionKind::Fallback;
            model.items.push_back(fallback);
            return model;
        };

        cmo::g_settings.advancedSubmenu = false;
        cmo::g_settings.advancedSubmenuLabel = L"Advanced";
        cmo::g_settings.advancedSubmenuItems =
            cmo::ParseAdvancedItems(L"Pin to Start, Open in Terminal");
        {
            cmo::MenuModel untouched = buildMenu();
            cmo::ReorganizeAdvancedItems(untouched.items);
            CHECK(untouched.items.size() == 7);
        }

        cmo::g_settings.advancedSubmenu = true;
        {
            cmo::MenuModel model = buildMenu();
            cmo::ReorganizeAdvancedItems(model.items);
            // Open, separator, Share, Advanced, separator, fallback.
            CHECK(model.items.size() == 6);
            CHECK(model.items.back().action == cmo::ActionKind::Fallback);
            const cmo::MenuItem* advanced = nullptr;
            for (const cmo::MenuItem& item : model.items) {
                if (item.label == L"Advanced") {
                    advanced = &item;
                }
            }
            CHECK(advanced != nullptr);
            CHECK(advanced && advanced->kind == cmo::ItemKind::Submenu);
            CHECK(advanced && advanced->iconRef == L"@glyph:E712");
            CHECK(advanced && advanced->children.size() == 2);
            CHECK(advanced && advanced->children[0].label == L"Scan with Malwarebytes");
            CHECK(advanced && advanced->children[1].label == L"Pin to Start");
            // Moved children stay reachable for invocation by id.
            const cmo::MenuItem* moved = cmo::FindById(model, 3);
            CHECK(moved != nullptr);
            CHECK(moved && moved->label == L"Scan with Malwarebytes");
            bool doubledSeparator = false;
            for (size_t i = 1; i < model.items.size(); ++i) {
                if (model.items[i].kind == cmo::ItemKind::Separator &&
                    model.items[i - 1].kind == cmo::ItemKind::Separator) {
                    doubledSeparator = true;
                }
            }
            CHECK(!doubledSeparator);
        }

        // No advanced items: no submenu is added.
        cmo::g_settings.advancedSubmenuItems.clear();
        {
            cmo::MenuModel model = buildMenu();
            model.items.erase(model.items.begin() + 2);
            cmo::ReorganizeAdvancedItems(model.items);
            CHECK(model.items.size() == 6);
            for (const cmo::MenuItem& item : model.items) {
                CHECK(item.label != L"Advanced");
            }
        }

        cmo::g_settings.advancedSubmenu = previousEnabled;
        cmo::g_settings.advancedSubmenuLabel = previousLabel;
        cmo::g_settings.advancedSubmenuItems = previousItems;
    }

    // Ampersand accelerators and trailing ellipses are normalized before
    // comparing labels: the shell's raw labels ("Add to &Favorites") never
    // match what the user sees or types.
    {
        CHECK(cmo::NormalizeMenuLabel(L"Add to &Favorites") == L"Add to Favorites");
        CHECK(cmo::NormalizeMenuLabel(L"Open wit&h...") == L"Open with");
        CHECK(cmo::NormalizeMenuLabel(L"Smith && Sons") == L"Smith & Sons");

        const bool previousEnabled = cmo::g_settings.advancedSubmenu;
        const std::wstring previousLabel = cmo::g_settings.advancedSubmenuLabel;
        const std::vector<std::wstring> previousItems =
            cmo::g_settings.advancedSubmenuItems;
        cmo::g_settings.advancedSubmenu = true;
        cmo::g_settings.advancedSubmenuLabel = L"More options";
        cmo::g_settings.advancedSubmenuItems =
            cmo::ParseAdvancedItems(L"Add to Favorites, Open with");

        auto extensionCommand = [](uint32_t id, const wchar_t* label,
                                   const wchar_t* verb) {
            cmo::MenuItem item{};
            item.id = id;
            item.kind = cmo::ItemKind::Command;
            item.action = cmo::ActionKind::ShellVerb;
            item.label = label;
            item.canonicalVerb = verb;
            item.flags = cmo::kModelExtension;
            return item;
        };

        CHECK(cmo::IsAdvancedItem(
            extensionCommand(1, L"Add to &Favorites", L"pintohomefile")));
        CHECK(cmo::IsAdvancedItem(extensionCommand(2, L"Open wit&h...", L"openas")));
        // Unknown verbs are third-party handlers.
        CHECK(cmo::IsAdvancedItem(
            extensionCommand(3, L"Extract Here", L"WinRAR.ExtractHere")));
        CHECK(cmo::IsAdvancedItem(extensionCommand(4, L"Scan with Malwarebytes", L"")));
        // Known Windows verbs stay unless listed.
        CHECK(!cmo::IsAdvancedItem(extensionCommand(5, L"Print", L"Print")));
        CHECK(!cmo::IsAdvancedItem(extensionCommand(6, L"Cu&t", L"cut")));

        cmo::g_settings.advancedSubmenu = previousEnabled;
        cmo::g_settings.advancedSubmenuLabel = previousLabel;
        cmo::g_settings.advancedSubmenuItems = previousItems;
    }

    // Merge dedup normalizes accelerators and ellipses so the shell's version
    // of a core item is not shown twice; the native offset is adopted.
    {
        cmo::MenuModel core = cmo::BuildCoreFileModel(onePath, cmo::Shape::Single);
        cmo::MenuModel cached{};
        cached.sig = core.sig;
        cmo::MenuItem shortcut{};
        shortcut.id = 20001;
        shortcut.kind = cmo::ItemKind::Command;
        shortcut.action = cmo::ActionKind::ShellVerb;
        shortcut.label = L"Create &shortcut";
        shortcut.canonicalVerb = L"link";
        shortcut.verbOffset = 33;
        shortcut.flags = cmo::kModelHasOffset | cmo::kModelExtension;
        cached.items.push_back(shortcut);
        cmo::MenuItem openWith{};
        openWith.id = 20002;
        openWith.kind = cmo::ItemKind::Command;
        openWith.action = cmo::ActionKind::ShellVerb;
        openWith.label = L"Open wit&h...";
        openWith.canonicalVerb = L"openas";
        openWith.verbOffset = 34;
        openWith.flags = cmo::kModelHasOffset | cmo::kModelExtension;
        cached.items.push_back(openWith);

        cmo::MenuModel merged = cmo::MergeCoreWithCached(core, cached);
        int shortcutCount = 0;
        int openWithCount = 0;
        bool adoptedShortcut = false;
        bool adoptedOpenWith = false;
        for (const cmo::MenuItem& item : merged.items) {
            if (cmo::NormalizeMenuLabel(item.label) == L"Create shortcut") {
                ++shortcutCount;
                adoptedShortcut = adoptedShortcut || item.verbOffset == 33;
            }
            if (cmo::NormalizeMenuLabel(item.label) == L"Open with") {
                ++openWithCount;
                adoptedOpenWith = adoptedOpenWith || item.verbOffset == 34;
            }
        }
        CHECK(shortcutCount == 1);
        CHECK(openWithCount == 1);
        CHECK(adoptedShortcut);
        CHECK(adoptedOpenWith);
    }

    // Instant menu open: while the suppressor is active the master menu
    // animation switch is off, and it is restored afterwards. Skipped when
    // the environment does not implement the SPI.
    {
        BOOL originalAnimation = TRUE;
        if (SystemParametersInfoW(SPI_GETMENUANIMATION, 0, &originalAnimation, 0)) {
            {
                cmo::MenuAnimationSuppressor suppressor;
                BOOL duringAnimation = TRUE;
                CHECK(SystemParametersInfoW(SPI_GETMENUANIMATION, 0, &duringAnimation,
                                            0));
                CHECK(duringAnimation == FALSE);
            }
            BOOL restoredAnimation = !originalAnimation;
            CHECK(SystemParametersInfoW(SPI_GETMENUANIMATION, 0, &restoredAnimation, 0));
            CHECK(restoredAnimation == originalAnimation);
        }
    }

    if (g_failures == 0) {
        wprintf(L"ALL TESTS PASSED\n");
        return 0;
    }
    fprintf(stderr, "%d test(s) failed\n", g_failures);
    return 1;
}
