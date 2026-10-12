#include "Test.h"
#include "game/ArmorFrames.h"

using namespace std::chrono_literals;
using ArmorFrames::ChooseCopy;
using ArmorFrames::Copy;
using ArmorFrames::CopyFacts;

namespace
{
	// A copy worn and in order: loaded, under the stand-in, placed a while ago.
	const CopyFacts held{ .worn = true, .exists = true, .loaded = true, .samePlace = true, .distance = 0.0f, .sincePlaced = 30s };
}

TEST("armor frames: a friend in power armor gets a copy at once, held under the stand-in as soon as it loads")
{
	// The museum report: the stand-in sent into its copy never got in (in=false sit=0 for fifteen seconds, twice),
	// and the friend was shown without armor meanwhile. Nothing waits for an engine climb now: the copy is placed on
	// the first update they are in, and held (hidden, without collision) on the first one it has its 3D.
	CHECK(ChooseCopy({ .worn = true }) == Copy::kPlace);
	CopyFacts facts{ .worn = true, .exists = true, .loaded = false, .sincePlaced = 0s };
	CHECK(ChooseCopy(facts) == Copy::kNone);  // its 3D is on its way
	facts.sincePlaced = 500ms;
	facts.loaded = true;
	CHECK(ChooseCopy(facts) == Copy::kHold);
	CHECK(ChooseCopy(held) == Copy::kHold);
	// Still attaching (no cell yet): kept, not placed again for ever on a cell seam.
	facts = held;
	facts.samePlace.reset();
	CHECK(ChooseCopy(facts) == Copy::kHold);
	// Up to the limit off: moved under the stand-in.
	facts = held;
	facts.distance = ArmorFrames::MOVE_LIMIT;
	CHECK(ChooseCopy(facts) == Copy::kHold);
}

TEST("armor frames: a copy that never loads, or is left in another place, is placed again")
{
	CopyFacts facts{ .worn = true, .exists = true, .loaded = false, .sincePlaced = ArmorFrames::LOAD_LIMIT };
	CHECK(ChooseCopy(facts) == Copy::kNone);
	facts.sincePlaced = ArmorFrames::LOAD_LIMIT + 1ms;
	CHECK(ChooseCopy(facts) == Copy::kReplace);
	// An interior we have left, or another worldspace.
	facts = held;
	facts.samePlace = false;
	CHECK(ChooseCopy(facts) == Copy::kReplace);
	// The stand-in jumped (placed again after a load, or its owner travelled): placed again there, not dragged along.
	facts = held;
	facts.distance = ArmorFrames::MOVE_LIMIT + 1.0f;
	CHECK(ChooseCopy(facts) == Copy::kReplace);
	facts.loaded = false;
	facts.sincePlaced = 0s;
	CHECK(ChooseCopy(facts) == Copy::kReplace);
}

TEST("armor frames: out of it, the copy stands where they got out a while, then goes")
{
	CopyFacts facts = held;
	facts.worn = false;
	facts.sinceOut = 0s;
	CHECK(ChooseCopy(facts) == Copy::kShow);
	facts.sinceOut = ArmorFrames::KEEP_AFTER_OUT - 1ms;
	CHECK(ChooseCopy(facts) == Copy::kShow);
	facts.sinceOut = ArmorFrames::KEEP_AFTER_OUT;
	CHECK(ChooseCopy(facts) == Copy::kDelete);
	// Wherever it stands: it is not moved once they are out.
	facts.samePlace = false;
	facts.distance = 5000.0f;
	facts.sinceOut = 1s;
	CHECK(ChooseCopy(facts) == Copy::kShow);
	// None, and none wanted.
	CHECK(ChooseCopy({}) == Copy::kNone);
	// Back in before it goes: the same copy is held under the stand-in again.
	facts = held;
	facts.sinceOut = 1s;
	CHECK(ChooseCopy(facts) == Copy::kHold);
}

