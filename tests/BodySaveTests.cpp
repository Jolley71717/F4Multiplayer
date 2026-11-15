// What a save keeps of a companion's new body under way (BodySave: Curie's 300 in a game that mirrors her quest), and
// what the game that loads it makes of the record. Each test names what the players would see go wrong if it failed:
// a robot Curie with no items and her 300 done, a synth Curie never dressed, or an old save that no longer loads.

#include "Test.h"

#include "game/BodySave.h"

#include <limits>

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

TEST("body save: a version 1 record's bytes, as 0.19.0's saves hold them, are read field by field and written the same, so a later build still loads those saves")
{
	// Written by hand from the layout, not by Encode: a change to the layout that changes Encode and Decode alike
	// still fails here. Little-endian, as the game's machines are.
	const std::vector<std::uint8_t> sample{
		0x01, 0x00,              // version 1
		0x01,                    // step: the reset sent
		0x01,                    // the affinity was read
		0x49, 0x22, 0x10, 0x00,  // the companion, 00102249
		0xD1, 0x65, 0x08, 0x00,  // the body for her items, 000865D1
		0x83, 0xA6, 0x09, 0x00,  // the destination, 0009A683
		0x26, 0x2F, 0x1E, 0x00,  // the outfit, 001E2F26
		0x00, 0x20, 0x00, 0x44,  // the affinity, 512.5
		0x02, 0x00,              // two faction changes
		0x1C, 0xC2, 0x01, 0x00, 0x01,  // 0001C21C at rank 1
		0xE0, 0xA1, 0x06, 0x00, 0xFF,  // 0006A1E0 at rank -1: out of it
	};
	CHECK(sample.size() == 36);
	CHECK(BodySave::RECORD_VERSION == 1);
	CHECK(BodySave::HEADER == 26);
	CHECK(BodySave::PER_FACTION == 5);
	CHECK(BodySave::MAX_LENGTH == 26 + 64 * 5);

	const auto read = Decode(sample);
	REQUIRE(read);
	CHECK(read->phase == Phase::kReset);
	CHECK(read->ref == CURIE);
	CHECK(read->items == ROBOT_BODY);
	CHECK(read->destination == AMARI);
	CHECK(read->outfit == OUTFIT);
	REQUIRE(read->affinity);
	CHECK(*read->affinity == 512.5f);
	REQUIRE(read->factions.size() == 2);
	CHECK(read->factions[0].faction == 0x0001C21C);
	CHECK(read->factions[0].rank == 1);
	CHECK(read->factions[1].faction == 0x0006A1E0);
	CHECK(read->factions[1].rank == -1);

	// The same change written now gives the same bytes.
	Pending change;
	change.ref = CURIE;
	change.items = ROBOT_BODY;
	change.destination = AMARI;
	change.outfit = OUTFIT;
	change.phase = Phase::kReset;
	change.affinity = 512.5f;
	change.factions = { { 0x0001C21C, 1 }, { 0x0006A1E0, -1 } };
	CHECK(*read == change);
	CHECK(Encode(change) == sample);

	// The other steps and an affinity not read differ in their own bytes alone.
	auto removal = sample;
	removal[2] = 0x00;
	change.phase = Phase::kRemoval;
	CHECK(Encode(change) == removal);
	auto dressed = sample;
	dressed[2] = 0x02;
	change.phase = Phase::kDressed;
	CHECK(Encode(change) == dressed);
	auto unread = dressed;
	unread[3] = 0x00;
	unread[20] = unread[21] = unread[22] = unread[23] = 0x00;
	change.affinity.reset();
	CHECK(Encode(change) == unread);
	const auto none = Decode(unread);
	REQUIRE(none);
	CHECK(!none->affinity);
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

TEST("body save: an affinity that is no number is never set on her: not written as read, and a record that holds one is left")
{
	for (const float bad : { std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity() }) {
		auto change = Change(Phase::kReset);
		change.affinity = bad;
		const auto bytes = Encode(change);
		REQUIRE(bytes.size() == BodySave::HEADER + 3 * BodySave::PER_FACTION);
		CHECK(bytes[3] == 0);  // written as not read: the rest of the change is still carried on
		CHECK(bytes[20] == 0);
		CHECK(bytes[21] == 0);
		CHECK(bytes[22] == 0);
		CHECK(bytes[23] == 0);
		const auto read = Decode(bytes);
		REQUIRE(read);
		CHECK(!read->affinity);
		CHECK(read->outfit == OUTFIT);
		CHECK(read->factions.size() == 3);
	}

	// A record that says its affinity was read and holds no number (a damaged co-save): left whole, as any the game
	// can't read.
	const auto good = Encode(Change(Phase::kDressed));
	REQUIRE(Decode(good));
	auto noNumber = good;
	noNumber[20] = 0x00, noNumber[21] = 0x00, noNumber[22] = 0xC0, noNumber[23] = 0x7F;  // a quiet NaN
	CHECK(!Decode(noNumber));
	auto infinite = good;
	infinite[20] = 0x00, infinite[21] = 0x00, infinite[22] = 0x80, infinite[23] = 0x7F;  // +infinity
	CHECK(!Decode(infinite));
	auto negative = good;
	negative[20] = 0x00, negative[21] = 0x00, negative[22] = 0x80, negative[23] = 0xFF;  // -infinity
	CHECK(!Decode(negative));
	// The same bytes with the affinity said to be not read are no affinity at all.
	noNumber[3] = 0;
	const auto unread = Decode(noNumber);
	REQUIRE(unread);
	CHECK(!unread->affinity);
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
	using BodySave::Left;
	using BodySave::LeftOut;
	auto change = Change(Phase::kReset);
	change.ref = RUNTIME;
	CHECK(Encode(change).empty());  // no companion to carry it on for
	CHECK(LeftOut(change) == Left::kCompanion);  // ... and the game's log says so as the change begins
	change = Change(Phase::kRemoval);
	change.items = RUNTIME;
	CHECK(Encode(change).empty());  // the removal can't be made again
	CHECK(LeftOut(change) == Left::kItems);
	change = Change(Phase::kRemoval);
	change.items = 0;
	CHECK(Encode(change).empty());
	CHECK(LeftOut(change) == Left::kItems);
	change.ref = RUNTIME;
	CHECK(LeftOut(change) == Left::kCompanion);  // the companion's reason first: it holds for every step
	for (const auto phase : { Phase::kRemoval, Phase::kReset, Phase::kDressed }) {
		CHECK(LeftOut(Change(phase)) == Left::kNone);
		CHECK(!Encode(Change(phase)).empty());
	}

	change = Change(Phase::kReset);
	change.items = RUNTIME;  // not needed any more: the removal was made
	change.destination = RUNTIME;
	change.outfit = RUNTIME;
	change.factions.push_back({ RUNTIME, 2 });
	CHECK(LeftOut(change) == Left::kNone);  // written from the reset on
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
