#include "Test.h"
#include "game/ResourceMirror.h"

#include <limits>
#include <map>
#include <set>
#include <vector>

// A friend's workshop pieces counted in our settlement: each builder's game says what its pieces count for, and a friend's
// game puts the sum over its copies on its workbench, as a modifier the session alone holds.
namespace
{
	using namespace ResourceMirror;
	using namespace std::chrono_literals;

	constexpr std::uint32_t BUILDER = 2;
	constexpr std::uint32_t OTHER = 3;
	constexpr std::uint32_t CROP = 0xFF000A01;
	constexpr std::uint32_t PUMP = 0xFF000A02;
	constexpr std::uint32_t WALL = 0xFF000A03;
	constexpr std::uint32_t TURRET = 0xFF000A04;
	constexpr std::uint32_t BENCH = 0x000250FE;
	constexpr std::uint32_t RED_ROCKET = 0x00054BAE;

	Values Food(float a_food) { return { a_food, 0, 0, 0, 0, 0 }; }
	Values Water(float a_water) { return { 0, a_water, 0, 0, 0, 0 }; }

	// Our pieces, as the builder's game finds them.
	struct Pieces
	{
		std::map<std::uint32_t, ResourceMirror::Look> looks;

		ResourceMirror::Look Look(std::uint32_t a_ref)
		{
			const auto it = looks.find(a_ref);
			return it != looks.end() ? it->second : ResourceMirror::Look{};
		}
	};

	ResourceMirror::Look Here(const Values& a_values) { return { ResourceMirror::Look::State::kHere, a_values }; }
	ResourceMirror::Look Away() { return { ResourceMirror::Look::State::kAway, {} }; }

	std::map<std::uint32_t, Values> Sent(const std::vector<Entry>& a_entries)
	{
		std::map<std::uint32_t, Values> out;
		for (const auto& entry : a_entries) {
			out[entry.refId] = entry.values;
		}
		return out;
	}
}

TEST("workshop resources: the builder's game sends what its pieces count for, and then only what changed")
{
	Pieces   pieces;
	Reporter reporter;
	pieces.looks[CROP] = Here(Food(0.5f));
	pieces.looks[PUMP] = Here(Water(3));
	pieces.looks[WALL] = Here({});
	for (const auto ref : { CROP, PUMP, WALL }) {
		CHECK(reporter.Track(ref));
	}
	CHECK(!reporter.Track(0x0001F00D));  // a piece the settlement came with: both saves count it already

	auto t = Clock::time_point{} + 1h;
	auto sent = Sent(reporter.Poll(pieces, t));
	CHECK(sent.size() == 2u);  // the wall counts for nothing, and never did: not sent
	CHECK(sent[CROP] == Food(0.5f));
	CHECK(sent[PUMP] == Water(3));
	CHECK(reporter.Counting() == 2u);

	t += POLL;
	CHECK(reporter.Poll(pieces, t).empty());  // nothing changed

	pieces.looks[CROP] = Here({});  // its settler went elsewhere: it grows nothing now
	t += POLL;
	sent = Sent(reporter.Poll(pieces, t));
	REQUIRE(sent.size() == 1u);
	CHECK(!Any(sent[CROP]));  // sent as nothing, so the friend's game takes it off
	CHECK(reporter.Counting() == 1u);

	t += 200ms;
	pieces.looks[CROP] = Here(Food(0.5f));
	CHECK(reporter.Poll(pieces, t).empty());  // not before the next poll
}

TEST("workshop resources: a piece out of the builder's loaded cells counts as it did, and a gone one is sent as nothing")
{
	Pieces   pieces;
	Reporter reporter;
	pieces.looks[CROP] = Here(Food(0.5f));
	pieces.looks[PUMP] = Here(Water(3));
	reporter.Track(CROP);
	reporter.Track(PUMP);
	auto t = Clock::time_point{} + 1h;
	CHECK(reporter.Poll(pieces, t).size() == 2u);

	pieces.looks[CROP] = Away();  // the builder went to another town
	pieces.looks.erase(PUMP);     // scrapped, or disabled by a script, or unlinked: no scrap report may take its copy
	t += POLL;
	auto gone = reporter.Poll(pieces, t);
	REQUIRE(gone.size() == 1u);
	CHECK(gone[0].refId == PUMP);
	CHECK(!Any(gone[0].values));  // the friend's next total leaves it out
	CHECK(reporter.Size() == 1u);
	t += POLL;
	CHECK(reporter.Poll(pieces, t).empty());  // once

	reporter.SendAll();  // someone joined: the piece away goes as last sent
	t += POLL;
	const auto sent = Sent(reporter.Poll(pieces, t));
	REQUIRE(sent.size() == 1u);
	CHECK(sent.at(CROP) == Food(0.5f));
}

TEST("workshop resources: all of them go again every SNAPSHOT, and values that are no count go as nothing")
{
	Pieces   pieces;
	Reporter reporter;
	pieces.looks[CROP] = Here({ -1.0f, std::nanf(""), 2.0f, 0.0005f, 1e9f, 0 });
	reporter.Track(CROP);
	auto t = Clock::time_point{} + 1h;
	auto sent = Sent(reporter.Poll(pieces, t));
	REQUIRE(sent.size() == 1u);
	CHECK(sent[CROP] == (Values{ 0, 0, 2.0f, 0, Protocol::MAX_RESOURCE_PIECE[kPower], 0 }));  // under EPSILON: nothing; past a piece's most: that
	t += POLL;
	CHECK(reporter.Poll(pieces, t).empty());
	t += SNAPSHOT;
	CHECK(reporter.Poll(pieces, t).size() == 1u);
}

