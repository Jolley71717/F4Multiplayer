#include "Test.h"

#include <cstring>
#include <exception>

int main(int a_argc, char** a_argv)
{
	const char* filter = a_argc > 1 ? a_argv[1] : nullptr;
	int         run = 0;
	int         failed = 0;
	for (const auto& test : Test::Cases()) {
		if (filter && !std::strstr(test.name, filter)) {
			continue;
		}
		++run;
		const int before = Test::Failures();
		try {
			test.body();
		} catch (const Test::Abort&) {
		} catch (const std::exception& e) {
			Test::Fail(test.name, 0, std::string("exception: ") + e.what());
		}
		const bool ok = Test::Failures() == before;
		failed += !ok;
		std::printf("%s %s\n", ok ? "ok    " : "FAILED", test.name);
	}
	std::printf("\n%d tests, %d failed\n", run, failed);
	return failed;
}
