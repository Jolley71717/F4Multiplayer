// What a game believes of another player's changes to its world (src/game/RemoteEdits.h), and the
// budget the server keeps per player (common/TokenBucket.h). The end-to-end cases, with a real server
// between the players, are in ServerWorldTests.cpp ("host authority").

#include "Test.h"

#include "TokenBucket.h"
#include "game/RemoteEdits.h"

#include <limits>

using namespace RemoteEdits;
using FoundDead::Npc;

namespace
{
	constexpr float NaN = std::numeric_limits<float>::quiet_NaN();
	constexpr float INF = std::numeric_limits<float>::infinity();

	constexpr Npc RAIDER{};
	constexpr Npc JARED{ .unique = true };  // a boss of a story quest
	constexpr Npc PRESTON{ .unique = true, .essential = true };
	constexpr Npc PROTECTED{ .essential = true };
	constexpr Npc RADIANT_TARGET{ .questAlias = true };  // a generic raider a radiant quest sent us after
}

TEST("remote edits: a friend's game switches on a generic NPC in ours, a story character only with a shared story")
{
	CHECK(MaySwitchOn(false, RAIDER));  // the mole rats that burrow up at Red Rocket
	CHECK(!MaySwitchOn(false, PRESTON));
	CHECK(!MaySwitchOn(false, RADIANT_TARGET));
	CHECK(MaySwitchOn(true, PRESTON));
	CHECK(MaySwitchOn(true, RADIANT_TARGET));
}

TEST("remote edits: a friend's kill kills anyone but our own story's unique or essential characters, unless they saw it")
{
	CHECK(MayKill(false, RAIDER, false));
	CHECK(MayKill(false, RADIANT_TARGET, false));  // radiant quests alias generic enemies: still everyone's kill
	CHECK(!MayKill(false, JARED, false));
	CHECK(!MayKill(false, PROTECTED, false));
	CHECK(MayKill(false, JARED, true));   // killed with the other player beside him
	CHECK(MayKill(true, PRESTON, false)); // a shared story: the host's story is everyone's
}

TEST("remote edits: a friend's companion is brought to us only beside its owner's stand-in")
{
	CHECK(OwnerNear(0.0f));
	CHECK(OwnerNear(1500.0f));
	CHECK(!OwnerNear(1501.0f));
	CHECK(!OwnerNear(NaN));
	CHECK(!OwnerNear(INF));

	const Spot dogmeat{ 0, 0x3C, 1000.0f, 1000.0f, 0.0f };
	CHECK(OwnerNear(Spot{ 0, 0x3C, 1300.0f, 1400.0f, 0.0f }, dogmeat));
	CHECK(!OwnerNear(Spot{ 0, 0x3C, 9000.0f, 1000.0f, 0.0f }, dogmeat));  // the owner is across the map
	CHECK(!OwnerNear(Spot{ 0x1F00, 0, 1000.0f, 1000.0f, 0.0f }, dogmeat));  // inside somewhere: the same numbers are nowhere near
	CHECK(!OwnerNear(std::nullopt, dogmeat));  // never heard from
}

TEST("remote edits: a death is witnessed from the actor's own cell or worldspace, within range of our copy")
{
	CHECK(Witnessed(true, 8192.0f));
	CHECK(!Witnessed(true, 8193.0f));
	CHECK(!Witnessed(false, 10.0f));
	CHECK(!Witnessed(true, NaN));

	const Spot boss{ 0x0001F00D, 0, 100.0f, 200.0f, 0.0f };  // inside Corvega
	CHECK(Witnessed(Spot{ 0x0001F00D, 0, 900.0f, 200.0f, 0.0f }, boss));
	CHECK(!Witnessed(Spot{ 0, 0x3C, 100.0f, 200.0f, 0.0f }, boss));  // outside the door, same numbers
	CHECK(!Witnessed(std::nullopt, boss));  // a replay from the session: nobody is known to have seen it
}

TEST("remote edits: a corpse is moved to where it fell for the reporter only from close by")
{
	CHECK(MayMoveCorpse(0.0f));
	CHECK(MayMoveCorpse(4096.0f));
	CHECK(!MayMoveCorpse(4097.0f));
	CHECK(!MayMoveCorpse(NaN));
	CHECK(!MayMoveCorpse(INF));
}