TEST("workshop resources: no more than MAX_TRACKED pieces are tracked")
{
	Reporter reporter;
	for (std::uint32_t i = 1; i <= MAX_TRACKED; ++i) {
		CHECK(reporter.Track(0xFF000000 + i));
	}
	CHECK(!reporter.Track(0xFF100000));
	CHECK(reporter.Track(0xFF000001));  // known already
}

namespace
{
	// Our copies of friends' pieces, and the workbench each one's report named.
	struct Copies
	{
		std::map<Key, std::uint32_t> benches;

		std::uint32_t BenchOf(const Key& a_key)
		{
			const auto it = benches.find(a_key);
			return it != benches.end() ? it->second : 0;
		}
	};
}

TEST("workshop resources: a friend's pieces count at the workbench of our copy of each, and only those we have a copy of")
{
	Tally  tally;
	Copies copies;
	CHECK(tally.Set(BUILDER, { CROP, Food(0.5f) }));
	CHECK(tally.Set(BUILDER, { PUMP, Water(3) }));
	CHECK(tally.Set(BUILDER, { TURRET, { 0, 0, 8.0f, 0, 0, 0 } }));
	CHECK(tally.Set(OTHER, { CROP, Food(1.0f) }));  // the other friend's own piece of that number
	copies.benches[{ BUILDER, CROP }] = BENCH;
	copies.benches[{ BUILDER, PUMP }] = BENCH;
	copies.benches[{ OTHER, CROP }] = RED_ROCKET;
	// No copy of the turret: one of our own pieces stands at its spot (two saves with one past), or it waits for its cell.

	const auto totals = tally.Totals(copies);
	REQUIRE(totals.size() == 2u);
	CHECK(totals.at(BENCH) == (Values{ 0.5f, 3.0f, 0, 0, 0, 0 }));
	CHECK(totals.at(RED_ROCKET) == Food(1.0f));

	copies.benches[{ BUILDER, TURRET }] = BENCH;  // placed now
	CHECK(tally.Totals(copies).at(BENCH)[kSafety] == 8.0f);

	CHECK(tally.Set(BUILDER, { PUMP, {} }));  // it counts for nothing now
	CHECK(tally.Totals(copies).at(BENCH)[kWater] == 0.0f);
	CHECK(tally.Size() == 3u);

	tally.Forget(BUILDER);  // they left
	const auto left = tally.Totals(copies);
	CHECK(left.size() == 1u);
	CHECK(!left.contains(BENCH));
}

TEST("workshop resources: no more than MAX_KEPT pieces are kept")
{
	Tally tally;
	for (std::uint32_t i = 1; i <= MAX_KEPT; ++i) {
		CHECK(tally.Set(BUILDER, { 0xFF000000 + i, Food(1) }));
	}
	CHECK(!tally.Set(BUILDER, { 0xFF100000, Food(1) }));
	CHECK(tally.Set(BUILDER, { 0xFF000001, Food(2) }));  // known already: changed
	CHECK(tally.Set(BUILDER, { 0xFF100000, {} }));       // nothing: nothing kept
	CHECK(tally.Dropped() == 1u);
}

TEST("workshop resources: a resource message names only pieces a game made, each value a count")
{
	using namespace Protocol;
	const ResourceStates states{ 4, { { 0xFF000042, Food(0.5f) }, { 0xFF001234, { 0, 3, 0, 1, 0, 2 } }, { 0xFF001235, {} } } };
	const auto           back = DecodeResourceStates(Encode(states, MessageType::kReportWorkshopResources));
	REQUIRE(back.has_value());
	CHECK(*back == states);
	CHECK(!DecodeResourceStates(Encode(ResourceStates{ 4, {} }, MessageType::kWorkshopResources)));                          // names nothing
	CHECK(!DecodeResourceStates(Encode(ResourceStates{ 4, { { 0x0001F00D, Food(1) } } }, MessageType::kWorkshopResources)));  // a piece of the world
	CHECK(!DecodeResourceStates(Encode(ResourceStates{ 4, { { 0xFF001234, Food(-1) } } }, MessageType::kWorkshopResources)));
	CHECK(!DecodeResourceStates(Encode(ResourceStates{ 4, { { 0xFF001234, Food(std::nanf("")) } } }, MessageType::kWorkshopResources)));
	CHECK(!DecodeResourceStates(Encode(ResourceStates{ 4, { { 0xFF001234, Food(MAX_RESOURCE_VALUE * 2) } } }, MessageType::kWorkshopResources)));
	CHECK(!DecodeResourceStates(Encode(ResourceStates{ 4, { { 0xFF001234, Food(std::numeric_limits<float>::infinity()) } } }, MessageType::kWorkshopResources)));
	auto bytes = Encode(states, MessageType::kWorkshopResources);
	bytes[1 + 4 + 1 + 4] = 0x40;  // the first entry's mask: a value past the last
	CHECK(!DecodeResourceStates(bytes));
	bytes = Encode(states, MessageType::kWorkshopResources);
	bytes.pop_back();
	CHECK(!DecodeResourceStates(bytes));
	std::vector<ResourceState> many(MAX_RESOURCE_ENTRIES + 10, { 0xFF000042, Food(1) });
	const auto                 capped = DecodeResourceStates(Encode(ResourceStates{ 4, many }, MessageType::kWorkshopResources));
	REQUIRE(capped.has_value());
	CHECK(capped->entries.size() == MAX_RESOURCE_ENTRIES);
	const auto chunks = Chunks(many);
	REQUIRE(chunks.size() == 2u);
	CHECK(chunks[1].size() == 10u);
}

