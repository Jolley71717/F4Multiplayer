// What a save keeps of a companion's new body under way (BodySave: Curie's 300 in a game that mirrors her quest), and
// what the game that loads it makes of the record. Each test names what the players would see go wrong if it failed:
// a robot Curie with no items and her 300 done, a synth Curie never dressed, or an old save that no longer loads.

#include "Test.h"

#include "game/BodySave.h"

using BodySave::Decode;
using BodySave::Encode;
using BodySave::Pending;
using BodySave::Phase;
using BodySave::Resolved;

namespace
{
	constexpr std::uint32_t CURIE = 0x00102249;
	constexpr std::uint32_t ROBOT_BODY = 0x000865D1;
	constexpr std::uint32_t AMARI = 0x0009A683;
	constexpr std::uint32_t OUTFIT = 0x001E2F26;
	constexpr std::uint32_t RUNTIME = 0xFF000F8B;  // a form made at runtime: another form after a load

	Pending Change(Phase a_phase)
	{
		Pending change;
		change.ref = CURIE;
		change.items = ROBOT_BODY;
		change.destination = AMARI;
		change.outfit = OUTFIT;
		change.phase = a_phase;
		change.affinity = 512.5f;
		change.factions = { { 0x000A1B85, 0 }, { 0x0001C21C, 1 }, { 0x0006A1E0, -1 } };
		return change;
	}

	// The load order did not change: each id is what it was.
	const auto SAME = [](std::uint32_t a_id) { return std::optional<std::uint32_t>{ a_id }; };
}

TEST("body save: a save made in any step of the change reads back as it was written, so the loaded game carries on from that step")
{
	for (const auto phase : { Phase::kRemoval, Phase::kReset, Phase::kDressed }) {
		const auto change = Change(phase);
		const auto bytes = Encode(change);
		CHECK(bytes.size() == BodySave::HEADER + 3 * BodySave::PER_FACTION);
		CHECK(bytes.size() <= BodySave::MAX_LENGTH);
		const auto read = Decode(bytes);
		REQUIRE(read);
		CHECK(*read == change);
		CHECK(read->factions.back().rank == -1);  // out of the faction: put back as removefromfaction
		const auto resolved = Resolved(*read, SAME);
		REQUIRE(resolved);
		CHECK(*resolved == change);
	}
}

TEST("body save: an affinity that could not be read stays not read, and a change with no outfit, destination or factions is kept")
{
	Pending change;
	change.ref = CURIE;
	change.items = ROBOT_BODY;
	change.phase = Phase::kReset;
	const auto bytes = Encode(change);
	CHECK(bytes.size() == BodySave::HEADER);
	const auto read = Decode(bytes);
	REQUIRE(read);
	CHECK(*read == change);
	CHECK(!read->affinity);  // not 0: a setav 0 would wipe what her new body has

	change.affinity = 0.0f;  // read, and 0
	const auto zero = Decode(Encode(change));
	REQUIRE(zero);
	REQUIRE(zero->affinity);
	CHECK(*zero->affinity == 0.0f);
}

TEST("body save: a save with no change under way, or made by an older build, has no record and loads as it always did")
{
	CHECK(!Decode({}));
	BodySave::Shared shared;
	CHECK(shared.Save().empty());  // nothing under way: no record written
	shared.Revert();
	CHECK(!shared.Take());  // no record read: nothing carried on
}

TEST("body save: form ids made at runtime are never written, since they name other forms after a load")
{
	auto change = Change(Phase::kReset);
	change.ref = RUNTIME;
	CHECK(Encode(change).empty());  // no companion to carry it on for
	change = Change(Phase::kRemoval);
	change.items = RUNTIME;
	CHECK(Encode(change).empty());  // the removal can't be made again
	change = Change(Phase::kRemoval);
	change.items = 0;
	CHECK(Encode(change).empty());

	change = Change(Phase::kReset);
	change.items = RUNTIME;  // not needed any more: the removal was made
	change.destination = RUNTIME;
	change.outfit = RUNTIME;
	change.factions.push_back({ RUNTIME, 2 });
	const auto read = Decode(Encode(change));
	REQUIRE(read);
	CHECK(read->ref == CURIE);
	CHECK(read->items == 0);
	CHECK(read->destination == 0);  // reset in place
	CHECK(read->outfit == 0);       // none set
	CHECK(read->factions.size() == 3);
	CHECK(read->affinity == change.affinity);
}

TEST("body save: more faction changes than the record holds are cut, not written past its longest length")
{
	auto change = Change(Phase::kReset);
	change.factions.clear();
	for (std::uint32_t i = 0; i < BodySave::MAX_FACTIONS + 10; ++i) {
		change.factions.push_back({ 0x00010000 + i, 0 });
	}
	const auto bytes = Encode(change);
	CHECK(bytes.size() == BodySave::MAX_LENGTH);
	const auto read = Decode(bytes);
	REQUIRE(read);
	CHECK(read->factions.size() == BodySave::MAX_FACTIONS);
}

