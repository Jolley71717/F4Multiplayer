#include "Test.h"

#include <cstring>
#include <exception>

int main(int a_argc, char** a_argv)
{
	const char* filter = a_argc > 1 ? a_argv[1] : nullptr;
	int         run = 0;
	int         failed = 0;
	int         known = 0;  // known bugs still failing
	for (const auto& test : Test::Cases()) {
		if (filter && !std::strstr(test.name, filter)) {
			continue;
		}
		++run;
		const int before = Test::Failures();
		Test::RunningKnownBug() = test.knownBug != nullptr;
		try {
			test.body();
		} catch (const Test::Abort&) {
		} catch (const std::exception& e) {
			Test::Fail(test.name, 0, std::string("exception: ") + e.what());
		}
		Test::RunningKnownBug() = false;
		const bool ok = Test::Failures() == before;
		if (test.knownBug) {
			// Still failing is what is expected; passing means the bug is gone and the mark should go too.
			Test::Failures() = before + ok;
			failed += ok;
			if (ok) {
				std::printf("FIXED? %s: passes now; make it a plain TEST (known bug: %s)\n", test.name, test.knownBug);
			} else {
				std::printf("known  %s (bug: %s)\n", test.name, test.knownBug);
				++known;
			}
			continue;
		}
		failed += !ok;
		std::printf("%s %s\n", ok ? "ok    " : "FAILED", test.name);
	}
	std::printf("\n%d tests, %d failed", run, failed);
	if (known > 0) {
		std::printf(", %d known bugs still failing", known);
	}
	std::printf("\n");
	return failed;
}
