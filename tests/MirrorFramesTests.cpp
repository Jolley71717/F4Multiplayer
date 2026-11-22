#include "Test.h"
#include "game/MirrorFrames.h"

using namespace std::chrono_literals;
using MirrorFrames::Choose;
using MirrorFrames::Count;
using MirrorFrames::Do;
using MirrorFrames::Facts;
using MirrorFrames::Gained;

namespace
{
	constexpr std::uint32_t T45 = 0x0004A9C1;
	constexpr std::uint32_t OTHER = 0x0019DED9;

	// A mirror ready to be switched in, out of any frame, its owner's copy in a T-45 frame.
	constexpr Facts Ready()
	{
		Facts f;
		f.ownerFrame = T45;
		f.ready = true;
		return f;
	}

	// The same, in the copy placed for it, all the way in.
	constexpr Facts InCopy()
	{
		Facts f = Ready();
		f.inFrame = true;
		f.inCopy = true;
		f.state = MirrorFrames::STATE_IN;
		f.copyPlaced = true;
		f.copyLoaded = true;
		f.copyBase = T45;
		return f;
	}
}

TEST("mirror frames: a friend's companion in power armor is put into a copy of the frame here, once the copy loads")
{
	// The player report: Piper in a suit in the host's game, out of one in the friend's.
	auto f = Ready();
	CHECK(Choose(f) == Do::kPlaceCopy);
	f.copyPlaced = true;
	f.copyBase = T45;
	f.sincePlaced = 100ms;
	CHECK(Choose(f) == Do::kNothing);  // its 3D on its way
	f.copyLoaded = true;
	CHECK(Choose(f) == Do::kGetIn);
	CHECK(Choose(InCopy()) == Do::kNothing);  // in, as the owner's copy is
}

TEST("mirror frames: out of the copy as the owner's copy gets out, or into another suit; never while still getting in")
{
	auto f = InCopy();
	f.ownerFrame = 0;
	CHECK(Choose(f) == Do::kGetOut);
	f.ownerFrame = OTHER;
	CHECK(Choose(f) == Do::kGetOut);  // then a copy of the new one (below)
	f.state = 1;                      // getting in: the engine's exit begins only from in
	CHECK(Choose(f) == Do::kNothing);
	// An exit begun and waiting on the engine (its preload): carried on, even with the owner's copy back in that suit.
	for (const std::uint8_t state : { 3, 4, 5 }) {
		f = InCopy();
		f.state = state;
		CHECK(Choose(f) == Do::kGetOut);
		f.ownerFrame = 0;
		CHECK(Choose(f) == Do::kGetOut);
	}
	f.state = MirrorFrames::STATE_NONE;  // not a state seen in a frame; nothing to carry on
	f.ownerFrame = T45;
	CHECK(Choose(f) == Do::kNothing);
	// Out of it, a copy of the old suit still placed: it goes, then a copy of the new one.
	f = Ready();
	f.ownerFrame = OTHER;
	f.copyPlaced = true;
	f.copyLoaded = true;
	f.copyBase = T45;
	CHECK(Choose(f) == Do::kDropCopy);
	f.copyPlaced = false;
	f.copyBase = 0;
	CHECK(Choose(f) == Do::kPlaceCopy);
}

TEST("mirror frames: a frame of its own here is never ours to take it out of")
{
	auto f = Ready();
	f.inFrame = true;
	f.state = MirrorFrames::STATE_IN;
	CHECK(Choose(f) == Do::kNothing);
	f.ownerFrame = 0;
	CHECK(Choose(f) == Do::kNothing);
}

TEST("mirror frames: a copy nobody gets into is not left standing")
{
	// The owner's copy got out before ours went in.
	auto f = Ready();
	f.ownerFrame = 0;
	f.copyPlaced = true;
	f.copyBase = T45;
	CHECK(Choose(f) == Do::kDropCopy);
	// The mirror is not ready any more (unloaded, its process dropped, dead): the copy goes, placed again once it is.
	f = Ready();
	f.ready = false;
	CHECK(Choose(f) == Do::kNothing);
	f.copyPlaced = true;
	f.copyBase = T45;
	CHECK(Choose(f) == Do::kDropCopy);
	// Nothing at all to do for a mirror out of one whose owner's copy is out too.
	CHECK(Choose(Facts{}) == Do::kNothing);
}

