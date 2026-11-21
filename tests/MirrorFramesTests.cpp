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
