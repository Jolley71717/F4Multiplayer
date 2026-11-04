#include "Test.h"
#include "game/ResourceMirror.h"

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
		std::map<std::uint32_t, Look> looks;

		Look Look(std::uint32_t a_ref)
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

TEST("workshop resources: a piece out of the builder's loaded cells counts as it did, and a gone one is forgotten unsent")
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
	pieces.looks.erase(PUMP);     // scrapped: its copy goes with the scrap
	t += POLL;
	CHECK(reporter.Poll(pieces, t).empty());
	CHECK(reporter.Size() == 1u);

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
	pieces.looks[CROP] = Here({ -1.0f, std::nanf(""), 2.0f, 0, 1e9f, 0 });
	reporter.Track(CROP);
	auto t = Clock::time_point{} + 1h;
	auto sent = Sent(reporter.Poll(pieces, t));
	REQUIRE(sent.size() == 1u);
	CHECK(sent[CROP] == (Values{ 0, 0, 2.0f, 0, Protocol::MAX_RESOURCE_VALUE, 0 }));
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

	applier.Forget();  // a save loaded: its workbenches hold none of ours
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
