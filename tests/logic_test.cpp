#define CMO_TESTING 1
#include "wh_api_stub.h"
#include "../mod.wh.cpp"
#include <functional>

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
    CHECK(cmo::IsReplaceableKind(cmo::ShellViewKind::NavPane));

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
    CHECK_EQ(single.items.back().label, std::wstring(L"Show classic menu"));
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
    CHECK_EQ(mergedModel.items.back().label, std::wstring(L"Show classic menu"));

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
    CHECK(restoredModel && restoredModel->items.back().label == L"Show classic menu");
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
          cmo::MenuPath::Ours);
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

    // Sort by / Group by are real submenus backed by documented IFolderView2
    // operations.
    {
        const cmo::MenuItem* sortBy = nullptr;
        const cmo::MenuItem* groupBy = nullptr;
        for (const cmo::MenuItem& item : backgroundModel.items) {
            if (item.label == L"Sort by") sortBy = &item;
            if (item.label == L"Group by") groupBy = &item;
        }
        CHECK(sortBy && sortBy->kind == cmo::ItemKind::Submenu);
        CHECK(sortBy && sortBy->iconRef == L"@glyph:E8CB");
        CHECK(sortBy && sortBy->children.size() == 7);
        CHECK(groupBy && groupBy->kind == cmo::ItemKind::Submenu);
        CHECK(groupBy && groupBy->iconRef == L"@glyph:E902");
        CHECK(groupBy && groupBy->children.size() == 8);
        bool hasSortName = false;
        bool hasSortDescending = false;
        bool hasGroupType = false;
        bool hasGroupNone = false;
        bool hasGroupDescending = false;
        if (sortBy) {
            for (const cmo::MenuItem& child : sortBy->children) {
                if (child.label == L"Name" &&
                    child.action == cmo::ActionKind::SortBy && child.sortIndex == 0) {
                    hasSortName = true;
                }
                if (child.label == L"Descending" &&
                    child.action == cmo::ActionKind::SortDirection &&
                    !child.sortAscending) {
                    hasSortDescending = true;
                }
            }
        }
        if (groupBy) {
            for (const cmo::MenuItem& child : groupBy->children) {
                if (child.label == L"Type" &&
                    child.action == cmo::ActionKind::GroupBy && child.sortIndex == 2) {
                    hasGroupType = true;
                }
                if (child.label == L"(None)" &&
                    child.action == cmo::ActionKind::GroupBy &&
                    child.sortIndex == cmo::kGroupNoneIndex) {
                    hasGroupNone = true;
                }
                if (child.label == L"Descending" &&
                    child.action == cmo::ActionKind::GroupDirection &&
                    !child.sortAscending) {
                    hasGroupDescending = true;
                }
            }
        }
        CHECK(hasSortName);
        CHECK(hasSortDescending);
        CHECK(hasGroupType);
        CHECK(hasGroupNone);
        CHECK(hasGroupDescending);

        // View offers the full set of modes plus arrange options.
        const cmo::MenuItem* viewSubmenu = nullptr;
        for (const cmo::MenuItem& item : backgroundModel.items) {
            if (item.label == L"View") {
                viewSubmenu = &item;
            }
        }
        CHECK(viewSubmenu && viewSubmenu->children.size() == 11);
        bool hasExtraLarge = false;
        bool hasTiles = false;
        bool hasAutoArrange = false;
        if (viewSubmenu) {
            for (const cmo::MenuItem& child : viewSubmenu->children) {
                if (child.label == L"Extra large icons" && child.iconSize == 256) {
                    hasExtraLarge = true;
                }
                if (child.label == L"Tiles" &&
                    child.action == cmo::ActionKind::ViewAction) {
                    hasTiles = true;
                }
                if (child.label == L"Auto arrange icons" &&
                    child.action == cmo::ActionKind::ViewAction) {
                    hasAutoArrange = true;
                }
            }
        }
        CHECK(hasExtraLarge);
        CHECK(hasTiles);
        CHECK(hasAutoArrange);
    }

    cmo::MenuModel desktopModel =
        cmo::BuildCoreModel(cmo::Scope::Desktop, {}, cmo::Shape::Single);
    bool hasPersonalize = false;
    bool hasDisplaySettings = false;
    for (const cmo::MenuItem& item : desktopModel.items) {
        if (item.label == L"Personalize") {
            hasPersonalize = true;
            CHECK(item.iconRef == L"@glyph:E790");
        }
        if (item.label == L"Display settings") {
            hasDisplaySettings = true;
            CHECK(item.iconRef == L"@glyph:E7F4");
        }
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

    // A cached submenu must not donate its empty descriptor to the core "New"
    // submenu; the cached duplicate is dropped instead of clearing anything or
    // invoking offset 0. New's children come from the ShellNew templates.
    {
        cmo::MenuModel newCore =
            cmo::BuildCoreModel(cmo::Scope::Desktop, {}, cmo::Shape::Single);
        cmo::MenuModel newCached{};
        newCached.sig = newCore.sig;
        cmo::MenuItem newSubmenu{};
        newSubmenu.id = 21001;
        newSubmenu.kind = cmo::ItemKind::Submenu;
        newSubmenu.action = cmo::ActionKind::Submenu;
        newSubmenu.label = L"&New";
        cmo::MenuItem newChild{};
        newChild.id = 21002;
        newChild.kind = cmo::ItemKind::Command;
        newChild.action = cmo::ActionKind::ShellVerb;
        newChild.label = L"Folder";
        newChild.canonicalVerb = L"shellnew";
        newChild.verbOffset = 12;
        newSubmenu.children.push_back(newChild);
        newCached.items.push_back(newSubmenu);

        cmo::MenuModel newMerged = cmo::MergeCoreWithCached(newCore, newCached);
        int newCount = 0;
        for (const cmo::MenuItem& item : newMerged.items) {
            if (cmo::NormalizeMenuLabel(item.label) != L"New") {
                continue;
            }
            ++newCount;
            CHECK(item.kind == cmo::ItemKind::Submenu);
            CHECK((item.flags & cmo::kModelHasOffset) == 0);
        }
        CHECK(newCount == 1);
    }

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
    // The desktop passes the same flags minus CMF_EXPLORE.
    CHECK(cmo::ShouldDeferContextMenu(0x00020490));   // desktop icon menu
    CHECK(cmo::ShouldDeferContextMenu(0x00020420));   // desktop background menu
    CHECK(!cmo::ShouldDeferContextMenu(0x00000805));  // CMF_DEFAULTONLY (open)
    CHECK(!cmo::ShouldDeferContextMenu(0x00000008));  // CMF_NOVERBS (Send to)
    CHECK(!cmo::ShouldDeferContextMenu(0x00000002));  // CMF_VERBSONLY
    CHECK(!cmo::ShouldDeferContextMenu(0x00000800));  // verb-state query
    CHECK(!cmo::ShouldDeferContextMenu(0x00008100));  // submenu build

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
        const bool previousWindows = cmo::g_settings.advancedSubmenuWindows;
        const bool previousThirdParty = cmo::g_settings.advancedSubmenuThirdParty;
        const std::wstring previousLabel = cmo::g_settings.advancedSubmenuLabel;
        const std::vector<std::wstring> previousItems =
            cmo::g_settings.advancedSubmenuItems;
        const std::vector<std::wstring> previousExclude =
            cmo::g_settings.advancedSubmenuExclude;

        auto reorganize = [](std::vector<cmo::MenuItem>& items) {
            cmo::AdvancedGroupingOptions options;
            options.moveWindows = cmo::g_settings.advancedSubmenuWindows;
            options.moveThirdParty = cmo::g_settings.advancedSubmenuThirdParty;
            options.label = cmo::g_settings.advancedSubmenuLabel;
            options.windowsItems = cmo::g_settings.advancedSubmenuItems;
            options.exclude = cmo::g_settings.advancedSubmenuExclude;
            cmo::ReorganizeAdvancedItems(items, options);
        };

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
            cmo::MenuItem fallback = makeCommand(7, L"Show classic menu", L"", 0);
            fallback.action = cmo::ActionKind::Fallback;
            model.items.push_back(fallback);
            return model;
        };

        cmo::g_settings.advancedSubmenuWindows = false;
        cmo::g_settings.advancedSubmenuThirdParty = false;
        cmo::g_settings.advancedSubmenuLabel = L"Advanced";
        cmo::g_settings.advancedSubmenuItems =
            cmo::ParseAdvancedItems(L"Pin to Start, Open in Terminal");
        {
            cmo::MenuModel untouched = buildMenu();
            reorganize(untouched.items);
            CHECK(untouched.items.size() == 7);
        }

        cmo::g_settings.advancedSubmenuWindows = true;
        cmo::g_settings.advancedSubmenuThirdParty = true;
        {
            cmo::MenuModel model = buildMenu();
            reorganize(model.items);
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
            CHECK(advanced && advanced->children.size() == 3);
            // Built-ins above, third-party handlers below, one separator.
            CHECK(advanced && advanced->children[0].label == L"Pin to Start");
            CHECK(advanced && advanced->children[1].kind == cmo::ItemKind::Separator);
            CHECK(advanced && advanced->children[2].label == L"Scan with Malwarebytes");
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
            reorganize(model.items);
            CHECK(model.items.size() == 6);
            for (const cmo::MenuItem& item : model.items) {
                CHECK(item.label != L"Advanced");
            }
        }

        cmo::g_settings.advancedSubmenuWindows = previousWindows;
        cmo::g_settings.advancedSubmenuThirdParty = previousThirdParty;
        cmo::g_settings.advancedSubmenuLabel = previousLabel;
        cmo::g_settings.advancedSubmenuItems = previousItems;
        cmo::g_settings.advancedSubmenuExclude = previousExclude;
    }

    // Ampersand accelerators and trailing ellipses are normalized before
    // comparing labels: the shell's raw labels ("Add to &Favorites") never
    // match what the user sees or types.
    {
        CHECK(cmo::NormalizeMenuLabel(L"Add to &Favorites") == L"Add to Favorites");
        CHECK(cmo::NormalizeMenuLabel(L"Open wit&h...") == L"Open with");
        CHECK(cmo::NormalizeMenuLabel(L"Smith && Sons") == L"Smith & Sons");

        const bool previousWindows = cmo::g_settings.advancedSubmenuWindows;
        const bool previousThirdParty = cmo::g_settings.advancedSubmenuThirdParty;
        const std::wstring previousLabel = cmo::g_settings.advancedSubmenuLabel;
        const std::vector<std::wstring> previousItems =
            cmo::g_settings.advancedSubmenuItems;
        cmo::g_settings.advancedSubmenuWindows = true;
        cmo::g_settings.advancedSubmenuThirdParty = true;
        cmo::g_settings.advancedSubmenuLabel = L"More options";
        cmo::g_settings.advancedSubmenuItems =
            cmo::ParseAdvancedItems(L"Add to Favorites, Open with");

        auto isAdvanced = [](const cmo::MenuItem& item) {
            return cmo::MatchesAnyToken(item, cmo::g_settings.advancedSubmenuItems) ||
                   cmo::IsThirdPartyItem(item);
        };

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

        CHECK(isAdvanced(
            extensionCommand(1, L"Add to &Favorites", L"pintohomefile")));
        CHECK(isAdvanced(extensionCommand(2, L"Open wit&h...", L"openas")));
        // Unknown verbs are third-party handlers.
        CHECK(isAdvanced(
            extensionCommand(3, L"Extract Here", L"WinRAR.ExtractHere")));
        CHECK(isAdvanced(extensionCommand(4, L"Scan with Malwarebytes", L"")));
        // Known Windows verbs stay unless listed.
        CHECK(!isAdvanced(extensionCommand(5, L"Print", L"Print")));
        CHECK(!isAdvanced(extensionCommand(6, L"Cu&t", L"cut")));

        cmo::g_settings.advancedSubmenuWindows = previousWindows;
        cmo::g_settings.advancedSubmenuThirdParty = previousThirdParty;
        cmo::g_settings.advancedSubmenuLabel = previousLabel;
        cmo::g_settings.advancedSubmenuItems = previousItems;
    }

    // More options ordering: Windows extras group above third-party handlers,
    // each group keeping the shell's relative order, with one separator
    // between them only when both groups exist.
    {
        const bool previousWindows = cmo::g_settings.advancedSubmenuWindows;
        const bool previousThirdParty = cmo::g_settings.advancedSubmenuThirdParty;
        const std::wstring previousLabel = cmo::g_settings.advancedSubmenuLabel;
        const std::vector<std::wstring> previousItems =
            cmo::g_settings.advancedSubmenuItems;
        cmo::g_settings.advancedSubmenuWindows = true;
        cmo::g_settings.advancedSubmenuThirdParty = true;
        cmo::g_settings.advancedSubmenuLabel = L"More options";
        cmo::g_settings.advancedSubmenuItems =
            cmo::ParseAdvancedItems(L"Share, Add to Favorites");

        auto reorganize = [](std::vector<cmo::MenuItem>& items) {
            cmo::AdvancedGroupingOptions options;
            options.moveWindows = cmo::g_settings.advancedSubmenuWindows;
            options.moveThirdParty = cmo::g_settings.advancedSubmenuThirdParty;
            options.label = cmo::g_settings.advancedSubmenuLabel;
            options.windowsItems = cmo::g_settings.advancedSubmenuItems;
            options.exclude = cmo::g_settings.advancedSubmenuExclude;
            cmo::ReorganizeAdvancedItems(items, options);
        };

        auto advancedItem = [](uint32_t id, const wchar_t* label,
                               const wchar_t* verb, uint32_t flags) {
            cmo::MenuItem menuItem{};
            menuItem.id = id;
            menuItem.kind = cmo::ItemKind::Command;
            menuItem.action = cmo::ActionKind::ShellVerb;
            menuItem.label = label;
            menuItem.canonicalVerb = verb;
            menuItem.flags = cmo::kModelExtension | flags;
            return menuItem;
        };

        std::vector<cmo::MenuItem> menu;
        menu.push_back(advancedItem(1, L"WinRAR", L"", cmo::kModelThirdParty));
        menu.push_back(advancedItem(2, L"Share", L"Windows.ModernShare", 0));
        menu.push_back(
            advancedItem(3, L"Scan with Malwarebytes", L"", cmo::kModelThirdParty));
        menu.push_back(advancedItem(4, L"Add to &Favorites", L"pintohomefile", 0));
        menu.push_back(advancedItem(5, L"TortoiseSVN", L"", cmo::kModelThirdParty));

        reorganize(menu);

        const cmo::MenuItem* submenu = nullptr;
        for (const cmo::MenuItem& menuItem : menu) {
            if (menuItem.kind == cmo::ItemKind::Submenu) {
                submenu = &menuItem;
            }
        }
        CHECK(submenu != nullptr);
        if (submenu) {
            CHECK(submenu->children.size() == 6);
            CHECK(submenu->children[0].label == L"Share");
            CHECK(submenu->children[1].label == L"Add to &Favorites");
            CHECK(submenu->children[2].kind == cmo::ItemKind::Separator);
            CHECK(submenu->children[3].label == L"WinRAR");
            CHECK(submenu->children[4].label == L"Scan with Malwarebytes");
            CHECK(submenu->children[5].label == L"TortoiseSVN");
        }

        // A single group gets no separator.
        std::vector<cmo::MenuItem> customOnly;
        customOnly.push_back(advancedItem(1, L"WinRAR", L"", cmo::kModelThirdParty));
        customOnly.push_back(advancedItem(2, L"TortoiseSVN", L"", cmo::kModelThirdParty));
        reorganize(customOnly);
        for (const cmo::MenuItem& menuItem : customOnly) {
            if (menuItem.kind == cmo::ItemKind::Submenu) {
                CHECK(menuItem.children.size() == 2);
                CHECK(menuItem.children[0].kind != cmo::ItemKind::Separator);
            }
        }

        std::vector<cmo::MenuItem> builtinOnly;
        builtinOnly.push_back(advancedItem(1, L"Share", L"Windows.ModernShare", 0));
        builtinOnly.push_back(
            advancedItem(2, L"Add to &Favorites", L"pintohomefile", 0));
        reorganize(builtinOnly);
        for (const cmo::MenuItem& menuItem : builtinOnly) {
            if (menuItem.kind == cmo::ItemKind::Submenu) {
                CHECK(menuItem.children.size() == 2);
                CHECK(menuItem.children[0].kind != cmo::ItemKind::Separator);
            }
        }

        cmo::g_settings.advancedSubmenuWindows = previousWindows;
        cmo::g_settings.advancedSubmenuThirdParty = previousThirdParty;
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

    // Submenu open delay: while the replacement menu is shown the system
    // MenuShowDelay is lowered and restored afterwards; it is never lengthened
    // and -1 leaves it alone. Skipped when the SPI is unavailable.
    {
        DWORD systemDelay = 0;
        if (SystemParametersInfoW(SPI_GETMENUSHOWDELAY, 0, &systemDelay, 0)) {
            const int previousDelay = cmo::g_settings.submenuDelayMs;
            DWORD check = 0;
            if (SystemParametersInfoW(SPI_SETMENUSHOWDELAY, 400, nullptr, 0) &&
                SystemParametersInfoW(SPI_GETMENUSHOWDELAY, 0, &check, 0) &&
                check == 400) {
                cmo::g_settings.submenuDelayMs = 100;
                {
                    cmo::MenuDelaySuppressor suppressor;
                    DWORD during = 0;
                    CHECK(SystemParametersInfoW(SPI_GETMENUSHOWDELAY, 0, &during, 0));
                    CHECK(during <= 100);
                }
                DWORD restored = 0;
                CHECK(SystemParametersInfoW(SPI_GETMENUSHOWDELAY, 0, &restored, 0));
                CHECK(restored == 400);

                // Never lengthens the delay.
                SystemParametersInfoW(SPI_SETMENUSHOWDELAY, 50, nullptr, 0);
                cmo::g_settings.submenuDelayMs = 150;
                {
                    cmo::MenuDelaySuppressor suppressor;
                    DWORD during = 0;
                    CHECK(SystemParametersInfoW(SPI_GETMENUSHOWDELAY, 0, &during, 0));
                    CHECK(during == 50);
                }
                CHECK(SystemParametersInfoW(SPI_GETMENUSHOWDELAY, 0, &restored, 0));
                CHECK(restored == 50);

                // -1 leaves the system value untouched.
                SystemParametersInfoW(SPI_SETMENUSHOWDELAY, 400, nullptr, 0);
                cmo::g_settings.submenuDelayMs = -1;
                {
                    cmo::MenuDelaySuppressor suppressor;
                    DWORD during = 0;
                    CHECK(SystemParametersInfoW(SPI_GETMENUSHOWDELAY, 0, &during, 0));
                    CHECK(during == 400);
                }
            }
            SystemParametersInfoW(SPI_SETMENUSHOWDELAY, systemDelay, nullptr, 0);
            cmo::g_settings.submenuDelayMs = previousDelay;
        }
    }

    // New submenu: ShellNew templates, unique names, and creation.
    {
        // Registry fixtures: one NullFile and one Data template.
        HKEY key = nullptr;
        const bool nullKeyOk =
            RegCreateKeyExW(HKEY_CURRENT_USER,
                            L"Software\\Classes\\.cmonull\\ShellNew", 0, nullptr, 0,
                            KEY_WRITE, nullptr, &key, nullptr) == ERROR_SUCCESS;
        if (nullKeyOk) {
            const wchar_t empty[] = L"";
            RegSetValueExW(key, L"NullFile", 0, REG_SZ,
                           reinterpret_cast<const BYTE*>(empty), sizeof(empty));
            RegCloseKey(key);
        }
        const BYTE dataBytes[] = {0x41, 0x42, 0x43};
        const bool dataKeyOk =
            RegCreateKeyExW(HKEY_CURRENT_USER,
                            L"Software\\Classes\\.cmodata\\ShellNew", 0, nullptr, 0,
                            KEY_WRITE, nullptr, &key, nullptr) == ERROR_SUCCESS;
        if (dataKeyOk) {
            RegSetValueExW(key, L"Data", 0, REG_BINARY, dataBytes,
                           sizeof(dataBytes));
            RegCloseKey(key);
        }
        // REG_EXPAND_SZ FileName templates (Office uses these).
        HKEY expandKey = nullptr;
        const bool expandKeyOk =
            RegCreateKeyExW(HKEY_CURRENT_USER,
                            L"Software\\Classes\\.cmoexp\\ShellNew", 0, nullptr, 0,
                            KEY_WRITE, nullptr, &expandKey, nullptr) == ERROR_SUCCESS;
        if (expandKeyOk) {
            const wchar_t templatePath[] = L"%SystemRoot%\\explorer.exe";
            RegSetValueExW(expandKey, L"FileName", 0, REG_EXPAND_SZ,
                           reinterpret_cast<const BYTE*>(templatePath),
                           sizeof(templatePath));
            RegCloseKey(expandKey);
        }
        // Data stored as REG_SZ text.
        HKEY stringKey = nullptr;
        const bool stringKeyOk =
            RegCreateKeyExW(HKEY_CURRENT_USER,
                            L"Software\\Classes\\.cmostr\\ShellNew", 0, nullptr, 0,
                            KEY_WRITE, nullptr, &stringKey, nullptr) == ERROR_SUCCESS;
        if (stringKeyOk) {
            const wchar_t textData[] = L"abc";
            RegSetValueExW(stringKey, L"Data", 0, REG_SZ,
                           reinterpret_cast<const BYTE*>(textData),
                           sizeof(textData));
            RegCloseKey(stringKey);
        }
        // HKCR is scanned first, mirroring the shell's own view.
        HKEY hkcrKey = nullptr;
        const bool hkcrKeyOk =
            RegCreateKeyExW(HKEY_CLASSES_ROOT, L".cmohkcr\\ShellNew", 0, nullptr, 0,
                            KEY_WRITE, nullptr, &hkcrKey, nullptr) == ERROR_SUCCESS;
        if (hkcrKeyOk) {
            const wchar_t empty[] = L"";
            RegSetValueExW(hkcrKey, L"NullFile", 0, REG_SZ,
                           reinterpret_cast<const BYTE*>(empty), sizeof(empty));
            RegCloseKey(hkcrKey);
        }
        // An HKCU extension key without ShellNew must not shadow the HKLM
        // template for the same extension.
        HKEY shadowKey = nullptr;
        const bool shadowHkcuOk =
            RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\.cmoshadow", 0,
                            nullptr, 0, KEY_WRITE, nullptr, &shadowKey, nullptr) ==
            ERROR_SUCCESS;
        if (shadowHkcuOk) {
            RegCloseKey(shadowKey);
        }
        const bool shadowHklmOk =
            RegCreateKeyExW(HKEY_LOCAL_MACHINE,
                            L"Software\\Classes\\.cmoshadow\\ShellNew", 0, nullptr, 0,
                            KEY_WRITE, nullptr, &key, nullptr) == ERROR_SUCCESS;
        if (shadowHklmOk) {
            const wchar_t empty[] = L"";
            RegSetValueExW(key, L"NullFile", 0, REG_SZ,
                           reinterpret_cast<const BYTE*>(empty), sizeof(empty));
            RegCloseKey(key);
        }

        cmo::ResetNewTemplatesForTesting();
        cmo::EnsureNewTemplates();
        CHECK(cmo::g_newTemplates.size() >= 2);
        CHECK(cmo::g_newTemplates[0].kind == cmo::NewTemplate::Kind::Folder);
        CHECK(cmo::g_newTemplates[1].kind == cmo::NewTemplate::Kind::Shortcut);
        bool foundNull = false;
        bool foundData = false;
        bool foundShadow = false;
        bool foundExpand = false;
        bool foundStringData = false;
        bool foundZip = false;
        bool foundHkcr = false;
        bool foundTxt = false;
        bool foundBmp = false;
        bool foundRtf = false;
        for (const cmo::NewTemplate& tmpl : cmo::g_newTemplates) {
            if (tmpl.extension == L".cmonull" &&
                tmpl.kind == cmo::NewTemplate::Kind::NullFile) {
                foundNull = true;
            }
            if (tmpl.extension == L".cmodata" &&
                tmpl.kind == cmo::NewTemplate::Kind::Data) {
                foundData = true;
            }
            if (tmpl.extension == L".cmoshadow" &&
                tmpl.kind == cmo::NewTemplate::Kind::NullFile) {
                foundShadow = true;
            }
            if (tmpl.extension == L".cmoexp" &&
                tmpl.kind == cmo::NewTemplate::Kind::FileName) {
                foundExpand = !tmpl.fileName.empty();
            }
            if (tmpl.extension == L".cmostr" &&
                tmpl.kind == cmo::NewTemplate::Kind::Data) {
                foundStringData = tmpl.data.size() == 3 && tmpl.data[0] == 'a';
            }
            if (tmpl.extension == L".zip") {
                foundZip = true;
            }
            if (tmpl.extension == L".cmohkcr" &&
                tmpl.kind == cmo::NewTemplate::Kind::NullFile) {
                foundHkcr = true;
            }
            if (tmpl.extension == L".txt" &&
                tmpl.kind == cmo::NewTemplate::Kind::NullFile) {
                foundTxt = true;
            }
            if (tmpl.extension == L".bmp" &&
                tmpl.kind == cmo::NewTemplate::Kind::NullFile) {
                foundBmp = true;
            }
            if (tmpl.extension == L".rtf" &&
                tmpl.kind == cmo::NewTemplate::Kind::Data &&
                tmpl.data.size() == 7 && tmpl.data[0] == '{') {
                foundRtf = true;
            }
        }
        CHECK(foundNull);
        CHECK(foundData);
        CHECK(foundShadow);
        CHECK(foundExpand);
        CHECK(foundStringData);
        CHECK(foundZip);
        CHECK(foundHkcr);
        CHECK(foundTxt);
        CHECK(foundBmp);
        CHECK(foundRtf);

        // The core background model exposes New as a submenu with children.
        cmo::MenuModel background =
            cmo::BuildCoreModel(cmo::Scope::Background, {}, cmo::Shape::Single);
        const cmo::MenuItem* newMenu = nullptr;
        for (const cmo::MenuItem& item : background.items) {
            if (item.label == L"New") {
                newMenu = &item;
            }
        }
        CHECK(newMenu != nullptr);
        CHECK(newMenu && newMenu->kind == cmo::ItemKind::Submenu);
        CHECK(newMenu && !newMenu->children.empty());
        bool hasFolderChild = false;
        for (const cmo::MenuItem& child : newMenu ? newMenu->children
                                                  : std::vector<cmo::MenuItem>{}) {
            if (child.label == L"Folder" &&
                child.action == cmo::ActionKind::NewItem) {
                hasFolderChild = true;
                CHECK(child.iconRef == L"@ext:folder");
            }
            if (child.label == L"Shortcut") {
                CHECK(child.iconRef == L"@ext:.lnk");
            }
            if (child.newIndex < cmo::g_newTemplates.size()) {
                const cmo::NewTemplate& tmpl = cmo::g_newTemplates[child.newIndex];
                if (tmpl.kind != cmo::NewTemplate::Kind::Folder &&
                    tmpl.kind != cmo::NewTemplate::Kind::Shortcut) {
                    CHECK(child.iconRef == L"@ext:" + tmpl.extension);
                }
            }
        }
        CHECK(hasFolderChild);

        // View submenu carries invented glyphs.
        const cmo::MenuItem* viewMenu = nullptr;
        for (const cmo::MenuItem& item : background.items) {
            if (item.label == L"View") {
                viewMenu = &item;
            }
        }
        CHECK(viewMenu != nullptr);
        CHECK(viewMenu && viewMenu->iconRef == L"@glyph:E890");
        bool hasLargeGlyph = false;
        bool hasDetailsGlyph = false;
        for (const cmo::MenuItem& child : viewMenu ? viewMenu->children
                                                   : std::vector<cmo::MenuItem>{}) {
            if (child.label == L"Large icons") {
                hasLargeGlyph = child.iconRef == L"@icon:view-large";
            }
            if (child.label == L"Details") {
                hasDetailsGlyph = child.iconRef == L"@icon:view-details";
            }
        }
        CHECK(hasLargeGlyph);
        CHECK(hasDetailsGlyph);

        if (nullKeyOk) {
            RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\.cmonull");
        }
        if (dataKeyOk) {
            RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\.cmodata");
        }
        if (shadowHkcuOk) {
            RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\.cmoshadow");
        }
        if (shadowHklmOk) {
            RegDeleteTreeW(HKEY_LOCAL_MACHINE, L"Software\\Classes\\.cmoshadow");
        }
        if (expandKeyOk) {
            RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\.cmoexp");
        }
        if (stringKeyOk) {
            RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\.cmostr");
        }
        if (hkcrKeyOk) {
            RegDeleteTreeW(HKEY_CLASSES_ROOT, L".cmohkcr");
        }
    }

    // New items: unique naming and creation in a temporary directory.
    {
        wchar_t tempPath[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, tempPath);
        const std::wstring dir = std::wstring(tempPath) + L"cmo-new-test";
        RemoveDirectoryW((dir + L"\\New folder").c_str());
        RemoveDirectoryW(dir.c_str());
        CHECK(CreateDirectoryW(dir.c_str(), nullptr) != 0);

        // Unique names: the first "New folder" exists, so (2) is chosen.
        CHECK(CreateDirectoryW((dir + L"\\New folder").c_str(), nullptr) != 0);
        CHECK(cmo::MakeUniquePath(dir, L"New folder", L"") ==
              dir + L"\\New folder (2)");
        CHECK(cmo::MakeUniquePath(dir, L"New CmoThing", L".cmt") ==
              dir + L"\\New CmoThing.cmt");

        cmo::NewTemplate folderTmpl{};
        folderTmpl.kind = cmo::NewTemplate::Kind::Folder;
        folderTmpl.displayName = L"Folder";
        std::wstring created;
        CHECK(cmo::CreateNewItemInFolder(folderTmpl, dir, created));
        CHECK(!created.empty());
        const DWORD folderAttrs = GetFileAttributesW(created.c_str());
        CHECK(folderAttrs != INVALID_FILE_ATTRIBUTES &&
              (folderAttrs & FILE_ATTRIBUTE_DIRECTORY) != 0);

        cmo::NewTemplate nullTmpl{};
        nullTmpl.kind = cmo::NewTemplate::Kind::NullFile;
        nullTmpl.displayName = L"CmoThing";
        nullTmpl.extension = L".cmt";
        created.clear();
        CHECK(cmo::CreateNewItemInFolder(nullTmpl, dir, created));
        CHECK(created == dir + L"\\New CmoThing.cmt");
        HANDLE file = CreateFileW(created.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                  nullptr, OPEN_EXISTING, 0, nullptr);
        CHECK(file != INVALID_HANDLE_VALUE);
        if (file != INVALID_HANDLE_VALUE) {
            DWORD size = GetFileSize(file, nullptr);
            CHECK(size == 0);
            CloseHandle(file);
        }

        cmo::NewTemplate dataTmpl{};
        dataTmpl.kind = cmo::NewTemplate::Kind::Data;
        dataTmpl.displayName = L"CmoData";
        dataTmpl.extension = L".cmd2";
        dataTmpl.data = {0x41, 0x42, 0x43};
        created.clear();
        CHECK(cmo::CreateNewItemInFolder(dataTmpl, dir, created));
        file = CreateFileW(created.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, 0, nullptr);
        CHECK(file != INVALID_HANDLE_VALUE);
        if (file != INVALID_HANDLE_VALUE) {
            BYTE readBack[8] = {};
            DWORD read = 0;
            CHECK(ReadFile(file, readBack, sizeof(readBack), &read, nullptr) != 0);
            CHECK(read == 3 && readBack[0] == 0x41 && readBack[2] == 0x43);
            CloseHandle(file);
        }

        const std::wstring source = dir + L"\\cmo-source.bin";
        HANDLE sourceFile = CreateFileW(source.c_str(), GENERIC_WRITE, 0, nullptr,
                                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        CHECK(sourceFile != INVALID_HANDLE_VALUE);
        if (sourceFile != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(sourceFile, "template", 8, &written, nullptr);
            CloseHandle(sourceFile);
        }
        cmo::NewTemplate fileTmpl{};
        fileTmpl.kind = cmo::NewTemplate::Kind::FileName;
        fileTmpl.displayName = L"CmoFile";
        fileTmpl.extension = L".cmf";
        fileTmpl.fileName = source;
        created.clear();
        CHECK(cmo::CreateNewItemInFolder(fileTmpl, dir, created));
        file = CreateFileW(created.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, 0, nullptr);
        CHECK(file != INVALID_HANDLE_VALUE);
        if (file != INVALID_HANDLE_VALUE) {
            char readBack[16] = {};
            DWORD read = 0;
            CHECK(ReadFile(file, readBack, sizeof(readBack), &read, nullptr) != 0);
            CHECK(read == 8 && memcmp(readBack, "template", 8) == 0);
            CloseHandle(file);
        }

        DeleteFileW(source.c_str());
        DeleteFileW((dir + L"\\New CmoThing.cmt").c_str());
        DeleteFileW((dir + L"\\New CmoData.cmd2").c_str());
        DeleteFileW((dir + L"\\New CmoFile.cmf").c_str());
        RemoveDirectoryW((dir + L"\\New folder (2)").c_str());
        RemoveDirectoryW((dir + L"\\New folder").c_str());
        RemoveDirectoryW(dir.c_str());
    }

    // v2 config: colors, booleans, fonts, sections, comments, errors.
    {
        uint32_t color = 0;
        CHECK(cmo::ParseColor(L"#11223344", color) && color == 0x11223344u);
        CHECK(cmo::ParseColor(L"#AABBCC", color) && color == 0xFFAABBCCu);
        CHECK(!cmo::ParseColor(L"#12345", color));
        bool flag = false;
        CHECK(cmo::ParseBool(L"yes", flag) && flag);
        CHECK(cmo::ParseBool(L"0", flag) && !flag);
        CHECK(!cmo::ParseBool(L"maybe", flag));
        std::wstring face;
        float size = 0;
        CHECK(cmo::ParseFont(L"Segoe UI, 9.5", face, size) && face == L"Segoe UI" &&
              size == 9.5f);

        cmo::RulesConfig config;
        std::vector<cmo::ConfigParseError> errors;
        const std::wstring text =
            L"; comment\n[appearance]\nbackground = #1E1E1EF0\nblur = true\n"
            L"cornerRadius = 10\nfont = Segoe UI, 9\n"
            L"[appearance.light]\ntextColor = #202020\n";
        CHECK(cmo::ParseRulesConfig(text, config, errors));
        CHECK(errors.empty());
        CHECK(config.appearance.background == 0x1E1E1EF0u);
        CHECK(config.appearance.blur);
        CHECK(config.appearance.cornerRadius == 10);
        CHECK(config.hasLightAppearance);
        CHECK(config.lightAppearance.textColor == 0xFF202020u);

        cmo::RulesConfig bad;
        std::vector<cmo::ConfigParseError> badErrors;
        CHECK(cmo::ParseRulesConfig(L"[appearance]\nbackground = nope\n", bad, badErrors));
        CHECK(!badErrors.empty() && badErrors[0].line == 2);
        CHECK(bad.appearance.background == 0xF01E1E1Eu);
        std::vector<cmo::ConfigParseError> unknownErrors;
        CHECK(cmo::ParseRulesConfig(L"[appearance]\nnotAKey = 1\n", bad, unknownErrors));
        CHECK(!unknownErrors.empty() && unknownErrors[0].line == 2);
    }

    // v2 predicates.
    {
        CHECK(cmo::GlobMatches(L"*Malwarebytes*", L"Scan with Malwarebytes"));
        CHECK(cmo::GlobMatches(L"Open with", L"Open with"));
        CHECK(!cmo::GlobMatches(L"Share", L"Sha&re"));  // glob is raw; label
                                                        // normalization is the
                                                        // predicate's job

        cmo::MenuItem item{};
        item.label = L"Add to &Favorites";
        item.canonicalVerb = L"pintohomefile";
        item.flags = cmo::kModelThirdParty;
        cmo::ItemContext ctx{};
        ctx.scope = cmo::Scope::Files;
        ctx.shape = cmo::Shape::Single;
        ctx.paths = {L"a.txt"};

        cmo::PredicateExpr expr;
        std::wstring error;
        CHECK(cmo::ParsePredicateExpr(L"label:\"Add to Favorites\"", expr, error));
        CHECK(cmo::PredicateExprMatches(expr, item, ctx));
        CHECK(cmo::ParsePredicateExpr(L"ext:.txt", expr, error));
        CHECK(cmo::PredicateExprMatches(expr, item, ctx));
        CHECK(cmo::ParsePredicateExpr(L"ext:.png", expr, error));
        CHECK(!cmo::PredicateExprMatches(expr, item, ctx));
        CHECK(cmo::ParsePredicateExpr(L"thirdParty", expr, error));
        CHECK(cmo::PredicateExprMatches(expr, item, ctx));
        CHECK(cmo::ParsePredicateExpr(L"scope:files and multi", expr, error));
        CHECK(!cmo::PredicateExprMatches(expr, item, ctx));
        CHECK(cmo::ParsePredicateExpr(L"scope:files and verb:pintohomefile", expr, error));
        CHECK(cmo::PredicateExprMatches(expr, item, ctx));
        CHECK(!cmo::ParsePredicateExpr(L"bogus:1", expr, error));
        CHECK(!error.empty());

        // Label predicates normalize & accelerators and trailing ellipses.
        cmo::MenuItem openWith{};
        openWith.label = L"Open wit&h...";
        CHECK(cmo::ParsePredicateExpr(L"label:\"Open with\"", expr, error));
        CHECK(cmo::PredicateExprMatches(expr, openWith, ctx));
    }

    // v2 rules: hide, keep, move, precedence, fallback protection.
    {
        cmo::RulesConfig config;
        std::wstring error;
        cmo::Rule hideRule{};
        hideRule.kind = cmo::RuleKind::Hide;
        CHECK(cmo::ParsePredicateExpr(L"label:\"Cast to Device\"", hideRule.match,
                                      error));
        config.rules.push_back(hideRule);
        cmo::Rule keepRule{};
        keepRule.kind = cmo::RuleKind::Keep;
        CHECK(cmo::ParsePredicateExpr(L"label:Share", keepRule.match, error));
        config.rules.push_back(keepRule);
        cmo::Rule moveRule{};
        moveRule.kind = cmo::RuleKind::Move;
        moveRule.destination = L"More options";
        CHECK(cmo::ParsePredicateExpr(L"thirdParty", moveRule.match, error));
        config.rules.push_back(moveRule);

        cmo::MenuModel model{};
        model.sig = cmo::ContextSignature{cmo::Scope::Files, L".txt", cmo::Shape::Single,
                                          cmo::Variant::Normal};
        auto make = [](uint32_t id, const wchar_t* label, uint32_t flags) {
            cmo::MenuItem item{};
            item.id = id;
            item.kind = cmo::ItemKind::Command;
            item.action = cmo::ActionKind::ShellVerb;
            item.label = label;
            item.flags = flags;
            return item;
        };
        model.items.push_back(make(1, L"Cast to Device", 0));
        model.items.push_back(make(2, L"Share", cmo::kModelThirdParty));
        model.items.push_back(make(3, L"WinRAR", cmo::kModelThirdParty));
        cmo::MenuItem fallback = make(4, L"Show classic menu", 0);
        fallback.action = cmo::ActionKind::Fallback;
        model.items.push_back(fallback);

        cmo::ItemContext ctx{};
        ctx.scope = cmo::Scope::Files;
        cmo::RulesApplication applied = cmo::ApplyRulesToModel(model, config, ctx);
        CHECK(applied.hasMoveRules);
        CHECK(model.items.size() == 3);  // Share, More options, fallback
        const cmo::MenuItem* more = nullptr;
        for (const cmo::MenuItem& item : model.items) {
            if (item.label == L"More options") more = &item;
        }
        CHECK(more != nullptr && more->kind == cmo::ItemKind::Submenu);
        CHECK(more && more->children.size() == 1);
        CHECK(more && more->children[0].label == L"WinRAR");
        CHECK(model.items.back().action == cmo::ActionKind::Fallback);

        // Hiding everything must keep the fallback.
        cmo::RulesConfig hideAll;
        cmo::Rule hideAllRule{};
        hideAllRule.kind = cmo::RuleKind::Hide;
        CHECK(cmo::ParsePredicateExpr(L"label:\"*\"", hideAllRule.match, error));
        hideAll.rules.push_back(hideAllRule);
        cmo::MenuModel emptyModel{};
        emptyModel.items.push_back(make(1, L"Open", 0));
        cmo::MenuItem fb = make(2, L"Show classic menu", 0);
        fb.action = cmo::ActionKind::Fallback;
        emptyModel.items.push_back(fb);
        cmo::ApplyRulesToModel(emptyModel, hideAll, ctx);
        CHECK(emptyModel.items.size() == 1);
        CHECK(emptyModel.items.back().action == cmo::ActionKind::Fallback);
    }

    // v2 custom commands: parsing, placeholders, insertion, nesting.
    {
        cmo::RulesConfig config;
        std::vector<cmo::ConfigParseError> errors;
        const std::wstring text =
            L"[command \"Open in VS Code\"]\n"
            L"command = code.exe \"%1\"\n"
            L"workingDir = %dir%\n"
            L"icon = C:\\Code.exe,0\n"
            L"match.ext = .cs, .cpp\n"
            L"menu = Tools\n"
            L"[submenu \"Tools\"]\n"
            L"icon = @glyph:E712\n"
            L"position = top\n";
        CHECK(cmo::ParseRulesConfig(text, config, errors));
        CHECK(config.commands.size() == 1);
        CHECK(config.commands[0].label == L"Open in VS Code");
        CHECK(config.commands[0].menuPath == L"Tools");
        CHECK(config.submenus.size() == 1);
        CHECK(config.submenus[0].position == cmo::SubmenuPositionKind::Top);

        cmo::InvocationContext ctx{};
        ctx.paths = {L"C:\\src\\a.cs", L"C:\\src\\b.cs"};
        const std::wstring expanded =
            cmo::ExpandCommandPlaceholders(L"code.exe \"%1\" -- %* -- %dir%", ctx);
        CHECK(expanded.find(L"C:\\src\\a.cs") != std::wstring::npos);
        CHECK(expanded.find(L"\"C:\\src\\a.cs\" \"C:\\src\\b.cs\"") !=
              std::wstring::npos);

        cmo::MenuModel model{};
        model.sig = cmo::ContextSignature{cmo::Scope::Files, L".cs", cmo::Shape::Single,
                                          cmo::Variant::Normal};
        cmo::MenuItem fb{};
        fb.id = 1;
        fb.kind = cmo::ItemKind::Command;
        fb.action = cmo::ActionKind::Fallback;
        fb.label = L"Show classic menu";
        model.items.push_back(fb);
        cmo::ItemContext itemCtx{};
        itemCtx.scope = cmo::Scope::Files;
        itemCtx.paths = {L"C:\\src\\a.cs"};
        cmo::InsertCustomItems(model, config, itemCtx);
        const cmo::MenuItem* tools = nullptr;
        for (const cmo::MenuItem& item : model.items) {
            if (item.label == L"Tools") tools = &item;
        }
        CHECK(tools != nullptr && tools->kind == cmo::ItemKind::Submenu);
        CHECK(tools && tools->children.size() == 1);
        CHECK(tools && tools->children[0].action == cmo::ActionKind::CustomCommand);
        CHECK(model.items.front().label == L"Tools");  // position = top

        // [rules] line parsing (needed by Task 6; no other task owned it).
        cmo::RulesConfig rulesConfig;
        std::vector<cmo::ConfigParseError> ruleErrors;
        CHECK(cmo::ParseRulesConfig(
            L"[rules]\nhide = label:\"Cast to Device\"\nkeep = label:Share\n"
            L"move = thirdParty -> \"More options\"\n",
            rulesConfig, ruleErrors));
        CHECK(rulesConfig.rules.size() == 3);
        CHECK(rulesConfig.rules[0].kind == cmo::RuleKind::Hide);
        CHECK(rulesConfig.rules[1].kind == cmo::RuleKind::Keep);
        CHECK(rulesConfig.rules[2].kind == cmo::RuleKind::Move);
        CHECK(rulesConfig.rules[2].destination == L"More options");
    }

    // v2 ConfigStore: snapshot swap, revision, bad text keeps previous.
    {
        cmo::ConfigStore store;
        CHECK(store.ApplyTextForTesting(L"[appearance]\ncornerRadius = 4\n"));
        CHECK(store.Revision() == 1);
        CHECK(store.Snapshot() && store.Snapshot()->appearance.cornerRadius == 4);
        CHECK(store.ApplyTextForTesting(L"[appearance]\ncornerRadius = nope\n"));
        CHECK(store.Revision() == 2);
        CHECK(store.Snapshot() && store.Snapshot()->appearance.cornerRadius == 8);
        CHECK(!cmo::ConfigFilePath().empty());
        CHECK(cmo::ConfigFilePath().find(L"menu.ini") != std::wstring::npos);
    }

    // v2 rules integration: hide applies and custom commands are inserted.
    {
        cmo::RulesConfig config;
        std::vector<cmo::ConfigParseError> errors;
        CHECK(cmo::ParseRulesConfig(
            L"[command \"Echo\"]\ncommand = cmd.exe /c echo %1\nmatch.scope = files\n"
            L"[rules]\nhide = label:\"Hide me\"\n",
            config, errors));
        cmo::ItemContext ctx{};
        ctx.scope = cmo::Scope::Files;
        ctx.paths = {L"C:\\a.txt"};
        cmo::MenuModel model = cmo::BuildCoreFileModel(ctx.paths, cmo::Shape::Single);
        cmo::MenuItem hidden{};
        hidden.id = 900;
        hidden.kind = cmo::ItemKind::Command;
        hidden.action = cmo::ActionKind::ShellVerb;
        hidden.label = L"Hide me";
        model.items.insert(model.items.end() - 1, hidden);
        cmo::ApplyRulesConfigToModel(model, config, ctx);
        for (const cmo::MenuItem& item : model.items) {
            CHECK(item.label != L"Hide me");
        }
        bool foundEcho = false;
        for (const cmo::MenuItem& item : model.items) {
            if (item.label == L"Echo" &&
                item.action == cmo::ActionKind::CustomCommand) {
                foundEcho = true;
                CHECK(item.customCommandIndex == 0);
            }
        }
        CHECK(foundEcho);
    }

    // v2 appearance resolution and DPI scaling.
    {
        cmo::RulesConfig config;
        config.appearance.cornerRadius = 8;
        config.appearance.textColor = 0xFFFFFFFFu;
        config.appearance.itemHeight = 28;
        config.hasLightAppearance = true;
        config.lightAppearance.textColor = 0xFF202020u;
        const cmo::Appearance dark = cmo::ResolveAppearance(config, true);
        CHECK(dark.textColor == 0xFFFFFFFFu);
        const cmo::Appearance light = cmo::ResolveAppearance(config, false);
        CHECK(light.textColor == 0xFF202020u);
        CHECK(light.itemHeight == 28);  // inherited from base

        cmo::LayoutMetrics at96 = cmo::ResolveLayoutMetrics(dark, 96, true);
        cmo::LayoutMetrics at144 = cmo::ResolveLayoutMetrics(dark, 144, true);
        CHECK(at96.itemHeight == 28);
        CHECK(at144.itemHeight == 42);
        CHECK(at144.fontSize == at96.fontSize * 1.5f);
    }

    // v2 layout geometry: heights, gutter, submenu recursion, disabled color.
    {
        cmo::LayoutMetrics metrics{};
        metrics.itemHeight = 28;
        metrics.separatorHeight = 7;
        metrics.iconSize = 16;
        metrics.padding = 6;
        metrics.gutterWidth = 22;
        metrics.submenuArrowWidth = 16;
        metrics.textColor = 0xFFFFFFFFu;
        metrics.disabledTextColor = 0x66FFFFFFu;

        std::vector<cmo::MenuItem> items;
        cmo::MenuItem open{};
        open.id = 1;
        open.kind = cmo::ItemKind::Command;
        open.action = cmo::ActionKind::ShellVerb;
        open.label = L"Open";
        items.push_back(open);
        cmo::MenuItem sep{};
        sep.id = 2;
        sep.kind = cmo::ItemKind::Separator;
        items.push_back(sep);
        cmo::MenuItem disabled{};
        disabled.id = 3;
        disabled.kind = cmo::ItemKind::Command;
        disabled.action = cmo::ActionKind::ShellVerb;
        disabled.label = L"Paste";
        disabled.flags = cmo::kModelDisabled;
        items.push_back(disabled);
        cmo::MenuItem sub{};
        sub.id = 4;
        sub.kind = cmo::ItemKind::Submenu;
        sub.action = cmo::ActionKind::Submenu;
        sub.label = L"More options";
        cmo::MenuItem child{};
        child.id = 5;
        child.kind = cmo::ItemKind::Command;
        child.action = cmo::ActionKind::ShellVerb;
        child.label = L"WinRAR";
        sub.children.push_back(child);
        items.push_back(sub);

        cmo::LayoutPanel panel = cmo::BuildLayoutPanel(items, metrics);
        CHECK(panel.items.size() == 4);
        CHECK(panel.size.cy == 4 + 28 + 7 + 28 + 28 + 4);
        CHECK(panel.items[0].rect.top == 4);
        CHECK(panel.items[0].rect.bottom == 32);
        CHECK(panel.items[1].rect.top == 32);
        CHECK(panel.items[2].rect.top == 39);
        CHECK(panel.items[2].textColor == 0x66FFFFFFu);
        CHECK(panel.items[0].textRect.left == 6 + 14 + 3 + 16 + 6);
        CHECK(panel.items[0].gutterRect.left == 6);
        CHECK(panel.children.size() == 1);
        CHECK(panel.children[0].items.size() == 1);
        CHECK(panel.items[3].submenuIndex == 0);
        CHECK(panel.items[3].textRect.right <= panel.size.cx - 16);
    }

    // v2 positioning: flip near edges, then clamp to the work area.
    {
        const RECT work = {0, 0, 1920, 1080};
        POINT at = cmo::ClampPanelPosition(POINT{100, 100}, SIZE{300, 400}, work);
        CHECK(at.x == 100 && at.y == 100);
        at = cmo::ClampPanelPosition(POINT{1800, 1000}, SIZE{300, 400}, work);
        CHECK(at.x == 1500);  // flipped left of the anchor
        CHECK(at.y == 600);   // flipped above the anchor
        at = cmo::ClampPanelPosition(POINT{-50, -50}, SIZE{300, 400}, work);
        CHECK(at.x == 0 && at.y == 0);

        const RECT parentItem = {100, 100, 300, 128};
        POINT sub = cmo::SubmenuPosition(parentItem, SIZE{200, 300}, work, 4);
        CHECK(sub.x == 296);
        CHECK(sub.y == 100);
        const RECT nearRight = {1700, 100, 1900, 128};
        sub = cmo::SubmenuPosition(nearRight, SIZE{200, 300}, work, 4);
        CHECK(sub.x == 1500);  // flipped to the parent's left
    }

    // v2 layout cache: key components, LRU, device invalidation.
    {
        cmo::LayoutCache cache;
        cache.SetMaxEntries(2);
        auto panel = std::make_shared<cmo::LayoutPanel>();
        panel->size = SIZE{100, 100};
        cmo::LayoutKey key{};
        key.sig = cmo::ContextSignature{cmo::Scope::Files, L".txt", cmo::Shape::Single,
                                        cmo::Variant::Normal};
        key.rulesRevision = 1;
        key.appearanceRevision = 1;
        key.dpi = 96;
        key.darkTheme = true;
        cache.Put(key, panel);
        CHECK(cache.Find(key) != nullptr);
        CHECK(cache.Size() == 1);

        cmo::LayoutKey otherTheme = key;
        otherTheme.darkTheme = false;
        CHECK(cache.Find(otherTheme) == nullptr);
        cmo::LayoutKey otherDpi = key;
        otherDpi.dpi = 144;
        CHECK(cache.Find(otherDpi) == nullptr);

        cache.InvalidateDevice();
        CHECK(cache.Find(key) == nullptr);
        CHECK(cache.Size() == 0);
    }

    // v2 mode selection and failure fallback.
    {
        CHECK(cmo::ResolveMenuMode(0, 0) == cmo::MenuMode::Custom);
        CHECK(cmo::ResolveMenuMode(1, 0) == cmo::MenuMode::HMenu);
        CHECK(cmo::ResolveMenuMode(0, 3) == cmo::MenuMode::HMenu);
        cmo::ModeController controller;
        controller.SetMode(cmo::MenuMode::Custom);
        CHECK(controller.Current() == cmo::MenuMode::Custom);
        controller.RecordFailure();
        controller.RecordFailure();
        CHECK(controller.Current() == cmo::MenuMode::Custom);
        controller.RecordFailure();
        CHECK(controller.Current() == cmo::MenuMode::HMenu);
        controller.RecordSuccess();
        CHECK(controller.ConsecutiveFailures() == 0);
    }

    // v2 window style decisions and pool reuse.
    {
        CHECK(cmo::MenuWindowExStyle() ==
              (WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP |
               WS_EX_NOACTIVATE));
        CHECK(cmo::MenuWindowStyle() == WS_POPUP);
        cmo::MenuWindowPool pool;
        cmo::MenuWindow* a = pool.Acquire();
        cmo::MenuWindow* b = pool.Acquire();
        CHECK(a != nullptr && b != nullptr && a != b);
        pool.Release(a);
        CHECK(pool.Acquire() == a);
        pool.DestroyAll();
    }

    // v2 content caches: empty and clear.
    {
        cmo::ContentCaches caches;
        CHECK(caches.TextCount() == 0);
        CHECK(caches.IconCount() == 0);
        caches.Clear();
        CHECK(caches.TextCount() == 0);
    }

    // v2 input state machine.
    {
        cmo::LayoutMetrics metrics{};
        metrics.itemHeight = 28;
        metrics.separatorHeight = 7;
        metrics.iconSize = 16;
        metrics.padding = 6;
        metrics.gutterWidth = 22;
        metrics.submenuArrowWidth = 16;
        metrics.textColor = 0xFFFFFFFFu;
        metrics.disabledTextColor = 0x66FFFFFFu;
        std::vector<cmo::MenuItem> items;
        for (int i = 0; i < 5; ++i) {
            cmo::MenuItem item{};
            item.id = static_cast<uint32_t>(i + 1);
            item.kind = cmo::ItemKind::Command;
            item.action = cmo::ActionKind::ShellVerb;
            item.label = std::wstring(L"Item ") + std::to_wstring(i);
            items.push_back(item);
        }
        cmo::MenuItem sep{};
        sep.id = 99;
        sep.kind = cmo::ItemKind::Separator;
        items.insert(items.begin() + 2, sep);
        cmo::LayoutPanel panel = cmo::BuildLayoutPanel(items, metrics);

        cmo::MenuInputState state{};
        cmo::MenuStateKey(state, panel, cmo::MenuInputEvent::KeyDown);
        CHECK(state.keyboardIndex == 0);
        cmo::MenuStateKey(state, panel, cmo::MenuInputEvent::KeyDown);
        CHECK(state.keyboardIndex == 1);
        cmo::MenuStateKey(state, panel, cmo::MenuInputEvent::KeyDown);
        CHECK(state.keyboardIndex == 3);  // skips separator
        cmo::MenuStateKey(state, panel, cmo::MenuInputEvent::KeyUp);
        CHECK(state.keyboardIndex == 1);
        cmo::MenuStateKey(state, panel, cmo::MenuInputEvent::KeyEnd);
        CHECK(state.keyboardIndex == 5);  // last enabled item

        cmo::MenuStateMouseMove(state, panel, 2);
        CHECK(state.hoverIndex == -1);  // separator is not hoverable
        cmo::MenuStateMouseMove(state, panel, 3);
        CHECK(state.hoverIndex == 3 && state.keyboardIndex == -1);
        cmo::MenuStateMouseLeave(state);
        CHECK(state.hoverIndex == -1);

        CHECK(cmo::MenuStateItemAt(panel, state, POINT{10, 28 + 28 + 7 + 5}) == 3);
        cmo::MenuStateWheel(state, panel, -1);
        CHECK(state.scrollOffset >= 0);
    }

    // v2 invocation descriptor round-trip.
    {
        cmo::MenuModel model = cmo::BuildCoreFileModel({L"a.txt"}, cmo::Shape::Single);
        cmo::MenuItem copy{};
        copy.id = 500;
        copy.kind = cmo::ItemKind::Command;
        copy.action = cmo::ActionKind::ShellVerb;
        copy.label = L"WinRAR";
        copy.canonicalVerb = L"WinRAR.ExtractHere";
        copy.verbOffset = 42;
        copy.flags = cmo::kModelHasOffset | cmo::kModelExtension;
        model.items.insert(model.items.end() - 1, copy);
        cmo::LayoutMetrics metrics{};
        cmo::LayoutPanel panel = cmo::BuildLayoutPanel(model.items, metrics);
        const cmo::LayoutItem* copyItem = nullptr;
        for (const cmo::LayoutItem& item : panel.items) {
            if (item.invocation.id == 500) {
                copyItem = &item;
            }
        }
        CHECK(copyItem != nullptr);
        cmo::MenuItem resolved{};
        CHECK(cmo::BuildMenuItemForInvocation(model, copyItem->invocation, resolved));
        CHECK(resolved.label == L"WinRAR");
        CHECK(resolved.verbOffset == 42);
        CHECK((resolved.flags & cmo::kModelExtension) != 0);
    }

    // v2 backdrop blur and corner mask.
    {
        std::vector<uint32_t> src(8 * 8, 0xFF000000u);
        src[0] = 0xFFFFFFFFu;
        std::vector<uint32_t> out;
        int outW = 0;
        int outH = 0;
        cmo::DownscaleAndBlur(src.data(), 8, 8, 2, 1, out, outW, outH);
        CHECK(outW == 4 && outH == 4);
        CHECK(out.size() == 16);
        CHECK(out[0] != out[15]);

        std::vector<uint8_t> mask;
        cmo::BuildRoundedRectMask(20, 20, 6, mask);
        CHECK(mask.size() == 400);
        CHECK(mask[10 * 20 + 10] == 255);
        CHECK(mask[0] == 0);
        CHECK(mask[19] == 0);
    }

    // v2.8 animation config and frame math.
    {
        using namespace cmo;
        // Effect list parsing.
        uint32_t effects = 0;
        CHECK(ParseAnimationEffects(L"fade, slide", effects));
        CHECK(effects == (kAnimFade | kAnimSlide));
        CHECK(ParseAnimationEffects(L"none", effects) && effects == 0);
        CHECK(ParseAnimationEffects(L"", effects) && effects == 0);
        CHECK(!ParseAnimationEffects(L"fade, bogus", effects));
        CHECK(effects == kAnimFade);
        CHECK(ParseAnimationEffects(L"unfold", effects) && effects == kAnimUnfold);
        CHECK(AnimationEffectsText(kAnimSlide | kAnimFade) == L"fade, slide");
        CHECK(AnimationEffectsText(0) == L"none");
        AnimEasing easing = AnimEasing::Linear;
        CHECK(ParseAnimEasing(L"Bounce", easing) && easing == AnimEasing::Bounce);
        CHECK(!ParseAnimEasing(L"nope", easing));

        // Spec resolution: separate open/close, close duration fallback.
        Appearance appearance{};
        appearance.animationOpen = kAnimFade | kAnimSlide;
        appearance.animationClose = kAnimUnfold;
        appearance.animationDuration = 200;
        appearance.animationCloseDuration = 0;
        appearance.animationFrameMs = 8;
        appearance.slideOffsetX = 20;
        appearance.slideOffsetY = -4;
        const AnimationSpec openSpec = ResolveAnimationSpec(appearance, true);
        CHECK(openSpec.animate);
        CHECK(openSpec.durationMs == 200);
        CHECK(openSpec.frameMs == 8);
        const AnimationSpec closeSpec = ResolveAnimationSpec(appearance, false);
        CHECK(closeSpec.animate);
        CHECK(closeSpec.durationMs == 200);
        CHECK(closeSpec.effects == kAnimUnfold);

        // Easing endpoints and overshoot.
        CHECK(ApplyAnimationEasing(AnimEasing::Linear, 0.0f) == 0.0f);
        CHECK(ApplyAnimationEasing(AnimEasing::Linear, 1.0f) == 1.0f);
        CHECK(ApplyAnimationEasing(AnimEasing::Bounce, 1.0f) == 1.0f);
        CHECK(ApplyAnimationEasing(AnimEasing::Elastic, 1.0f) == 1.0f);
        CHECK(ApplyAnimationEasing(AnimEasing::Back, 0.8f) > 1.0f);

        // Fade + slide endpoints (open and close mirror).
        const AnimationFrame start = ComputeAnimationFrame(openSpec, 0.0f, true);
        CHECK(start.opacity == 0.0f);
        CHECK(start.translateX == 20.0f);
        CHECK(start.translateY == -4.0f);
        const AnimationFrame end = ComputeAnimationFrame(openSpec, 1.0f, true);
        CHECK(end.opacity == 1.0f);
        CHECK(end.translateX == 0.0f);
        CHECK(end.translateY == 0.0f);
        const AnimationFrame closeStart =
            ComputeAnimationFrame(openSpec, 0.0f, false);
        CHECK(closeStart.opacity == 1.0f);
        CHECK(closeStart.translateX == 0.0f);

        // Each effect at its endpoints.
        AnimationSpec single{};
        single.animate = true;
        single.durationMs = 100;
        single.effects = kAnimScale;
        single.scaleFrom = 80;
        CHECK(std::fabs(ComputeAnimationFrame(single, 0.0f, true).scaleX - 0.8f) <
              0.001f);
        CHECK(std::fabs(ComputeAnimationFrame(single, 1.0f, true).scaleX - 1.0f) <
              0.001f);
        single.effects = kAnimCrt;
        const cmo::AnimationFrame crtStart =
            ComputeAnimationFrame(single, 0.0f, true);
        CHECK(crtStart.center);
        CHECK(crtStart.scaleY < 0.1f);
        CHECK(crtStart.scaleX < 1.0f);
        CHECK(crtStart.brightness > 0.9f);
        const cmo::AnimationFrame crtEnd =
            ComputeAnimationFrame(single, 1.0f, true);
        CHECK(crtEnd.center);
        CHECK(crtEnd.scaleY == 1.0f);
        CHECK(std::fabs(crtEnd.scaleX - 1.0f) < 0.001f);
        CHECK(crtEnd.brightness == 0.0f);
        single.effects = kAnimUnfold;
        CHECK(ComputeAnimationFrame(single, 0.0f, true).scaleX < 0.1f);
        CHECK(ComputeAnimationFrame(single, 1.0f, true).scaleX == 1.0f);
        single.effects = kAnimDissolve;
        CHECK(ComputeAnimationFrame(single, 0.0f, true).contentOpacity == 0.0f);
        CHECK(ComputeAnimationFrame(single, 1.0f, true).contentOpacity == 1.0f);

        // Overshoot easings keep opacity in range.
        single.effects = kAnimFade;
        single.easing = AnimEasing::Back;
        for (float t = 0.0f; t <= 1.0f; t += 0.05f) {
            const AnimationFrame f = ComputeAnimationFrame(single, t, true);
            CHECK(f.opacity >= 0.0f && f.opacity <= 1.0f);
        }

        // Deprecated alias and explicit keys.
        std::vector<ConfigParseError> warnings;
        RulesConfig legacy;
        CHECK(ParseRulesConfig(L"[appearance]\nanimation = slide\n", legacy,
                               warnings));
        CHECK(legacy.appearance.animationOpen == kAnimSlide);
        CHECK(legacy.appearance.animationClose == kAnimSlide);
        RulesConfig explicitConfig;
        CHECK(ParseRulesConfig(
            L"[appearance]\nanimation = slide\nanimationOpen = fade\n",
            explicitConfig, warnings));
        CHECK(explicitConfig.appearance.animationOpen == kAnimFade);
        CHECK(explicitConfig.appearance.animationClose == kAnimSlide);
        CHECK(kConfigSchemaVersion == 7);
        RulesConfig submenuConfig;
        CHECK(ParseRulesConfig(L"[appearance]\nanimateSubmenus = true\n",
                               submenuConfig, warnings));
        CHECK(submenuConfig.appearance.animateSubmenus);

        // Canonical rewrites keep the recognized effect subset.
        const ConfigSchemaEntry* openRow = SchemaFind(L"animationOpen");
        CHECK(openRow != nullptr);
        CHECK(NormalizeAppearanceValue(*openRow, L"scale, bogus") == L"scale");
        CHECK(NormalizeAppearanceValue(*openRow, L"bogus") == L"none");
        CHECK(NormalizeAppearanceValue(*openRow, L"fade, slide") ==
              L"fade, slide");
    }

    // v2.8 paste helpers: unique shortcut names in the destination folder.
    {
        const std::wstring dir = L"cmo-test-storage\\paste-test";
        CreateDirectoryW(L"cmo-test-storage", nullptr);
        CreateDirectoryW(dir.c_str(), nullptr);
        const std::wstring first = dir + L"\\a - Shortcut.lnk";
        const std::wstring second = dir + L"\\a - Shortcut (2).lnk";
        DeleteFileW(first.c_str());
        DeleteFileW(second.c_str());
        CHECK(cmo::MakeShortcutPath(dir, L"a.txt") == first);
        CHECK(cmo::WriteConfigFile(first, L"x"));
        CHECK(cmo::MakeShortcutPath(dir, L"a.txt") == second);
        DeleteFileW(first.c_str());
    }

    // v2.8 invoke resolution: live menu offsets win over cached ones.
    {
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, 30977 + 19, L"P&roperties");
        AppendMenuW(menu, MF_STRING, 30977 + 27, L"Restore previous &versions");
        cmo::MenuItem target{};
        target.label = L"Restore previous &versions";
        const auto found = cmo::FindNativeOffsetInMenu(menu, 30977, nullptr, target);
        CHECK(found.has_value() && *found == 27);
        DestroyMenu(menu);
    }

    // v2.8 extension state: never inherit a native check marker.
    {
        CHECK(cmo::MapExtensionMenuState(MFS_CHECKED) == cmo::kModelNone);
        CHECK(cmo::MapExtensionMenuState(MFS_CHECKED | MFS_DISABLED) ==
              cmo::kModelDisabled);
        CHECK(cmo::MapExtensionMenuState(MFS_DEFAULT) == cmo::kModelDefault);
        CHECK(cmo::MapMenuState(MFS_CHECKED) == cmo::kModelChecked);
    }

    // v2.8 merge: a cached submenu donates its children to an empty core
    // submenu (the shell's Send to list used to be dropped, leaving it pruned).
    {
        cmo::MenuModel core{};
        cmo::MenuItem sendTo{};
        sendTo.id = 1;
        sendTo.kind = cmo::ItemKind::Submenu;
        sendTo.action = cmo::ActionKind::Submenu;
        sendTo.label = L"Send to";
        core.items.push_back(sendTo);

        cmo::MenuModel cached{};
        cmo::MenuItem nativeSendTo = sendTo;
        nativeSendTo.label = L"Se&nd to";
        cmo::MenuItem child{};
        child.id = 2;
        child.kind = cmo::ItemKind::Command;
        child.action = cmo::ActionKind::ShellVerb;
        child.label = L"Compressed (zipped) folder";
        nativeSendTo.children.push_back(child);
        cached.items.push_back(nativeSendTo);

        const cmo::MenuModel merged = cmo::MergeCoreWithCached(core, cached);
        CHECK(merged.items.size() == 1);
        CHECK(merged.items[0].children.size() == 1);
        CHECK(merged.items[0].children[0].label ==
              L"Compressed (zipped) folder");
    }

    // v2 cache keys include config revisions; device loss clears layouts.
    {
        cmo::RulesConfig config;
        config.revision = 7;
        cmo::MenuModel emptyModel{};
        cmo::LayoutKey a = cmo::MakeLayoutKey(
            cmo::ContextSignature{cmo::Scope::Files, L".txt", cmo::Shape::Single,
                                  cmo::Variant::Normal},
            config, 96, true, emptyModel, 0);
        cmo::LayoutKey b = a;
        CHECK(a == b);
        config.revision = 8;
        cmo::LayoutKey c = cmo::MakeLayoutKey(a.sig, config, 96, true, emptyModel, 0);
        CHECK(!(a == c));

        auto panel = std::make_shared<cmo::LayoutPanel>();
        cmo::g_layoutCache.SetMaxEntries(4);
        cmo::g_layoutCache.Put(a, panel);
        CHECK(cmo::g_layoutCache.Find(a) != nullptr);
        cmo::g_layoutCache.InvalidateDevice();
        CHECK(cmo::g_layoutCache.Find(a) == nullptr);
    }

    // v2 review fixes: inline comments, %dir%, submenu match gating, split.
    {
        cmo::RulesConfig config;
        std::vector<cmo::ConfigParseError> errors;
        CHECK(cmo::ParseRulesConfig(
            L"[appearance]\ncornerRadius = 4 ; trailing comment\n", config,
            errors));
        CHECK(config.appearance.cornerRadius == 4);

        cmo::InvocationContext ctx{};
        ctx.directory = L"C:\\src";
        ctx.paths = {L"C:\\src\\a.cs"};
        CHECK(cmo::ExpandCommandPlaceholders(L"%dir%", ctx) == L"C:\\src");

        std::wstring file;
        std::wstring parameters;
        CHECK(cmo::SplitCommandLine(L"code.exe \"%1\" --flag", file, parameters));
        CHECK(file == L"code.exe");
        CHECK(parameters == L"\"%1\" --flag");

        cmo::RulesConfig submenus;
        CHECK(cmo::ParseRulesConfig(
            L"[submenu \"OnlyFolders\"]\nmatch.scope = folders\n"
            L"[submenu \"Everywhere\"]\n",
            submenus, errors));
        cmo::MenuModel model{};
        cmo::MenuItem fb{};
        fb.id = 1;
        fb.kind = cmo::ItemKind::Command;
        fb.action = cmo::ActionKind::Fallback;
        fb.label = L"Show classic menu";
        model.items.push_back(fb);
        cmo::ItemContext itemCtx{};
        itemCtx.scope = cmo::Scope::Files;
        itemCtx.paths = {L"C:\\src\\a.cs"};
        cmo::InsertCustomItems(model, submenus, itemCtx);
        bool onlyFolders = false;
        bool everywhere = false;
        for (const cmo::MenuItem& item : model.items) {
            if (item.label == L"OnlyFolders") {
                onlyFolders = true;
            }
            if (item.label == L"Everywhere") {
                everywhere = true;
            }
        }
        CHECK(!onlyFolders);
        CHECK(everywhere);
    }

    // v2.1 config encoding: round-trip, BOMs, BOM-less, invalid.
    {
        const std::wstring sample = L"; comment\r\n[appearance]\r\ncornerRadius = 4\r\n";
        const std::vector<uint8_t> encoded = cmo::EncodeConfigText(sample);
        CHECK(encoded.size() >= 3 && encoded[0] == 0xEF && encoded[1] == 0xBB &&
              encoded[2] == 0xBF);
        std::wstring decoded;
        CHECK(cmo::DecodeConfigBytes(encoded, decoded));
        CHECK(decoded == sample);

        // UTF-16LE with BOM.
        std::vector<uint8_t> utf16;
        utf16.push_back(0xFF);
        utf16.push_back(0xFE);
        for (wchar_t c : sample) {
            utf16.push_back(static_cast<uint8_t>(c & 0xFF));
            utf16.push_back(static_cast<uint8_t>((c >> 8) & 0xFF));
        }
        CHECK(cmo::DecodeConfigBytes(utf16, decoded));
        CHECK(decoded == sample);

        // BOM-less UTF-8 and BOM-less UTF-16LE.
        const std::string narrow = "; x\n";
        std::vector<uint8_t> utf8(narrow.begin(), narrow.end());
        CHECK(cmo::DecodeConfigBytes(utf8, decoded));
        CHECK(decoded == L"; x\n");
        std::vector<uint8_t> utf16NoBom;
        for (wchar_t c : sample) {
            utf16NoBom.push_back(static_cast<uint8_t>(c & 0xFF));
            utf16NoBom.push_back(static_cast<uint8_t>((c >> 8) & 0xFF));
        }
        CHECK(cmo::DecodeConfigBytes(utf16NoBom, decoded));
        CHECK(decoded == sample);

        // Undecodable bytes fail without throwing.
        std::vector<uint8_t> invalid = {0x81, 0xFE, 0xFF, 0x00, 0x80};
        CHECK(!cmo::DecodeConfigBytes(invalid, decoded));
    }

    // v2.1 ConfigStore: on-open reload, no thread, last good kept.
    {
        const std::wstring path = cmo::ConfigFilePath();
        DeleteFileW(path.c_str());
        cmo::ConfigStore store;
        store.EnsureLoaded();
        CHECK(store.Revision() == 1);  // default file loaded
        CHECK(store.Snapshot() != nullptr);

        CHECK(cmo::WriteConfigFile(path, L"[appearance]\ncornerRadius = 7\n"));
        store.RefreshIfChanged();
        CHECK(store.Revision() == 2);
        CHECK(store.Snapshot()->appearance.cornerRadius == 7);

        store.RefreshIfChanged();  // unchanged: no bump
        CHECK(store.Revision() == 2);

        CHECK(cmo::WriteConfigFile(path, L"[appearance]\ncornerRadius = nope\n"));
        store.RefreshIfChanged();
        CHECK(store.Revision() == 3);
        CHECK(store.Snapshot()->appearance.cornerRadius == 8);
        std::wstring corrected;
        CHECK(cmo::ReadConfigFile(path, corrected));
        CHECK(corrected.find(L"cornerRadius = 8") != std::wstring::npos);
        CHECK(corrected.find(L"nope") == std::wstring::npos);
        DeleteFileW(path.c_str());
    }

    // v2.1 model fingerprint: flags, labels, and structure all matter.
    {
        cmo::MenuModel model = cmo::BuildCoreFileModel({L"a.txt"}, cmo::Shape::Single);
        cmo::RulesConfig config;
        config.revision = 3;
        const cmo::LayoutKey base = cmo::MakeLayoutKey(
            cmo::ContextSignature{cmo::Scope::Files, L".txt", cmo::Shape::Single,
                                  cmo::Variant::Normal},
            config, 96, true, model, 0);
        CHECK(base.modelFingerprint != 0);
        CHECK(base == cmo::MakeLayoutKey(base.sig, config, 96, true, model, 0));

        cmo::MenuModel flagged = model;
        flagged.items[0].flags |= cmo::kModelChecked;
        CHECK(!(base == cmo::MakeLayoutKey(base.sig, config, 96, true, flagged, 0)));

        cmo::MenuModel relabeled = model;
        relabeled.items[0].label += L"!";
        CHECK(!(base == cmo::MakeLayoutKey(base.sig, config, 96, true, relabeled, 0)));

        cmo::MenuModel restructured = model;
        cmo::MenuItem extra{};
        extra.id = 4242;
        extra.kind = cmo::ItemKind::Command;
        extra.label = L"Extra";
        restructured.items.push_back(extra);
        CHECK(!(base == cmo::MakeLayoutKey(base.sig, config, 96, true, restructured, 0)));
    }

    // v2.1 LRU: eviction order, promotion, clear releases.
    {
        cmo::LruMap<int> map;
        int released = 0;
        auto release = [&released](int) { ++released; };
        map.SetMaxEntries(2);
        map.Insert(L"a", 1, release);
        map.Insert(L"b", 2, release);
        CHECK(map.Size() == 2);
        CHECK(map.Find(L"a") != nullptr && *map.Find(L"a") == 1);
        map.Insert(L"c", 3, release);
        CHECK(map.Size() == 2);
        CHECK(released == 1);  // "b" was the least recent
        CHECK(map.Find(L"b") == nullptr);
        CHECK(map.Find(L"c") != nullptr);
        map.Clear(release);
        CHECK(map.Size() == 0);
        CHECK(released == 3);
    }

    // v2.1 appearance additions: parse, defaults, metrics.
    {
        cmo::RulesConfig config;
        std::vector<cmo::ConfigParseError> errors;
        CHECK(cmo::ParseRulesConfig(
            L"[appearance]\nverticalPadding = 8\nminWidth = 120\nmaxWidth = 400\n"
            L"itemPadding = 3\nseparatorSpacing = 2\nmarkerWidth = 18\n"
            L"fontWeight = bold\nfontStyle = italic\ncornerRadii = 2, 4, 6, 8\n"
            L"shadowOpacity = 90\nshadowBlur = 20\nmarker = bar\n"
            L"markerColor = #11223344\nheaderColor = #55667788\n"
            L"showAccelerators = strip\n",
            config, errors));
        CHECK(config.appearance.verticalPadding == 8);
        CHECK(config.appearance.minWidth == 120);
        CHECK(config.appearance.itemPadding == 3);
        CHECK(config.appearance.hasCornerRadii);
        CHECK(config.appearance.cornerRadii.bottomRight == 6);
        CHECK(config.appearance.marker == cmo::MarkerStyle::Bar);
        CHECK(config.appearance.fontWeight == cmo::FontWeightKind::Bold);
        CHECK(config.appearance.acceleratorMode == cmo::AcceleratorMode::Strip);

        const cmo::LayoutMetrics metrics =
            cmo::ResolveLayoutMetrics(config.appearance, 96, true);
        CHECK(metrics.verticalPadding == 8);
        CHECK(metrics.markerWidth == 18);
        CHECK(metrics.itemPadding == 3);
        CHECK(metrics.cornerRadii.topRight == 4);
        CHECK(metrics.markerColor == 0x11223344u);
        CHECK(metrics.headerColor == 0x55667788u);

        std::vector<cmo::ConfigParseError> badErrors;
        CHECK(cmo::ParseRulesConfig(L"[appearance]\nmarker = zigzag\n", config,
                                    badErrors));
        CHECK(!badErrors.empty());
        CHECK(config.appearance.marker == cmo::MarkerStyle::Dot);
        CHECK(cmo::ParseRulesConfig(L"[appearance]\ncornerRadii = 1, 2, 3\n", config,
                                    badErrors));
        CHECK(!config.appearance.hasCornerRadii);
    }

    // v2.1 per-item overrides: parse, glob, last wins, children.
    {
        cmo::RulesConfig config;
        std::vector<cmo::ConfigParseError> errors;
        CHECK(cmo::ParseRulesConfig(
            L"[item \"TortoiseSVN*\"]\nicon = C:\\svn.ico,0\nlabel = SVN\n"
            L"marker = bar\n"
            L"[item \"TortoiseSVN commit\"]\nlabel = Commit\n",
            config, errors));
        CHECK(config.overrides.size() == 2);

        cmo::MenuModel model{};
        cmo::MenuItem item{};
        item.id = 1;
        item.kind = cmo::ItemKind::Command;
        item.action = cmo::ActionKind::ShellVerb;
        item.label = L"TortoiseSVN commit";
        model.items.push_back(item);
        cmo::MenuItem child{};
        child.id = 2;
        child.kind = cmo::ItemKind::Command;
        child.action = cmo::ActionKind::ShellVerb;
        child.label = L"TortoiseSVN log";
        model.items.push_back(child);

        cmo::ItemContext ctx{};
        ctx.scope = cmo::Scope::Files;
        cmo::ApplyItemOverrides(model.items, config, ctx);
        CHECK(model.items[0].displayLabel == L"Commit");  // last match wins
        CHECK(model.items[0].markerOverride ==
              static_cast<int>(cmo::MarkerStyle::Bar));
        CHECK(model.items[0].iconRef == L"C:\\svn.ico,0");
        CHECK(model.items[1].displayLabel == L"SVN");
    }

    // v2.1 separators and headers: parse, insert, not selectable.
    {
        cmo::RulesConfig config;
        std::vector<cmo::ConfigParseError> errors;
        CHECK(cmo::ParseRulesConfig(
            L"[command \"Tools\"]\ntype = header\n"
            L"[command \"---\"]\ntype = separator\n",
            config, errors));
        CHECK(config.commands.size() == 2);
        CHECK(config.commands[0].type == cmo::CommandType::Header);
        CHECK(config.commands[1].type == cmo::CommandType::Separator);

        cmo::MenuModel model{};
        cmo::MenuItem fb{};
        fb.id = 1;
        fb.kind = cmo::ItemKind::Command;
        fb.action = cmo::ActionKind::Fallback;
        fb.label = L"Show classic menu";
        model.items.push_back(fb);
        cmo::ItemContext ctx{};
        ctx.scope = cmo::Scope::Files;
        cmo::InsertCustomItems(model, config, ctx);
        CHECK(model.items.size() == 3);
        CHECK(model.items[0].kind == cmo::ItemKind::Header);
        CHECK(model.items[0].label == L"Tools");
        CHECK(model.items[1].kind == cmo::ItemKind::Separator);

        cmo::LayoutMetrics metrics{};
        cmo::LayoutPanel panel = cmo::BuildLayoutPanel(model.items, metrics);
        cmo::MenuInputState state{};
        cmo::MenuStateKey(state, panel, cmo::MenuInputEvent::KeyDown);
        CHECK(state.keyboardIndex == 2);  // header and separator skipped
    }

    // v2.1 built-in actions: parse and map into the model.
    {
        cmo::RulesConfig config;
        std::vector<cmo::ConfigParseError> errors;
        CHECK(cmo::ParseRulesConfig(
            L"[command \"Copy path\"]\naction = copypath\n"
            L"[command \"New window\"]\naction = opennewwindow\n"
            L"[command \"Properties\"]\naction = properties\n",
            config, errors));
        CHECK(config.commands[0].action == cmo::BuiltinAction::CopyPath);

        cmo::MenuModel model{};
        cmo::MenuItem fb{};
        fb.id = 1;
        fb.kind = cmo::ItemKind::Command;
        fb.action = cmo::ActionKind::Fallback;
        fb.label = L"Show classic menu";
        model.items.push_back(fb);
        cmo::ItemContext ctx{};
        ctx.scope = cmo::Scope::Files;
        cmo::InsertCustomItems(model, config, ctx);
        CHECK(model.items[0].action == cmo::ActionKind::Builtin);
        CHECK(model.items[0].builtinAction == cmo::BuiltinAction::CopyPath);
        CHECK(model.items[2].builtinAction == cmo::BuiltinAction::Properties);

        std::vector<cmo::ConfigParseError> badErrors;
        CHECK(cmo::ParseRulesConfig(L"[command \"X\"]\naction = explode\n", config,
                                    badErrors));
        CHECK(!badErrors.empty());
        CHECK(config.commands.size() == 1 &&
              config.commands[0].action == cmo::BuiltinAction::None);
    }

    // v2.1 accelerators: &&, single &, trailing &, ranges.
    {
        cmo::AcceleratorText text = cmo::StripAccelerators(L"P&roperties");
        CHECK(text.text == L"Properties");
        CHECK(text.underlineRanges.size() == 1);
        CHECK(text.underlineRanges[0].first == 1);  // 'r'

        text = cmo::StripAccelerators(L"SVN Chec&kout");
        CHECK(text.text == L"SVN Checkout");
        CHECK(text.underlineRanges[0].first == 8);

        text = cmo::StripAccelerators(L"Fish && Chips");
        CHECK(text.text == L"Fish & Chips");
        CHECK(text.underlineRanges.empty());

        text = cmo::StripAccelerators(L"Open with &");
        CHECK(text.text == L"Open with &");
        CHECK(text.underlineRanges.empty());

        text = cmo::StripAccelerators(L"");
        CHECK(text.text.empty());
    }

    // v2.1 geometry: padding, separator spacing, x-bounded hit tests.
    {
        cmo::LayoutMetrics metrics{};
        metrics.itemHeight = 28;
        metrics.separatorHeight = 7;
        metrics.verticalPadding = 4;
        metrics.separatorSpacing = 3;
        metrics.iconSize = 16;
        metrics.padding = 6;
        metrics.gutterWidth = 22;
        metrics.submenuArrowWidth = 16;
        metrics.markerWidth = 14;
        metrics.textColor = 0xFFFFFFFFu;

        std::vector<cmo::MenuItem> items;
        cmo::MenuItem open{};
        open.id = 1;
        open.kind = cmo::ItemKind::Command;
        open.action = cmo::ActionKind::ShellVerb;
        open.label = L"Open";
        items.push_back(open);
        cmo::MenuItem sep{};
        sep.id = 2;
        sep.kind = cmo::ItemKind::Separator;
        items.push_back(sep);
        cmo::MenuItem next{};
        next.id = 3;
        next.kind = cmo::ItemKind::Command;
        next.action = cmo::ActionKind::ShellVerb;
        next.label = L"Next";
        items.push_back(next);

        cmo::LayoutPanel panel = cmo::BuildLayoutPanel(items, metrics);
        CHECK(panel.items[0].rect.top == 4);
        CHECK(panel.items[1].rect.bottom - panel.items[1].rect.top == 7 + 6);
        CHECK(panel.items[2].rect.top == 4 + 28 + 13);
        CHECK(panel.size.cy == 4 + 28 + 13 + 28 + 4);

        cmo::MenuInputState state{};
        CHECK(cmo::MenuStateItemAt(panel, state, POINT{-1, 10}) == -1);
        CHECK(cmo::MenuStateItemAt(panel, state, POINT{panel.size.cx + 5, 10}) == -1);
        CHECK(cmo::MenuStateItemAt(panel, state, POINT{5, 10}) == 0);
    }

    // v2.1 session routing: deepest level, margin-adjusted panel points.
    {
        std::vector<RECT> rects = {{0, 0, 200, 300}, {180, 50, 380, 200}};
        CHECK(cmo::SessionLevelAtPoint(rects, POINT{10, 10}) == 0);
        CHECK(cmo::SessionLevelAtPoint(rects, POINT{190, 60}) == 1);
        CHECK(cmo::SessionLevelAtPoint(rects, POINT{400, 10}) == -1);

        const POINT panel = cmo::PanelPointForWindow(
            RECT{100, 100, 400, 400}, 12, POINT{120, 150});
        CHECK(panel.x == 8);
        CHECK(panel.y == 38);
    }

    // v2.1 shadow: per-corner mask and blurred bitmap.
    {
        std::vector<uint8_t> mask;
        cmo::BuildRoundedRectMaskRadii(20, 20, 0, 8, 0, 8, mask);
        CHECK(mask.size() == 400);
        CHECK(mask[0] == 255);             // top-left radius 0: square corner
        CHECK(mask[19] == 0);              // top-right radius 8: cut
        CHECK(mask[19 * 20] == 0);         // bottom-left radius 8: cut
        CHECK(mask[19 * 20 + 19] == 255);  // bottom-right radius 0: square

        std::vector<uint32_t> pixels;
        int outW = 0;
        int outH = 0;
        cmo::CornerRadii radii{};
        radii.topLeft = radii.topRight = radii.bottomRight = radii.bottomLeft = 6;
        CHECK(cmo::BlurPasses(0) == 0);
        CHECK(cmo::BlurPasses(12) == 3);
        CHECK(cmo::BlurPasses(64) == 16);
        CHECK(cmo::BuildShadowBitmap(60, 40, radii, 12, 8, 128, 0xFF000000, 4,
                                     pixels, outW, outH));
        CHECK(outW > 0 && outH > 0);
        CHECK(pixels.size() == static_cast<size_t>(outW) * outH);
        bool anyAlpha = false;
        for (uint32_t pixel : pixels) {
            if ((pixel >> 24) != 0) {
                anyAlpha = true;
                CHECK((pixel & 0x00FFFFFF) == 0);  // black, premultiplied
            }
        }
        CHECK(anyAlpha);
    }

    // v2.1 marker column geometry and style resolution.
    {
        cmo::MarkerStyle style = cmo::MarkerStyle::None;
        CHECK(cmo::ParseMarkerStyle(L"dot", style) && style == cmo::MarkerStyle::Dot);
        CHECK(cmo::ParseMarkerStyle(L"BAR", style) && style == cmo::MarkerStyle::Bar);
        CHECK(!cmo::ParseMarkerStyle(L"zigzag", style));

        cmo::LayoutMetrics metrics{};
        metrics.itemHeight = 28;
        metrics.separatorHeight = 7;
        metrics.verticalPadding = 0;
        metrics.markerWidth = 14;
        metrics.iconSize = 16;
        metrics.padding = 6;
        metrics.marker = cmo::MarkerStyle::Dot;
        cmo::MenuItem item{};
        item.id = 1;
        item.kind = cmo::ItemKind::Command;
        item.action = cmo::ActionKind::ShellVerb;
        item.label = L"Open";
        item.flags = cmo::kModelChecked;
        std::vector<cmo::MenuItem> items = {item};
        cmo::LayoutPanel panel = cmo::BuildLayoutPanel(items, metrics);
        CHECK(panel.items[0].markerRect.right > panel.items[0].markerRect.left);
        CHECK(panel.items[0].iconRect.left >= panel.items[0].markerRect.right);
        CHECK(panel.items[0].textRect.left ==
              panel.items[0].iconRect.left + metrics.iconSize + metrics.padding);
    }

    // v2.1 width clamping: min and max, never below content.
    {
        cmo::LayoutMetrics metrics{};
        metrics.itemHeight = 28;
        metrics.verticalPadding = 0;
        metrics.padding = 6;
        metrics.markerWidth = 14;
        metrics.iconSize = 16;
        cmo::MenuItem item{};
        item.id = 1;
        item.kind = cmo::ItemKind::Command;
        item.action = cmo::ActionKind::ShellVerb;
        item.label = L"Short";
        std::vector<cmo::MenuItem> items = {item};

        const cmo::LayoutPanel autoPanel = cmo::BuildLayoutPanel(items, metrics);
        const int contentWidth = autoPanel.size.cx;

        metrics.minWidth = contentWidth + 50;
        CHECK(cmo::BuildLayoutPanel(items, metrics).size.cx == contentWidth + 50);

        metrics.minWidth = 0;
        metrics.maxWidth = contentWidth - 20;
        CHECK(cmo::BuildLayoutPanel(items, metrics).size.cx == contentWidth);

        metrics.maxWidth = contentWidth + 30;
        CHECK(cmo::BuildLayoutPanel(items, metrics).size.cx == contentWidth);
    }

    // v2.1 review fixes: caps, font mapping, itemPadding, label, targets.
    {
        cmo::RulesConfig config;
        std::vector<cmo::ConfigParseError> errors;
        CHECK(cmo::ParseRulesConfig(L"[appearance]\nshadowBlur = 20000\n", config,
                                    errors));
        CHECK(config.appearance.shadowBlur == 64);
        CHECK(!errors.empty());
        CHECK(cmo::ParseRulesConfig(L"[appearance]\nmaxWidth = 1000000\n", config,
                                    errors));
        CHECK(config.appearance.maxWidth == 4096);
        CHECK(cmo::ParseRulesConfig(L"[appearance]\nitemHeight = 100000\n", config,
                                    errors));
        CHECK(config.appearance.itemHeight == 256);
        CHECK(cmo::ParseRulesConfig(
            L"[appearance]\nshadowBlur = 32\nmaxWidth = 4096\n", config, errors));

        std::vector<uint32_t> pixels;
        int outW = 0;
        int outH = 0;
        cmo::CornerRadii radii{};
        CHECK(!cmo::BuildShadowBitmap(4000, 4000, radii, 64, 64, 120, 0xFF000000,
                                      4, pixels, outW, outH));

        CHECK(cmo::FontWeightToDwrite(cmo::FontWeightKind::Bold) ==
              DWRITE_FONT_WEIGHT_BOLD);
        CHECK(cmo::FontStyleToDwrite(cmo::FontStyleKind::Italic) ==
              DWRITE_FONT_STYLE_ITALIC);
        CHECK(cmo::FontWeightToDwrite(cmo::FontWeightKind::Normal) ==
              DWRITE_FONT_WEIGHT_NORMAL);

        cmo::LayoutMetrics metrics{};
        metrics.itemHeight = 28;
        metrics.verticalPadding = 0;
        metrics.padding = 6;
        metrics.itemPadding = 10;
        metrics.markerWidth = 14;
        metrics.iconSize = 16;
        cmo::MenuItem item{};
        item.id = 1;
        item.kind = cmo::ItemKind::Command;
        item.action = cmo::ActionKind::ShellVerb;
        item.label = L"Open";
        std::vector<cmo::MenuItem> items = {item};
        cmo::LayoutPanel padded = cmo::BuildLayoutPanel(items, metrics);
        CHECK(padded.items[0].markerRect.left == 10);

        cmo::LayoutMetrics autoMetrics{};
        autoMetrics.itemHeight = 28;
        autoMetrics.verticalPadding = 0;
        autoMetrics.padding = 6;
        autoMetrics.markerWidth = 14;
        autoMetrics.iconSize = 16;
        cmo::MenuItem longItem = item;
        longItem.displayLabel = L"TortoiseSVN commit with a very long label";
        std::vector<cmo::MenuItem> shortItems = {item};
        std::vector<cmo::MenuItem> longItems = {longItem};
        CHECK(cmo::BuildLayoutPanel(longItems, autoMetrics).size.cx >
              cmo::BuildLayoutPanel(shortItems, autoMetrics).size.cx);

        cmo::InvocationContext ctx{};
        ctx.directory = L"C:\\src";
        auto targets =
            cmo::BuiltinActionTargets(cmo::BuiltinAction::OpenNewWindow, ctx);
        CHECK(targets.size() == 1 && targets[0] == L"C:\\src");
        ctx.paths = {L"C:\\a", L"C:\\b"};
        targets = cmo::BuiltinActionTargets(cmo::BuiltinAction::OpenNewWindow, ctx);
        CHECK(targets.size() == 2);
        targets = cmo::BuiltinActionTargets(cmo::BuiltinAction::Properties, ctx);
        CHECK(targets.size() == 1 && targets[0] == L"C:\\a");
    }

    // v2.2 warm-up coordination: jitter, mutex name, cache Has.
    {
        CHECK(cmo::WarmupJitterMs(0) == 0);
        CHECK(cmo::WarmupJitterMs(3000) == 3000);
        CHECK(cmo::WarmupJitterMs(3001) == 0);
        CHECK(cmo::WarmupJitterMs(123456) >= 0 && cmo::WarmupJitterMs(123456) <= 3000);
        const std::wstring mutexName = cmo::WarmupMutexName();
        CHECK(mutexName.rfind(L"Local\\", 0) == 0);

        cmo::Cache cache;
        cmo::ContextSignature sig{cmo::Scope::Files, L".txt", cmo::Shape::Single,
                                  cmo::Variant::Normal};
        CHECK(!cache.Has(sig));
        cmo::MenuModel model{};
        model.sig = sig;
        cache.Put(std::move(model));
        CHECK(cache.Has(sig));
    }

    // v2.2 config schema: every default accepted, unknown keys rejected,
    // error text names the valid values.
    {
        CHECK(cmo::kConfigSchemaVersion >= 1);
        for (const cmo::ConfigSchemaEntry& entry : cmo::kAppearanceSchema) {
            cmo::Appearance appearance{};
            CHECK(cmo::ApplyAppearanceValue(appearance, entry.key,
                                            entry.defaultValue));
        }
        cmo::Appearance appearance{};
        CHECK(!cmo::ApplyAppearanceValue(appearance, L"notAKey", L"1"));
        CHECK(cmo::SchemaFind(L"ITEMHEIGHT") != nullptr);
        CHECK(cmo::SchemaFind(L"notakey") == nullptr);

        cmo::RulesConfig config;
        std::vector<cmo::ConfigParseError> errors;
        CHECK(cmo::ParseRulesConfig(L"[appearance]\nmarker = zigzag\n", config,
                                    errors));
        CHECK(!errors.empty());
        CHECK(errors[0].message.find(L"dot|check|bar|none") != std::wstring::npos);
    }

    // v2.2 [meta]: parse the version and read it from raw text.
    {
        cmo::RulesConfig config;
        std::vector<cmo::ConfigParseError> errors;
        CHECK(cmo::ParseRulesConfig(L"[meta]\nschemaVersion = 3\n", config, errors));
        CHECK(config.schemaVersion == 3);
        CHECK(cmo::ParseRulesConfig(L"[meta]\nnotAKey = 1\n", config, errors));
        CHECK(!errors.empty());

        CHECK(cmo::ReadSchemaVersion(L"[meta]\nschemaVersion = 2\n") == 2);
        CHECK(cmo::ReadSchemaVersion(L"[appearance]\nitemHeight = 28\n") == 0);
        CHECK(cmo::ReadSchemaVersion(L"[meta]\nschemaVersion = x\n") == 0);
    }

    // v2.2 generated default file: every key, defaults, meta, parseable.
    {
        const std::wstring text = cmo::GenerateDefaultConfigText();
        for (const cmo::ConfigSchemaEntry& entry : cmo::kAppearanceSchema) {
            CHECK(text.find(entry.key) != std::wstring::npos);
            CHECK(text.find(cmo::NormalizeAppearanceValue(
                      entry, entry.defaultValue)) != std::wstring::npos);
        }
        CHECK(text.find(L"[meta]") != std::wstring::npos);
        CHECK(text.find(L"schemaVersion = 7") != std::wstring::npos);
        CHECK(text.find(L"[rules]") != std::wstring::npos);
        CHECK(text.find(L"[command ") != std::wstring::npos);
        cmo::RulesConfig config;
        std::vector<cmo::ConfigParseError> errors;
        CHECK(cmo::ParseRulesConfig(text, config, errors));
        CHECK(config.schemaVersion == 7);
    }

    // v2.3/v2.4 review fixes: inert examples, '#' comments.
    {
        cmo::RulesConfig generated;
        std::vector<cmo::ConfigParseError> errors;
        CHECK(cmo::ParseRulesConfig(cmo::GenerateDefaultConfigText(), generated,
                                    errors));
        CHECK(generated.commands.empty());
        CHECK(generated.submenus.empty());
        CHECK(generated.overrides.empty());

        cmo::RulesConfig comments;
        CHECK(cmo::ParseRulesConfig(
            L"# a comment\n[appearance]\nbackground = #11223344\n", comments,
            errors));
        CHECK(comments.appearance.background == 0x11223344u);
    }

    // v2.4 readable colors: R, G, B, A decimals; hex still accepted.
    {
        uint32_t color = 0;
        CHECK(cmo::ParseColor(L"30, 30, 30, 240", color) && color == 0xF01E1E1Eu);
        CHECK(cmo::ParseColor(L"255, 0, 128", color) && color == 0xFFFF0080u);
        CHECK(cmo::ParseColor(L" 1 , 2 , 3 , 4 ", color) && color == 0x04010203u);
        CHECK(!cmo::ParseColor(L"256, 0, 0, 0", color));
        CHECK(!cmo::ParseColor(L"1, 2", color));
        CHECK(!cmo::ParseColor(L"1, 2, 3, 4, 5", color));
        CHECK(cmo::ParseColor(L"#11223344", color) && color == 0x11223344u);
        CHECK(cmo::FormatColorRgba(0xF01E1E1E) == L"30, 30, 30, 240");
        CHECK(cmo::FormatColorRgba(0xFFFF0080) == L"255, 0, 128, 255");
    }

    // v2.4 canonical config: active defaults, groups, one appearance section.
    {
        const std::wstring text = cmo::GenerateDefaultConfigText();
        CHECK(cmo::CountSubstring(text, L"[appearance]") == 1);
        CHECK(text.find(L"; --- Colors ---") != std::wstring::npos);
        CHECK(text.find(L"; --- Layout ---") != std::wstring::npos);
        CHECK(text.find(L"background = 30, 30, 30, 240") != std::wstring::npos);
        CHECK(text.find(L"; background") == std::wstring::npos);
        CHECK(text.find(L"itemHeight = 28") != std::wstring::npos);
        CHECK(text.find(L"; itemPadding = 6") != std::wstring::npos);
        CHECK(text.find(L"unset") != std::wstring::npos);
        cmo::RulesConfig config;
        std::vector<cmo::ConfigParseError> errors;
        CHECK(cmo::ParseRulesConfig(text, config, errors));
        CHECK(config.appearance.itemHeight == 28);
        CHECK(config.appearance.background == 0xF01E1E1Eu);
        CHECK(config.commands.empty());
        CHECK(config.submenus.empty());
        CHECK(config.overrides.empty());

        const std::wstring legacy =
            L"; my notes\n[appearance]\nitemHeight = 30\n[appearance]\n"
            L"background = #11223344\n[rules]\n; my rule\n"
            L"hide = label:\"Cast to Device\"\n[meta]\nschemaVersion = 0\n";
        const std::wstring canonical = cmo::CanonicalizeConfig(legacy, 7);
        CHECK(cmo::CountSubstring(canonical, L"[appearance]") == 1);
        CHECK(canonical.find(L"itemHeight = 30") != std::wstring::npos);
        CHECK(canonical.find(L"background = 34, 51, 68, 17") != std::wstring::npos);
        CHECK(canonical.find(L"hide = label:\"Cast to Device\"") != std::wstring::npos);
        CHECK(canonical.find(L"; my rule") != std::wstring::npos);
        CHECK(canonical.find(L"schemaVersion = 7") != std::wstring::npos);
        cmo::RulesConfig reparsed;
        CHECK(cmo::ParseRulesConfig(canonical, reparsed, errors));
        CHECK(reparsed.appearance.itemHeight == 30);
        CHECK(reparsed.rules.size() == 1);
    }

    // v2.4 round-trip: every schema key's custom value survives.
    {
        auto testValue = [](const cmo::ConfigSchemaEntry& entry) -> std::wstring {
            switch (entry.type) {
                case cmo::SettingType::Bool:
                    return wcscmp(entry.defaultValue, L"true") == 0 ? L"false"
                                                                    : L"true";
                case cmo::SettingType::Int:
                    return std::to_wstring(_wtoi(entry.defaultValue) + 1);
                case cmo::SettingType::Color:
                    return L"1, 2, 3, 4";
                case cmo::SettingType::Font:
                    return L"Arial, 10";
                case cmo::SettingType::IntList:
                    return L"1, 2, 3, 4";
                case cmo::SettingType::Enum: {
                    const std::wstring values(entry.validValues);
                    const size_t bar = values.rfind(L'|');
                    return bar == std::wstring::npos ? values : values.substr(bar + 1);
                }
            }
            return L"";
        };
        std::wstring custom = L"[appearance]\n";
        for (const cmo::ConfigSchemaEntry& entry : cmo::kAppearanceSchema) {
            custom += entry.key;
            custom += L" = ";
            custom += testValue(entry);
            custom += L"\n";
        }
        const std::wstring canonical = cmo::CanonicalizeConfig(custom, 5);
        for (const cmo::ConfigSchemaEntry& entry : cmo::kAppearanceSchema) {
            const std::wstring expected =
                std::wstring(entry.key) + L" = " +
                cmo::NormalizeAppearanceValue(entry, testValue(entry));
            CHECK(canonical.find(expected) != std::wstring::npos);
        }
        const std::wstring themed =
            L"[appearance]\nitemHeight = 28\n[appearance.light]\nitemHeight = 40\n";
        const std::wstring themeCanonical = cmo::CanonicalizeConfig(themed, 5);
        CHECK(themeCanonical.find(L"[appearance.light]") != std::wstring::npos);
        CHECK(themeCanonical.find(L"itemHeight = 40") != std::wstring::npos);
    }

    // v2.4 lifecycle: old files canonicalize, bad files are untouched.
    {
        const std::wstring path = cmo::ConfigFilePath();
        DeleteFileW(path.c_str());
        cmo::ConfigStore store;
        store.EnsureLoaded();
        CHECK(store.Revision() == 1);

        CHECK(cmo::WriteConfigFile(
            path, L"[appearance]\nitemHeight = 30\n[meta]\nschemaVersion = 0\n"));
        store.RefreshIfChanged();
        CHECK(store.Revision() == 2);
        CHECK(store.Snapshot()->appearance.itemHeight == 30);
        std::wstring text;
        CHECK(cmo::ReadConfigFile(path, text));
        CHECK(text.find(L"schemaVersion = 7") != std::wstring::npos);
        CHECK(text.find(L"itemHeight = 30") != std::wstring::npos);
        CHECK(text.find(L"; itemPadding = 6") != std::wstring::npos);

        const std::wstring bad = L"this is not a config";
        CHECK(cmo::WriteConfigFile(path, bad));
        store.RefreshIfChanged();
        CHECK(store.Revision() == 3);
        std::wstring rewritten;
        CHECK(cmo::ReadConfigFile(path, rewritten));
        CHECK(rewritten.find(L"[appearance]") != std::wstring::npos);
        CHECK(rewritten.find(L"schemaVersion = 7") != std::wstring::npos);
        DeleteFileW(path.c_str());
    }

    // v2.4 guide mentions every schema key.
    {
        HANDLE file = CreateFileW(L"docs\\CONFIG.md", GENERIC_READ, FILE_SHARE_READ,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                  nullptr);
        CHECK(file != INVALID_HANDLE_VALUE);
        if (file != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER size = {};
            GetFileSizeEx(file, &size);
            std::vector<char> bytes(static_cast<size_t>(size.QuadPart));
            DWORD read = 0;
            ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read,
                     nullptr);
            CloseHandle(file);
            std::wstring text;
            if (!bytes.empty()) {
                const int wide = MultiByteToWideChar(
                    CP_UTF8, 0, bytes.data(), static_cast<int>(read), nullptr, 0);
                text.resize(static_cast<size_t>(wide));
                MultiByteToWideChar(CP_UTF8, 0, bytes.data(),
                                    static_cast<int>(read), text.data(), wide);
            }
            for (const cmo::ConfigSchemaEntry& entry : cmo::kAppearanceSchema) {
                CHECK(text.find(entry.key) != std::wstring::npos);
            }
        }
    }

    // v2.5 review fixes: shadow placement/falloff, theme unset, idempotence.
    {
        CHECK(cmo::ShadowMargin(12, 12, 0, 2) == 26);
        CHECK(cmo::ShadowMargin(64, 64, 32, -32) == 160);
        CHECK(cmo::ShadowMargin(0, 0, 0, 0) == 0);

        cmo::CornerRadii radii{};
        std::vector<uint32_t> pixels;
        int outW = 0;
        int outH = 0;
        CHECK(cmo::BuildShadowBitmap(40, 40, radii, 0, 8, 255, 0xFF000000, 4,
                                     pixels, outW, outH));
        const uint32_t center = pixels[(outH / 2) * outW + outW / 2] >> 24;
        const uint32_t corner = pixels[0] >> 24;
        CHECK(center > 200);
        CHECK(corner < 64);  // a blurred shadow fades toward its edge

        // The shadow color tints the bitmap (premultiplied red).
        CHECK(cmo::BuildShadowBitmap(40, 40, radii, 0, 8, 255, 0xFFFF0000, 4,
                                     pixels, outW, outH));
        const uint32_t tinted = pixels[(outH / 2) * outW + outW / 2];
        CHECK(((tinted >> 16) & 0xFF) > 200);
        CHECK(((tinted >> 8) & 0xFF) < 32);
        CHECK((tinted & 0xFF) < 32);

        const std::wstring themed =
            L"[appearance]\n[appearance.light]\nitemPadding = nope\n"
            L"cornerRadii = 1, 2, 3\nmarkerColor = nope\n";
        const std::wstring canonical = cmo::CanonicalizeConfig(themed, 3);
        CHECK(canonical.find(L"\nitemPadding = 6\n") == std::wstring::npos);
        CHECK(canonical.find(L"\ncornerRadii = 2, 4, 6, 8\n") == std::wstring::npos);
        CHECK(canonical.find(L"\nmarkerColor = 255, 255, 255, 255\n") ==
              std::wstring::npos);

        const std::wstring messy =
            L"[appearance]\nitemHeight = 30\n[rules]\n; note\nbadline\n"
            L"[custom]\nfoo = bar\n[meta]\nschemaVersion = 0\n";
        const std::wstring once = cmo::CanonicalizeConfig(messy, 3);
        const std::wstring twice = cmo::CanonicalizeConfig(once, 3);
        CHECK(once == twice);

        cmo::Appearance appearance{};
        appearance.blurStrength = 12;
        CHECK(cmo::ResolveLayoutMetrics(appearance, 96, true).blurPasses == 3);
        CHECK(cmo::ResolveLayoutMetrics(appearance, 192, true).blurPasses == 6);
    }

    // v2.6 named and stock icon tables.
    {
        std::wstring glyph;
        CHECK(cmo::ResolveNamedIcon(L"copy", glyph) && glyph == L"E8C8");
        CHECK(cmo::ResolveNamedIcon(L"DELETE", glyph) && glyph == L"E74D");
        CHECK(!cmo::ResolveNamedIcon(L"notanicon", glyph));
        CHECK(cmo::IconRefGlyph(L"@glyph:E8C8") == L"E8C8");
        CHECK(cmo::IconRefGlyph(L"@icon:copy") == L"E8C8");
        CHECK(cmo::IconRefGlyph(L"@ext:.txt").empty());

        int id = 0;
        bool shellStock = false;
        CHECK(cmo::ResolveStockIcon(L"info", id, shellStock) && !shellStock);
        CHECK(cmo::ResolveStockIcon(L"folder", id, shellStock) && shellStock);
        CHECK(!cmo::ResolveStockIcon(L"notastock", id, shellStock));
    }

    // v2.6 view actions have distinct icons.
    {
        cmo::MenuModel model =
            cmo::BuildCoreModel(cmo::Scope::Background, {}, cmo::Shape::Single);
        std::vector<std::wstring> viewIcons;
        for (const cmo::MenuItem& item : model.items) {
            if (item.kind == cmo::ItemKind::Submenu && !item.children.empty()) {
                for (const cmo::MenuItem& child : item.children) {
                    if (child.viewAction !=
                            static_cast<uint32_t>(cmo::ViewAction::None) &&
                        !child.iconRef.empty()) {
                        viewIcons.push_back(child.iconRef);
                    }
                }
            }
        }
        for (size_t i = 0; i < viewIcons.size(); ++i) {
            for (size_t j = i + 1; j < viewIcons.size(); ++j) {
                CHECK(viewIcons[i] != viewIcons[j]);
            }
        }
        CHECK(viewIcons.size() >= 8);
    }

    // v2.6 generated lines document values and ranges.
    {
        const std::wstring text = cmo::GenerateDefaultConfigText();
        CHECK(text.find(L"none | fade | slide") != std::wstring::npos);
        CHECK(text.find(L"1-256") != std::wstring::npos);
        CHECK(text.find(L"dot | check | bar | none") != std::wstring::npos);
        CHECK(text.find(L"schemaVersion = 7") != std::wstring::npos);
    }

    // v2.6 advanced grouping: toggles, exclude, keep.
    {
        auto makeItem = [](uint32_t id, const wchar_t* label, uint32_t flags) {
            cmo::MenuItem item{};
            item.id = id;
            item.kind = cmo::ItemKind::Command;
            item.action = cmo::ActionKind::ShellVerb;
            item.label = label;
            item.flags = flags;
            return item;
        };
        auto build = [&makeItem] {
            std::vector<cmo::MenuItem> items;
            items.push_back(makeItem(1, L"Share", 0));
            items.push_back(makeItem(2, L"WinRAR", cmo::kModelThirdParty));
            cmo::MenuItem fallback = makeItem(3, L"Show classic menu", 0);
            fallback.action = cmo::ActionKind::Fallback;
            items.push_back(fallback);
            return items;
        };
        auto hasSubmenu = [](const std::vector<cmo::MenuItem>& items) {
            for (const cmo::MenuItem& item : items) {
                if (item.kind == cmo::ItemKind::Submenu) return true;
            }
            return false;
        };

        cmo::AdvancedGroupingOptions both;
        both.moveWindows = true;
        both.moveThirdParty = true;
        both.windowsItems = {L"Share"};
        std::vector<cmo::MenuItem> items = build();
        cmo::ReorganizeAdvancedItems(items, both);
        CHECK(hasSubmenu(items));

        cmo::AdvancedGroupingOptions thirdOff;
        thirdOff.moveWindows = true;
        thirdOff.moveThirdParty = false;
        thirdOff.windowsItems = {L"Share"};
        items = build();
        cmo::ReorganizeAdvancedItems(items, thirdOff);
        CHECK(items.size() == 3);  // Share moved, WinRAR stayed

        cmo::AdvancedGroupingOptions excluded;
        excluded.exclude = {L"WinRAR"};
        items = build();
        cmo::ReorganizeAdvancedItems(items, excluded);
        bool winrarTop = false;
        for (const cmo::MenuItem& item : items) {
            if (item.label == L"WinRAR") winrarTop = true;
        }
        CHECK(winrarTop);

        cmo::RulesConfig config;
        std::vector<cmo::ConfigParseError> errors;
        CHECK(cmo::ParseRulesConfig(L"[rules]\nkeep = label:WinRAR\n", config,
                                    errors));
        cmo::AdvancedGroupingOptions kept;
        kept.rules = &config;
        items = build();
        cmo::ReorganizeAdvancedItems(items, kept);
        winrarTop = false;
        for (const cmo::MenuItem& item : items) {
            if (item.label == L"WinRAR") winrarTop = true;
        }
        CHECK(winrarTop);
    }

    // v2.7 theme store: files are complete, isolated from menu.ini, and
    // tolerant.
    {
        using namespace cmo;
        CHECK(ThemeSlug(L"Windows 11 Dark") == L"windows-11-dark");
        CHECK(ThemeSlug(L"AMOLED Black") == L"amoled-black");
        CHECK(ThemeSlug(L"Terminal Green") == L"terminal-green");

        const int nord = ThemeIndexFromName(L"Nord");
        const std::wstring themePath = ThemeFilePath(nord);
        DeleteFileW(themePath.c_str());

        // First use creates a complete file.
        g_themeStore.ApplySelectedTheme(nord);
        std::wstring text;
        CHECK(ReadConfigFile(themePath, text));
        for (const ConfigSchemaEntry& entry : kAppearanceSchema) {
            CHECK(text.find(entry.key) != std::wstring::npos);
        }

        // menu.ini is untouched by theme selection, and its dark/light
        // overrides are ignored while a theme is active.
        const std::wstring configPath = ConfigFilePath();
        CHECK(WriteConfigFile(configPath,
                              L"[appearance]\nshadowColor = 1, 2, 3, 4\n"
                              L"[appearance.dark]\nbackground = 9, 9, 9, 255\n"));
        std::wstring before;
        CHECK(ReadConfigFile(configPath, before));
        g_themeStore.ApplySelectedTheme(ThemeIndexFromName(L"Dracula"));
        std::wstring after;
        CHECK(ReadConfigFile(configPath, after));
        CHECK(before == after);

        // A missing key takes the template value, not menu.ini's or the
        // schema's. CRLF lines and unknown keys are tolerated like menu.ini.
        CHECK(WriteConfigFile(themePath,
                              L"[appearance]\r\nbackground = 1, 2, 3, 255\r\n"
                              L"unknownKey = 1\r\n"));
        g_themeStore.ApplySelectedTheme(nord);
        CHECK(g_themeStore.Snapshot()->background == 0xFF010203);
        CHECK(g_themeStore.Snapshot()->shadowOpacity == 130);  // Nord template
        CHECK(ReadConfigFile(themePath, text));
        CHECK(text.find(L"shadowOpacity = 130") != std::wstring::npos);
        CHECK(text.find(L"unknownKey") == std::wstring::npos);

        // Invalid values fall back to the template and are rewritten.
        CHECK(WriteConfigFile(themePath, L"[appearance]\ncornerRadius = nope\n"));
        g_themeStore.ApplySelectedTheme(nord);
        CHECK(g_themeStore.Snapshot()->cornerRadius == 6);
        CHECK(ReadConfigFile(themePath, text));
        CHECK(text.find(L"cornerRadius = 6") != std::wstring::npos);

        // Light/dark sections in a theme file are ignored and stripped.
        CHECK(WriteConfigFile(themePath,
                              L"[appearance]\nbackground = 1, 2, 3, 255\n"
                              L"[appearance.dark]\nbackground = 9, 9, 9, 255\n"));
        g_themeStore.ApplySelectedTheme(nord);
        CHECK(g_themeStore.Snapshot()->background == 0xFF010203);
        CHECK(ReadConfigFile(themePath, text));
        CHECK(text.find(L"[appearance.dark]") == std::wstring::npos);

        // Edits are picked up by RefreshIfChanged.
        const uint64_t revision = g_themeStore.Revision();
        CHECK(WriteConfigFile(themePath,
                              L"[appearance]\nbackground = 7, 8, 9, 255\n"));
        g_themeStore.RefreshIfChanged();
        CHECK(g_themeStore.Snapshot()->background == 0xFF070809);
        CHECK(g_themeStore.Revision() > revision);

        // Custom (menu.ini) has no theme appearance.
        g_themeStore.ApplySelectedTheme(0);
        CHECK(g_themeStore.Snapshot() == nullptr);
        RulesConfig userConfig;
        userConfig.appearance.background = 0xFF112233;
        CHECK(EffectiveAppearance(userConfig, true).background == 0xFF112233);
        g_themeStore.ApplySelectedTheme(nord);
        CHECK(EffectiveAppearance(userConfig, true).background == 0xFF070809);

        // Layout keys include the theme revision.
        ContextSignature sig{};
        MenuModel model{};
        const LayoutKey key1 = MakeLayoutKey(sig, userConfig, 96, true, model, 1);
        const LayoutKey key2 = MakeLayoutKey(sig, userConfig, 96, true, model, 2);
        CHECK(!(key1 == key2));

        DeleteFileW(themePath.c_str());
        DeleteFileW(configPath.c_str());
    }

    // v2.7 theme files: light/dark sections are reported, not silently
    // ignored.
    {
        CHECK(!cmo::ThemeTextHasSubThemeSections(
            L"[appearance]\nbackground = 1, 2, 3, 255\n"));
        CHECK(cmo::ThemeTextHasSubThemeSections(
            L"[appearance.dark]\nbackground = 0, 0, 0, 255\n"));
        CHECK(cmo::ThemeTextHasSubThemeSections(
            L"[appearance.light]\nbackground = 255, 255, 255, 255\n"));
        CHECK(cmo::ThemeTextHasSubThemeSections(
            L"[APPEARANCE.DARK]\nbackground = 0, 0, 0, 255\n"));
    }

    // v2.7 theme files: every built-in theme generates a complete appearance
    // block, and Windows 11 Dark/Light are fixed single palettes.
    {
        CHECK(cmo::ThemeIndexFromName(L"Cyberpunk") > 0);
        CHECK(cmo::ThemeIndexFromName(L"Synthwave") > 0);
        CHECK(cmo::ThemeIndexFromName(L"Terminal Green") > 0);
        CHECK(cmo::ThemeIndexFromName(L"Amber CRT") > 0);
        CHECK(cmo::ThemeIndexFromName(L"Tokyo Night") > 0);
        for (size_t i = 1; i < cmo::kThemesCount; ++i) {
            const std::wstring text = cmo::GenerateThemeText(static_cast<int>(i));
            CHECK(text.find(L"[appearance]") == 0);
            for (const cmo::ConfigSchemaEntry& entry : cmo::kAppearanceSchema) {
                CHECK(text.find(entry.key) != std::wstring::npos);
            }
            cmo::RulesConfig parsed;
            std::vector<cmo::ConfigParseError> errors;
            CHECK(cmo::ParseRulesConfig(text, parsed, errors));
            CHECK(errors.empty());
        }
        const int light = cmo::ThemeIndexFromName(L"Windows 11 Light");
        const std::wstring lightText = cmo::GenerateThemeText(light);
        CHECK(lightText.find(L"[appearance.dark]") == std::wstring::npos);
        CHECK(lightText.find(L"background = 243, 243, 243, 242") !=
              std::wstring::npos);
        const int dark = cmo::ThemeIndexFromName(L"Windows 11 Dark");
        const std::wstring darkText = cmo::GenerateThemeText(dark);
        CHECK(darkText.find(L"[appearance.light]") == std::wstring::npos);
        CHECK(darkText.find(L"background = 32, 32, 32, 242") !=
              std::wstring::npos);
    }

    // v2.6 theme names map to preset indices.
    {
        CHECK(cmo::ThemeIndexFromName(L"Custom (menu.ini)") == 0);
        CHECK(cmo::ThemeIndexFromName(L"dracula") == 6);
        CHECK(cmo::ThemeIndexFromName(L"NORD") == 5);
        CHECK(cmo::ThemeIndexFromName(L"nonsense") == 0);
    }

    // v2.7 capture matching and submenu classification.
    {
        cmo::PendingQueue queue;
        cmo::PendingCapture first{};
        first.menu = reinterpret_cast<HMENU>(1);
        first.idCmdFirst = 21;
        queue.Push(first);
        cmo::PendingCapture second{};
        second.menu = reinterpret_cast<HMENU>(2);
        second.idCmdFirst = 22;
        queue.Push(second);
        cmo::PendingCapture taken{};
        CHECK(queue.TakeForMenu(reinterpret_cast<HMENU>(1), taken));
        CHECK(taken.idCmdFirst == 21);
        CHECK(!queue.HasPending());

        cmo::MenuItem submenu{};
        submenu.kind = cmo::ItemKind::Submenu;
        submenu.action = cmo::ActionKind::Submenu;
        submenu.label = L"New";
        submenu.flags = cmo::kModelExtension;
        CHECK(!cmo::IsThirdPartyItem(submenu));
        cmo::MenuItem command{};
        command.kind = cmo::ItemKind::Command;
        command.action = cmo::ActionKind::ShellVerb;
        command.canonicalVerb = L"WinRAR.ExtractHere";
        command.flags = cmo::kModelExtension;
        CHECK(cmo::IsThirdPartyItem(command));
    }

    {
        // Shell-only open items join the open group instead of the tail.
        cmo::MenuModel core = cmo::BuildCoreModel(
            cmo::Scope::Folders, {L"C:\\dir"}, cmo::Shape::Single);
        cmo::MenuModel cached{};
        cmo::MenuItem tab{};
        tab.kind = cmo::ItemKind::Command;
        tab.action = cmo::ActionKind::ShellVerb;
        tab.label = L"Open in new tab";
        tab.canonicalVerb = L"opennewtab";
        cached.items.push_back(tab);
        cmo::MenuItem props{};
        props.kind = cmo::ItemKind::Command;
        props.action = cmo::ActionKind::ShellVerb;
        props.label = L"Properties";
        props.canonicalVerb = L"properties";
        cached.items.push_back(props);
        const cmo::MenuModel merged = cmo::MergeCoreWithCached(core, cached);
        int tabIndex = -1;
        int propsIndex = -1;
        int openNewIndex = -1;
        for (size_t i = 0; i < merged.items.size(); ++i) {
            if (merged.items[i].canonicalVerb == L"opennewtab") {
                tabIndex = static_cast<int>(i);
            }
            if (merged.items[i].canonicalVerb == L"properties") {
                propsIndex = static_cast<int>(i);
            }
            if (merged.items[i].canonicalVerb == L"opennew") {
                openNewIndex = static_cast<int>(i);
            }
        }
        CHECK(tabIndex >= 0);
        CHECK(openNewIndex >= 0);
        CHECK(propsIndex >= 0);
        CHECK(tabIndex > openNewIndex);
        CHECK(tabIndex < propsIndex);
    }

    {
        // The built-in "Open in new process" entry is appended once.
        std::vector<cmo::MenuItem> items;
        cmo::AppendOpenNewProcessEntry(items);
        CHECK(items.size() >= 2);
        CHECK(items.back().builtinAction == cmo::BuiltinAction::OpenNewProcess);
        CHECK(items[items.size() - 2].kind == cmo::ItemKind::Separator);
        const size_t before = items.size();
        cmo::AppendOpenNewProcessEntry(items);
        CHECK_EQ(items.size(), before);
    }

    {
        std::vector<cmo::MenuItem> items;
        cmo::AppendSettingsEntry(items);
        CHECK(items.size() >= 2);
        CHECK(items.back().action == cmo::ActionKind::Builtin);
        CHECK(items.back().builtinAction == cmo::BuiltinAction::OpenSettings);
        CHECK_EQ(items.back().label, std::wstring(L"Menu settings\u2026"));
        CHECK(items[items.size() - 2].kind == cmo::ItemKind::Separator);
        // Idempotent: calling again does not duplicate the row.
        const size_t before = items.size();
        cmo::AppendSettingsEntry(items);
        CHECK_EQ(items.size(), before);
    }

    {
        cmo::LayoutItem item{};
        item.control.kind = cmo::ControlKind::IntSlider;
        item.rect = {0, 0, 300, 28};
        item.trackRect = {100, 12, 200, 16};
        item.thumbRect = {145, 8, 157, 20};
        item.fieldRect = {210, 4, 290, 24};
        CHECK(cmo::HitTestControlPart(item, POINT{150, 14}) ==
              cmo::ControlPart::Thumb);
        CHECK(cmo::HitTestControlPart(item, POINT{120, 14}) ==
              cmo::ControlPart::Track);
        CHECK(cmo::HitTestControlPart(item, POINT{250, 14}) ==
              cmo::ControlPart::Field);
        CHECK(cmo::HitTestControlPart(item, POINT{50, 14}) ==
              cmo::ControlPart::Row);
        CHECK(cmo::HitTestControlPart(item, POINT{50, 90}) ==
              cmo::ControlPart::None);

        std::wstring buf = L"1";
        size_t caret = 1;
        CHECK(cmo::ApplyFieldKey(buf, caret, 0, L'2', true, false));
        CHECK_EQ(buf, std::wstring(L"12"));
        CHECK_EQ(caret, size_t(2));
        CHECK(cmo::ApplyFieldKey(buf, caret, VK_BACK, 0, true, false));
        CHECK_EQ(buf, std::wstring(L"1"));
        CHECK(cmo::ApplyFieldKey(buf, caret, 0, L'-', true, false));
        CHECK_EQ(buf, std::wstring(L"-1"));
        CHECK_EQ(caret, size_t(2));  // the insert shifts the caret
        CHECK(!cmo::ApplyFieldKey(buf, caret, 0, L'-', true, false));
        buf = L"";
        caret = 0;
        CHECK(!cmo::ApplyFieldKey(buf, caret, 0, L'x', true, false));
        CHECK(cmo::ApplyFieldKey(buf, caret, 0, L'a', false, true));
        CHECK(cmo::ApplyFieldKey(buf, caret, 0, L'F', false, true));
        CHECK_EQ(buf, std::wstring(L"aF"));
        CHECK(!cmo::ApplyFieldKey(buf, caret, 0, L'g', false, true));
        caret = 1;
        CHECK(cmo::ApplyFieldKey(buf, caret, VK_LEFT, 0, false, true));
        CHECK_EQ(caret, size_t(0));
        CHECK(cmo::ApplyFieldKey(buf, caret, VK_DELETE, 0, false, true));
        CHECK_EQ(buf, std::wstring(L"F"));

        cmo::ControlSpec slider{};
        slider.kind = cmo::ControlKind::IntSlider;
        slider.minValue = 0;
        slider.maxValue = 100;
        std::wstring canonical;
        CHECK(cmo::CommitFieldBuffer(slider, L"  42 ", canonical));
        CHECK_EQ(canonical, std::wstring(L"42"));
        CHECK(cmo::CommitFieldBuffer(slider, L"999", canonical));
        CHECK_EQ(canonical, std::wstring(L"100"));
        CHECK(!cmo::CommitFieldBuffer(slider, L"12x", canonical));
        cmo::ControlSpec color{};
        color.kind = cmo::ControlKind::TextField;
        CHECK(cmo::CommitFieldBuffer(color, L"#11223344", canonical));
        CHECK_EQ(canonical, std::wstring(L"34, 51, 68, 17"));
    }

    {
        cmo::LayoutMetrics m;
        std::vector<cmo::MenuItem> items(1);
        items[0].label = L"Item height";
        items[0].control.kind = cmo::ControlKind::IntSlider;
        items[0].control.key = L"itemHeight";
        cmo::LayoutPanel p = cmo::BuildLayoutPanel(items, m, nullptr);
        const cmo::LayoutItem& it = p.items[0];
        CHECK(it.control.kind == cmo::ControlKind::IntSlider);
        CHECK(it.trackRect.left >= it.textRect.left);
        CHECK(it.fieldRect.left > it.trackRect.left);
        CHECK(it.fieldRect.right <= p.size.cx);
        CHECK_EQ(static_cast<int>(it.rect.bottom - it.rect.top), m.itemHeight);

        std::vector<cmo::MenuItem> area(1);
        area[0].control.kind = cmo::ControlKind::ColorArea;
        area[0].controlHeight = 120;
        cmo::LayoutPanel p2 = cmo::BuildLayoutPanel(area, m, nullptr);
        CHECK_EQ(
            static_cast<int>(p2.items[0].rect.bottom - p2.items[0].rect.top),
            120);
        CHECK(p2.size.cy > 120);
        CHECK(p2.items[0].areaRect.right <= p2.size.cx);

        // Existing non-control rows keep their geometry.
        std::vector<cmo::MenuItem> plain(1);
        plain[0].label = L"Plain";
        cmo::LayoutPanel p3 = cmo::BuildLayoutPanel(plain, m, nullptr);
        CHECK_EQ(
            static_cast<int>(p3.items[0].rect.bottom - p3.items[0].rect.top),
            m.itemHeight);
        CHECK_EQ(static_cast<int>(p3.items[0].controlRect.right -
                                  p3.items[0].controlRect.left),
                 0);
    }

    {
        cmo::SettingsWriteState st;
        cmo::SettingsMarkDirty(st, 1000, 400);
        CHECK(st.pending);
        CHECK(!cmo::SettingsWriteDue(st, 1399));
        CHECK(cmo::SettingsWriteDue(st, 1400));
        cmo::SettingsMarkDirty(st, 1500, 400);  // debounce extends
        CHECK(!cmo::SettingsWriteDue(st, 1800));
        CHECK(cmo::SettingsWriteDue(st, 1900));
        cmo::SettingsWriteFinished(st, true, 1900, 1000);
        CHECK(!st.pending);
        CHECK(!st.failed);
        cmo::SettingsMarkDirty(st, 2000, 400);
        cmo::SettingsWriteFinished(st, false, 2400, 1000);
        CHECK(st.pending);
        CHECK(st.failed);
        CHECK(!cmo::SettingsWriteDue(st, 3399));
        CHECK(cmo::SettingsWriteDue(st, 3400));
        cmo::SettingsRequestFlush(st);
        CHECK(cmo::SettingsWriteDue(st, 3400));

        cmo::SettingsTarget target;
        target.kind = cmo::SettingsTargetKind::MenuIniBase;
        target.section = L"appearance";
        cmo::Appearance working;
        working.itemHeight = 33;
        std::vector<cmo::ConfigOverride> changes =
            cmo::BuildChangeOverrides(working, {L"itemHeight"}, target);
        CHECK_EQ(changes.size(), size_t(1));
        CHECK_EQ(changes[0].value, std::wstring(L"33"));
        CHECK(!changes[0].remove);
        working.itemPadding = -1;
        changes = cmo::BuildChangeOverrides(working, {L"itemPadding"}, target);
        CHECK(changes[0].remove);
        target.section = L"appearance.dark";
        changes = cmo::BuildChangeOverrides(working, {L"itemHeight"}, target);
        CHECK_EQ(changes[0].section, std::wstring(L"appearance.dark"));

        const std::wstring file =
            L"[appearance]\nbackground = 9, 9, 9, 255\nitemHeight = 40\n"
            L"[rules]\nhide = label:\"X\"\n";
        const std::wstring out = cmo::BuildMenuIniTextWithChanges(
            file, {{L"appearance", L"itemHeight", L"33", false}});
        CHECK(out.find(L"itemHeight = 33") != std::wstring::npos);
        CHECK(out.find(L"background = 9, 9, 9, 255") != std::wstring::npos);
        CHECK(out.find(L"hide = label:\"X\"") != std::wstring::npos);

        // Theme write: preset fallback survives, only [appearance] is kept.
        // Real preset snippets start with an [appearance] header.
        const std::wstring theme = cmo::BuildThemeTextWithChanges(
            L"[appearance]\nitemHeight = 30\nbackground = 1, 2, 3, 255\n",
            L"background = 10, 20, 30, 255\n",
            {{L"appearance", L"background", L"40, 50, 60, 255", false}});
        CHECK(theme.find(L"[appearance]") != std::wstring::npos);
        CHECK(theme.find(L"[appearance.light]") == std::wstring::npos);
        CHECK(theme.find(L"[rules]") == std::wstring::npos);
        CHECK(theme.find(L"itemHeight = 30") != std::wstring::npos);
        CHECK(theme.find(L"background = 40, 50, 60, 255") !=
              std::wstring::npos);
    }

    {
        const cmo::ControlSpec slider =
            cmo::MakeControlSpec(*cmo::SchemaFind(L"blurStrength"));
        CHECK(slider.kind == cmo::ControlKind::IntSlider);
        CHECK_EQ(slider.minValue, 0);
        CHECK_EQ(slider.maxValue, 64);
        CHECK(cmo::MakeControlSpec(*cmo::SchemaFind(L"blur")).kind ==
              cmo::ControlKind::Toggle);
        CHECK(cmo::MakeControlSpec(*cmo::SchemaFind(L"background")).kind ==
              cmo::ControlKind::ColorSwatch);
        const cmo::ControlSpec easing =
            cmo::MakeControlSpec(*cmo::SchemaFind(L"animationEasing"));
        CHECK(easing.kind == cmo::ControlKind::Enum);
        CHECK_EQ(easing.options.size(), size_t(6));
        CHECK(cmo::MakeControlSpec(*cmo::SchemaFind(L"markerColor")).unsetCapable);

        auto findChild = [](const std::vector<cmo::MenuItem>& items,
                            const std::wstring& label) -> const cmo::MenuItem* {
            for (const cmo::MenuItem& item : items) {
                if (item.label == label) return &item;
            }
            return nullptr;
        };

        cmo::SettingsModelInputs in;
        // All conditional pages present so every schema key is reachable once.
        in.selectedEffects = cmo::kAnimFade | cmo::kAnimSlide | cmo::kAnimScale;
        std::vector<cmo::MenuItem> root = cmo::BuildSettingsTree(in);
        // Root order: header, status, target, seven groups, windhawk, actions.
        CHECK(root.size() >= 12);
        CHECK_EQ(root[0].label, std::wstring(L"Menu settings"));
        CHECK(root[2].kind == cmo::ItemKind::Submenu);  // target
        // Group order follows the schema's first-occurrence order.
        CHECK_EQ(root[3].label, std::wstring(L"Colors"));
        CHECK_EQ(root[4].label, std::wstring(L"Layout"));
        CHECK_EQ(root[5].label, std::wstring(L"Selection marker"));
        CHECK_EQ(root[6].label, std::wstring(L"Text"));
        CHECK_EQ(root[7].label, std::wstring(L"Shadow"));
        CHECK_EQ(root[8].label, std::wstring(L"Effects"));
        CHECK_EQ(root[9].label, std::wstring(L"Animation"));
        CHECK_EQ(root[10].label, std::wstring(L"Windhawk settings\u2026"));

        // Every schema key appears exactly once in the tree.
        std::map<std::wstring, int> seen;
        std::function<void(const std::vector<cmo::MenuItem>&)> walk =
            [&](const std::vector<cmo::MenuItem>& items) {
                for (const cmo::MenuItem& item : items) {
                    if (!item.control.key.empty() &&
                        item.control.key[0] != L'@') {
                        ++seen[item.control.key];
                    }
                    walk(item.children);
                }
            };
        walk(root);
        for (const cmo::ConfigSchemaEntry& row : cmo::kAppearanceSchema) {
            if (wcscmp(row.key, L"animation") == 0) {
                CHECK_EQ(seen[row.key], 0);  // deprecated, never shown
            } else {
                CHECK_EQ(seen[row.key], 1);
            }
        }

        const cmo::MenuItem* colorsGroup = findChild(root, L"Colors");
        CHECK(colorsGroup != nullptr);
        CHECK(colorsGroup->children.back().control.key.rfind(L"@reset:", 0) ==
              0);

        // Conditional pages follow the selected effects.
        in.selectedEffects = cmo::kAnimFade;
        root = cmo::BuildSettingsTree(in);
        const cmo::MenuItem* animation = findChild(root, L"Animation");
        CHECK(animation != nullptr);
        CHECK(findChild(animation->children, L"Slide options\u2026") == nullptr);
        CHECK(findChild(animation->children, L"Scale options\u2026") == nullptr);
        in.selectedEffects = cmo::kAnimSlide;
        root = cmo::BuildSettingsTree(in);
        animation = findChild(root, L"Animation");
        CHECK(animation != nullptr);
        CHECK(findChild(animation->children, L"Slide options\u2026") != nullptr);
        CHECK(findChild(animation->children, L"Replay open animation") !=
              nullptr);
        in.selectedEffects = cmo::kAnimScale | cmo::kAnimCrt;
        root = cmo::BuildSettingsTree(in);
        animation = findChild(root, L"Animation");
        CHECK(findChild(animation->children, L"Scale options\u2026") != nullptr);
    }

    {
        CHECK_EQ(cmo::ToggleAnimationEffect(cmo::kAnimFade | cmo::kAnimSlide,
                                            cmo::kAnimSlide),
                 cmo::kAnimFade);
        CHECK_EQ(cmo::ToggleAnimationEffect(cmo::kAnimFade | cmo::kAnimSlide,
                                            cmo::kAnimCrt),
                 (cmo::kAnimFade | cmo::kAnimSlide | cmo::kAnimCrt));
        // "none" is effect mask 0 (there is no kAnimNone constant).
        CHECK_EQ(cmo::ToggleAnimationEffect(0, 0u), 0u);
        CHECK_EQ(cmo::ToggleAnimationEffect(cmo::kAnimFade | cmo::kAnimSlide,
                                            0u),
                 0u);
        CHECK_EQ(cmo::ToggleAnimationEffect(0u, cmo::kAnimFade),
                 cmo::kAnimFade);
        CHECK_EQ(cmo::ToggleAnimationEffect(cmo::kAnimFade, cmo::kAnimFade),
                 0u);
    }

    {
        cmo::HotkeySpec hk{};
        CHECK(cmo::ParseHotkey(L"Ctrl+Alt+M", hk));
        CHECK_EQ(hk.modifiers, static_cast<UINT>(MOD_CONTROL | MOD_ALT));
        CHECK_EQ(hk.virtualKey, static_cast<UINT>('M'));
        CHECK(cmo::ParseHotkey(L" ctrl + shift + f12 ", hk));
        CHECK_EQ(hk.modifiers, static_cast<UINT>(MOD_CONTROL | MOD_SHIFT));
        CHECK_EQ(hk.virtualKey, static_cast<UINT>(VK_F12));
        CHECK(cmo::ParseHotkey(L"Win+1", hk));
        CHECK_EQ(hk.modifiers, static_cast<UINT>(MOD_WIN));
        CHECK_EQ(hk.virtualKey, static_cast<UINT>('1'));
        CHECK(!cmo::ParseHotkey(L"", hk));
        CHECK(!cmo::ParseHotkey(L"Ctrl", hk));
        CHECK(!cmo::ParseHotkey(L"F12", hk));
        CHECK(!cmo::ParseHotkey(L"Ctrl+Banana", hk));
        CHECK(!cmo::ParseHotkey(L"Ctrl+Alt", hk));
    }

    {
        cmo::HsvColor h = cmo::RgbToHsv(0xFFFF0000u);
        CHECK(std::fabs(h.h - 0.0f) < 0.5f);
        CHECK(std::fabs(h.s - 1.0f) < 0.001f);
        CHECK(std::fabs(h.v - 1.0f) < 0.001f);
        CHECK(std::fabs(cmo::RgbToHsv(0xFF00FF00u).h - 120.0f) < 0.5f);
        CHECK(std::fabs(cmo::RgbToHsv(0xFF0000FFu).h - 240.0f) < 0.5f);
        CHECK_EQ(cmo::HsvToRgb(cmo::HsvColor{0, 0, 1}, 255), 0xFFFFFFFFu);
        CHECK_EQ(cmo::HsvToRgb(cmo::HsvColor{0, 0, 0}, 255), 0xFF000000u);
        CHECK_EQ(cmo::HsvToRgb(cmo::HsvColor{0, 1, 1}, 255), 0xFFFF0000u);
        CHECK_EQ(cmo::HsvToRgb(cmo::HsvColor{120, 1, 1}, 255), 0xFF00FF00u);
        CHECK_EQ(cmo::HsvToRgb(cmo::HsvColor{240, 1, 1}, 255), 0xFF0000FFu);
        CHECK_EQ(cmo::HsvToRgb(cmo::HsvColor{0, 1, 1}, 0) >> 24, 0u);
        for (uint32_t c : {0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu, 0xFF808080u,
                           0xFF123456u}) {
            CHECK_EQ(cmo::HsvToRgb(cmo::RgbToHsv(c), 0xFF), c);
        }

        const std::vector<uint8_t> px = cmo::BuildSvSquarePixels(0.0f, 2, 2);
        CHECK_EQ(px.size(), size_t(16));
        auto pixel = [&](int x, int y) {
            const size_t o = (static_cast<size_t>(y) * 2 + x) * 4;
            return (static_cast<uint32_t>(px[o + 3]) << 24) |
                   (static_cast<uint32_t>(px[o + 2]) << 16) |
                   (static_cast<uint32_t>(px[o + 1]) << 8) | px[o];
        };
        CHECK_EQ(pixel(0, 0), 0xFFFFFFFFu);  // s=0, v=1
        CHECK_EQ(pixel(1, 0), 0xFFFF0000u);  // s=1, v=1 at hue 0
        CHECK_EQ(pixel(0, 1), 0xFF000000u);  // v=0
        CHECK_EQ(pixel(1, 1), 0xFF000000u);
    }

    {
        int v = 0;
        CHECK(cmo::ParseIntField(L"42", 0, 100, v));
        CHECK_EQ(v, 42);
        CHECK(cmo::ParseIntField(L"  -7 ", -32, 32, v));
        CHECK_EQ(v, -7);
        CHECK(cmo::ParseIntField(L"999", 0, 100, v));
        CHECK_EQ(v, 100);
        CHECK(cmo::ParseIntField(L"-999", 0, 100, v));
        CHECK_EQ(v, 0);
        CHECK(!cmo::ParseIntField(L"", 0, 100, v));
        CHECK(!cmo::ParseIntField(L"12x", 0, 100, v));
        CHECK(!cmo::ParseIntField(L"-", 0, 100, v));

        uint32_t argb = 0;
        CHECK(cmo::ParseColorField(L"#11223344", argb));
        CHECK_EQ(argb, 0x11223344u);
        CHECK(cmo::ParseColorField(L"#123456", argb));
        CHECK_EQ(argb, 0xFF123456u);
        CHECK(!cmo::ParseColorField(L"#12345", argb));
        CHECK(cmo::ParseColorField(L"10, 20, 30, 40", argb));
        CHECK_EQ(argb, 0x280A141Eu);

        CHECK_EQ(cmo::SliderValueFromX(0, 0, 100, 0, 100, 1), 0);
        CHECK_EQ(cmo::SliderValueFromX(100, 0, 100, 0, 100, 1), 100);
        CHECK_EQ(cmo::SliderValueFromX(50, 0, 100, 0, 100, 1), 50);
        CHECK_EQ(cmo::SliderValueFromX(53, 0, 100, 0, 100, 10), 50);
        CHECK_EQ(cmo::SliderValueFromX(57, 0, 100, 0, 100, 10), 60);
        CHECK_EQ(cmo::SliderValueFromX(-10, 0, 100, 0, 100, 1), 0);
        CHECK_EQ(cmo::SliderValueFromX(0, 0, 0, 0, 100, 1), 0);
        CHECK_EQ(cmo::SliderValueFromX(5, 0, 100, 7, 7, 1), 7);
        CHECK_EQ(cmo::SliderXFromValue(0, 10, 100, 0, 100), 10);
        CHECK_EQ(cmo::SliderXFromValue(100, 10, 100, 0, 100), 110);
        CHECK_EQ(cmo::SliderXFromValue(50, 10, 100, 0, 100), 60);
    }

    {
        using cmo::SettingsTargetKind;
        CHECK(cmo::DefaultSettingsTargetKind(false, false, false) ==
              SettingsTargetKind::MenuIniBase);
        CHECK(cmo::DefaultSettingsTargetKind(false, false, true) ==
              SettingsTargetKind::MenuIniBase);
        CHECK(cmo::DefaultSettingsTargetKind(true, true, true) ==
              SettingsTargetKind::MenuIniDark);
        CHECK(cmo::DefaultSettingsTargetKind(true, true, false) ==
              SettingsTargetKind::MenuIniLight);
        CHECK(cmo::DefaultSettingsTargetKind(true, false, true) ==
              SettingsTargetKind::MenuIniBase);

        cmo::SettingsTarget t =
            cmo::ResolveSettingsTarget(0, SettingsTargetKind::MenuIniDark);
        CHECK(t.kind == SettingsTargetKind::MenuIniDark);
        CHECK_EQ(t.section, std::wstring(L"appearance.dark"));
        t = cmo::ResolveSettingsTarget(0, SettingsTargetKind::MenuIniBase);
        CHECK_EQ(t.section, std::wstring(L"appearance"));
        t = cmo::ResolveSettingsTarget(4, SettingsTargetKind::MenuIniDark);
        CHECK(t.kind == SettingsTargetKind::ThemeFile);
        CHECK_EQ(t.themeIndex, 4);
        CHECK_EQ(t.section, std::wstring(L"appearance"));

        const cmo::ConfigSchemaEntry& itemHeight =
            *cmo::SchemaFind(L"itemHeight");
        cmo::ConfigOverride r = cmo::ResetOverrideForTarget(t, itemHeight);
        CHECK(r.remove);
        r = cmo::ResetOverrideForTarget(
            cmo::ResolveSettingsTarget(0, SettingsTargetKind::MenuIniBase),
            itemHeight);
        CHECK(!r.remove);
        CHECK_EQ(r.value, std::wstring(L"28"));
        r = cmo::ResetOverrideForTarget(
            cmo::ResolveSettingsTarget(0, SettingsTargetKind::MenuIniDark),
            itemHeight);
        CHECK(r.remove);
        r = cmo::ResetOverrideForTarget(
            cmo::ResolveSettingsTarget(0, SettingsTargetKind::MenuIniBase),
            *cmo::SchemaFind(L"itemPadding"));
        CHECK(r.remove);  // unset-capable base key restores the derived default
    }

    {
        wchar_t tempDir[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, tempDir);
        const std::wstring path =
            std::wstring(tempDir) + L"cmo-atomic-write-test.ini";
        DeleteFileW(path.c_str());
        CHECK(cmo::WriteConfigFile(path, L"alpha\n"));
        std::wstring text;
        CHECK(cmo::ReadConfigFile(path, text));
        CHECK_EQ(text, std::wstring(L"alpha\n"));
        CHECK(cmo::WriteConfigFile(path, L"beta\n"));
        text.clear();
        CHECK(cmo::ReadConfigFile(path, text));
        CHECK_EQ(text, std::wstring(L"beta\n"));
        // A write into a missing directory fails and leaves no temp behind.
        const std::wstring bad =
            std::wstring(tempDir) + L"cmo-no-such-dir\\x.ini";
        CHECK(!cmo::WriteConfigFile(bad, L"x"));
        CHECK(!cmo::WriteConfigFile(bad + L".tmp", L"x"));
        DeleteFileW(path.c_str());
    }

    {
        const std::wstring base =
            L"[appearance]\nbackground = 0, 0, 0, 255\nitemHeight = 40\n";
        std::wstring out = cmo::CanonicalizeConfigWithOverrides(
            base, cmo::kConfigSchemaVersion,
            {{L"appearance", L"itemHeight", L"33", false}});
        CHECK(out.find(L"itemHeight = 33") != std::wstring::npos);

        const std::wstring withLight =
            L"[appearance]\nitemHeight = 28\n"
            L"[appearance.light]\nitemHeight = 30\ntextColor = 1, 2, 3, 255\n";
        out = cmo::CanonicalizeConfigWithOverrides(
            withLight, cmo::kConfigSchemaVersion,
            {{L"appearance.light", L"itemHeight", L"", true}});
        const size_t lightStart = out.find(L"\n[appearance.light]");
        CHECK(lightStart != std::wstring::npos);
        const size_t lightEnd = out.find(L"\n[", lightStart + 1);
        const std::wstring light =
            out.substr(lightStart + 1,
                       lightEnd == std::wstring::npos
                           ? std::wstring::npos
                           : lightEnd - lightStart - 1);
        CHECK(light.find(L"itemHeight") == std::wstring::npos);
        CHECK(light.find(L"textColor") != std::wstring::npos);

        // Removing an unset-capable key emits the commented (unset) form;
        // setting it emits an active value.
        out = cmo::CanonicalizeConfigWithOverrides(
            L"", cmo::kConfigSchemaVersion,
            {{L"appearance", L"itemPadding", L"", true}});
        CHECK(out.find(L"; itemPadding = 6") != std::wstring::npos);
        out = cmo::CanonicalizeConfigWithOverrides(
            L"", cmo::kConfigSchemaVersion,
            {{L"appearance", L"itemPadding", L"6", false}});
        CHECK(out.find(L"\nitemPadding = 6") != std::wstring::npos);

        // Structured sections survive.
        out = cmo::CanonicalizeConfigWithOverrides(
            base + L"[rules]\nhide = label:\"X\"\n", cmo::kConfigSchemaVersion,
            {});
        CHECK(out.find(L"[rules]") != std::wstring::npos);
        CHECK(out.find(L"hide = label:\"X\"") != std::wstring::npos);
    }

    {
        cmo::Appearance a;
        std::wstring out;
        CHECK(cmo::AppearanceValueText(a, *cmo::SchemaFind(L"background"), out));
        CHECK_EQ(out, std::wstring(L"30, 30, 30, 240"));
        a.background = 0x11223344;
        CHECK(cmo::AppearanceValueText(a, *cmo::SchemaFind(L"background"), out));
        CHECK_EQ(out, std::wstring(L"34, 51, 68, 17"));

        a.fontFace = L"Consolas";
        a.fontSize = 11.0f;
        CHECK(cmo::AppearanceValueText(a, *cmo::SchemaFind(L"font"), out));
        CHECK_EQ(out, std::wstring(L"Consolas, 11"));

        a.animationOpen = cmo::kAnimFade | cmo::kAnimSlide;
        CHECK(cmo::AppearanceValueText(a, *cmo::SchemaFind(L"animationOpen"),
                                       out));
        CHECK_EQ(out, std::wstring(L"fade, slide"));

        a.fontWeight = cmo::FontWeightKind::Bold;
        CHECK(cmo::AppearanceValueText(a, *cmo::SchemaFind(L"fontWeight"), out));
        CHECK_EQ(out, std::wstring(L"bold"));
        a.animationAnchorAtCursor = false;
        CHECK(cmo::AppearanceValueText(a, *cmo::SchemaFind(L"animationAnchor"),
                                       out));
        CHECK_EQ(out, std::wstring(L"center"));
        a.marker = cmo::MarkerStyle::Bar;
        CHECK(cmo::AppearanceValueText(a, *cmo::SchemaFind(L"marker"), out));
        CHECK_EQ(out, std::wstring(L"bar"));
        a.acceleratorMode = cmo::AcceleratorMode::Strip;
        CHECK(cmo::AppearanceValueText(a, *cmo::SchemaFind(L"showAccelerators"),
                                       out));
        CHECK_EQ(out, std::wstring(L"strip"));
        a.animationEasing = cmo::AnimEasing::Bounce;
        CHECK(cmo::AppearanceValueText(a, *cmo::SchemaFind(L"animationEasing"),
                                       out));
        CHECK_EQ(out, std::wstring(L"bounce"));

        a.itemPadding = -1;
        CHECK(!cmo::AppearanceValueText(a, *cmo::SchemaFind(L"itemPadding"),
                                        out));
        a.hasCornerRadii = false;
        CHECK(!cmo::AppearanceValueText(a, *cmo::SchemaFind(L"cornerRadii"),
                                        out));
        a.hasMarkerColor = false;
        CHECK(!cmo::AppearanceValueText(a, *cmo::SchemaFind(L"markerColor"),
                                        out));
        a.hasHeaderColor = false;
        CHECK(!cmo::AppearanceValueText(a, *cmo::SchemaFind(L"headerColor"),
                                        out));

        // Every settable key formats, re-applies, and formats identically.
        for (const cmo::ConfigSchemaEntry& row : cmo::kAppearanceSchema) {
            cmo::Appearance src;
            std::wstring first;
            if (!cmo::AppearanceValueText(src, row, first)) {
                continue;
            }
            cmo::Appearance round;
            CHECK(cmo::ApplyAppearanceValue(round, row.key, first, nullptr));
            std::wstring second;
            CHECK(cmo::AppearanceValueText(round, row, second));
            CHECK_EQ(first, second);
        }
    }

    if (g_failures == 0) {
        wprintf(L"ALL TESTS PASSED\n");
        return 0;
    }
    fprintf(stderr, "%d test(s) failed\n", g_failures);
    return 1;
}
