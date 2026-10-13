#include "Test.h"
#include "game/ArmorFrames.h"

#include <limits>

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

TEST("armor frames: the copy the stand-in is in is the engine's: never held, moved, placed again or deleted by us")
{
	// Switched in, the engine disables the copy (no 3D) and leaves it where it was placed while the stand-in walks off in
	// it. Taken for a copy that never loaded, or one left behind, it was deleted from under the stand-in.
	CopyFacts facts = held;
	facts.engaged = true;
	CHECK(ArmorFrames::ChooseCopy(facts) == Copy::kNone);
	facts.loaded = false;
	facts.sincePlaced = ArmorFrames::LOAD_LIMIT + 1s;
	CHECK(ArmorFrames::ChooseCopy(facts) == Copy::kNone);
	facts.distance = ArmorFrames::MOVE_LIMIT * 10.0f;
	CHECK(ArmorFrames::ChooseCopy(facts) == Copy::kNone);
	facts.samePlace = false;
	CHECK(ArmorFrames::ChooseCopy(facts) == Copy::kNone);
	// Nobody here can be in a disabled frame; nor is it handed over or deleted when they get out: the stand-in goes with it.
	facts.occupied = true;
	CHECK(ArmorFrames::ChooseCopy(facts) == Copy::kNone);
	facts.worn = false;
	facts.sinceOut = ArmorFrames::KEEP_AFTER_OUT;
	CHECK(ArmorFrames::ChooseCopy(facts) == Copy::kNone);
}

TEST("armor frames: the stand-in is switched in as soon as its copy is held under it")
{
	using ArmorFrames::ChooseSwitch;
	using ArmorFrames::Switch;
	using ArmorFrames::SwitchFacts;
	const SwitchFacts ready{ .worn = true, .ready = true, .held = true };
	CHECK(ChooseSwitch(ready) == Switch::kIn);
	// Its copy not loaded (or not placed) yet, or the stand-in not drawn yet: it waits, in their clothes.
	auto facts = ready;
	facts.held = false;
	CHECK(ChooseSwitch(facts) == Switch::kNone);
	facts = ready;
	facts.ready = false;
	CHECK(ChooseSwitch(facts) == Switch::kNone);
	// Not in power armor: nothing to switch into.
	facts = ready;
	facts.worn = false;
	CHECK(ChooseSwitch(facts) == Switch::kNone);
	CHECK(ChooseSwitch({}) == Switch::kNone);
	// In it already: left so.
	facts = ready;
	facts.in = true;
	CHECK(ChooseSwitch(facts) == Switch::kNone);
	facts.held = false;
	facts.ready = false;
	CHECK(ChooseSwitch(facts) == Switch::kNone);
}

TEST("armor frames: out of their armor, a stand-in still in its frame is replaced")
{
	using ArmorFrames::ChooseSwitch;
	using ArmorFrames::Switch;
	using ArmorFrames::SwitchFacts;
	// The engine's own exit waits on the animation graph and, with no room to step out, on an AI package: never for a
	// stand-in. Whatever else holds.
	CHECK(ChooseSwitch({ .in = true }) == Switch::kOut);
	CHECK(ChooseSwitch({ .in = true, .rebuilt = true, .sinceIn = 1h }) == Switch::kOut);
	CHECK(ChooseSwitch({ .in = true, .ready = true, .held = true }) == Switch::kOut);
	CHECK(ChooseSwitch({ .in = true, .tries = ArmorFrames::MAX_SWITCH_TRIES }) == Switch::kOut);
	// Out, and the stand-in out too (it never got in, or is already the fresh one): nothing to do.
	CHECK(ChooseSwitch({ .ready = true, .held = true }) == Switch::kNone);
	CHECK(ChooseSwitch({ .tries = ArmorFrames::MAX_SWITCH_TRIES }) == Switch::kNone);
}