namespace
{
	// Our workbenches, as the friend's game holds them: what each holds of ours, by Add.
	struct Benches
	{
		std::map<std::uint32_t, Values> held;
		std::set<std::uint32_t>         here{ BENCH, RED_ROCKET };
		int                             adds = 0;

		bool Add(std::uint32_t a_bench, Kind a_kind, float a_delta)
		{
			if (!here.contains(a_bench)) {
				return false;
			}
			held[a_bench][a_kind] += a_delta;
			++adds;
			return true;
		}

		std::optional<float> Temporary(std::uint32_t a_bench, Kind a_kind)
		{
			if (!here.contains(a_bench)) {
				return std::nullopt;
			}
			return held[a_bench][a_kind];
		}
	};
}

TEST("workshop resources: the totals go on the workbenches as what we add, and come off again")
{
	Benches benches;
	Applier applier;
	auto    t = Clock::time_point{} + 1h;
	CHECK(applier.Apply(benches, { { BENCH, { 0.5f, 3, 0, 0, 0, 0 } } }, t) == 2u);
	CHECK(benches.held[BENCH] == (Values{ 0.5f, 3, 0, 0, 0, 0 }));
	CHECK(applier.Apply(benches, { { BENCH, { 0.5f, 3, 0, 0, 0, 0 } } }, t) == 0u);  // as it was: nothing done
	CHECK(benches.adds == 2);

	CHECK(applier.Apply(benches, { { BENCH, { 1.5f, 3, 0, 0, 0, 0 } }, { RED_ROCKET, Water(2) } }, t) == 2u);
	CHECK(benches.held[BENCH][kFood] == 1.5f);
	CHECK(benches.held[RED_ROCKET][kWater] == 2.0f);

	CHECK(applier.Apply(benches, { { RED_ROCKET, Water(2) } }, t) == 2u);  // nothing at Sanctuary now
	CHECK(!Any(benches.held[BENCH]));
	CHECK(!applier.On().contains(BENCH));

	CHECK(applier.Apply(benches, { { RED_ROCKET, Water(2) }, { 0x0001F00D, Food(1) } }, t) == 0u);  // no such workbench here: nothing held of it
	CHECK(!applier.On().contains(0x0001F00D));

	CHECK(applier.TakeAllOff(benches) == 1u);  // the session ended
	CHECK(!Any(benches.held[RED_ROCKET]));
	CHECK(applier.On().empty());
}

TEST("workshop resources: before a save everything of ours comes off the workbenches, and goes back after")
{
	Benches benches;
	Applier applier;
	auto    t = Clock::time_point{} + 1h;
	applier.Apply(benches, { { BENCH, { 0.5f, 3, 8, 0, 0, 0 } } }, t);
	CHECK(applier.Hush(benches, t) == 3u);
	CHECK(!Any(benches.held[BENCH]));
	CHECK(applier.Apply(benches, { { BENCH, { 0.5f, 3, 8, 0, 0, 0 } } }, t + 1s) == 0u);  // the save isn't over
	CHECK(!Any(benches.held[BENCH]));
	applier.SaveDone();
	CHECK(applier.Apply(benches, { { BENCH, { 0.5f, 3, 8, 0, 0, 0 } } }, t + 1s) == 3u);
	CHECK(benches.held[BENCH] == (Values{ 0.5f, 3, 8, 0, 0, 0 }));

	applier.Hush(benches, t + 10s);  // no word that the save is over: let go after HUSH_LIMIT
	CHECK(applier.Apply(benches, { { BENCH, Food(1) } }, t + 10s + HUSH_LIMIT - 1ms) == 0u);
	CHECK(applier.Apply(benches, { { BENCH, Food(1) } }, t + 10s + HUSH_LIMIT) == 1u);
	CHECK(benches.held[BENCH] == Food(1));

	CHECK(applier.TakeAllOff(benches) == 1u);  // a load is coming
	CHECK(applier.ForgetWorld(benches, false) == 0u);  // the save's workbenches hold none of ours
	CHECK(applier.On().empty());
}

TEST("workshop resources: a friend's beds go on no workbench (the game counts beds by the base its recount sets)")
{
	Benches benches;
	Applier applier;
	const auto t = Clock::time_point{} + 1h;
	CHECK(applier.Apply(benches, { { BENCH, { 0, 0, 0, 4, 0, 0 } } }, t) == 0u);
	CHECK(!Any(benches.held[BENCH]));
	CHECK(applier.Apply(benches, { { BENCH, { 1, 0, 0, 4, 0, 0 } } }, t) == 1u);
	CHECK(benches.held[BENCH] == Food(1));
}

TEST("workshop resources: the save's record of what it held of friends' pieces reads back, and a bad one reads as nothing")
{
	CHECK(EncodeRecord({}).empty());  // the hush took everything off: no record
	const std::vector<std::pair<std::uint32_t, Values>> held{ { BENCH, { 0.5f, 0, 8, 0, 0, 2 } }, { RED_ROCKET, Water(3) } };
	const auto                                          bytes = EncodeRecord(held);
	CHECK(bytes.size() == 2u + (5 + 3 * 4) + (5 + 4));
	CHECK(DecodeRecord(bytes) == held);
	auto cut = bytes;
	cut.pop_back();
	CHECK(DecodeRecord(cut).empty());
	auto longer = bytes;
	longer.push_back(0);
	CHECK(DecodeRecord(longer).empty());
	auto badMask = bytes;
	badMask[2 + 4] = 0x80;
	CHECK(DecodeRecord(badMask).empty());
}

