// Why the game turns our player away from a power armor frame with "You cannot use this at this time." (FrameUse::Judge,
// as Equipment::FrameUseOf reads the engine's answers): each test is a frame as the engine has it when the player
// presses E on it, and names what the player sees.

#include "Test.h"

#include "game/FrameUse.h"

#include <cmath>

namespace
{
	using FrameUse::Facts;
	using FrameUse::Verdict;

	constexpr std::uint32_t PLAYER = 0x00000014;
	constexpr std::uint32_t RAIDER = 0x0002A001;
	constexpr float         UP_MIN = 0.7071f;  // about what the engine's InteractionUtils::QUprightAngleCosine is

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

TEST("frame use: a frame tipped a little, within what the game allows, is still upright")
{
	auto frame = Standing();
	frame.up = std::cos(0.3f);
	CHECK(FrameUse::Upright(frame));
	CHECK(FrameUse::Judge(frame) == Verdict::kFree);
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
