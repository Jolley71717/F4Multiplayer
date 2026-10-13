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
	};

	inline std::vector<Case>& Cases()
	{
		static std::vector<Case> cases;
		return cases;
	}

	struct Register
	{
		Register(const char* a_name, std::function<void()> a_body) { Cases().push_back({ a_name, std::move(a_body) }); }
	};

	// Thrown by REQUIRE to stop the current test.
	struct Abort
	{};

	inline int& Failures()
	{
		static int failures = 0;
		return failures;
	}

	inline void Fail(const char* a_file, int a_line, const std::string& a_what)
	{
		++Failures();
		std::printf("  FAILED %s:%d: %s\n", a_file, a_line, a_what.c_str());
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