TEST("workshop resources: a piece counts for at most its kind's most, and a workbench for at most MAX_BENCH from each friend and from all")
{
	using namespace Protocol;
	// On the wire: a value past a piece's most is read as that, at the server's relay and at the friend's game alike.
	const auto read = DecodeResourceStates(Encode(ResourceStates{ 4, { { 0xFF001234, { 500.0f, 41.0f, 60.0f, 3.0f, 99999.0f, 2.0f } } } }, MessageType::kReportWorkshopResources));
	REQUIRE(read.has_value());
	CHECK(read->entries[0].values == (Values{ MAX_RESOURCE_PIECE[kFood], 41.0f, MAX_RESOURCE_PIECE[kSafety], 3.0f, MAX_RESOURCE_PIECE[kPower], 2.0f }));
	for (std::size_t k = 0; k < KINDS; ++k) {
		CHECK(MAX_RESOURCE_PIECE[k] * 10 <= MAX_BENCH[k]);  // ten of the biggest piece of any kind fit on a workbench
	}

	// The tally: each piece clamped, each friend's sum at a workbench, and the sum of all friends', at most MAX_BENCH.
	Tally  tally;
	Copies copies;
	CHECK(tally.Set(BUILDER, { 0xFF000001, Water(1e6f) }));
	CHECK(tally.Of({ BUILDER, 0xFF000001 }) == Water(MAX_RESOURCE_PIECE[kWater]));
	for (std::uint32_t i = 1; i <= 100; ++i) {
		CHECK(tally.Set(BUILDER, { 0xFF000000 + i, Water(100) }));  // 10000 water from one friend
		CHECK(tally.Set(OTHER, { 0xFF000000 + i, Food(5) }));        // and 500 food from another
		copies.benches[{ BUILDER, 0xFF000000 + i }] = BENCH;
		copies.benches[{ OTHER, 0xFF000000 + i }] = BENCH;
	}
	CHECK(tally.Set(OTHER, { 0xFF000200, Water(100) }));
	copies.benches[{ OTHER, 0xFF000200 }] = BENCH;
	auto totals = tally.Totals(copies);
	CHECK(totals.at(BENCH)[kWater] == MAX_BENCH[kWater]);  // one friend's capped, and the other's 100 on top capped again
	CHECK(totals.at(BENCH)[kFood] == MAX_BENCH[kFood]);
	tally.Forget(BUILDER);
	totals = tally.Totals(copies);
	CHECK(totals.at(BENCH)[kWater] == 100.0f);
	CHECK(totals.at(BENCH)[kFood] == MAX_BENCH[kFood]);

	CHECK(tally.Set(OTHER, { 0xFF000300, { std::nanf(""), -5, 0.0004f, 0, 0, 0 } }));  // no count of anything: nothing kept
	CHECK(!tally.Of({ OTHER, 0xFF000300 }).has_value());
}

TEST("workshop resources: taking off takes all of what we added, however little, so no remainder goes into a save")
{
	Benches benches;
	Applier applier;
	const auto t = Clock::time_point{} + 1h;
	applier.Apply(benches, { { BENCH, Food(1) } }, t);
	CHECK(applier.Apply(benches, { { BENCH, Food(1.0005f) } }, t) == 0u);  // a change under EPSILON: none
	CHECK(applier.Apply(benches, { { BENCH, Food(0.0005f) } }, t) == 1u);  // most of it off, a remainder under EPSILON on
	REQUIRE(applier.On().contains(BENCH));
	const float remainder = applier.On().at(BENCH)[kFood];
	CHECK(remainder > 0.0f);
	CHECK(remainder < EPSILON);
	const auto adds = benches.adds;
	CHECK(applier.Hush(benches, t) == 1u);  // the remainder comes off for the save
	CHECK(benches.adds == adds + 1);
	CHECK(applier.On().empty());
	CHECK(std::fabs(benches.held[BENCH][kFood]) < 1e-6f);
}

TEST("workshop resources: a second save before the first is over keeps friends' pieces off until both are")
{
	Benches benches;
	Applier applier;
	const Values all{ 0.5f, 3, 8, 0, 0, 0 };
	auto         t = Clock::time_point{} + 1h;
	applier.Apply(benches, { { BENCH, all } }, t);
	CHECK(applier.Hush(benches, t) == 3u);
	CHECK(applier.Hush(benches, t + 100ms) == 0u);  // a quicksave as the autosave is written: nothing more to take off
	applier.SaveDone();                             // the first is over
	CHECK(applier.Hushed());
	CHECK(applier.Apply(benches, { { BENCH, all } }, t + 200ms) == 0u);
	CHECK(!Any(benches.held[BENCH]));
	applier.SaveDone();  // and the second
	CHECK(!applier.Hushed());
	CHECK(applier.Apply(benches, { { BENCH, all } }, t + 300ms) == 3u);
	applier.SaveDone();  // a word too many changes nothing
	CHECK(applier.Apply(benches, { { BENCH, all } }, t + 400ms) == 0u);
	CHECK(benches.held[BENCH] == all);
}

