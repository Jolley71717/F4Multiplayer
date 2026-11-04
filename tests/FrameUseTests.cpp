// Why the game turns our player away from a power armor frame with "You cannot use this at this time." (FrameUse::Judge,
// as Equipment::FrameUseOf reads the engine's answers): each test is a frame as the engine has it when the player
// presses E on it, and names what the player sees.

#include "Test.h"

#include "game/FrameUse.h"

#include <cmath>
#include <limits>
#include <string_view>

namespace
{
	using FrameUse::Facts;
	using FrameUse::Verdict;

	constexpr std::uint32_t PLAYER = 0x00000014;
	constexpr std::uint32_t RAIDER = 0x0002A001;
	constexpr float         UP_MIN = 0.9848f;  // the engine's InteractionUtils::QUprightAngleCosine: cos of fFurnitureUprightAngle, 10 degrees

	// A frame standing upright with its one seat free: the player gets in.
	Facts Standing()
	{
		return { .up = 1.0f, .upMin = UP_MIN, .markers = 1, .used = 0, .free = true };
	}
}

TEST("frame use: a frame standing upright with nobody in it lets the player in")
{
	CHECK(FrameUse::Judge(Standing()) == Verdict::kFree);
}

TEST("frame use: a frame a raider is in turns the player away without a word, as the game does")
{
	auto frame = Standing();
	frame.used = 1;
	frame.user = RAIDER;
	frame.userIn = true;
	frame.othersThanPlayer = true;
	frame.free = false;
	CHECK(FrameUse::Judge(frame) == Verdict::kOtherUser);
}

TEST("frame use: a frame knocked onto its back says the player cannot use it at this time")
{
	auto frame = Standing();
	frame.up = std::cos(1.4f);  // lying nearly flat
	frame.free = false;
	CHECK(!FrameUse::Upright(frame));
	CHECK(FrameUse::Judge(frame) == Verdict::kTilted);
}

TEST("frame use: a frame tipped a little, within the 10 degrees the game allows, is still upright")
{
	auto frame = Standing();
	frame.up = std::cos(0.1f);  // under 6 degrees
	CHECK(FrameUse::Upright(frame));
	CHECK(FrameUse::Judge(frame) == Verdict::kFree);
}

TEST("frame use: a frame leaning 15 degrees, on a slope, say, is past what the game allows and is stood up")
{
	auto frame = Standing();
	frame.up = std::cos(15.0f * 3.14159265f / 180.0f);
	frame.free = false;
	CHECK(FrameUse::Judge(frame) == Verdict::kTilted);
	const auto mend = FrameUse::ChooseMend(frame);
	CHECK(mend.standUp);
	CHECK(!mend.freeSeat);
}

TEST("frame use: a frame whose angle is not a number is not upright to the game either, and is stood up")
{
	auto frame = Standing();
	frame.up = std::cos(std::numeric_limits<float>::quiet_NaN());
	frame.free = false;
	CHECK(!FrameUse::Upright(frame));
	CHECK(FrameUse::Judge(frame) == Verdict::kTilted);
	CHECK(FrameUse::ChooseMend(frame).standUp);
}

TEST("frame use: a frame whose seat the game still has as our player's, with the player out of it, is a stale use")
{
	auto frame = Standing();
	frame.used = 1;
	frame.user = PLAYER;
	frame.userIsPlayer = true;
	frame.userIn = false;
	frame.free = false;
	CHECK(FrameUse::Judge(frame) == Verdict::kStaleUse);
}

TEST("frame use: the frame the player is climbing into is in use, not stale")
{
	auto frame = Standing();
	frame.used = 1;
	frame.user = PLAYER;
	frame.userIsPlayer = true;
	frame.userIn = true;
	frame.free = false;
	CHECK(FrameUse::Judge(frame) == Verdict::kInUse);
}

TEST("frame use: the game's own answer wins: a free seat lets the player in whatever else is read")
{
	auto frame = Standing();
	frame.up = 0.0f;  // read from an angle the engine does not go by (an actor's, say)
	frame.used = 1;
	frame.user = PLAYER;
	CHECK(FrameUse::Judge(frame) == Verdict::kFree);
}

TEST("frame use: furniture with no seat enabled is told apart")
{
	auto frame = Standing();
	frame.markers = 0;
	frame.free = false;
	CHECK(FrameUse::Judge(frame) == Verdict::kNoMarker);
}

TEST("frame use: every verdict has a name for the log")
{
	for (const auto verdict : { Verdict::kFree, Verdict::kOtherUser, Verdict::kTilted, Verdict::kStaleUse, Verdict::kInUse, Verdict::kNoMarker }) {
		CHECK(std::string_view{ FrameUse::Name(verdict) } != "-");
	}
}

TEST("frame use: a seat the game still has as our player's, with the player out of everything, is given back")
{
	auto frame = Standing();
	frame.used = 1;
	frame.user = PLAYER;
	frame.userMarker = 0;
	frame.userIsPlayer = true;
	frame.free = false;
	const auto mend = FrameUse::ChooseMend(frame);
	CHECK(mend.freeSeat);
	CHECK(!mend.standUp);
}

TEST("frame use: the seat is left alone while the player is still on their way into or out of furniture")
{
	auto frame = Standing();
	frame.used = 1;
	frame.user = PLAYER;
	frame.userIsPlayer = true;
	frame.playerBusy = true;
	frame.free = false;
	CHECK(!FrameUse::ChooseMend(frame).Any());
}