TEST("armor frames: a switch-in that does not take is tried again a little later, then given up")
{
	using ArmorFrames::ChooseSwitch;
	using ArmorFrames::Switch;
	using ArmorFrames::SwitchFacts;
	SwitchFacts facts{ .worn = true, .ready = true, .held = true, .tries = 1, .sinceTried = 0s };
	CHECK(ChooseSwitch(facts) == Switch::kNone);  // not every frame
	facts.sinceTried = ArmorFrames::SWITCH_RETRY - 1ms;
	CHECK(ChooseSwitch(facts) == Switch::kNone);
	facts.sinceTried = ArmorFrames::SWITCH_RETRY;
	CHECK(ChooseSwitch(facts) == Switch::kIn);
	facts.tries = ArmorFrames::MAX_SWITCH_TRIES - 1;
	CHECK(ChooseSwitch(facts) == Switch::kIn);
	// Given up: shown out of its armor (their clothes, the copy hidden under it) for the rest of this wear.
	facts.tries = ArmorFrames::MAX_SWITCH_TRIES;
	CHECK(ChooseSwitch(facts) == Switch::kGaveUp);
	facts.held = false;
	facts.ready = false;
	CHECK(ChooseSwitch(facts) == Switch::kGaveUp);
	// It took after all: in.
	facts.in = true;
	CHECK(ChooseSwitch(facts) == Switch::kNone);
	// Out of their armor: nothing given up any more.
	facts.in = false;
	facts.worn = false;
	CHECK(ChooseSwitch(facts) == Switch::kNone);
}

TEST("armor frames: the pieces go on only once the engine has the stand-in in its frame")
{
	using ArmorFrames::ChooseDress;
	using ArmorFrames::Dress;
	using ArmorFrames::Item;
	using ArmorFrames::Wears;
	// Out of a frame (whatever their status says: they may be in theirs, and the stand-in not in its own yet, or ever):
	// never a piece or the frame on its own body.
	CHECK(ChooseDress(false, false) == Dress::kClothes);
	CHECK(ChooseDress(false, true) == Dress::kClothes);
	CHECK(!Wears(Dress::kClothes, Item::kPiece));
	CHECK(!Wears(Dress::kClothes, Item::kFrame));
	CHECK(Wears(Dress::kClothes, Item::kOther));
	CHECK(Wears(Dress::kClothes, Item::kWeapon));
	// In its frame, on the power armor skeleton: their pieces and their weapons; neither clothes (under the armor at best,
	// or in the frame's slots) nor the frame, which the engine has put on it already.
	CHECK(ChooseDress(true, true) == Dress::kArmor);
	CHECK(Wears(Dress::kArmor, Item::kPiece));
	CHECK(Wears(Dress::kArmor, Item::kWeapon));
	CHECK(!Wears(Dress::kArmor, Item::kOther));
	CHECK(!Wears(Dress::kArmor, Item::kFrame));
}

TEST("armor frames: the pieces wait for the power armor skeleton, which the switch-in only queues")
{
	using ArmorFrames::ChooseDress;
	using ArmorFrames::ChooseSwitch;
	using ArmorFrames::Dress;
	using ArmorFrames::Item;
	using ArmorFrames::Switch;
	using ArmorFrames::SwitchFacts;
	using ArmorFrames::Wears;
	// The switch-in returns with the stand-in in its frame (ExtraPowerArmor, the power armor race) but still on its own
	// body: the rebuild of its 3D is a task of a later frame. The pieces put on there and then hung in the air off the
	// settler body until that task ran, on every entry.
	CHECK(ChooseDress(true, false) == Dress::kWait);
	CHECK(!Wears(Dress::kWait, Item::kPiece));
	CHECK(!Wears(Dress::kWait, Item::kWeapon));
	CHECK(!Wears(Dress::kWait, Item::kOther));
	CHECK(!Wears(Dress::kWait, Item::kFrame));
	// Nothing to do about it while the task is on its way; the pieces go on the update the skeleton is found.
	SwitchFacts facts{ .worn = true, .in = true, .rebuilt = false, .sinceIn = 0s };
	CHECK(ChooseSwitch(facts) == Switch::kNone);
	facts.sinceIn = ArmorFrames::SKELETON_LIMIT;
	CHECK(ChooseSwitch(facts) == Switch::kNone);
	facts.rebuilt = true;
	facts.sinceIn = 1h;
	CHECK(ChooseSwitch(facts) == Switch::kNone);
	CHECK(ChooseDress(true, true) == Dress::kArmor);
}

