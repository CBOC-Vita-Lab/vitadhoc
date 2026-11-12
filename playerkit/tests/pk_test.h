/* A tiny test harness. Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE). */
#ifndef PK_TEST_H
#define PK_TEST_H

#include <stdio.h>
#include <string>
#include <vector>

namespace pktest {
struct Case { const char *name; void (*fn)(); };
std::vector<Case> &cases();
extern int g_failed_checks;
struct Reg { Reg(const char *n, void (*f)()) { Case c = { n, f }; cases().push_back(c); } };
}  // namespace pktest

#define TEST(name) static void test_##name(); static pktest::Reg reg_##name(#name, test_##name); static void test_##name()
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); pktest::g_failed_checks++; } } while (0)
#define CHECK_EQ(a, b) do { long long _a = (long long) (a), _b = (long long) (b); if (_a != _b) { fprintf(stderr, "    FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, _a, _b); pktest::g_failed_checks++; } } while (0)
#define CHECK_STR(a, b) do { std::string _a = (a), _b = (b); if (_a != _b) { fprintf(stderr, "    FAIL %s:%d: %s == %s (\"%s\" vs \"%s\")\n", __FILE__, __LINE__, #a, #b, _a.c_str(), _b.c_str()); pktest::g_failed_checks++; } } while (0)

#endif
