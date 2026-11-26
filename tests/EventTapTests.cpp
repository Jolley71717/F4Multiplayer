// The event tap's rules (EventTapRules.h): a hit event another plugin's sink stops before ours is still handled, once.
// Each test names what a player would see go wrong if it failed.

#include "Test.h"
#include "game/EventTapRules.h"

#include <array>
#include <vector>

namespace
{
	using namespace EventTapRules;

	constexpr std::uintptr_t NOTIFY = 0x140546930;

	std::array<std::uint8_t, 5> CallTo(std::uintptr_t a_at, std::uintptr_t a_target)
	{
		const auto rel = static_cast<std::uint32_t>(a_target - (a_at + 5));
		return { 0xE8, static_cast<std::uint8_t>(rel), static_cast<std::uint8_t>(rel >> 8), static_cast<std::uint8_t>(rel >> 16), static_cast<std::uint8_t>(rel >> 24) };
	}
}

TEST("event tap: the sites are patched only when both call the hit event's Notify")
{
	// A site that is not the expected call (another plugin's patch, another version) must leave the engine alone: all or none.
	const std::array<std::uintptr_t, 2> at{ 0x14052D01D, 0x14052D0FE };
	const std::array<std::array<std::uint8_t, 5>, 2> good{ CallTo(at[0], NOTIFY), CallTo(at[1], NOTIFY) };
	CHECK(Check(good, at, NOTIFY).All());

	auto other = good;
	other[1] = CallTo(at[1], 0x140999999);  // another plugin's hook sits at the second
	const auto some = Check(other, at, NOTIFY);
	CHECK(!some.All());
	CHECK((some.calling) == (1u));

	auto jump = good;
	jump[0][0] = 0xE9;  // a jmp, not a call
	CHECK(!Check(jump, at, NOTIFY).All());
}

TEST("event tap: an event our sink got is not handled again by the hook")
{
	// A hit seen by both the hook and our sink is handled once: the sink's. A hit our sink never got is the hook's.
	Track track;
	int  event = 0;
	REQUIRE(track.Begin(&event));
	CHECK(track.Sunk(&event));
	CHECK(track.End());  // sunk: the hook does nothing more

	REQUIRE(track.Begin(&event));
	CHECK(!track.End());  // no sink saw it (stopped ahead of ours): the hook handles it
	CHECK((track.Depth()) == (0u));
}

TEST("event tap: a hit raised inside a sink is followed on its own")
{
	// A sink that raises a hit while the first is still being sent (a nested notify on one thread): each is judged by its own address.
	Track track;
	int   outer = 0;
	int   inner = 0;
	REQUIRE(track.Begin(&outer));
	REQUIRE(track.Begin(&inner));
	CHECK(track.Sunk(&inner));
	CHECK(track.End());          // the inner reached us
	CHECK(!track.End());         // the outer did not
	CHECK(!track.Sunk(&inner));  // nothing in flight
}

TEST("event tap: an event no hook sent is told apart")
{
	// Our sink told of an event raised another way (or the hook not in) counts as direct and marks nothing.
	Track track;
	int   other = 0;
	int   sent = 0;
	CHECK(!track.Sunk(&other));
	REQUIRE(track.Begin(&sent));
	CHECK(!track.Sunk(&other));
	CHECK(!track.End());
}

TEST("event tap: nesting deeper than the book leaves the event to the sinks")
{
	Track                  track;
	std::array<int, Track::DEPTH + 1> events{};
	for (std::size_t i = 0; i < Track::DEPTH; ++i) {
		REQUIRE(track.Begin(&events[i]));
	}
	CHECK(!track.Begin(&events[Track::DEPTH]));  // not followed, so the hook neither counts nor handles it
	CHECK((track.Depth()) == (Track::DEPTH));
}

TEST("event tap: the counters tell what the hook saw and what our sink got")
{
	Tally tally;
	tally.Sent();
	tally.Sent();
	tally.Sent();
	tally.Reached();
	tally.Reached();
	tally.Stopped();
	tally.Direct();
	CHECK((tally.SentCount()) == (3u));
	CHECK((tally.ReachedCount()) == (2u));
	CHECK((tally.StoppedCount()) == (1u));
	CHECK((tally.DirectCount()) == (1u));
	tally.Clear();
	CHECK((tally.SentCount()) == (0u));
}