TEST("armor frames: a stand-in whose power armor skeleton never comes is placed again, and that counts as a try")
{
	using ArmorFrames::ChooseSwitch;
	using ArmorFrames::Switch;
	using ArmorFrames::SwitchFacts;
	// The switch-in queues nothing when the model loader already holds a load for the actor; and whatever else may keep
	// the task from running. The stand-in is replaced, as for a switch that never took; the fresh one is tried at once
	// (its copy held, nothing tried since), and after the limit for the wear the friend is shown in clothes.
	SwitchFacts facts{ .worn = true, .in = true, .rebuilt = false, .sinceIn = ArmorFrames::SKELETON_LIMIT + 1ms };
	CHECK(ChooseSwitch(facts) == Switch::kStuck);
	facts.ready = true;
	facts.held = true;
	CHECK(ChooseSwitch(facts) == Switch::kStuck);
	facts.tries = ArmorFrames::MAX_SWITCH_TRIES;  // in it already: not given up on, replaced
	CHECK(ChooseSwitch(facts) == Switch::kStuck);
	// Out of their armor meanwhile: out, whatever the skeleton.
	facts.worn = false;
	CHECK(ChooseSwitch(facts) == Switch::kOut);
	// The fresh one, with the tries of this wear kept.
	CHECK(ChooseSwitch({ .worn = true, .ready = true, .held = true, .tries = 1, .sinceTried = 1h }) == Switch::kIn);
	CHECK(ChooseSwitch({ .worn = true, .ready = true, .held = true, .tries = ArmorFrames::MAX_SWITCH_TRIES, .sinceTried = 1h }) == Switch::kGaveUp);
}

TEST("armor frames: a whole wear, from getting in to the frame left where they got out")
{
	using ArmorFrames::ChooseSwitch;
	using ArmorFrames::Switch;
	// They get in: a copy is placed at the stand-in, which waits in their clothes.
	CopyFacts copy{ .worn = true };
	CHECK(ChooseCopy(copy) == Copy::kPlace);
	CHECK(ChooseSwitch({ .worn = true, .ready = true, .held = false }) == Switch::kNone);
	// The copy's 3D is in: held under the stand-in, which goes into it on that same update.
	copy = held;
	CHECK(ChooseCopy(copy) == Copy::kHold);
	CHECK(ChooseSwitch({ .worn = true, .ready = true, .held = ChooseCopy(copy) == Copy::kHold }) == Switch::kIn);
	// In it, on its own body still: nothing put on. A frame or so later, on the power armor skeleton: the pieces.
	CHECK(ArmorFrames::ChooseDress(true, false) == ArmorFrames::Dress::kWait);
	CHECK(ChooseSwitch({ .worn = true, .in = true, .rebuilt = false, .sinceIn = 16ms }) == Switch::kNone);
	CHECK(ArmorFrames::ChooseDress(true, true) == ArmorFrames::Dress::kArmor);
	// In it, walking off: the copy (disabled, left behind) is the engine's.
	copy.engaged = true;
	copy.loaded = false;
	copy.distance = 2000.0f;
	CHECK(ChooseCopy(copy) == Copy::kNone);
	CHECK(ChooseSwitch({ .worn = true, .in = true, .rebuilt = true, .sinceIn = 1h }) == Switch::kNone);
	// Out: the stand-in goes (a fresh one comes out of armor), and a copy is placed where it stood, shown a while.
	CHECK(ChooseSwitch({ .worn = false, .in = true }) == Switch::kOut);
	CopyFacts left{ .worn = false, .exists = true, .loaded = true, .samePlace = true, .distance = 0.0f, .sincePlaced = 0s, .sinceOut = 0s };
	CHECK(ChooseCopy(left) == Copy::kShow);
	CHECK(ChooseSwitch({ .worn = false, .in = false, .ready = true }) == Switch::kNone);
	left.sinceOut = ArmorFrames::KEEP_AFTER_OUT;
	CHECK(ChooseCopy(left) == Copy::kDelete);
	// Somebody here got into it first: theirs.
	left.occupied = true;
	CHECK(ChooseCopy(left) == Copy::kAbandon);
}