TEST("workshop resources: a save's record is made fresh for it: what its hush left on, or all that is on if the hush didn't say")
{
	Benches    benches;
	Applier    applier;
	SaveRecord record;
	auto       t = Clock::time_point{} + 1h;
	applier.Apply(benches, { { BENCH, Food(1) }, { RED_ROCKET, Water(2) } }, t);
	record.Live(applier.On());

	// The hush can't take Red Rocket's off (no such workbench here now): the save's record names it.
	benches.here.erase(RED_ROCKET);
	record.BeforeSave();
	CHECK(applier.Hush(benches, t) == 1u);
	record.Hushed(applier.On());
	auto held = DecodeRecord(record.Take());
	REQUIRE(held.size() == 1u);
	CHECK(held[0].first == RED_ROCKET);
	CHECK(held[0].second == Water(2));
	applier.SaveDone();

	// The next save's hush takes everything off: its record is empty, not the last save's.
	benches.here.insert(RED_ROCKET);
	record.BeforeSave();
	CHECK(applier.Hush(benches, t + 1s) == 1u);
	record.Hushed(applier.On());
	CHECK(record.Take().empty());
	applier.SaveDone();

	// A hush that threw (or never ran) said nothing: the record holds all that is on now.
	applier.Apply(benches, { { BENCH, Food(1) } }, t + 2s);
	record.Live(applier.On());
	record.BeforeSave();
	held = DecodeRecord(record.Take());
	REQUIRE(held.size() == 1u);
	CHECK(held[0] == (std::pair<std::uint32_t, Values>{ BENCH, Food(1) }));
	CHECK(DecodeRecord(record.Take()) == held);  // a save with no hush at all: still what is on
}

TEST("workshop resources: a new world forgets what we put on the old one's workbenches, taking off what is still there")
{
	Benches benches;
	Applier applier;
	const auto t = Clock::time_point{} + 1h;
	applier.Apply(benches, { { BENCH, { 1, 3, 0, 0, 0, 0 } }, { RED_ROCKET, Water(2) } }, t);
	applier.Hush(benches, t);  // a hush that never got its word
	applier.Apply(benches, { { BENCH, { 1, 3, 0, 0, 0, 0 } }, { RED_ROCKET, Water(2) } }, t + HUSH_LIMIT);
	REQUIRE(applier.On().size() == 2u);

	// A new game from the main menu: Sanctuary's workbench kept what we put on (the engine didn't set it back), Red
	// Rocket's was set back.
	benches.held[RED_ROCKET] = {};
	const auto adds = benches.adds;
	CHECK(applier.ForgetWorld(benches, true) == 2u);
	CHECK(benches.adds == adds + 2);
	CHECK(!Any(benches.held[BENCH]));
	CHECK(!Any(benches.held[RED_ROCKET]));  // nothing taken off one that holds none of ours
	CHECK(applier.On().empty());
	CHECK(!applier.Hushed());
	CHECK(applier.ForgetWorld(benches, true) == 0u);

	// A loaded save's record taken off the workbenches it names (one not here: nothing).
	benches.held[BENCH] = Food(2);
	const std::vector<std::pair<std::uint32_t, Values>> record{ { BENCH, Food(2) }, { 0x0001F00D, Water(1) } };
	CHECK(TakeOffRecord(benches, record) == 1u);
	CHECK(!Any(benches.held[BENCH]));
}


TEST("workshop resources: what couldn't be sent goes again at the next poll, a gone piece's nothing until it is sent")
{
	Pieces   pieces;
	Reporter reporter;
	pieces.looks[CROP] = Here(Food(0.5f));
	pieces.looks[PUMP] = Here(Water(3));
	pieces.looks[TURRET] = Here({ 0, 0, 5, 0, 0, 0 });
	for (const auto ref : { CROP, PUMP, TURRET }) {
		reporter.Track(ref);
	}
	auto t = Clock::time_point{} + 1h;
	REQUIRE(reporter.Poll(pieces, t).size() == 3u);

	// The pump is scrapped, the crop's builder goes to another town, the turret is unchanged: the pump's nothing is all
	// there is to send, and the send queue is full.
	pieces.looks.erase(PUMP);
	pieces.looks[CROP] = Away();
	t += POLL;
	auto gone = reporter.Poll(pieces, t);
	REQUIRE(gone.size() == 1u);
	CHECK(gone[0].refId == PUMP);
	CHECK(!reporter.Tracks(PUMP));
	reporter.Unsent(gone);
	CHECK(reporter.Owed() == 1u);

	t += POLL;
	auto again = reporter.Poll(pieces, t);  // no snapshot names it: it goes again on its own
	REQUIRE(again.size() == 1u);
	CHECK(again[0].refId == PUMP);
	CHECK(!Any(again[0].values));
	reporter.Unsent(again);  // still full

	t += POLL;
	again = reporter.Poll(pieces, t);
	REQUIRE(again.size() == 1u);
	CHECK(again[0].refId == PUMP);
	CHECK(reporter.Owed() == 0u);  // sent this time
	t += POLL;
	CHECK(reporter.Poll(pieces, t).empty());  // and not again
	t += SNAPSHOT;
	CHECK(!Sent(reporter.Poll(pieces, t)).contains(PUMP));

	// A loaded piece's entry and an away one's, unsent, go again too, though neither changed.
	t += POLL;
	reporter.Unsent(std::vector<Entry>{ { TURRET, { 0, 0, 5, 0, 0, 0 } }, { CROP, Food(0.5f) } });
	auto sent = Sent(reporter.Poll(pieces, t));
	REQUIRE(sent.size() == 2u);
	CHECK(sent[TURRET] == (Values{ 0, 0, 5, 0, 0, 0 }));
	CHECK(sent[CROP] == Food(0.5f));  // as last sent
	t += POLL;
	CHECK(reporter.Poll(pieces, t).empty());

	// One whose unsent entry said something goes as it counts now: its loss after an unsent value, as nothing.
	t += POLL;
	reporter.Unsent(std::vector<Entry>{ { TURRET, { 0, 0, 5, 0, 0, 0 } } });
	pieces.looks.erase(TURRET);
	gone = reporter.Poll(pieces, t);
	REQUIRE(gone.size() == 1u);
	CHECK(gone[0].refId == TURRET);
	CHECK(!Any(gone[0].values));

	reporter.Unsent(gone);
	reporter.Clear();  // the session ended: the friend's game forgets all of ours with it
	CHECK(reporter.Owed() == 0u);
}

