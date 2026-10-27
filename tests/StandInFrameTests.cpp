// The frame a friend's stand-in goes into (ArmorFrames::ChooseWornBase, as RemotePlayers' WornFrame uses it), from the
// first status a friend's game sends: one loaded with them already in power armor sends "in power armor" from its first
// status on, never a change from out to in, and the stand-in placed for it then must still end up in a frame.

#include "Test.h"
#include "game/ArmorFrames.h"

#include <chrono>
#include <set>

using namespace std::chrono_literals;
using ArmorFrames::ChooseWornBase;
using ArmorFrames::WornBase;

namespace
{
	using Clock = std::chrono::steady_clock;

	constexpr std::uint32_t VANILLA_FRAME = 0x0017F6E4;  // a frame every game has
	constexpr std::uint32_t PLUGIN_FRAME = 0x0A001F2C;   // a frame from a plugin this game lacks

	// One friend as a receiving game keeps them (RemotePlayers): what their statuses said (Hear), how long the frame they
	// told has been none here (UnknownFrame, updated each frame), and the frame their stand-in goes into from that.
	struct Friend
	{
		std::set<std::uint32_t>   framesHere{ VANILLA_FRAME, ArmorFrames::STAND_IN_FRAME };
		ArmorFrames::Heard        heard;
		ArmorFrames::UnknownFrame unknown;

		void Status(bool a_on, std::uint32_t a_frame) { heard = ArmorFrames::Hear(heard, a_on, a_frame, 0); }

		bool ToldHere() const { return heard.on && framesHere.contains(heard.frame); }

		// One frame of the receiving game: the base its stand-in goes into (0: none).
		std::uint32_t Frame(Clock::time_point a_now)
		{
			unknown.Update(heard.on, ToldHere(), a_now);
			switch (ChooseWornBase(heard.on, ToldHere(), unknown.Overdue(a_now), framesHere.contains(ArmorFrames::STAND_IN_FRAME))) {
			case WornBase::kTold:
				return heard.frame;
			case WornBase::kStandIn:
				return ArmorFrames::STAND_IN_FRAME;
			case WornBase::kNone:
				break;
			}
			return 0;
		}
	};

	// A fresh stand-in, placed while they are in a frame here: its copy goes, loads and is held under it, and it is
	// switched in (ChooseCopy, ChooseSwitch), with nothing put on it for a while (it was dressed on its first frame).
	bool GoesIn(bool a_worn)
	{
		if (ArmorFrames::ChooseCopy({ .worn = a_worn }) != ArmorFrames::Copy::kPlace) {
			return false;
		}
		const ArmorFrames::CopyFacts held{ .worn = a_worn, .exists = true, .loaded = true, .samePlace = true, .sincePlaced = 1s };
		if (ArmorFrames::ChooseCopy(held) != ArmorFrames::Copy::kHold) {
			return false;
		}
		const ArmorFrames::SwitchFacts facts{ .worn = a_worn, .ready = true, .held = true, .sinceDressed = 1s };
		return ArmorFrames::ChooseSwitch(facts) == ArmorFrames::Switch::kIn;
	}
}

TEST("stand-in frame: the frame told, a frame here, is the one; none out of power armor")
{
	CHECK(ChooseWornBase(true, true, false, true) == WornBase::kTold);
	CHECK(ChooseWornBase(true, true, true, true) == WornBase::kTold);
	CHECK(ChooseWornBase(true, true, false, false) == WornBase::kTold);
	CHECK(ChooseWornBase(false, true, true, true) == WornBase::kNone);
	CHECK(ChooseWornBase(false, false, true, true) == WornBase::kNone);
	// Theirs unknown here: none for the first second of the wear, then the frame every game has, if this one has it.
	CHECK(ChooseWornBase(true, false, false, true) == WornBase::kNone);
	CHECK(ChooseWornBase(true, false, true, true) == WornBase::kStandIn);
	CHECK(ChooseWornBase(true, false, true, false) == WornBase::kNone);
}

TEST("stand-in frame: the frame is overdue a second into a wear with it unknown, and starts over once it is known or they are out")
{
	ArmorFrames::UnknownFrame unknown;
	const auto t0 = Clock::now();
	CHECK(!unknown.Overdue(t0));
	unknown.Update(true, false, t0);
	CHECK(!unknown.Overdue(t0));
	unknown.Update(true, false, t0 + 900ms);
	CHECK(!unknown.Overdue(t0 + 900ms));
	CHECK(unknown.Update(true, false, t0 + 1s));  // the one log line of the wear
	CHECK(unknown.Overdue(t0 + 1s));
	unknown.Update(true, false, t0 + 5s);
	CHECK(unknown.Overdue(t0 + 5s));
	// Named: not overdue, and a second of not knowing again before it is.
	unknown.Update(true, true, t0 + 6s);
	CHECK(!unknown.Overdue(t0 + 6s));
	unknown.Update(true, false, t0 + 7s);
	CHECK(!unknown.Overdue(t0 + 7s + 500ms));
	unknown.Update(true, false, t0 + 8s);
	CHECK(unknown.Overdue(t0 + 8s));
	// Out: nothing overdue, and the next wear waits its second again.
	unknown.Update(false, false, t0 + 9s);
	CHECK(!unknown.Overdue(t0 + 9s));
	unknown.Update(true, false, t0 + 10s);
	CHECK(!unknown.Overdue(t0 + 10s));
}