TEST("armor frames: a copy somebody here got into is theirs, worn or not")
{
	CopyFacts facts = held;
	facts.worn = false;
	facts.occupied = true;
	CHECK(ChooseCopy(facts) == Copy::kAbandon);
	facts.sinceOut = ArmorFrames::KEEP_AFTER_OUT;  // not deleted under them
	CHECK(ChooseCopy(facts) == Copy::kAbandon);
	// The friend gets in again meanwhile: not moved under the stand-in with them in it; a fresh one comes next.
	facts.worn = true;
	CHECK(ChooseCopy(facts) == Copy::kAbandon);
	facts.samePlace = false;
	CHECK(ChooseCopy(facts) == Copy::kAbandon);
}

TEST("armor frames: where they got in is their newest state only if it came just now")
{
	CHECK(ArmorFrames::FreshEnough(0ms));
	CHECK(ArmorFrames::FreshEnough(ArmorFrames::WORN_AT_MAX_AGE));
	CHECK(!ArmorFrames::FreshEnough(ArmorFrames::WORN_AT_MAX_AGE + 1ms));
	CHECK(!ArmorFrames::FreshEnough(30s));  // their states stopped coming (a load): the next one is where they are
}

TEST("armor frames: our copy of a pre-placed frame is hidden only when it is the one they wear")
{
	using ArmorFrames::ChooseOriginal;
	using ArmorFrames::Original;
	using ArmorFrames::OriginalFacts;
	constexpr std::uint32_t museum = 0x0003F2B4;  // a frame placed by a plugin
	constexpr std::uint32_t base = 0x0019DED9;
	const OriginalFacts here{ .found = true, .created = false, .base = base, .frame = true, .loaded = true, .fromSpot = 12.0f };

	CHECK(ChooseOriginal(museum, base, here) == Original::kHide);
	// A frame made in their game, or none: they wear a copy of nothing we have.
	CHECK(ChooseOriginal(0, base, here) == Original::kNone);
	CHECK(ChooseOriginal(0xFF001234, base, here) == Original::kNone);
	CHECK(ChooseOriginal(museum, 0, here) == Original::kNone);
	// Not here, deleted, one of ours made at runtime, or another frame by that ID.
	auto facts = here;
	facts.found = false;
	CHECK(ChooseOriginal(museum, base, facts) == Original::kMissing);
	facts = here;
	facts.created = true;
	CHECK(ChooseOriginal(museum, base, facts) == Original::kMissing);
	facts = here;
	facts.base = 0x00140F1A;
	CHECK(ChooseOriginal(museum, base, facts) == Original::kMissing);
	// Our own player's suit, or one somebody here is in, is never hidden.
	facts = here;
	facts.ours = true;
	CHECK(ChooseOriginal(museum, base, facts) == Original::kOurs);
	facts = here;
	facts.inUse = true;
	CHECK(ChooseOriginal(museum, base, facts) == Original::kInUse);
	// Far away: looked at again once it loads.
	facts = here;
	facts.loaded = false;
	CHECK(ChooseOriginal(museum, base, facts) == Original::kNotLoaded);
}

