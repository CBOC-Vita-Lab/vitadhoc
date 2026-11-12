/* Test runner. Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE). */
#include "pk_test.h"

namespace pktest {
std::vector<Case> &cases() { static std::vector<Case> c; return c; }
int g_failed_checks = 0;
}

int main()
{
	int failed = 0;
	for (size_t i = 0; i < pktest::cases().size(); i++) {
		int before = pktest::g_failed_checks;
		pktest::cases()[i].fn();
		bool ok = pktest::g_failed_checks == before;
		printf("[%s] %s\n", ok ? " ok " : "FAIL", pktest::cases()[i].name);
		if (!ok) failed++;
	}
	printf("\n%d tests, %d failed, %d failed checks\n", (int) pktest::cases().size(), failed, pktest::g_failed_checks);
	return failed ? 1 : 0;
}
