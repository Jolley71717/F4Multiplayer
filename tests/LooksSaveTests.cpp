// A friend's LooksMenu looks kept out of our saves (game/BodyMorphs.h's StandIns, game/LooksSave.h): a stand-in's removal from
// LooksMenu's store is queued, and a save written before it is answered keeps the friend's sliders and overlays under the
// stand-in's form ID, which the game hands to the next reference it makes. Such IDs stay listed until their removal is
// answered, go into the save's record, and after its load are cleared once off whatever reference has the ID, but for a
// stand-in of ours.

#include "Test.h"
#include "game/BodyMorphs.h"
#include "game/LooksSave.h"

#include <cstring>
#include <optional>
#include <set>
#include <vector>

namespace
{
	using BodyMorphs::Then;
	using Ids = std::vector<std::uint32_t>;

	constexpr std::uint32_t STAND_IN = 0xFF001234;
	constexpr std::uint32_t OTHER = 0xFF000800;

	// A model of the game's side of the scrub (LooksMenu's FrameScrub, game-side and not run here): each listed ID that a
	// reference has now, but for a stand-in of ours, gets a removal queued. What these tests pin is StandIns' part.
	struct World
	{
		BodyMorphs::StandIns    standIns;
		std::set<std::uint32_t> refs;     // the references the game has now
		std::set<std::uint32_t> ourOwn;   // our stand-ins among them (RemotePlayers, Puppets)
		Ids                     removals; // queued, in order

		void Scrub()
		{
			for (const auto id : standIns.Scrubbable()) {
				if (refs.contains(id) && !ourOwn.contains(id)) {
					standIns.Clearing(id);
					removals.push_back(id);
				}
			}
		}
	};

	// What a save written now records, read back as a load reads it.
	Ids SavedAndRead(const BodyMorphs::StandIns& a_standIns)
	{
		const auto bytes = LooksSave::Encode(a_standIns.Saved());
		if (bytes.empty()) {
			return {};
		}
		const auto read = LooksSave::Decode(bytes);
		return read ? *read : Ids{ 0 };
	}
}

TEST("looks save: a stand-in removed before a save stays listed until its removal is answered")
{
	BodyMorphs::StandIns standIns;
	standIns.Setting(STAND_IN);
	REQUIRE(standIns.Answered(STAND_IN) == Then::kUpdate);
	REQUIRE(standIns.Leaving(STAND_IN));  // the save takes the stand-in out just before it
	standIns.Clearing(STAND_IN);
	CHECK(standIns.Listed(STAND_IN));
	CHECK(standIns.Scrubbable().empty());  // its removal is under way: not queued again
	CHECK(SavedAndRead(standIns) == Ids{ STAND_IN });
	standIns.Cleared(STAND_IN);  // answered after the save was written
	CHECK(!standIns.Listed(STAND_IN));
	CHECK(SavedAndRead(standIns).empty());  // the next save has no record
}

TEST("looks save: a stand-in whose sets are queued at a save is listed in the record")
{
	BodyMorphs::StandIns standIns;
	standIns.Setting(STAND_IN);
	CHECK(SavedAndRead(standIns) == Ids{ STAND_IN });  // its sets may be answered before LooksMenu writes its store
	REQUIRE(!standIns.Leaving(STAND_IN));
	CHECK(SavedAndRead(standIns) == Ids{ STAND_IN });
	REQUIRE(standIns.Answered(STAND_IN) == Then::kClear);
	standIns.Clearing(STAND_IN);
	CHECK(SavedAndRead(standIns) == Ids{ STAND_IN });
	standIns.Cleared(STAND_IN);
	CHECK(SavedAndRead(standIns).empty());
}

TEST("looks save: a removal never answered, or one for a stand-in already gone, is left for a scrub")
{
	BodyMorphs::StandIns standIns;
	standIns.Clearing(STAND_IN);
	standIns.Unanswered(STAND_IN);  // not answered in time
	CHECK(standIns.Scrubbable() == Ids{ STAND_IN });
	standIns.Setting(OTHER);
	REQUIRE(!standIns.Leaving(OTHER));
	REQUIRE(standIns.Answered(OTHER) == Then::kClear);
	standIns.Unanswered(OTHER);  // no reference left to clear: whatever has its ID later is
	CHECK(standIns.Scrubbable() == (Ids{ OTHER, STAND_IN }));
	CHECK(SavedAndRead(standIns) == (Ids{ OTHER, STAND_IN }));
}