TEST("mirror frames: the switch-in is given up for that wear after a copy that never loads, or tries that do not take")
{
	auto f = Ready();
	f.copyPlaced = true;
	f.copyBase = T45;
	f.sincePlaced = MirrorFrames::COPY_LIMIT + 1s;
	CHECK(Choose(f) == Do::kGiveUp);
	f.copyLoaded = true;
	f.sincePlaced = 1s;
	f.tries = 1;
	f.sinceTried = 500ms;
	CHECK(Choose(f) == Do::kNothing);  // tried again a moment later
	f.sinceTried = MirrorFrames::RETRY;
	CHECK(Choose(f) == Do::kGetIn);
	f.tries = MirrorFrames::MAX_TRIES;
	CHECK(Choose(f) == Do::kGiveUp);
	// Given up: shown out of one until the owner's copy is in another (or out and in again).
	f = Ready();
	f.gaveUp = true;
	CHECK(Choose(f) == Do::kNothing);
}

TEST("mirror frames: what the switch-in gave and the exit left is given back, nothing of its own")
{
	const std::vector<Count> before{ { 0x100, 1 }, { 0x200, 2 } };
	// The pieces went back into the frame: nothing to give back.
	CHECK(Gained(before, before).empty());
	CHECK(Gained(before, { { 0x200, 2 }, { 0x100, 1 } }).empty());
	// One piece stayed on it, and one more of a piece it had.
	const auto gained = Gained(before, { { 0x100, 1 }, { 0x200, 3 }, { 0x300, 1 } });
	CHECK(gained.size() == 2);
	CHECK(std::ranges::contains(gained, Count{ 0x200, 1 }));
	CHECK(std::ranges::contains(gained, Count{ 0x300, 1 }));
	// Fewer than before (a piece of its own went into the frame) is never made up for here.
	CHECK(Gained(before, { { 0x100, 0 } }).empty());
	// Stacks of one item are counted together.
	const std::vector<Count> one{ { 0x100, 1 } };
	const std::vector<Count> twoStacks{ { 0x100, 1 }, { 0x100, 1 } };
	CHECK(Gained(one, twoStacks) == one);
}

TEST("mirror frames: a dead, dying or knocked down mirror is never taken out of its copy, whatever its owner's copy does")
{
	using MirrorFrames::Down;
	CHECK(Down(true, 0, 0));
	CHECK(Down(false, MirrorFrames::LIFE_DYING, 0));
	CHECK(Down(false, MirrorFrames::LIFE_DEAD, 0));
	CHECK(Down(false, 0, 1));   // ragdolled
	CHECK(!Down(false, 0, 0));  // alive, on its feet
	CHECK(!Down(false, 8, 0));  // bleeding out on its feet: an exit as any

	Facts f = InCopy();
	f.down = true;
	f.ready = false;  // Ready asks it to be alive
	f.ownerFrame = 0;  // its owner's copy out (or its gear report gone with its death)
	CHECK(Choose(f) == Do::kNothing);
	f.ownerFrame = OTHER;
	CHECK(Choose(f) == Do::kNothing);
	for (std::uint8_t s = 3; s <= MirrorFrames::STATE_LAST_EXIT; ++s) {
		f.state = s;  // an exit begun as it died is not carried on over the body
		CHECK(Choose(f) == Do::kNothing);
	}
	// Up again (a knock-down over): out as before.
	f = InCopy();
	f.ownerFrame = 0;
	CHECK(Choose(f) == Do::kGetOut);
	// Down and out of any frame: its copy goes as any copy nobody is in.
	f = Ready();
	f.down = true;
	f.ready = false;
	f.copyPlaced = true;
	f.copyBase = T45;
	CHECK(Choose(f) == Do::kDropCopy);
}

TEST("mirror frames: as mirroring stops, a body or one the exit did not take out stays in its copy, and the copy is the game's")
{
	using MirrorFrames::AfterExit;
	using MirrorFrames::ChooseLeaving;
	using MirrorFrames::Leaving;
	CHECK(ChooseLeaving(false, false) == Leaving::kDrop);
	CHECK(ChooseLeaving(false, true) == Leaving::kDrop);  // a body out of any frame: nobody is in the copy
	CHECK(ChooseLeaving(true, false) == Leaving::kExit);
	CHECK(ChooseLeaving(true, true) == Leaving::kRelease);  // never exited: left in it, the copy left to the game
	CHECK(AfterExit(false) == Leaving::kDrop);
	CHECK(AfterExit(true) == Leaving::kRelease);  // refused, failed, or the preload still loading: never orphaned
}

namespace
{
	using MirrorFrames::ChooseLeaving;
	using MirrorFrames::HOLD;
	using MirrorFrames::HoldWhileSilent;
	using MirrorFrames::Leaving;

	// A mirror in its copy, on its feet, its owner's copy gone silent: held, still a mirror, at the start of each ending below.
	constexpr bool HeldAt(MirrorFrames::Duration a_silentFor)
	{
		return HoldWhileSilent(true, a_silentFor);
	}
}