TEST("stand-in frame: a friend whose first status already says power armor, their frame named, has a stand-in that goes into it at once, with no change from out to in")
{
	// Their game was loaded with them in the armor: its first status in the session says so and names the frame the
	// engine keeps as worn (ArmorFrames::WornSeat). We never heard them out of it.
	Friend bob;
	const auto t0 = Clock::now();
	bob.Status(true, VANILLA_FRAME);
	CHECK(bob.Frame(t0) == VANILLA_FRAME);
	CHECK(GoesIn(bob.Frame(t0 + 100ms) != 0));
	// The second status says the same: nothing changes.
	bob.Status(true, VANILLA_FRAME);
	CHECK(bob.Frame(t0 + 2s) == VANILLA_FRAME);
}

TEST("stand-in frame: a friend in power armor whose frame is none, or one this game lacks, is shown in a frame every game has, not out of their suit")
{
	const auto t0 = Clock::now();
	for (const auto told : { 0u, PLUGIN_FRAME }) {
		Friend bob;
		bob.Status(true, told);
		// The first second: the next status of a wear names its frame, and nothing is placed meanwhile.
		CHECK(bob.Frame(t0) == 0u);
		CHECK(bob.Frame(t0 + 500ms) == 0u);
		// Then the frame every game has. Before, their stand-in wore the clothes under their suit until they got out and
		// in again.
		CHECK(bob.Frame(t0 + 1s) == ArmorFrames::STAND_IN_FRAME);
		CHECK(GoesIn(bob.Frame(t0 + 1100ms) != 0));
		CHECK(bob.Frame(t0 + 30s) == ArmorFrames::STAND_IN_FRAME);
		// Out: none, and the copy left where they got out goes as any does.
		bob.Status(false, 0);
		CHECK(bob.Frame(t0 + 31s) == 0u);
		CHECK(ArmorFrames::ChooseCopy({ .worn = false, .exists = true, .loaded = true, .sinceOut = ArmorFrames::KEEP_AFTER_OUT }) == ArmorFrames::Copy::kDelete);
	}
}

TEST("stand-in frame: their own frame named after the stand-in went into the stand-in frame takes over as any change of frame does")
{
	Friend bob;
	const auto t0 = Clock::now();
	bob.Status(true, 0);
	CHECK(bob.Frame(t0) == 0u);
	CHECK(bob.Frame(t0 + 1s) == ArmorFrames::STAND_IN_FRAME);
	bob.Status(true, VANILLA_FRAME);
	CHECK(bob.Frame(t0 + 2s) == VANILLA_FRAME);
	// The copy of the stand-in frame is another base: placed again as theirs while the stand-in is not in it yet; once it
	// is, the engine has it, and it stays in its frame (worn, it shows only the frame item and their pieces).
	const ArmorFrames::CopyFacts other{ .worn = true, .exists = true, .loaded = true, .samePlace = true, .sincePlaced = 1s, .sameBase = false };
	CHECK(ArmorFrames::ChooseCopy(other) == ArmorFrames::Copy::kReplace);
	auto engaged = other;
	engaged.engaged = true;
	CHECK(ArmorFrames::ChooseCopy(engaged) == ArmorFrames::Copy::kNone);
	CHECK(ArmorFrames::ChooseSwitch({ .worn = true, .in = true, .rebuilt = true }) == ArmorFrames::Switch::kNone);
}

TEST("stand-in frame: a frame from a plugin this game lacks, told while out of the armor and kept into the next wear, still gets the stand-in frame")
{
	// Hear keeps the frame they told across their getting out; the next wear's first status names none.
	Friend bob;
	const auto t0 = Clock::now();
	bob.Status(true, PLUGIN_FRAME);
	bob.Status(false, 0);
	CHECK(bob.heard.frame == PLUGIN_FRAME);
	bob.Status(true, 0);
	CHECK(bob.Frame(t0) == 0u);
	CHECK(bob.Frame(t0 + 1s) == ArmorFrames::STAND_IN_FRAME);
	// A game without that frame either (a mod made it no frame): none, as before.
	Friend bare;
	bare.framesHere = { VANILLA_FRAME };
	bare.Status(true, PLUGIN_FRAME);
	CHECK(bare.Frame(t0) == 0u);
	CHECK(bare.Frame(t0 + 5s) == 0u);
}