TEST("looks save: the record round-trips, at most 512 IDs, and a bad length refuses it")
{
	const Ids ids = { 0xFF000003, 0xFF000001, 0xFF000002, 0xFF000001 };
	const auto bytes = LooksSave::Encode(ids);
	CHECK(bytes.size() == 2 + 4 * ids.size());
	const auto read = LooksSave::Decode(bytes);
	REQUIRE(read);
	CHECK(*read == (Ids{ 0xFF000001, 0xFF000002, 0xFF000003 }));  // sorted, each once

	Ids many;
	for (std::uint32_t i = 0; i < BodyMorphs::MAX_SAVED + 100; ++i) {
		many.push_back(0xFF100000 + i);
	}
	const auto full = LooksSave::Encode(many);
	CHECK(full.size() == 2 + 4 * BodyMorphs::MAX_SAVED);
	const auto readFull = LooksSave::Decode(full);
	REQUIRE(readFull);
	CHECK(readFull->size() == BodyMorphs::MAX_SAVED);

	auto shorter = bytes;
	shorter.pop_back();
	CHECK(!LooksSave::Decode(shorter));
	auto longer = bytes;
	longer.push_back(0);
	CHECK(!LooksSave::Decode(longer));
	CHECK(!LooksSave::Decode(std::vector<std::uint8_t>{ 1 }));
	CHECK(!LooksSave::Decode(std::vector<std::uint8_t>{}));

	// A count past MAX_SAVED, with the bytes to match, as a damaged record might have.
	std::vector<std::uint8_t> over(2 + 4 * (BodyMorphs::MAX_SAVED + 1), 0xFF);
	const auto count = static_cast<std::uint16_t>(BodyMorphs::MAX_SAVED + 1);
	std::memcpy(over.data(), &count, sizeof(count));
	CHECK(!LooksSave::Decode(over));

	// The list a save remembers is capped the same way.
	BodyMorphs::StandIns standIns;
	for (const auto id : many) {
		standIns.Clearing(id);
	}
	CHECK(standIns.Saved().size() == BodyMorphs::MAX_SAVED);
}

TEST("looks save: the record holds only runtime references' IDs, and one naming another refuses it")
{
	CHECK(LooksSave::Encode({ 0x0001A4D7 }).empty());  // a plugin's reference is never a stand-in
	const auto bytes = LooksSave::Encode({ 0x0001A4D7, STAND_IN });
	const auto read = LooksSave::Decode(bytes);
	REQUIRE(read);
	CHECK(*read == Ids{ STAND_IN });

	auto damaged = bytes;
	damaged.back() = 0x00;  // the ID's top byte: no longer a runtime one
	CHECK(!LooksSave::Decode(damaged));
}

TEST("looks save: the list's rules clear a listed ID found on another reference after a load once, and it leaves the list")
{
	World world;
	LooksSave::Loaded record;
	record.Revert();  // the load reverts the co-save first, then reads the record
	REQUIRE(record.Read(LooksSave::Encode({ STAND_IN, OTHER })) == std::optional<std::size_t>{ 2 });
	world.standIns.Reset();  // then the game says it loaded: the list is the record's
	world.standIns.Restore(record.Take());
	CHECK(world.standIns.Uncleared() == 2);

	world.Scrub();
	CHECK(world.removals.empty());  // no reference has either ID yet

	world.refs.insert(STAND_IN);  // a raider spawned, given the stand-in's old ID
	world.Scrub();
	world.Scrub();  // a second later, the removal not answered yet
	CHECK(world.removals == Ids{ STAND_IN });
	world.standIns.Cleared(STAND_IN);
	world.Scrub();
	CHECK(world.removals == Ids{ STAND_IN });
	CHECK(!world.standIns.Listed(STAND_IN));
	CHECK(SavedAndRead(world.standIns) == Ids{ OTHER });  // still owed: the next save remembers it
}

TEST("looks save: a stand-in placed under a listed ID is cleared before its own looks are set, and once")
{
	BodyMorphs::StandIns standIns;
	standIns.Restore({ STAND_IN });
	CHECK(!standIns.Placing(OTHER));  // an ID not listed: set at once

	// Placed under the old stand-in's ID: its old looks go first, and the scrub does not queue them again.
	REQUIRE(standIns.Placing(STAND_IN));
	standIns.Clearing(STAND_IN);
	CHECK(standIns.Scrubbable().empty());
	CHECK(SavedAndRead(standIns) == Ids{ STAND_IN });
	standIns.Cleared(STAND_IN);
	CHECK(standIns.Released(STAND_IN));  // now its own are set
	CHECK(!standIns.Released(STAND_IN));
	CHECK(!standIns.Placing(STAND_IN));  // placed again: nothing left to clear
	standIns.Setting(STAND_IN);
	REQUIRE(standIns.Answered(STAND_IN) == Then::kUpdate);
	CHECK(SavedAndRead(standIns) == Ids{ STAND_IN });  // listed now as a stand-in of ours

	// One whose removal is never answered is set anyway (over the old looks), and its ID left for later.
	BodyMorphs::StandIns silent;
	silent.Restore({ OTHER });
	REQUIRE(silent.Placing(OTHER));
	silent.Clearing(OTHER);
	silent.Unanswered(OTHER);
	CHECK(silent.Released(OTHER));
	CHECK(silent.Listed(OTHER));
}

