#pragma once

// A tiny test framework: TEST("name") { CHECK(...); REQUIRE(...); }. Run F4MPTests.exe, optionally
// with a word: only tests whose name contains it run. The exit code is the number of failed tests.

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <functional>
#include <random>
#include <string>
#include <string_view>
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

	// A known bug's test is running.
	inline bool& RunningKnownBug()
	{
		static bool running = false;
		return running;
	}

	// Failures of a known bug's KNOWN_CHECKs: the bug itself, still there.
	inline int& KnownFailures()
	{
		static int failures = 0;
		return failures;
	}

	inline void Fail(const char* a_file, int a_line, const std::string& a_what)
	{
		++Failures();
		std::printf("  FAILED %s:%d: %s\n", a_file, a_line, a_what.c_str());
	}

	// A KNOWN_CHECK failed: in a known bug's test that is the bug, anywhere else a plain failure.
	inline void FailKnown(const char* a_file, int a_line, const std::string& a_what)
	{
		if (!RunningKnownBug()) {
			Fail(a_file, a_line, a_what);
			return;
		}
		++KnownFailures();
		std::printf("  still fails %s:%d: %s\n", a_file, a_line, a_what.c_str());
	}

	// A file name in the temp folder that no other test, and no other run of the tests going on at the
	// same time, uses.
	inline std::filesystem::path TempPath(std::string_view a_stem, std::string_view a_extension)
	{
		std::random_device random;
		const auto         unique = (static_cast<std::uint64_t>(random()) << 32) | random();
		return std::filesystem::temp_directory_path() / std::format("{}-{:016x}{}", a_stem, unique, a_extension);
	}
}

#define TEST_CONCAT2(a, b) a##b
#define TEST_CONCAT(a, b) TEST_CONCAT2(a, b)
#define TEST(name)                                                                                   \
	static void TEST_CONCAT(test_body_, __LINE__)();                                                 \
	static const Test::Register TEST_CONCAT(test_reg_, __LINE__){ name, TEST_CONCAT(test_body_, __LINE__) }; \
	static void TEST_CONCAT(test_body_, __LINE__)()

// A test of a bug not fixed yet: written for the right behaviour, so today it fails. The bug's symptom
// is checked with KNOWN_CHECK, whose failures are printed but not counted. Any other failure in it (a
// CHECK, a REQUIRE, an exception) counts as usual, and so does the symptom going away, until the mark
// is taken off.
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

// The symptom of a known bug (TEST_KNOWN_BUG): expected to fail until the bug is fixed.
#define KNOWN_CHECK(expr)                                   \
	do {                                                    \
		if (!(expr)) {                                      \
			Test::FailKnown(__FILE__, __LINE__, #expr);     \
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
