// Keeping the last launches' logs (src/LogRotation.h), on real files in a temporary folder: a player
// who restarted the game after a problem still has the log that saw it.

#include "Test.h"

#include "LogRotation.h"

#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

namespace
{
	namespace fs = std::filesystem;

	// A folder of its own under %TEMP%, removed at the end of the test.
	struct Folder
	{
		fs::path path = fs::temp_directory_path() / std::format("f4mp-logrotation-{:08x}", std::random_device{}());

		Folder() { fs::create_directories(path); }
		~Folder()
		{
			std::error_code ec;
			fs::remove_all(path, ec);
		}
	};

	void Write(const fs::path& a_path, std::string_view a_text)
	{
		std::ofstream out{ a_path, std::ios::binary | std::ios::trunc };
		out << a_text;
	}

	std::string Read(const fs::path& a_path)
	{
		std::ifstream     in{ a_path, std::ios::binary };
		std::stringstream text;
		text << in.rdbuf();
		return text.str();
	}

	// One game launch: the log is rotated, then opened empty and written (as F4SE::Init does).
	std::error_code Launch(const fs::path& a_log, std::string_view a_text)
	{
		const auto ec = LogRotation::Rotate(a_log, 3);
		Write(a_log, a_text);
		return ec;
	}
}

TEST("log rotation: after each launch .1 is the launch before, .2 the one before that; only three are kept")
{
	Folder     folder;
	const auto log = folder.path / "F4Multiplayer.log";
	for (const auto text : { "first", "second", "third", "fourth", "fifth" }) {
		CHECK(!Launch(log, text));
	}
	CHECK(Read(log) == "fifth");
	CHECK(Read(LogRotation::Kept(log, 1)) == "fourth");
	CHECK(Read(LogRotation::Kept(log, 2)) == "third");
	CHECK(Read(LogRotation::Kept(log, 3)) == "second");
	CHECK(!fs::exists(LogRotation::Kept(log, 4)));
	CHECK(LogRotation::Kept(log, 2).filename() == "F4Multiplayer.2.log");
}

TEST("log rotation: a launch that wrote nothing does not push the useful logs out")
{
	Folder     folder;
	const auto log = folder.path / "F4Multiplayer.log";
	CHECK(!Launch(log, "the session with the bug"));
	CHECK(!Launch(log, ""));  // the game closed before the log got a line
	CHECK(!Launch(log, "now"));
	CHECK(Read(LogRotation::Kept(log, 1)) == "the session with the bug");
	CHECK(!fs::exists(LogRotation::Kept(log, 2)));

	// No log at all (the first launch ever) is no error either.
	Folder empty;
	CHECK(!LogRotation::Rotate(empty.path / "F4Multiplayer.log", 3));
}

TEST("log rotation: a log another program holds open is reported, not thrown, and the game still starts")
{
	Folder     folder;
	const auto log = folder.path / "F4Multiplayer.log";
	CHECK(!Launch(log, "older"));
	CHECK(!Launch(log, "held"));
	{
		// A viewer or editor holding the file open (std::ofstream shares it for reading and writing,
		// not for renaming).
		std::ofstream viewer{ log, std::ios::app };
		REQUIRE(viewer);
		std::error_code ec;
		bool            threw = false;
		try {
			ec = LogRotation::Rotate(log, 3);
		} catch (...) {
			threw = true;
		}
		CHECK(!threw);
#ifdef _WIN32
		CHECK(ec);
#else
		CHECK(!ec);  // Linux lets a file another program holds open be renamed: the rotation goes through
#endif
	}
#ifdef _WIN32
	// The held log is still there to be overwritten; the older one moved on and is kept.
	CHECK(Read(log) == "held");
#else
	CHECK(Read(LogRotation::Kept(log, 1)) == "held");
#endif
	CHECK(Read(LogRotation::Kept(log, 2)) == "older");
	// The next launch, with nothing holding it, is back to normal.
	CHECK(!Launch(log, "next"));
	CHECK(Read(LogRotation::Kept(log, 1)) == "held");
}
