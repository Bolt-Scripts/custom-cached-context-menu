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

    if (g_failures == 0) {
        wprintf(L"ALL TESTS PASSED\n");
        return 0;
    }
    fprintf(stderr, "%d test(s) failed\n", g_failures);
    return 1;
}