TEST("remote edits: a friend's explosion is set off only if it is a thrown one, and away from us unless friendly fire is on")
{
	// Radii no throw has: refused whatever else.
	CHECK(!MaySetOff(0.0f, 5000.0f, true));
	CHECK(!MaySetOff(-1.0f, 5000.0f, true));
	CHECK(!MaySetOff(NaN, 5000.0f, true));
	CHECK(!MaySetOff(1025.0f, 5000.0f, true));  // a nuke, an artillery strike
	CHECK(MaySetOff(1024.0f, 5000.0f, false));

	// A frag grenade (500): with friendly fire off, not within 500 + a body's width of us.
	CHECK(!MaySetOff(500.0f, 600.0f, false));
	CHECK(MaySetOff(500.0f, 1000.0f, false));
	CHECK(MaySetOff(500.0f, 600.0f, true));
	CHECK(!MaySetOff(500.0f, NaN, false));  // unknown counts as near
}

TEST("remote edits: a friend's build is shown no smaller than a tenth and no bigger than ten times")
{
	CHECK(ClampScale(1.0f) == 1.0f);
	CHECK(ClampScale(2.5f) == 2.5f);
	CHECK(ClampScale(100.0f) == 10.0f);
	CHECK(ClampScale(0.0f) == 0.1f);
	CHECK(ClampScale(-3.0f) == 0.1f);
	CHECK(ClampScale(NaN) == 1.0f);
	CHECK(ClampScale(INF) == 10.0f);
}

TEST("remote edits: a friend's scrap is applied in the build area, waits while its volumes aren't loaded, and is refused outside")
{
	CHECK(ScrapReach(BuildArea::kLoaded, true, 99999.0f) == Verdict::kApply);  // the volumes decide, not the distance
	CHECK(ScrapReach(BuildArea::kLoaded, false, 10.0f) == Verdict::kRefuse);
	CHECK(ScrapReach(BuildArea::kNotLoaded, false, 10.0f) == Verdict::kWait);
	CHECK(ScrapReach(BuildArea::kNotLoaded, false, 99999.0f) == Verdict::kWait);
	CHECK(ScrapReach(BuildArea::kNoVolumes, false, 5000.0f) == Verdict::kApply);
	CHECK(ScrapReach(BuildArea::kNoVolumes, false, 5001.0f) == Verdict::kRefuse);
	CHECK(ScrapReach(BuildArea::kNoVolumes, false, NaN) == Verdict::kRefuse);
}

TEST("remote edits: a friend is shown as a settler or our own default, never as any other NPC they name")
{
	constexpr std::uint32_t OUR_DEFAULT = 0x0002268A;  // the ini's iPuppetBaseForm
	CHECK(AllowedAppearance(0x0020A578, OUR_DEFAULT));
	CHECK(AllowedAppearance(0x0020A57B, OUR_DEFAULT));
	CHECK(AllowedAppearance(OUR_DEFAULT, OUR_DEFAULT));
	CHECK(!AllowedAppearance(0x0024A381, OUR_DEFAULT));  // a plain NPC with scripts of its own
	CHECK(!AllowedAppearance(0x0002268A, 0x0020A578));   // what another player's ini picked is not ours
}

TEST("token bucket: a burst, then one per refill, never more than the burst after a long quiet")
{
	using namespace std::chrono_literals;
	const TokenBucket::Clock::time_point start{ 1h };
	TokenBucket bucket{ 3.0, 2.0 };  // three at once, two a second
	CHECK(bucket.Take(start));
	CHECK(bucket.Take(start));
	CHECK(bucket.Take(start));
	CHECK(!bucket.Take(start));
	CHECK(!bucket.Take(start + 400ms));  // 0.8 back
	CHECK(bucket.Take(start + 500ms));   // 1.0 back
	CHECK(!bucket.Take(start + 500ms));
	CHECK(bucket.Take(start + 1500ms));
	CHECK(bucket.Take(start + 1500ms));
	CHECK(!bucket.Take(start + 1500ms));

	// An hour of quiet gives back the burst, not 7200.
	const auto later = start + 1h;
	int taken = 0;
	while (bucket.Take(later)) {
		++taken;
	}
	CHECK(taken == 3);

	// A refused try costs nothing, and a clock that seems to go back gives nothing back.
	CHECK(!bucket.Take(later - 10s));
	CHECK(bucket.Take(later + 500ms));
}