TEST("workshop resources: a load takes off only what its save's record names, never below what the workbench holds")
{
	Benches benches;
	Applier applier;
	const auto t = Clock::time_point{} + 1h;
	applier.Apply(benches, { { BENCH, { 2, 3, 0, 0, 0, 0 } } }, t);
	REQUIRE(applier.On().contains(BENCH));

	// Before the load, the workbench (one made at runtime) can't be reached: what is on it stays ours on the world going.
	benches.here.erase(BENCH);
	CHECK(applier.TakeAllOff(benches) == 0u);
	REQUIRE(applier.On().contains(BENCH));

	// The loaded save has a workbench of that ID, holding its own record's leftover (a hush that failed then).
	benches.here.insert(BENCH);
	benches.held[BENCH] = Food(2);
	const auto adds = benches.adds;
	CHECK(applier.ForgetWorld(benches, false) == 0u);  // a load: forgotten, nothing taken off for it
	CHECK(benches.adds == adds);
	CHECK(applier.On().empty());
	const std::vector<std::pair<std::uint32_t, Values>> record{ { BENCH, Food(2) } };
	CHECK(TakeOffRecord(benches, record) == 1u);
	CHECK(benches.held[BENCH] == Values{});  // 0, not -2 (the old world's 2 and the record's 2 both taken)

	// A record naming more than the workbench holds (another clear took it, or the engine set it back): let be.
	CHECK(TakeOffRecord(benches, record) == 0u);
	CHECK(benches.held[BENCH] == Values{});
	benches.held[BENCH] = Food(1);
	CHECK(TakeOffRecord(benches, record) == 0u);
	CHECK(benches.held[BENCH] == Food(1));

	// Within EPSILON of it: what it holds is taken, and no more.
	benches.held[BENCH] = Food(2.0f - EPSILON / 2);
	CHECK(TakeOffRecord(benches, record) == 1u);
	CHECK(benches.held[BENCH][kFood] == 0.0f);

	// A record value we could never have added (below 0, or no number): nothing.
	benches.held[BENCH] = Food(5);
	CHECK(TakeOffRecord(benches, { { BENCH, { -2, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), 0, 0, 0 } } }) == 0u);
	CHECK(benches.held[BENCH] == Food(5));
}

TEST("workshop resources: a new game's clear and a second clear of the same value leave no workbench below 0")
{
	Benches benches;
	Applier applier;
	const auto t = Clock::time_point{} + 1h;
	applier.Apply(benches, { { BENCH, { 2, 3, 0, 0, 0, 0 } } }, t);

	// A new game whose engine kept a part of it only: nothing of that value is taken off.
	benches.held[BENCH] = { 2, 1, 0, 0, 0, 0 };
	CHECK(applier.ForgetWorld(benches, true) == 1u);
	CHECK(benches.held[BENCH] == Water(1));

	// The same value cleared twice (a record naming what the new game's clear took): the second finds it gone.
	applier.Apply(benches, { { BENCH, Food(2) } }, t);
	benches.held[BENCH] = Food(2);
	CHECK(applier.ForgetWorld(benches, true) == 1u);
	CHECK(TakeOffRecord(benches, { { BENCH, Food(2) } }) == 0u);
	CHECK(benches.held[BENCH] == Values{});
}

