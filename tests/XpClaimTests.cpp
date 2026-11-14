// XP gained before the kill that earned it was seen, waiting for its death event (game/XpClaim.h, Party::TrackXp).

#include "Test.h"

#include "game/XpClaim.h"

#include <chrono>
#include <optional>

using namespace std::chrono_literals;

namespace
{
	constexpr auto WINDOW = 5s;

	// A frame every 100 ms from a_from to a_to (excluded), stopped or not; what ran out, if anything.
	std::optional<XpClaim::Held> Frames(XpClaim& a_claim, XpClaim::Time a_from, XpClaim::Time a_to, bool a_stopped)
	{
		std::optional<XpClaim::Held> out;
		for (auto t = a_from; t < a_to; t += 100ms) {
			if (auto expired = a_claim.Tick(t, a_stopped)) {
				out = expired;
			}
		}
		return out;
	}
}

TEST("xp claim: with no stop, XP no kill claims runs out after the window")
{
	XpClaim    claim{ WINDOW };
	const auto t0 = std::chrono::steady_clock::now();
	claim.Tick(t0, false);
	CHECK(claim.Gain(40.0f, t0));
	CHECK(!Frames(claim, t0 + 100ms, t0 + WINDOW, false));
	CHECK(claim.Xp() == 40.0f);
	const auto expired = claim.Tick(t0 + WINDOW, false);
	REQUIRE(expired.has_value());
	CHECK(expired->xp == 40.0f);
	CHECK(expired->since == t0);
	CHECK(expired->stopped == 0s);
	CHECK(claim.Xp() == 0.0f);
	CHECK(claim.Claim().xp == 0.0f);  // too late for its death event
}

TEST("xp claim: with no stop, a death event inside the window claims the XP")
{
	XpClaim    claim{ WINDOW };
	const auto t0 = std::chrono::steady_clock::now();
	claim.Tick(t0, false);
	claim.Gain(25.0f, t0);
	claim.Tick(t0 + 1s, false);
	claim.Gain(5.0f, t0 + 1s);  // more of it: the window runs from the last gain
	CHECK(!Frames(claim, t0 + 1100ms, t0 + 5900ms, false));
	const auto claimed = claim.Claim();
	CHECK(claimed.xp == 30.0f);
	CHECK(claimed.since == t0);
	CHECK(claim.Xp() == 0.0f);
}

TEST("xp claim: XP held across a 16 s menu stop is claimed by the death event after the menu closes")
{
	XpClaim    claim{ WINDOW };
	const auto t0 = std::chrono::steady_clock::now();
	claim.Tick(t0, false);
	claim.Gain(60.0f, t0);  // the lethal blow's XP; the victim falls
	CHECK(!Frames(claim, t0 + 100ms, t0 + 500ms, false));
	// The pause menu, 0.5 s later, for 16 s: the fall (and its death event) stand still.
	CHECK(!Frames(claim, t0 + 500ms, t0 + 16500ms, true));
	CHECK(claim.Xp() == 60.0f);
	// Back in the game: the fall ends 3 s later.
	CHECK(!Frames(claim, t0 + 16500ms, t0 + 19500ms, false));
	const auto claimed = claim.Claim();
	CHECK(claimed.xp == 60.0f);
	CHECK(claimed.since == t0);
}

TEST("xp claim: after a stop the window runs in full from the menu's close, then the XP runs out")
{
	XpClaim    claim{ WINDOW };
	const auto t0 = std::chrono::steady_clock::now();
	claim.Tick(t0, false);
	claim.Gain(60.0f, t0);
	Frames(claim, t0 + 100ms, t0 + 4s, false);
	Frames(claim, t0 + 4s, t0 + 10s, true);  // stopped near the end of the window
	CHECK(!Frames(claim, t0 + 10s, t0 + 14900ms, false));
	CHECK(claim.Xp() == 60.0f);
	const auto expired = claim.Tick(t0 + 15s, false);
	REQUIRE(expired.has_value());
	CHECK(expired->xp == 60.0f);
	CHECK(expired->stopped == 6s);
}

TEST("xp claim: XP gained while the game is stopped is never held for a kill")
{
	XpClaim    claim{ WINDOW };
	const auto t0 = std::chrono::steady_clock::now();
	claim.Tick(t0, false);
	claim.Tick(t0 + 100ms, true);  // a terminal opens
	claim.Tick(t0 + 200ms, true);
	CHECK(!claim.Gain(15.0f, t0 + 200ms));  // its hack's XP
	CHECK(claim.Xp() == 0.0f);
	claim.Tick(t0 + 300ms, false);
	CHECK(claim.Claim().xp == 0.0f);  // a death event later claims none of it
}

TEST("xp claim: kill XP held before a stop keeps out a terminal's XP gained during it")
{
	XpClaim    claim{ WINDOW };
	const auto t0 = std::chrono::steady_clock::now();
	claim.Tick(t0, false);
	claim.Gain(20.0f, t0);
	claim.Tick(t0 + 100ms, true);
	claim.Tick(t0 + 200ms, true);
	CHECK(!claim.Gain(15.0f, t0 + 200ms));
	Frames(claim, t0 + 300ms, t0 + 3s, true);
	Frames(claim, t0 + 3s, t0 + 4s, false);
	CHECK(claim.Claim().xp == 20.0f);
}

TEST("xp claim: XP read on a stop's first frame came while the game ran, so it is held")
{
	XpClaim    claim{ WINDOW };
	const auto t0 = std::chrono::steady_clock::now();
	claim.Tick(t0, false);
	claim.Tick(t0 + 100ms, true);  // the menu opened after our last read: the blow may have landed before it
	CHECK(claim.Gain(30.0f, t0 + 100ms));
	Frames(claim, t0 + 200ms, t0 + 20s, true);
	Frames(claim, t0 + 20s, t0 + 22s, false);
	CHECK(claim.Claim().xp == 30.0f);
}

TEST("xp claim: a load forgets the XP held and the stop")
{
	XpClaim    claim{ WINDOW };
	const auto t0 = std::chrono::steady_clock::now();
	claim.Tick(t0, true);
	claim.Tick(t0 + 100ms, true);
	claim.Reset();
	CHECK(claim.Gain(10.0f, t0 + 200ms));  // the first frame after it isn't taken as one of a stop
	claim.Reset();
	CHECK(claim.Xp() == 0.0f);
	CHECK(!claim.Tick(t0 + 10s, false));
}