TEST("mirror frames: a mirror in its copy is held there while its owner's copy is silent, and only then")
{
	CHECK(HeldAt(MirrorFrames::Duration{}));
	CHECK(HeldAt(3s));  // NpcPlace's LOST_AFTER: where it was let go (and taken out) before
	CHECK(HeldAt(HOLD));
	CHECK(!HeldAt(HOLD + 1ms));
	CHECK(!HoldWhileSilent(false, 1s));  // out of any copy: let go to its own AI as any silent mirror
}

TEST("mirror frames: a hold that runs out takes it out with the engine's exit, as a mirror, never leaves the copy to the game")
{
	// The common case: a companion that followed its player through a door, ran far, or whose owner's game hung. Let go when
	// the hold runs out, it leaves while still a mirror (StopMirroring calls Leave first), so the exit takes it.
	CHECK(!HeldAt(HOLD + 1s));
	CHECK(ChooseLeaving(true, false, true) == Leaving::kExit);
	CHECK(MirrorFrames::AfterExit(false) == Leaving::kDrop);  // out: the copy goes, nothing of it in the save
	// The 0.21 hold let go to its own AI first: the exit refused an NPC our game ran, and the copy was left to the game with
	// it alive in it (a frame for good in the friend's save). Not a mirror any more, that is still the only safe way.
	CHECK(ChooseLeaving(true, false, false) == Leaving::kRelease);
}

TEST("mirror frames: its death within the hold leaves the body in its suit, the copy the game's")
{
	CHECK(HeldAt(3044ms));  // rig, 2026-11-21: the death came 44 ms past LOST_AFTER
	CHECK(HeldAt(3080ms));  // ...and the server's let-go of it 3 ms before it: held as well (NpcSync's owner-0 hold)
	CHECK(ChooseLeaving(true, true, true) == Leaving::kRelease);
	// A body the exit is never run on, whatever its owner's copy does meanwhile.
	auto f = InCopy();
	f.down = true;
	f.ownerFrame = 0;
	CHECK(Choose(f) == Do::kNothing);
}

TEST("mirror frames: its owner's states back within the hold, it is mirrored on in its copy as before")
{
	// Never let go: the copy and what was wanted are kept, and it follows its owner's copy from there.
	CHECK(HeldAt(5s));
	CHECK(Choose(InCopy()) == Do::kNothing);  // its owner's copy still in the suit
	auto f = InCopy();
	f.ownerFrame = 0;  // got out meanwhile
	CHECK(Choose(f) == Do::kGetOut);
}

TEST("mirror frames: a copy is buried only while its handle names it; one the engine let go of is forgotten, its record dropped")
{
	using MirrorFrames::HowToLetGo;
	using MirrorFrames::LetGo;
	constexpr std::uint32_t COPY = 0xFF0012B4;
	// Nothing placed: nothing to do.
	CHECK(HowToLetGo(0, 0) == LetGo::kNothing);
	CHECK(HowToLetGo(0, COPY) == LetGo::kNothing);
	// Out of it with its cell loaded: still there under its ID, buried as every copy.
	CHECK(HowToLetGo(COPY, COPY) == LetGo::kDiscard);
	// The exit run while its cell is not loaded here (the friend walked off through a door, a save, the session's end):
	// the demote let the engine unload it at once, its form freed (the reference reads ID 0) and its record kept under the
	// cell (rig, 2026-11-22: it stood in the friend's save). Never buried through the dead handle: forgotten, its record
	// dropped.
	CHECK(HowToLetGo(COPY, 0) == LetGo::kForget);
	// Its handle naming another form now (its ID handed out again): never buried, the other form is not ours.
	CHECK(HowToLetGo(COPY, 0xFF0012B5) == LetGo::kForget);
}

TEST("mirror frames: the hold ended by its owner leaving, its cell unloading, a save or the session's end takes it out as a mirror")
{
	// Each is a stop as any (UpdateMirrors' owner-gone and unloaded stops, Session's LeaveAll before a save or a load,
	// NpcSync::Reset's LeaveAll before its mirrors are let go): left while still a mirror, on its feet, taken out.
	for (const auto silentFor : { 0s, 4s, 9s }) {
		CHECK(HeldAt(silentFor));
		CHECK(ChooseLeaving(true, false, true) == Leaving::kExit);
	}
	// Knocked down at that moment (alive, ragdolled): never exited under a ragdoll; the copy is left to the game with it
	// (counted apart from the bodies: releasedKnocked).
	CHECK(ChooseLeaving(true, true, true) == Leaving::kRelease);
	// Out of its copy by then (its exit carried on): only the copy goes.
	CHECK(ChooseLeaving(false, false, true) == Leaving::kDrop);
	CHECK(ChooseLeaving(false, false, false) == Leaving::kDrop);
}
