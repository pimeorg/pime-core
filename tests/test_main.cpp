#include "test_framework.hpp"

namespace pime::test {

namespace {
int g_failures = 0;
}

std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

void check(bool condition, const char* expression, const char* file, int line) {
    if (condition) return;
    ++g_failures;
    std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expression);
}

int failures() { return g_failures; }

int run_all() {
    for (const Case& test : registry()) {
        const int before = g_failures;
        test.function();
        if (g_failures != before) std::fprintf(stderr, "  in %s\n", test.name);
    }
    if (g_failures == 0) {
        std::printf("all tests passed (%zu)\n", registry().size());
        return 0;
    }
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return 1;
}

}  // namespace pime::test

int main() { return pime::test::run_all(); }
