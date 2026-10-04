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
    cmo::PendingCapture* taken = queue.Take();
    CHECK(taken != nullptr);
    CHECK(taken && taken->idCmdFirst == 11);
    CHECK(queue.Take() == nullptr);

    queue.Push(capture);
    queue.ExpireOlderThan(1200, 500);
    CHECK(queue.Take() != nullptr);

    queue.Push(capture);
    queue.ExpireOlderThan(2000, 500);
    CHECK(queue.Take() == nullptr);

    queue.Push(capture);
    queue.Clear();
    CHECK(queue.Take() == nullptr);

    std::vector<std::wstring> onePath{L"a.txt"};
    cmo::MenuModel single = cmo::BuildCoreFileModel(onePath, cmo::Shape::Single);
    CHECK(!single.items.empty());
    CHECK_EQ(single.items.front().label, std::wstring(L"Open"));
    CHECK_EQ(single.items.back().label, std::wstring(L"Show more options"));
    CHECK_EQ(single.sig.typeKey, std::wstring(L".txt"));

    std::vector<std::wstring> threePaths{L"a.txt", L"b.txt", L"c.txt"};
    cmo::MenuModel multi = cmo::BuildCoreFileModel(threePaths, cmo::Shape::Multi);
    bool foundOpenItems = false;
    bool cutDisabled = false;
    for (const cmo::MenuItem& item : multi.items) {
        if (item.label == L"Open 3 items") {
            foundOpenItems = true;
        }
        if (item.label == L"Cut" && (item.flags & cmo::kModelDisabled)) {
            cutDisabled = true;
        }
    }
    CHECK(foundOpenItems);
    CHECK(cutDisabled);

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

    if (g_failures == 0) {
        wprintf(L"ALL TESTS PASSED\n");
        return 0;
    }
    fprintf(stderr, "%d test(s) failed\n", g_failures);
    return 1;
}