TEST("body save: a record that is cut short, too long, of another version or of an unknown step is left, not half read")
{
	const auto bytes = Encode(Change(Phase::kReset));
	REQUIRE(Decode(bytes));
	for (std::size_t length = 0; length < bytes.size(); ++length) {
		CHECK(!Decode(std::span{ bytes.data(), length }));
	}
	auto longer = bytes;
	longer.push_back(0);
	CHECK(!Decode(longer));

	auto version = bytes;
	version[0] = 2;  // a later build's layout
	CHECK(!Decode(version));
	auto step = bytes;
	step[2] = 3;  // no such step
	CHECK(!Decode(step));
	auto flags = bytes;
	flags[3] = 2;
	CHECK(!Decode(flags));
	auto count = bytes;
	count[BodySave::HEADER - 2] = 4;  // says four factions, holds three
	CHECK(!Decode(count));
	auto companion = bytes;
	companion[7] = 0xFF;  // the companion's id, its top byte: a runtime form
	CHECK(!Decode(companion));
	auto faction = bytes;
	faction[BodySave::HEADER + 3] = 0xFF;  // the first faction's top byte
	CHECK(!Decode(faction));

	auto removal = Encode(Change(Phase::kRemoval));
	REQUIRE(Decode(removal));
	removal[8] = removal[9] = removal[10] = removal[11] = 0;  // no body for her items, and the removal still to make
	CHECK(!Decode(removal));
}

TEST("body save: a load order that changed moves each id with its plugin, and a plugin that is gone takes only its own part")
{
	// The plugin at 00 moved to 01.
	const auto moved = [](std::uint32_t a_id) { return std::optional<std::uint32_t>{ a_id | 0x01000000 }; };
	const auto change = Change(Phase::kRemoval);
	const auto shifted = Resolved(change, moved);
	REQUIRE(shifted);
	CHECK(shifted->ref == (CURIE | 0x01000000));
	CHECK(shifted->items == (ROBOT_BODY | 0x01000000));
	CHECK(shifted->destination == (AMARI | 0x01000000));
	CHECK(shifted->outfit == (OUTFIT | 0x01000000));
	REQUIRE(shifted->factions.size() == 3);
	CHECK(shifted->factions[1].faction == (0x0001C21C | 0x01000000));
	CHECK(shifted->factions[1].rank == 1);
	CHECK(shifted->affinity == change.affinity);
	CHECK(shifted->phase == Phase::kRemoval);

	// Each of these is gone from the loaded game in turn.
	const auto without = [](std::uint32_t a_gone) {
		return [a_gone](std::uint32_t a_id) { return a_id == a_gone ? std::nullopt : std::optional<std::uint32_t>{ a_id }; };
	};
	CHECK(!Resolved(change, without(CURIE)));               // no companion: nothing to carry on
	CHECK(!Resolved(change, without(ROBOT_BODY)));          // the removal can't be made again
	const auto reset = Change(Phase::kReset);
	const auto noBody = Resolved(reset, without(ROBOT_BODY));  // not needed after the removal
	REQUIRE(noBody);
	CHECK(noBody->items == 0);
	const auto noAmari = Resolved(reset, without(AMARI));
	REQUIRE(noAmari);
	CHECK(noAmari->destination == 0);  // reset where she stands
	const auto noOutfit = Resolved(reset, without(OUTFIT));
	REQUIRE(noOutfit);
	CHECK(noOutfit->outfit == 0);
	const auto noFaction = Resolved(reset, without(0x0001C21C));
	REQUIRE(noFaction);
	REQUIRE(noFaction->factions.size() == 2);
	CHECK(noFaction->factions[0].faction == 0x000A1B85);
	CHECK(noFaction->factions[1].faction == 0x0006A1E0);
	// A resolver that answers with a runtime id is not believed.
	CHECK(!Resolved(reset, [](std::uint32_t) { return std::optional<std::uint32_t>{ RUNTIME }; }));
}

TEST("body save: a save notes the step the change is in as it is made, nothing once it is over, and a loaded record is handed on once")
{
	BodySave::Shared shared;
	shared.Set(Change(Phase::kRemoval));
	auto read = Decode(shared.Save());
	REQUIRE(read);
	CHECK(read->phase == Phase::kRemoval);
	shared.Set(Change(Phase::kDressed));  // the change moved on
	read = Decode(shared.Save());
	REQUIRE(read);
	CHECK(read->phase == Phase::kDressed);
	shared.Set(std::nullopt);  // over
	CHECK(shared.Save().empty());

	// A load: the co-save reverted, then the record read; the main thread takes it once the game says it loaded.
	shared.Revert();
	shared.Load(Change(Phase::kReset));
	const auto taken = shared.Take();
	REQUIRE(taken);
	CHECK(taken->phase == Phase::kReset);
	CHECK(!shared.Take());

	// A load of a save with no record after one with: the first's record does not carry over.
	shared.Load(Change(Phase::kReset));
	shared.Revert();
	CHECK(!shared.Take());
}
