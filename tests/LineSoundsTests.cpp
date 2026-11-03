#include "Test.h"

#include "game/LineSounds.h"

#include <limits>
#include <utility>
#include <vector>

// The bookkeeping behind friends' lines' sounds (Conversations): every sound has a hard end, a speaker says one
// line at a time, and only a few play at once, so a sound that never ends by itself can't run on or pile up.
namespace
{
	using Sounds = LineSounds<int>;
	using Why = Sounds::Why;
	using namespace std::chrono_literals;

	const auto t0 = Sounds::Clock::time_point{} + 1h;

	struct Ended
	{
		std::vector<std::pair<int, Why>> list;
		auto operator()() { return [this](Sounds::Entry& a_entry, Why a_why) { list.emplace_back(a_entry.handle, a_why); }; }
	};
}

TEST("line sounds: a sound's hard end is its recording's length and a second, within bounds")
{
	CHECK(Sounds::CapFor(4.0) == 5s);
	CHECK(Sounds::CapFor(0.3) == Sounds::SHORTEST);  // a short bark still gets the floor
	CHECK(Sounds::CapFor(29.5) == Sounds::LONGEST);  // 30.5 s is more than any line is believed to be
	CHECK(Sounds::CapFor(5000.0) == Sounds::LONGEST);
	CHECK(Sounds::CapFor(std::nullopt) == Sounds::UNKNOWN);  // the file's length could not be read
	CHECK(Sounds::CapFor(0.0) == Sounds::UNKNOWN);
	CHECK(Sounds::CapFor(-1.0) == Sounds::UNKNOWN);
	CHECK(Sounds::CapFor(std::numeric_limits<double>::infinity()) == Sounds::UNKNOWN);
	CHECK(Sounds::CapFor(std::numeric_limits<double>::quiet_NaN()) == Sounds::UNKNOWN);
	CHECK(Sounds::UNKNOWN <= Sounds::LONGEST);
}

TEST("line sounds: a sound is ended at its hard end, not before, and only once")
{
	Sounds sounds;
	Ended  ended;
	sounds.Start(1, 10, t0, 4.0, ended());  // ends 5 s in
	sounds.Start(2, 20, t0, std::nullopt, ended());  // ends UNKNOWN in
	CHECK(ended.list.empty());
	CHECK(sounds.EndDue(t0 + 4900ms, ended()) == 0);
	CHECK(sounds.Kept() == 2);
	CHECK(sounds.EndDue(t0 + 5s, ended()) == 1);
	REQUIRE(ended.list.size() == 1);
	CHECK(ended.list[0] == std::pair(10, Why::kTooLong));
	CHECK(sounds.EndDue(t0 + 6s, ended()) == 0);  // not ended again
	CHECK(sounds.EndDue(t0 + Sounds::UNKNOWN, ended()) == 1);
	REQUIRE(ended.list.size() == 2);
	CHECK(ended.list[1] == std::pair(20, Why::kTooLong));
	CHECK(sounds.Kept() == 0);
}

TEST("line sounds: a speaker's new line ends the one before it, which is no cap")
{
	Sounds sounds;
	Ended  ended;
	sounds.Start(7, 1, t0, 10.0, ended());
	sounds.Start(7, 2, t0 + 1s, 10.0, ended());
	REQUIRE(ended.list.size() == 1);
	CHECK(ended.list[0] == std::pair(1, Why::kNext));
	CHECK(sounds.Kept() == 1);
	// The new line has its own hard end, from when it started.
	CHECK(sounds.EndDue(t0 + 11500ms, ended()) == 0);
	CHECK(sounds.EndDue(t0 + 12s, ended()) == 1);
	CHECK(ended.list.back() == std::pair(2, Why::kTooLong));
}

TEST("line sounds: past the most at once, the oldest is ended before the new one plays")
{
	Sounds sounds;
	Ended  ended;
	for (int i = 0; i < static_cast<int>(Sounds::MAX_PLAYING); ++i) {
		sounds.Start(100 + i, i, t0 + std::chrono::seconds(i), 20.0, ended());
	}
	CHECK(ended.list.empty());
	CHECK(sounds.Kept() == Sounds::MAX_PLAYING);
	sounds.Start(999, 99, t0 + 10s, 20.0, ended());
	REQUIRE(ended.list.size() == 1);
	CHECK(ended.list[0] == std::pair(0, Why::kTooMany));
	CHECK(sounds.Kept() == Sounds::MAX_PLAYING);
	sounds.Start(998, 98, t0 + 11s, 20.0, ended());
	REQUIRE(ended.list.size() == 2);
	CHECK(ended.list[1] == std::pair(1, Why::kTooMany));
}

TEST("line sounds: many speakers' sounds that never end don't pile up")
{
	// The reported bug: each new speaker's line added a sound that ran on forever. However many lines come, at
	// most MAX_PLAYING are kept, and none outlives LONGEST.
	Sounds sounds;
	Ended  ended;
	auto   now = t0;
	for (int i = 0; i < 200; ++i, now += 700ms) {
		sounds.Start(static_cast<Sounds::Key>(i), i, now, std::nullopt, ended());
		sounds.EndDue(now, ended());
		CHECK(sounds.Kept() <= Sounds::MAX_PLAYING);
	}
	sounds.EndDue(now + Sounds::LONGEST, ended());
	CHECK(sounds.Kept() == 0);
	CHECK(ended.list.size() == 200);
}

TEST("line sounds: the session's end ends every sound")
{
	Sounds           sounds;
	Ended            ended;
	std::vector<int> all;
	sounds.Start(1, 1, t0, 3.0, ended());
	sounds.Start(2, 2, t0, 3.0, ended());
	sounds.EndAll([&](Sounds::Entry& a_entry) { all.push_back(a_entry.handle); });
	CHECK((all == std::vector<int>{ 1, 2 }));
	CHECK(sounds.Kept() == 0);
	CHECK(sounds.EndDue(t0 + 1h, ended()) == 0);
	CHECK(ended.list.empty());
}