TEST("looks save: the list's rules leave a listed ID that is one of our stand-ins to its own removal")
{
	World world;
	world.standIns.Restore({ STAND_IN, OTHER });

	// One being set, and one the game tells is ours (FrameScrub's stand-in checks, modelled here): neither is scrubbed.
	world.refs = { STAND_IN, OTHER };
	world.ourOwn = { OTHER };
	world.standIns.Setting(STAND_IN);
	world.Scrub();
	CHECK(world.removals.empty());

	// It goes as stand-ins go, and its own removal takes the ID off the list.
	REQUIRE(world.standIns.Answered(STAND_IN) == Then::kUpdate);
	REQUIRE(world.standIns.Leaving(STAND_IN));
	world.standIns.Clearing(STAND_IN);
	world.standIns.Cleared(STAND_IN);
	CHECK(!world.standIns.Listed(STAND_IN));

	// Once the other is not ours any more, whatever has its ID is cleared.
	world.ourOwn.clear();
	world.Scrub();
	CHECK(world.removals == Ids{ OTHER });
}

TEST("looks save: revert empties the list, and the game's load keeps what the record listed")
{
	BodyMorphs::StandIns standIns;
	LooksSave::Loaded    record;
	standIns.Setting(STAND_IN);
	standIns.Clearing(OTHER);
	record.Read(LooksSave::Encode({ OTHER }));  // an earlier load's record, never taken
	record.Revert();
	standIns.Reset();
	standIns.Restore(record.Take());
	CHECK(standIns.Uncleared() == 0);
	CHECK(SavedAndRead(standIns).empty());  // a new game, or a save with no record: nothing owed

	record.Revert();
	REQUIRE(record.Read(LooksSave::Encode({ OTHER })));
	standIns.Reset();
	standIns.Restore(record.Take());
	CHECK(standIns.Listed(OTHER));
	CHECK(standIns.Scrubbable() == Ids{ OTHER });
	CHECK(record.Take().empty());  // taken once
}

TEST("looks save: a record that can't be read lists nothing after the load")
{
	LooksSave::Loaded record;
	record.Revert();
	REQUIRE(record.Read(LooksSave::Encode({ OTHER })));
	CHECK(!record.Read(std::vector<std::uint8_t>{ 1 }));  // a damaged record read after it: none
	BodyMorphs::StandIns standIns;
	standIns.Clearing(STAND_IN);
	standIns.Reset();
	standIns.Restore(record.Take());
	CHECK(standIns.Uncleared() == 0);
}

TEST("looks save: a load's record is listed only once the game says it loaded, so an answer from before the load can't unlist it")
{
	// Save, then load that save: the stand-in's removal is still queued as the load's thread reads the record, and frames
	// may run on (a load that outlasts the hold) and see it answered.
	BodyMorphs::StandIns standIns;
	LooksSave::Loaded    record;
	standIns.Setting(STAND_IN);
	REQUIRE(standIns.Answered(STAND_IN) == Then::kUpdate);
	REQUIRE(standIns.Leaving(STAND_IN));
	standIns.Clearing(STAND_IN);
	const auto saved = LooksSave::Encode(standIns.Saved());

	record.Revert();  // the load's thread
	REQUIRE(record.Read(saved));
	CHECK(standIns.Listed(STAND_IN));  // the list is the main thread's, untouched by the load's
	standIns.Cleared(STAND_IN);  // a frame meanwhile: the old world's removal answered
	CHECK(!standIns.Listed(STAND_IN));

	standIns.Reset();  // the game says it loaded (LooksMenu::OnGameLoaded, after its clearings are dropped)
	standIns.Restore(record.Take());
	CHECK(standIns.Listed(STAND_IN));  // the loaded save's LooksMenu store may hold the looks again
	CHECK(standIns.Scrubbable() == Ids{ STAND_IN });
}

TEST("looks save: a stand-in placed under a listed ID has whatever its own looks leave out rebuilt once the old ones are removed")
{
	// Its 3D may have been built wearing the old body and tattoos; its own sets rebuild only what they set (kUpdate).
	const auto none = BodyMorphs::RebuildOnRelease(false, false);
	CHECK(none.morphs);
	CHECK(none.overlays);
	const auto sliders = BodyMorphs::RebuildOnRelease(true, false);
	CHECK(!sliders.morphs);  // rebuilt once its sliders are set
	CHECK(sliders.overlays);
	const auto overlays = BodyMorphs::RebuildOnRelease(false, true);
	CHECK(overlays.morphs);
	CHECK(!overlays.overlays);
	const auto both = BodyMorphs::RebuildOnRelease(true, true);
	CHECK(!both.morphs);
	CHECK(!both.overlays);
}