TEST("workshop resources: the builder's world gone (a load, a new game, the main menu), its pieces that counted go as nothing")
{
	Pieces   pieces;
	Reporter reporter;
	pieces.looks[CROP] = Here(Food(0.5f));
	pieces.looks[PUMP] = Here(Water(3));
	pieces.looks[WALL] = Here({});
	pieces.looks[TURRET] = Here({ 0, 0, 5, 0, 0, 0 });
	for (const auto ref : { CROP, PUMP, WALL, TURRET }) {
		reporter.Track(ref);
	}
	auto t = Clock::time_point{} + 1h;
	REQUIRE(reporter.Poll(pieces, t).size() == 3u);

	// The turret's last entry couldn't be sent. Then the builder starts a new game: none of its pieces is there, and a look
	// finds their IDs naming nothing, as for a piece whose cell isn't loaded.
	reporter.Unsent(std::vector<Entry>{ { TURRET, { 0, 0, 5, 0, 0, 0 } } });
	pieces.looks = { { CROP, Away() }, { PUMP, Away() }, { WALL, Away() }, { TURRET, Away() } };
	CHECK(reporter.WorldGone() == 3u);  // the crop, the pump and the turret count at the friend's; the wall never did
	CHECK(reporter.Size() == 0u);
	CHECK(reporter.Counting() == 0u);

	t += POLL;
	auto sent = Sent(reporter.Poll(pieces, t));
	REQUIRE(sent.size() == 3u);
	for (const auto ref : { CROP, PUMP, TURRET }) {
		REQUIRE(sent.contains(ref));
		CHECK(!Any(sent[ref]));
	}
	CHECK(!sent.contains(WALL));
	CHECK(reporter.Owed() == 0u);
	t += POLL;
	CHECK(reporter.Poll(pieces, t).empty());  // once
	t += SNAPSHOT;
	CHECK(reporter.Poll(pieces, t).empty());  // and no snapshot names them

	// A load of a save that has some of them: the pump, found again before the poll, goes as it counts there and not as
	// nothing; the crop, not found yet, as nothing. A word refused then is owed still.
	for (const auto ref : { CROP, PUMP }) {
		pieces.looks[ref] = Here(ref == CROP ? Food(0.5f) : Water(3));
		reporter.Track(ref);
	}
	t += POLL;
	REQUIRE(reporter.Poll(pieces, t).size() == 2u);
	CHECK(reporter.WorldGone() == 2u);
	pieces.looks[CROP] = Away();
	reporter.Track(PUMP);
	t += POLL;
	auto entries = reporter.Poll(pieces, t);
	sent = Sent(entries);
	REQUIRE(entries.size() == 2u);  // one entry each
	CHECK(sent[PUMP] == Water(3));
	CHECK(!Any(sent[CROP]));
	reporter.Unsent(std::vector<Entry>{ { CROP, {} } });
	t += POLL;
	entries = reporter.Poll(pieces, t);
	REQUIRE(entries.size() == 1u);
	CHECK(entries[0].refId == CROP);
	CHECK(!Any(entries[0].values));

	// Twice over (the main menu, then the new game after it): the second finds nothing more.
	CHECK(reporter.WorldGone() == 1u);
	CHECK(reporter.WorldGone() == 0u);
	t += POLL;
	sent = Sent(reporter.Poll(pieces, t));
	REQUIRE(sent.size() == 1u);
	CHECK(!Any(sent[PUMP]));
}

TEST("workshop resources: from a load's start to its new world's clear, nothing of friends' goes on a workbench")
{
	Benches    benches;
	Applier    applier;
	const auto t = Clock::time_point{} + 1h;
	const std::map<std::uint32_t, Values> totals{ { BENCH, { 1, 3, 0, 0, 0, 0 } } };
	applier.Apply(benches, totals, t);
	CHECK(applier.BeforeLoad(benches) == 2u);
	CHECK(!Any(benches.held[BENCH]));
	CHECK(applier.Loading());

	// The load outlasts the save/load hold: frames run again, with the old world's copies still known. Nothing goes on, a
	// save's hush and its word don't end the block, and neither does HUSH_LIMIT.
	CHECK(applier.Apply(benches, totals, t + 61s) == 0u);
	applier.Hush(benches, t + 62s);
	applier.SaveDone();
	CHECK(applier.Apply(benches, totals, t + 62s + HUSH_LIMIT) == 0u);
	CHECK(!Any(benches.held[BENCH]));
	CHECK(applier.On().empty());

	// The new world's clear: nothing to forget that is still on, and the totals go on again.
	const auto adds = benches.adds;
	CHECK(applier.ForgetWorld(benches, false) == 0u);
	CHECK(benches.adds == adds);
	CHECK(!applier.Loading());
	CHECK(applier.Apply(benches, totals, t + 70s) == 2u);
	CHECK(benches.held[BENCH] == (Values{ 1, 3, 0, 0, 0, 0 }));

	// A new game (no load before it) isn't blocked at all.
	Applier fresh;
	CHECK(!fresh.Loading());
	CHECK(fresh.Apply(benches, { { RED_ROCKET, Water(2) } }, t) == 1u);
}

TEST("workshop resources: a clear never raises a workbench: a record's tiny value against a slightly negative modifier")
{
	Benches benches;
	// A corrupt record's value under EPSILON, and a modifier just below 0 (within EPSILON of it): min(value, held) is
	// negative, and taking it off would add.
	benches.held[BENCH] = Food(-EPSILON / 4);
	const auto adds = benches.adds;
	CHECK(TakeOffRecord(benches, { { BENCH, Food(EPSILON / 2) } }) == 0u);
	CHECK(benches.adds == adds);
	CHECK(benches.held[BENCH][kFood] == -EPSILON / 4);
	// At exactly 0 too: nothing to take.
	benches.held[BENCH] = {};
	CHECK(TakeOffRecord(benches, { { BENCH, Food(EPSILON / 2) } }) == 0u);
	CHECK(benches.adds == adds);
	// One it holds is still taken.
	benches.held[BENCH] = Food(EPSILON / 2);
	CHECK(TakeOffRecord(benches, { { BENCH, Food(EPSILON / 2) } }) == 1u);
	CHECK(benches.held[BENCH][kFood] == 0.0f);
}

