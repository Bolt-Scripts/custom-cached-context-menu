// Compiles the mod exactly as Windhawk does: without CMO_TESTING, so the test
// hooks in tests/test_hooks.inc are excluded. This catches production code
// that accidentally depends on them.
#include "wh_api_stub.h"
#include "../custom-cached-context-menu.wh.cpp"

int main() {
    return 0;
}