TEST("armor frames: a stand-in placed again while they are still in it goes straight back in")
{
	using ArmorFrames::ChooseSwitch;
	using ArmorFrames::Switch;
	// Through a door, after a save or a load, or with a new look: the old one went with its frame; the fresh one has no
	// copy and has tried nothing.
	CHECK(ChooseCopy({ .worn = true }) == Copy::kPlace);
	CHECK(ChooseSwitch({ .worn = true, .ready = true, .held = false, .tries = 0 }) == Switch::kNone);
	CHECK(ChooseCopy(held) == Copy::kHold);
	CHECK(ChooseSwitch({ .worn = true, .ready = true, .held = true, .tries = 0 }) == Switch::kIn);
	// The copy of the old one, should it still be ours (a copy left where they got out a moment ago, and they are back
	// in): held under the fresh one and used.
	CopyFacts back = held;
	back.sinceOut = 1s;
	CHECK(ChooseCopy(back) == Copy::kHold);
}

TEST("armor frames: our copy of a pre-placed frame is hidden only while it stands where it was placed")
{
	// The receiver's own suit, taken from the museum and parked at a power armor station: a friend parks theirs at the
	// same station and gets in, and ours, a step away, was hidden from under our player.
	using ArmorFrames::ChooseOriginal;
	using ArmorFrames::Original;
	using ArmorFrames::OriginalFacts;
	constexpr std::uint32_t museum = 0x000370E3;
	constexpr std::uint32_t base = 0x0019DED9;
	const OriginalFacts here{ .found = true, .created = false, .base = base, .frame = true, .loaded = true, .fromSpot = 3.0f };

	CHECK(ChooseOriginal(museum, base, here) == Original::kHide);  // never moved
	auto facts = here;
	facts.fromStart = ArmorFrames::START_TOLERANCE;
	CHECK(ChooseOriginal(museum, base, facts) == Original::kHide);
	facts.fromStart = ArmorFrames::START_TOLERANCE + 0.5f;
	CHECK(ChooseOriginal(museum, base, facts) == Original::kUsed);
	facts.fromStart = 30000.0f;  // Concord to Sanctuary
	CHECK(ChooseOriginal(museum, base, facts) == Original::kUsed);
	// Wherever they got in, and loaded or not: where it stands now says nothing of whose it is.
	facts.fromSpot = 0.0f;
	CHECK(ChooseOriginal(museum, base, facts) == Original::kUsed);
	facts.loaded = false;
	facts.fromSpot.reset();
	CHECK(ChooseOriginal(museum, base, facts) == Original::kUsed);
	// Not a number: not known to stand where it was placed.
	facts = here;
	facts.fromStart = std::numeric_limits<float>::quiet_NaN();
	CHECK(ChooseOriginal(museum, base, facts) == Original::kUsed);
	// Our player in it or last out of it, someone here in it, or a quest of ours on it: told as such first.
	facts = here;
	facts.fromStart = 500.0f;
	facts.ours = true;
	CHECK(ChooseOriginal(museum, base, facts) == Original::kOurs);
	facts.ours = false;
	facts.inUse = true;
	CHECK(ChooseOriginal(museum, base, facts) == Original::kInUse);
	facts.inUse = false;
	facts.quest = true;
	CHECK(ChooseOriginal(museum, base, facts) == Original::kQuest);
	// Not the same frame, or one made in this game: nothing of ours to leave alone.
	facts = here;
	facts.fromStart = 500.0f;
	facts.created = true;
	CHECK(ChooseOriginal(museum, base, facts) == Original::kMissing);
}