TEST("event tap: our sink goes ahead of the plugins' sinks and behind the engine's")
{
	// The engine's sinks (here the even numbers) finish the hit event, so ours must not stand before them; a plugin's sink
	// (odd) that stops events must not stand before ours.
	const auto engine = [](int a_sink) { return a_sink % 2 == 0; };
	std::array<int, 6> sinks{ 2, 4, 11, 13, 15, 99 };  // 99 is ours, registered last
	bool               moved = false;
	std::size_t        index = 0;
	REQUIRE(PlaceAfterLeading(sinks.data(), sinks.size(), 99, engine, &moved, &index));
	CHECK(moved);
	CHECK(index == 2u);
	CHECK((sinks == std::array<int, 6>{ 2, 4, 99, 11, 13, 15 }));

	REQUIRE(PlaceAfterLeading(sinks.data(), sinks.size(), 99, engine, &moved, &index));
	CHECK(!moved);  // already there
	CHECK((sinks == std::array<int, 6>{ 2, 4, 99, 11, 13, 15 }));

	// Pushed back behind plugins again (a late registration moved it, or the rig's `front off`): put back.
	std::array<int, 6> behind{ 2, 4, 11, 99, 13, 15 };
	REQUIRE(PlaceAfterLeading(behind.data(), behind.size(), 99, engine, &moved));
	CHECK(moved);
	CHECK((behind == std::array<int, 6>{ 2, 4, 99, 11, 13, 15 }));

	// Before the engine's sinks (a plugin put it first): behind them.
	std::array<int, 4> first{ 99, 2, 4, 11 };
	REQUIRE(PlaceAfterLeading(first.data(), first.size(), 99, engine, &moved));
	CHECK(moved);
	CHECK((first == std::array<int, 4>{ 2, 4, 99, 11 }));

	CHECK(!PlaceAfterLeading(sinks.data(), sinks.size(), 7, engine));  // not registered: nothing moved
	CHECK((sinks == std::array<int, 6>{ 2, 4, 99, 11, 13, 15 }));

	std::array<int, 1> one{ 99 };
	REQUIRE(PlaceAfterLeading(one.data(), one.size(), 99, engine, &moved));
	CHECK(!moved);
	CHECK(!PlaceAfterLeading(static_cast<int*>(nullptr), 0, 7, engine));

	// An engine sink that stands later (after a plugin's) is not part of the leading run.
	std::array<int, 4> late{ 2, 11, 4, 99 };
	REQUIRE(PlaceAfterLeading(late.data(), late.size(), 99, engine, &moved));
	CHECK((late == std::array<int, 4>{ 2, 99, 11, 4 }));
}

TEST("event tap: the position of a sink in the list")
{
	std::array<int, 3> sinks{ 1, 2, 3 };
	CHECK((Position(sinks.data(), sinks.size(), 3)) == (2u));
	CHECK(Position(sinks.data(), sinks.size(), 9) == 3u);  // not there: the size
}

TEST("event tap: the sinks are listed by module and offset, ours marked")
{
	const std::vector<SinkRow> rows{ { "Fallout4.exe", 0x2A4B10, false }, { "SS2.dll", 0x1F00, false }, { "F4Multiplayer.dll", 0x9000, true } };
	CHECK(DescribeSinks(rows) == "3 sinks: [0] Fallout4.exe+2A4B10, [1] SS2.dll+1F00, [2] F4Multiplayer.dll+9000 (ours)");
	CHECK((DescribeSinks({})) == ("0 sinks"));
	const std::vector<SinkRow> one{ { "", 0x10, true } };
	CHECK((DescribeSinks(one)) == ("1 sink: [0] ?+10 (ours)"));
}

TEST("event tap: the missing-events warning names the cause only when the hook saw hits stopped")
{
	// Told when hits were sent and ours never got them; silent when no hit was stopped (then the cause is not a sink ahead of ours).
	CHECK(CauseNote(40, 0).empty());
	CHECK(CauseNote(0, 0).empty());
	const auto note = CauseNote(40, 12);
	CHECK(note.find("40 hit events") != std::string::npos);
	CHECK(note.find("stopped 12") != std::string::npos);
}
