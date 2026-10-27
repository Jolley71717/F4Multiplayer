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

TEST("armor frames: the stand-in goes in once the engine will pick their frame item, steered by an item that makes it, never held back long")
{
	using ArmorFrames::ChoosePick;
	using ArmorFrames::Pick;
	using ArmorFrames::PickFacts;
	// Their frame item named, and the engine picks it already (the stand-in wears their vault suit): at once.
	CHECK(ChoosePick({ .named = true, .picksTheirs = true }) == Pick::kSwitch);
	// It picks another (the AtomCat frame, its default): the item that makes it pick theirs is put on, once, and the
	// switch-in waits for it to be on.
	PickFacts facts{ .named = true, .key = true };
	CHECK(ChoosePick(facts) == Pick::kPutOn);
	facts.sincePut = 16ms;
	CHECK(ChoosePick(facts) == Pick::kWait);
	facts.keyOn = true;
	facts.sincePut = 1s;
	CHECK(ChoosePick(facts) == Pick::kWait);  // on, but the engine does not pick by it yet
	facts.picksTheirs = true;
	CHECK(ChoosePick(facts) == Pick::kSwitch);
	// Taken off again (a gear check, a change of theirs) before it worked: put on again, not every frame.
	facts = { .named = true, .key = true, .sincePut = ArmorFrames::KEY_GAP - 1ms };
	CHECK(ChoosePick(facts) == Pick::kWait);
	facts.sincePut = ArmorFrames::KEY_GAP;
	CHECK(ChoosePick(facts) == Pick::kPutOn);
	// Nothing makes the engine pick theirs, or it still does not after PICK_WAIT: in all the same, in its pick.
	CHECK(ChoosePick({ .named = true, .key = false }) == Pick::kGiveUp);
	CHECK(ChoosePick({ .named = true, .key = true, .keyOn = true, .waited = ArmorFrames::PICK_WAIT, .sincePut = 1s }) == Pick::kGiveUp);
	// Their frame item not told yet: in once the clothes it was dressed in are on, so the engine picks by what they got
	// in with; at most PICK_WAIT later.
	CHECK(ChoosePick({ .named = false, .dressed = true }) == Pick::kSwitch);
	CHECK(ChoosePick({ .named = false, .dressed = false, .waited = 30ms }) == Pick::kWait);
	CHECK(ChoosePick({ .named = false, .dressed = false, .waited = ArmorFrames::PICK_WAIT }) == Pick::kGiveUp);
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

TEST("armor frames: back in within moments of getting out, the copy left there is used only when it is the frame they are in")
{
	// Out of one suit and straight into another at a station: the copy of the first, left where they got out, is placed
	// again as the frame they are in now, not dressed on the stand-in as the first.
	CopyFacts back = held;
	back.sinceOut = 1s;
	CHECK(ChooseCopy(back) == Copy::kHold);
	back.sameBase = false;
	CHECK(ChooseCopy(back) == Copy::kReplace);
	CHECK(ChooseCopy({ .worn = true }) == Copy::kPlace);  // the next update, once it is gone
	// Out of their armor, the copy left there is theirs whatever frame they tell next.
	CopyFacts left{ .worn = false, .exists = true, .loaded = true, .samePlace = true, .sinceOut = 1s, .sameBase = false };
	CHECK(ChooseCopy(left) == Copy::kShow);
	// The engine's while the stand-in is in it, whatever its base.
	back.engaged = true;
	CHECK(ChooseCopy(back) == Copy::kNone);
}

TEST("armor frames: the limits go by the time the game ran, not the time a menu had it stopped")
{
	using ArmorFrames::ChooseSwitch;
	using ArmorFrames::Played;
	using ArmorFrames::Switch;
	using Steady = std::chrono::steady_clock;
	// A frame at 60 fps.
	constexpr auto frame = std::chrono::duration_cast<Steady::duration>(std::chrono::duration<double>(1.0 / 60.0));
	Played     played;
	auto       clock = Steady::time_point{} + 2h;
	const auto run = [&](Steady::duration a_for, bool a_paused) {
		for (const auto end = clock + a_for; clock < end;) {
			clock += frame;
			played.Advance(clock, a_paused);
		}
		return played.Now();
	};
	// The stand-in is switched in just as we open the Pip-Boy, and the rebuild the switch-in queued has not run when we
	// close it half a minute later: its time on its own skeleton has only begun.
	const auto switchedAt = run(1s, false);
	run(30s, true);
	auto facts = ArmorFrames::SwitchFacts{ .worn = true, .in = true, .rebuilt = false, .sinceIn = run(100ms, false) - switchedAt };
	CHECK(facts.sinceIn < 200ms);
	CHECK(ChooseSwitch(facts) == Switch::kNone);
	// The game runs on and it never comes: placed again, by the game's own time.
	facts.sinceIn = run(ArmorFrames::SKELETON_LIMIT, false) - switchedAt;
	CHECK(ChooseSwitch(facts) == Switch::kStuck);
	// By the clock the menu counted, and it was placed again the moment the menu closed.
	CHECK(ChooseSwitch({ .worn = true, .in = true, .rebuilt = false, .sinceIn = 30s }) == Switch::kStuck);

	// A copy's 3D, and the frame item pick, wait out a load the same way.
	const auto placedAt = played.Now();
	run(20s, true);
	const CopyFacts loading{ .worn = true, .exists = true, .loaded = false, .samePlace = true, .sincePlaced = run(1s, false) - placedAt };
	CHECK(ChooseCopy(loading) == Copy::kNone);
	const auto pickSince = played.Now();
	run(5s, true);
	CHECK(ArmorFrames::ChoosePick({ .named = false, .dressed = false, .waited = run(100ms, false) - pickSince }) == ArmorFrames::Pick::kWait);
}

TEST("armor frames: a frame the game took far too long over counts for a quarter of a second")
{
	using ArmorFrames::Played;
	using Steady = std::chrono::steady_clock;
	// Its window in the background stops its frames: the next one comes a minute later.
	Played     played;
	const auto start = Steady::time_point{} + 2h;
	const auto first = played.Advance(start, false);
	CHECK(played.Advance(start + 16ms, false) - first == 16ms);
	CHECK(played.Advance(start + 16ms + 1min, false) - first == 16ms + ArmorFrames::LONGEST_FRAME);
	// A time never set is long ago.
	CHECK(played.Now() - ArmorFrames::PlayTime{} > ArmorFrames::SWITCH_RETRY);
}

namespace
{
	// A thigh at a swing of a_angle radians about the x axis: what a walking graph writes into the leg bone.
	ArmorFrames::LegPose Thigh(float a_angle)
	{
		const float          c = std::cos(a_angle);
		const float          s = std::sin(a_angle);
		ArmorFrames::LegPose pose{ { 1.0f, 0.0f, 0.0f, 0.0f, c, -s, 0.0f, s, c } };
		return pose;
	}

	int roots[2];  // stand-ins' 3D, only compared: the first one, and the one placed after it
}

TEST("armor frames: a stand-in in its frame that glides after a friend who walks is placed again, and that counts as a try")
{
	using ArmorFrames::ChooseSwitch;
	using ArmorFrames::Switch;
	using ArmorFrames::SwitchFacts;
	// Live 2026-10-15: in its frame, on the power armor skeleton, all six pieces on, and it glided along standing while the
	// friend walked: its graph played nothing. A fresh stand-in in the next wear walked. Still legs for STILL_LIMIT while
	// they walk: placed again, as a stand-in whose skeleton never came is.
	// Its remedies in place spent (see the test after this one).
	SwitchFacts facts{ .worn = true, .in = true, .rebuilt = true, .still = ArmorFrames::STILL_LIMIT, .sinceIn = 10s, .remedies = ArmorFrames::MAX_REMEDIES };
	CHECK(ChooseSwitch(facts) == Switch::kStill);
	facts.tries = ArmorFrames::MAX_SWITCH_TRIES - 2;  // a try left after this one: replaced
	CHECK(ChooseSwitch(facts) == Switch::kStill);
	// None left after it (live 2026-10-18: the third replacement was shown out of its armor beside an empty frame, while its
	// friend walked in theirs): kept in its frame as it is.
	facts.tries = ArmorFrames::MAX_SWITCH_TRIES - 1;
	CHECK(ChooseSwitch(facts) == Switch::kKept);
	facts.tries = ArmorFrames::MAX_SWITCH_TRIES;
	CHECK(ChooseSwitch(facts) == Switch::kKept);
	// A switch-in that never took is still given up on.
	CHECK(ChooseSwitch({ .worn = true, .ready = true, .held = true, .tries = ArmorFrames::MAX_SWITCH_TRIES, .sinceTried = 1h }) == Switch::kGaveUp);
	// Not yet that long: left as it is.
	facts = { .worn = true, .in = true, .rebuilt = true, .still = ArmorFrames::STILL_LIMIT - 1ms, .sinceIn = 10s };
	CHECK(ChooseSwitch(facts) == Switch::kNone);
	// Out of their armor: out, however still.
	facts.still = 1h;
	facts.worn = false;
	CHECK(ChooseSwitch(facts) == Switch::kOut);
	// Still on its own skeleton: that is the skeleton's wait, whatever its legs do (they are not looked at then).
	facts = { .worn = true, .in = true, .rebuilt = false, .still = 1h, .sinceIn = 1s };
	CHECK(ChooseSwitch(facts) == Switch::kNone);
	facts.sinceIn = ArmorFrames::SKELETON_LIMIT + 1ms;
	CHECK(ChooseSwitch(facts) == Switch::kStuck);
}

TEST("armor frames: a stand-in in its frame with still legs is tried in place before it is replaced, a different remedy each time")
{
	using ArmorFrames::ChooseSwitch;
	using ArmorFrames::Remedy;
	using ArmorFrames::Switch;
	using ArmorFrames::SwitchFacts;
	// Live 2026-10-19: 12 of 13 switch-ins played no walk, and each wear's three replacements went exactly as the first.
	// Every stand-in is first tried in place, once with each remedy, in this order; a try is counted only after.
	SwitchFacts facts{ .worn = true, .in = true, .rebuilt = true, .still = ArmorFrames::STILL_LIMIT, .sinceIn = 10s };
	CHECK(ChooseSwitch(facts) == Switch::kRemedy);
	CHECK(ArmorFrames::RemedyOf(facts.remedies) == Remedy::kSetUp);
	facts.remedies = 1;
	CHECK(ChooseSwitch(facts) == Switch::kRemedy);
	CHECK(ArmorFrames::RemedyOf(facts.remedies) == Remedy::kOwnUpdate);
	facts.remedies = ArmorFrames::MAX_REMEDIES;
	CHECK(ChooseSwitch(facts) == Switch::kStill);
	// Each remedy has the whole STILL_LIMIT again: not before.
	facts = { .worn = true, .in = true, .rebuilt = true, .still = ArmorFrames::STILL_LIMIT - 1ms, .sinceIn = 10s, .remedies = 1 };
	CHECK(ChooseSwitch(facts) == Switch::kNone);
	// On the last try too, before it is kept as it is.
	facts = { .worn = true, .in = true, .rebuilt = true, .still = ArmorFrames::STILL_LIMIT, .sinceIn = 10s, .tries = ArmorFrames::MAX_SWITCH_TRIES - 1 };
	CHECK(ChooseSwitch(facts) == Switch::kRemedy);
	facts.remedies = ArmorFrames::MAX_REMEDIES;
	CHECK(ChooseSwitch(facts) == Switch::kKept);
	// Out of their armor, or still on its own skeleton: no remedy.
	facts = { .worn = false, .in = true, .rebuilt = true, .still = 1h, .sinceIn = 10s };
	CHECK(ChooseSwitch(facts) == Switch::kOut);
	facts = { .worn = true, .in = true, .rebuilt = false, .still = 1h, .sinceIn = 1s };
	CHECK(ChooseSwitch(facts) == Switch::kNone);
}

TEST("armor frames: a stand-in's legs count as still only while its friend walks and nothing in its graph moves them")
{
	ArmorFrames::Stride   stride;
	ArmorFrames::PlayTime now{ 1h };
	const void*           root = &roots[0];
	const auto            frame = [&](std::optional<ArmorFrames::LegPose> a_pose, bool a_walks) {
		now += 16ms;
		return stride.Update(root, a_pose, a_walks, now);
	};
	// A dead graph: the thigh never moves. While the friend stands, nothing is counted.
	for (int i = 0; i < 300; ++i) {
		frame(Thigh(0.0f), false);
	}
	CHECK(stride.Still() == ArmorFrames::Duration{});
	// They walk: the time adds up, frame by frame.
	for (int i = 0; i < 100; ++i) {
		frame(Thigh(0.0f), true);
	}
	CHECK(stride.Still() == 100 * 16ms);
	// A stop in between keeps what was counted; walking on adds to it.
	frame(Thigh(0.0f), false);
	frame(Thigh(0.0f), true);
	CHECK(stride.Still() == 101 * 16ms);
	// A swing of a few degrees is a step: still from there afresh.
	frame(Thigh(0.1f), true);
	CHECK(stride.Still() == ArmorFrames::Duration{});
	// A frame that took the game long counts for no more than LONGEST_FRAME.
	now += 10s;
	frame(Thigh(0.1f), true);
	CHECK(stride.Still() == ArmorFrames::LONGEST_FRAME);
	// No leg to look at (not in its frame, or no such bone): nothing counted, and it starts over.
	frame(std::nullopt, true);
	CHECK(stride.Still() == ArmorFrames::Duration{});
	// A new 3D starts over too.
	for (int i = 0; i < 50; ++i) {
		frame(Thigh(0.0f), true);
	}
	CHECK(stride.Still() > 0ms);
	root = &roots[1];
	CHECK(frame(Thigh(0.0f), true) == ArmorFrames::Duration{});
}

TEST("armor frames: a stand-in that walks in its frame is never placed again for still legs, however long the walk")
{
	using ArmorFrames::ChooseSwitch;
	using ArmorFrames::Switch;
	// A walk cycle swings the thigh some 25 degrees each way about once a second, the friend walking all the while,
	// sampled every frame of a fast game and of slow ones (three frames to a step). Never STILL_LIMIT.
	for (const auto step : { 16ms, 100ms, 333ms }) {
		ArmorFrames::Stride   stride;
		ArmorFrames::PlayTime now{ 1h };
		bool                  replaced = false;
		for (auto t = 0ms; t < 60s; t += step) {
			now += step;
			const float swing = 0.44f * std::sin(6.2832f * static_cast<float>(t.count()) / 1000.0f);
			const auto  still = stride.Update(&roots[0], Thigh(swing), true, now);
			replaced |= ChooseSwitch({ .worn = true, .in = true, .rebuilt = true, .still = still, .sinceIn = 10s }) != Switch::kNone;
		}
		CHECK(!replaced);
	}
	// A dead one, sampled the same ways, is placed again a little after STILL_LIMIT of walking (a frame counting for
	// no more than LONGEST_FRAME, as the play clock has it).
	for (const auto step : { 16ms, 100ms, 250ms }) {
		ArmorFrames::Stride   stride;
		ArmorFrames::PlayTime now{ 1h };
		auto                  walked = 0ms;
		for (;;) {
			now += step;
			const auto still = stride.Update(&roots[0], Thigh(0.2f), true, now);
			if (ChooseSwitch({ .worn = true, .in = true, .rebuilt = true, .still = still, .sinceIn = 10s }) != Switch::kNone) {
				break;
			}
			walked += step;
			REQUIRE(walked < 10s);
		}
		CHECK(walked >= ArmorFrames::STILL_LIMIT - step);
		CHECK(walked <= ArmorFrames::STILL_LIMIT + step);
	}
}

TEST("armor frames: still legs count only while the friend walks on their own legs, and only when we can see the stand-in")
{
	using ArmorFrames::InSight;
	using ArmorFrames::Walks;
	// Their own moveMode: walking 0x66, running 0xA6, sprinting 0x1A6, standing 0.
	CHECK(Walks(0x66, Protocol::kNone, 92.0f));
	CHECK(Walks(0xA6, Protocol::kRunning, 373.0f));
	CHECK(Walks(0x1A6, Protocol::kRunning, 500.0f));
	CHECK(!Walks(0x00, Protocol::kNone, 0.0f));
	// Carried by a lift or a vehicle: covering ground, legs still, none of the bits.
	CHECK(!Walks(0x00, Protocol::kNone, 300.0f));
	// Off the ground: no stride either.
	CHECK(!Walks(0x66, Protocol::kInAir, 300.0f));
	CHECK(!Walks(0xA6, static_cast<std::uint8_t>(Protocol::kInAir | Protocol::kJumping), 373.0f));
	// Turning on the spot, or all but standing: the graph is not told to move.
	CHECK(!Walks(0x66, Protocol::kNone, 10.0f));

	// Our heading 0 looks along +y.
	CHECK(InSight(0.0f, 1000.0f, 0.0f));
	CHECK(InSight(500.0f, 1000.0f, 0.0f));   // some 27 degrees off
	CHECK(!InSight(1000.0f, 100.0f, 0.0f));  // off to the side
	CHECK(!InSight(0.0f, -1000.0f, 0.0f));   // behind us
	CHECK(InSight(0.0f, -100.0f, 0.0f));     // right behind us, but this near
	CHECK(!InSight(0.0f, 3500.0f, 0.0f));    // too far
	// Facing east (a quarter turn clockwise), one to the east is ahead.
	CHECK(InSight(1000.0f, 0.0f, 1.5708f));
	CHECK(!InSight(-1000.0f, 0.0f, 1.5708f));
}

TEST("armor frames: the switch-in waits until what was last put on the stand-in has settled on its body")
{
	using ArmorFrames::ChoosePick;
	using ArmorFrames::ChooseSwitch;
	using ArmorFrames::Pick;
	using ArmorFrames::PickFacts;
	using ArmorFrames::Switch;
	using ArmorFrames::SwitchFacts;
	// 0.8.7-rc1 soak: their status and their gear came together as they got in, the stand-in was dressed in the gear and
	// switched in 70 ms later, with the biped of that dress still loading, and every such stand-in glided in its frame.
	SwitchFacts facts{ .worn = true, .ready = true, .held = true, .sinceDressed = 70ms };
	CHECK(ChooseSwitch(facts) == Switch::kNone);
	facts.sinceDressed = ArmorFrames::DRESS_SETTLE - 1ms;
	CHECK(ChooseSwitch(facts) == Switch::kNone);
	facts.sinceDressed = ArmorFrames::DRESS_SETTLE;
	CHECK(ChooseSwitch(facts) == Switch::kIn);
	// Never dressed at all (their gear not told yet): nothing to settle.
	CHECK(ChooseSwitch({ .worn = true, .ready = true, .held = true }) == Switch::kIn);
	// A retry after one that did not take settles too.
	facts = { .worn = true, .ready = true, .held = true, .sinceDressed = 10ms, .tries = 1, .sinceTried = ArmorFrames::SWITCH_RETRY };
	CHECK(ChooseSwitch(facts) == Switch::kNone);
	facts.sinceDressed = ArmorFrames::DRESS_SETTLE;
	CHECK(ChooseSwitch(facts) == Switch::kIn);
	// In it already: the settle is nothing to it.
	CHECK(ChooseSwitch({ .worn = true, .in = true, .rebuilt = true, .sinceIn = 1s, .sinceDressed = 10ms }) == Switch::kNone);
	CHECK(ChooseSwitch({ .in = true, .sinceDressed = 10ms }) == Switch::kOut);

	// The item put on so that the engine picks their frame item: the pick is theirs the moment it is on, the switch-in
	// still waits for it to settle (the fresh stand-ins of the soak went in 40 to 70 ms after it, and one in four glided).
	PickFacts pick{ .named = true, .picksTheirs = true, .key = true, .keyOn = true, .sincePut = 41ms };
	CHECK(ChoosePick(pick) == Pick::kWait);
	pick.sincePut = ArmorFrames::DRESS_SETTLE;
	CHECK(ChoosePick(pick) == Pick::kSwitch);
	// Theirs without anything put on (it wears their vault suit): at once.
	CHECK(ChoosePick({ .named = true, .picksTheirs = true }) == Pick::kSwitch);
	// Not named yet, in the clothes it was dressed in: the dress settles through the switch facts, not here.
	CHECK(ChoosePick({ .named = false, .dressed = true }) == Pick::kSwitch);
}

TEST("armor frames: our copy of a frame another game runs is theirs wherever our player left it, however it got where it stands")
{
	using ArmorFrames::ChooseOriginal;
	using ArmorFrames::ChooseParkedOriginal;
	using ArmorFrames::Original;
	using ArmorFrames::OriginalFacts;
	constexpr std::uint32_t museum = 0x0003F2B4;
	constexpr std::uint32_t base = 0x0019DED9;
	// Synced (NpcSync moved ours to where their game has it): the move is no use of it by our player, and our player's
	// last use of it before they took it is none either. Hidden while they wear it where they got in.
	OriginalFacts facts{ .found = true, .base = base, .frame = true, .loaded = true, .ours = true, .fromStart = 5000.0f, .fromSpot = 12.0f, .synced = true };
	CHECK(ChooseOriginal(museum, base, facts) == Original::kHide);
	facts.synced = false;
	CHECK(ChooseOriginal(museum, base, facts) == Original::kOurs);
	facts.ours = false;
	CHECK(ChooseOriginal(museum, base, facts) == Original::kUsed);
	// Hidden wherever it stands: where their game has it, or on its way there (their state moves ours once in view).
	// Where their stand-in was as their status came is no measure of that for one who joined mid-wear (they got in
	// somewhere in their own world, and their stand-in appeared where they are now), and ours stood shown beside them
	// for the whole wear. Not written off for the wear: looked at again, loaded or not.
	facts = { .found = true, .base = base, .frame = true, .loaded = true, .moved = true, .fromStart = 5000.0f, .fromSpot = 3000.0f, .synced = true };
	CHECK(ChooseOriginal(museum, base, facts) == Original::kHide);
	facts.fromSpot.reset();  // in another place altogether, as yet
	CHECK(ChooseOriginal(museum, base, facts) == Original::kHide);
	facts.loaded = false;
	CHECK(ChooseOriginal(museum, base, facts) == Original::kNotLoaded);
	facts.loaded = true;
	facts.fromSpot = 12.0f;
	CHECK(ChooseOriginal(museum, base, facts) == Original::kHide);
	// One nobody else runs is hidden only where they got in, as ever.
	facts.synced = false;
	facts.moved = false;
	facts.fromStart = 0.0f;
	facts.fromSpot = 3000.0f;
	CHECK(ChooseOriginal(museum, base, facts) == Original::kElsewhere);
	// Someone here in it, or a quest of ours holding it, keeps it shown as ever.
	facts.inUse = true;
	CHECK(ChooseOriginal(museum, base, facts) == Original::kInUse);
	facts.inUse = false;
	facts.quest = true;
	CHECK(ChooseOriginal(museum, base, facts) == Original::kQuest);
	// Parked: ours stands for theirs once it is there, is hidden while theirs stands elsewhere.
	facts = { .found = true, .base = base, .frame = true, .loaded = true, .ours = true, .fromStart = 5000.0f, .fromSpot = 12.0f, .synced = true };
	CHECK(ChooseParkedOriginal(museum, base, facts) == Original::kThere);
	facts.fromSpot = 3000.0f;
	CHECK(ChooseParkedOriginal(museum, base, facts) == Original::kHide);
	facts.synced = false;
	CHECK(ChooseParkedOriginal(museum, base, facts) == Original::kOurs);
	facts.ours = false;
	CHECK(ChooseParkedOriginal(museum, base, facts) == Original::kUsed);
}