TEST("workshop resources: the main menu forgets the world before's workbenches, taking off only what each still holds")
{
	Benches    benches;
	Applier    applier;
	const auto t = Clock::time_point{} + 1h;
	const std::map<std::uint32_t, Values> totals{ { BENCH, { 1, 3, 0, 0, 0, 0 } }, { RED_ROCKET, Water(2) } };
	REQUIRE(applier.Apply(benches, totals, t) == 3u);

	// Quit to the main menu: the engine set Red Rocket's modifier back, Sanctuary's it kept. Only Sanctuary's is taken
	// off, and nothing goes on while the menu is open.
	benches.held[RED_ROCKET] = {};
	CHECK(applier.MainMenu(benches, true, true) == 2u);
	CHECK(!Any(benches.held[BENCH]));
	CHECK(!Any(benches.held[RED_ROCKET]));  // not taken below 0
	CHECK(applier.On().empty());
	CHECK(applier.AtMainMenu());
	CHECK(applier.Apply(benches, totals, t + 1s) == 0u);
	CHECK(applier.MainMenu(benches, true, false) == 0u);  // every look while it is open: nothing more
	CHECK(applier.Apply(benches, totals, t + 2s) == 0u);
	CHECK(!Any(benches.held[BENCH]));

	// A coc from the menu: a new world with no message. Its first save's hush, and the session's end, take nothing off a
	// workbench that holds nothing of ours: no save gets a negative value.
	benches.held[BENCH] = Food(4);  // the coc world's own modifier, none of it ours
	CHECK(applier.MainMenu(benches, false, false) == 0u);
	CHECK(!applier.AtMainMenu());
	const auto adds = benches.adds;
	CHECK(applier.Hush(benches, t + 3s) == 0u);
	applier.SaveDone();
	CHECK(applier.TakeAllOff(benches) == 0u);
	CHECK(benches.adds == adds);
	CHECK(benches.held[BENCH] == Food(4));
	CHECK(benches.held[RED_ROCKET] == Values{});

	// Once the menu is closed, the totals go on again, on top of what the world holds of its own.
	CHECK(applier.Apply(benches, totals, t + 4s) == 3u);
	CHECK(benches.held[BENCH] == (Values{ 5, 3, 0, 0, 0, 0 }));
}

TEST("workshop resources: a main menu opened and closed between two looks is seen by its count, an engine that kept ours or not")
{
	Benches    benches;
	Applier    applier;
	const auto t = Clock::time_point{} + 1h;
	applier.Apply(benches, { { BENCH, Food(2) }, { RED_ROCKET, Water(2) } }, t);
	// The coc world is up before the next look: Sanctuary's workbench (same ID) holds a modifier of its own below ours,
	// Red Rocket's still holds ours.
	benches.held[BENCH] = Food(1);
	CHECK(applier.MainMenu(benches, false, true) == 1u);
	CHECK(benches.held[BENCH] == Food(1));  // not all of ours held: let be
	CHECK(!Any(benches.held[RED_ROCKET]));
	CHECK(applier.On().empty());
	CHECK(!applier.AtMainMenu());
	CHECK(applier.Apply(benches, { { BENCH, Food(2) } }, t + 1s) == 1u);

	// The session ends at the main menu: a look first, so the end takes off only what is still held.
	benches.held[BENCH] = {};  // set back as the game quit to the menu
	CHECK(applier.MainMenu(benches, true, true) == 0u);
	CHECK(applier.TakeAllOff(benches) == 0u);
	CHECK(benches.held[BENCH] == Values{});
	CHECK(applier.On().empty());
}

TEST("workshop resources: the main menu doesn't end a load's block or a save's hush")
{
	Benches    benches;
	Applier    applier;
	const auto t = Clock::time_point{} + 1h;
	const std::map<std::uint32_t, Values> totals{ { BENCH, Water(3) } };
	applier.Apply(benches, totals, t);

	// A load from the main menu: the menu is open as the load starts, and closes during it.
	CHECK(applier.MainMenu(benches, true, true) == 1u);
	applier.BeforeLoad(benches);
	CHECK(applier.MainMenu(benches, true, false) == 0u);
	CHECK(applier.MainMenu(benches, false, false) == 0u);
	CHECK(applier.Loading());
	CHECK(applier.Apply(benches, totals, t + 61s) == 0u);
	CHECK(!Any(benches.held[BENCH]));
	applier.ForgetWorld(benches, false);
	CHECK(applier.Apply(benches, totals, t + 62s) == 1u);

	// A save's hush outlasts a look at the menu too.
	applier.Hush(benches, t + 63s);
	CHECK(applier.MainMenu(benches, false, true) == 0u);
	CHECK(applier.Hushed());
	CHECK(applier.Apply(benches, totals, t + 64s) == 0u);
	applier.SaveDone();
	CHECK(applier.Apply(benches, totals, t + 65s) == 1u);
}

TEST("workshop resources: a piece found again in the new world, away before the next poll, still sends that it counts for nothing")
{
	Pieces   pieces;
	Reporter reporter;
	pieces.looks[PUMP] = Here(Water(3));
	reporter.Track(PUMP);
	auto t = Clock::time_point{} + 1h;
	REQUIRE(reporter.Poll(pieces, t).size() == 1u);

	// The main menu, then a coc: the new world's pump (same ID) is found, and its cell unloads within the second.
	CHECK(reporter.WorldGone() == 1u);
	reporter.Track(PUMP);
	pieces.looks[PUMP] = Away();
	t += POLL;
	auto entries = reporter.Poll(pieces, t);
	REQUIRE(entries.size() == 1u);
	CHECK(entries[0].refId == PUMP);
	CHECK(!Any(entries[0].values));  // the friend's game counts the old world's value no more
	CHECK(reporter.Owed() == 0u);
	t += POLL;
	CHECK(reporter.Poll(pieces, t).empty());  // once
	CHECK(reporter.Counting() == 0u);

	// Loaded again: sent as it counts there.
	pieces.looks[PUMP] = Here(Water(2));
	t += POLL;
	entries = reporter.Poll(pieces, t);
	REQUIRE(entries.size() == 1u);
	CHECK(entries[0].values == Water(2));

	// A piece tracked for the first time, away, and owing nothing, still sends nothing.
	reporter.Track(CROP);
	pieces.looks[CROP] = Away();
	t += POLL;
	CHECK(reporter.Poll(pieces, t).empty());
}