TEST("armor frames: our copy of a pre-placed frame is hidden only where they got into theirs")
{
	using ArmorFrames::ChooseOriginal;
	using ArmorFrames::Original;
	using ArmorFrames::OriginalFacts;
	constexpr std::uint32_t museum = 0x0003F2B4;
	constexpr std::uint32_t base = 0x0019DED9;
	const OriginalFacts here{ .found = true, .created = false, .base = base, .frame = true, .loaded = true, .fromSpot = 0.0f };

	CHECK(ChooseOriginal(museum, base, here) == Original::kHide);
	// Up to the radius: they were lined up on it, or about to be.
	auto facts = here;
	facts.fromSpot = ArmorFrames::ORIGINAL_RADIUS;
	CHECK(ChooseOriginal(museum, base, facts) == Original::kHide);
	facts.fromSpot = 180.0f;  // as far as a player activates something
	CHECK(ChooseOriginal(museum, base, facts) == Original::kHide);
	// Ours was moved (our player walked off in it and left it elsewhere, say): theirs is not ours.
	facts.fromSpot = ArmorFrames::ORIGINAL_RADIUS + 1.0f;
	CHECK(ChooseOriginal(museum, base, facts) == Original::kElsewhere);
	facts.fromSpot = 5000.0f;
	CHECK(ChooseOriginal(museum, base, facts) == Original::kElsewhere);
	// They got in somewhere else altogether (another cell or worldspace): no distance to compare.
	facts.fromSpot.reset();
	CHECK(ChooseOriginal(museum, base, facts) == Original::kElsewhere);
	// Once seen elsewhere (or moved, or used, while hidden) in this wear, it stays shown even back on the spot.
	facts = here;
	facts.moved = true;
	CHECK(ChooseOriginal(museum, base, facts) == Original::kElsewhere);
	facts.loaded = false;
	CHECK(ChooseOriginal(museum, base, facts) == Original::kElsewhere);
	// Not loaded: where it stands is looked at once it is.
	facts = here;
	facts.loaded = false;
	facts.fromSpot.reset();
	CHECK(ChooseOriginal(museum, base, facts) == Original::kNotLoaded);
	// Occupied, or our player's own, whatever the distance.
	facts = here;
	facts.inUse = true;
	CHECK(ChooseOriginal(museum, base, facts) == Original::kInUse);
	facts = here;
	facts.ours = true;
	facts.fromSpot = 5000.0f;
	CHECK(ChooseOriginal(museum, base, facts) == Original::kOurs);
}

TEST("armor frames: our copy is hidden only when it is a power armor frame of the base they sent")
{
	using ArmorFrames::ChooseOriginal;
	using ArmorFrames::Original;
	using ArmorFrames::OriginalFacts;
	constexpr std::uint32_t museum = 0x0003F2B4;
	constexpr std::uint32_t base = 0x0019DED9;
	const OriginalFacts here{ .found = true, .created = false, .base = base, .frame = true, .loaded = true, .fromSpot = 0.0f };

	// A reference that is furniture but no frame (a chair, a terminal), even of the base they sent: never hidden.
	auto facts = here;
	facts.frame = false;
	CHECK(ChooseOriginal(museum, base, facts) == Original::kNotFrame);
	// A frame of another base than the one they sent: not theirs.
	CHECK(ChooseOriginal(museum, base + 1, here) == Original::kMissing);
	facts.base = 0x00012345;
	CHECK(ChooseOriginal(museum, 0x00012345, facts) == Original::kNotFrame);
}

TEST("armor frames: our copy is never hidden while a quest of ours holds it")
{
	// The museum's suit (000370E3) fills the main quest's power armor alias, the target of its objective, until our
	// player has done that step: hidden while a friend wears theirs, our player could neither see nor get into it.
	using ArmorFrames::ChooseOriginal;
	using ArmorFrames::Original;
	using ArmorFrames::OriginalFacts;
	constexpr std::uint32_t museum = 0x000370E3;
	constexpr std::uint32_t base = 0x0019DED9;
	const OriginalFacts here{ .found = true, .created = false, .base = base, .frame = true, .loaded = true, .fromSpot = 3.0f };

	CHECK(ChooseOriginal(museum, base, here) == Original::kHide);
	auto facts = here;
	facts.quest = true;
	CHECK(ChooseOriginal(museum, base, facts) == Original::kQuest);
	// Wherever it stands, loaded or not.
	facts.fromSpot = 5000.0f;
	CHECK(ChooseOriginal(museum, base, facts) == Original::kQuest);
	facts.loaded = false;
	CHECK(ChooseOriginal(museum, base, facts) == Original::kQuest);
	// Our player's own suit, or one someone is in, is told as such first.
	facts = here;
	facts.quest = true;
	facts.ours = true;
	CHECK(ChooseOriginal(museum, base, facts) == Original::kOurs);
	facts.ours = false;
	facts.inUse = true;
	CHECK(ChooseOriginal(museum, base, facts) == Original::kInUse);
	// Not the same frame at all: nothing of ours to leave alone.
	facts = here;
	facts.quest = true;
	facts.base = 0x00140F1A;
	CHECK(ChooseOriginal(museum, base, facts) == Original::kMissing);
}
