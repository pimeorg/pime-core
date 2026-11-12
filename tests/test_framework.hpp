#ifndef PIME_TESTS_FRAMEWORK_HPP
#define PIME_TESTS_FRAMEWORK_HPP

#include <cstdio>
#include <string>
#include <vector>

namespace pime::test {

struct Case {
    const char* name;
    void (*function)();
};

std::vector<Case>& registry();
void check(bool condition, const char* expression, const char* file, int line);
int failures();
int run_all();

}  // namespace pime::test

#define TEST(name)                                                         \
    static void name();                                                    \
    static const bool name##_registered = []() {                           \
        pime::test::registry().push_back({#name, &name});                  \
        return true;                                                       \
    }();                                                                   \
    static void name()

#define CHECK(expression)                                                  \
    pime::test::check(static_cast<bool>(expression), #expression,          \
                      __FILE__, __LINE__)

#define CHECK_EQ(a, b) CHECK((a) == (b))

#endif
