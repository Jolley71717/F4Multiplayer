#pragma once

// A tiny test framework: TEST("name") { CHECK(...); REQUIRE(...); }. Run F4MPTests.exe, optionally
// with a word: only tests whose name contains it run. The exit code is the number of failed tests.

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace Test
{
	struct Case
	{
		const char*           name;
		std::function<void()> body;
		const char*           knownBug = nullptr;  // TEST_KNOWN_BUG: what is wrong, while the test still fails
	};

	inline std::vector<Case>& Cases()
	{
		static std::vector<Case> cases;
		return cases;
	}

	struct Register
	{
		Register(const char* a_name, std::function<void()> a_body, const char* a_knownBug = nullptr) { Cases().push_back({ a_name, std::move(a_body), a_knownBug }); }
	};

	// Thrown by REQUIRE to stop the current test.
	struct Abort
	{};

	inline int& Failures()
	{
		static int failures = 0;
		return failures;
	}

	// A known bug's test is running: its failures are the bug, not news.
	inline bool& RunningKnownBug()
	{
		static bool running = false;
		return running;
	}

	inline void Fail(const char* a_file, int a_line, const std::string& a_what)
	{
		++Failures();
		std::printf("  %s %s:%d: %s\n", RunningKnownBug() ? "still fails" : "FAILED", a_file, a_line, a_what.c_str());
	}
}

#define TEST_CONCAT2(a, b) a##b
#define TEST_CONCAT(a, b) TEST_CONCAT2(a, b)
#define TEST(name)                                                                                   \
	static void TEST_CONCAT(test_body_, __LINE__)();                                                 \
	static const Test::Register TEST_CONCAT(test_reg_, __LINE__){ name, TEST_CONCAT(test_body_, __LINE__) }; \
	static void TEST_CONCAT(test_body_, __LINE__)()

// A test of a bug not fixed yet: written for the right behaviour, so today it fails. Its failures are
// printed but not counted; once it passes, that counts as a failure until the mark is taken off.
#define TEST_KNOWN_BUG(name, bug)                                                                    \
	static void TEST_CONCAT(test_body_, __LINE__)();                                                 \
	static const Test::Register TEST_CONCAT(test_reg_, __LINE__){ name, TEST_CONCAT(test_body_, __LINE__), bug }; \
	static void TEST_CONCAT(test_body_, __LINE__)()

// Records a failure and carries on.
#define CHECK(expr)                                         \
	do {                                                    \
		if (!(expr)) {                                      \
			Test::Fail(__FILE__, __LINE__, #expr);          \
		}                                                   \
	} while (false)

// Records a failure and stops this test.
#define REQUIRE(expr)                                       \
	do {                                                    \
		if (!(expr)) {                                      \
			Test::Fail(__FILE__, __LINE__, #expr);          \
			throw Test::Abort{};                            \
		}                                                   \
	} while (false)