TEST("frame use: a seat the game has as someone else's is never taken from them")
{
	auto frame = Standing();
	frame.used = 1;
	frame.user = RAIDER;
	frame.free = false;
	CHECK(!FrameUse::ChooseMend(frame).Any());
	frame.othersThanPlayer = true;
	frame.up = 0.0f;
	CHECK(!FrameUse::ChooseMend(frame).Any());
}

TEST("frame use: a frame lying on its side is stood up, and a stale seat of the player's in it given back too")
{
	auto frame = Standing();
	frame.up = std::cos(1.5f);
	frame.used = 1;
	frame.user = PLAYER;
	frame.userIsPlayer = true;
	frame.free = false;
	const auto mend = FrameUse::ChooseMend(frame);
	CHECK(mend.standUp);
	CHECK(mend.freeSeat);
}

TEST("frame use: nothing is mended on a frame the game lets the player into")
{
	CHECK(!FrameUse::ChooseMend(Standing()).Any());
}

TEST("frame use: with nothing else in our player's way, the holds say so")
{
	const FrameUse::Holds holds;
	CHECK(!holds.Any());
	CHECK(FrameUse::Describe(holds) == "nothing else holds");
}

TEST("frame use: a copy of a friend's frame the session keeps our player out of is told apart from the engine's refusals")
{
	FrameUse::Holds holds;
	holds.keptOut = true;
	CHECK(holds.Any());
	CHECK(FrameUse::Describe(holds) == "kept out by the session (a friend's frame)");
}

TEST("frame use: everything that holds is named, each once, in one line")
{
	FrameUse::Holds holds;
	holds.activationBlocked = true;
	holds.activateOff = true;
	holds.playerBusy = true;
	CHECK(FrameUse::Describe(holds) == "activation blocked, Activate input off, our player busy in furniture or a frame");
	holds.disabled = true;
	holds.no3d = true;
	holds.inputBlocked = true;
	holds.keptOut = true;
	const auto all = FrameUse::Describe(holds);
	CHECK(all.find("disabled") != std::string::npos);
	CHECK(all.find("no 3D") != std::string::npos);
	CHECK(all.find("player input blocked") != std::string::npos);
	CHECK(all.find("kept out") == 0);
}

// The use hook runs on the engine's seating thread and takes our player's part of the facts from the main thread's last
// word (FrameUse::Seat, Equipment::ShareSeat), not from the player live.
namespace
{
	constexpr std::uint32_t FRAME = 0xFF000801;
	constexpr std::uint32_t OTHER_FRAME = 0xFF000802;

	// The seat the game still has as our player's after they got out (as the stale-use tests above).
	Facts StaleSeat()
	{
		auto frame = Standing();
		frame.used = 1;
		frame.user = PLAYER;
		frame.userMarker = 0;
		frame.userIsPlayer = true;
		frame.free = false;
		return frame;
	}
}

TEST("frame use: the seat word has our player in a frame they occupy or wear, and in no other")
{
	CHECK(!FrameUse::SeatedIn({}, FRAME));
	CHECK(FrameUse::SeatedIn({ .busy = true, .occupied = FRAME }, FRAME));
	CHECK(FrameUse::SeatedIn({ .busy = true, .worn = FRAME }, FRAME));
	CHECK(!FrameUse::SeatedIn({ .busy = true, .worn = OTHER_FRAME }, FRAME));
	CHECK(!FrameUse::SeatedIn({}, 0));  // no frame: nobody is in it, whatever the word says
}

TEST("frame use: from the seat word, a stale seat of our player's out of everything is given back, as the live read did")
{
	auto read = StaleSeat();
	read.userIn = true;  // whatever the hook might have read: the word decides
	read.playerBusy = true;
	const auto facts = FrameUse::WithSeat(read, {}, FRAME);
	CHECK(!facts.userIn);
	CHECK(!facts.playerBusy);
	CHECK(FrameUse::Judge(facts) == Verdict::kStaleUse);
	CHECK(FrameUse::ChooseMend(facts).freeSeat);
}

TEST("frame use: from the seat word, the seat of a frame our player is in is theirs and is not given back")
{
	const auto facts = FrameUse::WithSeat(StaleSeat(), { .busy = true, .worn = FRAME }, FRAME);
	CHECK(facts.userIn);
	CHECK(facts.playerBusy);
	CHECK(FrameUse::Judge(facts) == Verdict::kInUse);
	CHECK(!FrameUse::ChooseMend(facts).freeSeat);
}

TEST("frame use: from the seat word, our player busy in other furniture keeps their stale seat here as it is")
{
	const auto facts = FrameUse::WithSeat(StaleSeat(), { .busy = true, .occupied = OTHER_FRAME }, FRAME);
	CHECK(!facts.userIn);
	CHECK(facts.playerBusy);
	CHECK(!FrameUse::ChooseMend(facts).freeSeat);
}

TEST("frame use: the seat word changes nothing of another user's seat, nor of a free frame")
{
	auto other = StaleSeat();
	other.user = RAIDER;
	other.userIsPlayer = false;
	other.userIn = true;
	other.othersThanPlayer = true;
	const auto facts = FrameUse::WithSeat(other, {}, FRAME);
	CHECK(facts.userIn);
	CHECK(FrameUse::Judge(facts) == Verdict::kOtherUser);
	CHECK(!FrameUse::ChooseMend(facts).Any());
	const auto free = FrameUse::WithSeat(Standing(), { .busy = false }, FRAME);
	CHECK(FrameUse::Judge(free) == Verdict::kFree);
	CHECK(!free.userIn);
}
