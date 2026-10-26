// What a game believes of another player's changes to its world (src/game/RemoteEdits.h), and the
// budget the server keeps per player (common/TokenBucket.h). The end-to-end cases, with a real server
// between the players, are in ServerWorldTests.cpp ("host authority").

#include "Test.h"

#include "TokenBucket.h"
#include "game/DeathReports.h"
#include "game/RemoteEdits.h"

#include <chrono>
#include <limits>
#include <algorithm>
#include <map>
#include <vector>

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

TEST("remote edits: an NPC off here that a friend's game switched on and killed is switched on to die, once its cell is drawn")
{
	CHECK(SwitchOnToKill(true, false, true, false, RAIDER));  // the Red Rocket mole rat their game enabled and killed at once
	CHECK(!SwitchOnToKill(true, false, false, false, RAIDER));  // not drawn here yet: waits
	CHECK(!SwitchOnToKill(false, false, true, false, RAIDER));  // on already: the kill takes it as it is
	CHECK(!SwitchOnToKill(true, true, true, false, RAIDER));  // deleted here
	CHECK(!SwitchOnToKill(true, false, true, false, PRESTON));  // our own story's character stays off
	CHECK(SwitchOnToKill(true, false, true, true, PRESTON));  // unless the story is shared
}

TEST("remote edits: a friend's kill kills anyone but our own story's unique or essential characters, unless they saw it")
{
	CHECK(MayKill(false, RAIDER, false));
	CHECK(MayKill(false, RADIANT_TARGET, false));  // radiant quests alias generic enemies: still everyone's kill
	CHECK(!MayKill(false, JARED, false));
	CHECK(!MayKill(false, PROTECTED, false));
	CHECK(MayKill(false, JARED, true));   // the reporter stood beside him
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

TEST("remote edits: a friend's explosion goes off near us whoever stands beside it, or beside an NPC we run, if a throw could make it")
{
	const Spot us{ 0, 0x3C, 1000.0f, 1000.0f, 0.0f };
	CHECK(SetOff({ 0, 0x3C, 1050.0f, 1000.0f, 0.0f }, us, 500.0f) == Blast::kSetOff);  // at our feet: the raiders beside us die here too
	CHECK(SetOff({ 0, 0x3C, 7000.0f, 1000.0f, 0.0f }, us, 500.0f) == Blast::kSetOff);  // 6000 away
	CHECK(SetOff({ 0, 0x3C, 7001.0f, 1000.0f, 0.0f }, us, 500.0f) == Blast::kAway);
	CHECK(SetOff({ 0x1F00, 0, 1000.0f, 1000.0f, 0.0f }, us, 500.0f) == Blast::kAway);  // inside somewhere, same numbers
	CHECK(SetOff({ 0, 0x3C, NaN, 1000.0f, 0.0f }, us, 500.0f) == Blast::kAway);

	// Far from our player, beside an NPC our game runs: the reporter left that NPC's blow to us, so it goes off for it.
	CHECK(SetOff({ 0, 0x3C, 12000.0f, 1000.0f, 0.0f }, us, 500.0f, true) == Blast::kSetOff);
	CHECK(SetOff({ 0x1F00, 0, 12000.0f, 1000.0f, 0.0f }, us, 500.0f, true) == Blast::kAway);  // never in another space
	CHECK(SetOff({ 0, 0x3C, 12000.0f, 1000.0f, 0.0f }, us, 3000.0f, true) == Blast::kNotThrown);

	// Radii no throw has: refused wherever it is.
	CHECK(SetOff(us, us, 1024.0f) == Blast::kSetOff);
	CHECK(SetOff(us, us, 1025.0f) == Blast::kNotThrown);  // a nuke, an artillery strike
	CHECK(SetOff(us, us, 0.0f) == Blast::kNotThrown);
	CHECK(SetOff(us, us, -1.0f) == Blast::kNotThrown);
	CHECK(SetOff(us, us, NaN) == Blast::kNotThrown);
	CHECK(SetOff(us, us, INF) == Blast::kNotThrown);
}

TEST("remote edits: a friend's explosion set off by an NPC we have no copy of is no blast of ours; a player's or a mirrored NPC's is")
{
	CHECK(SourceHere(0, 0));                    // the friend's own throw
	CHECK(SourceHere(0x0001F00D, 0x0001F00D));  // a plugin's NPC, mirrored here
	CHECK(SourceHere(0xFF000123, 0xFF004567));  // a spawn, and our copy of it
	CHECK(!SourceHere(0xFF000123, 0));          // a spawn never placed here (a DLC's NPC we lack): a fight we can't see
}

TEST("remote edits: an explosion's blow on a copy is the owner's exactly when that explosion goes off in the owner's game")
{
	using namespace std::chrono_literals;
	const auto t = std::chrono::steady_clock::time_point{} + 100s;
	const Spot blast{ 0, 0x3C, 1000.0f, 1000.0f, 0.0f };
	// Reported (the gate passed it): the owner's game sets it off beside its NPC wherever its player is.
	CHECK(DealtThere(Seen{ blast, 500.0f, Fate::kShared, t }));
	// A world trap's, a destructible object's own: every game sets its own off.
	CHECK(DealtThere(Seen{ blast, 300.0f, Fate::kEachOwn, t }));
	// Ours alone (kept: the server might not have passed it on; or no watch, no owner of ours): dealt nowhere else.
	CHECK(!DealtThere(Seen{ blast, 500.0f, Fate::kOurs, t }));
}

TEST("remote edits: each blow of an explosion is dealt in exactly one game, whichever game runs the NPC and wherever its player stands")
{
	using namespace std::chrono_literals;
	const auto t = std::chrono::steady_clock::time_point{} + 100s;
	// Alice's grenade lands beside a raider Bob's game runs. Alice's game has a copy of it; Bob's has the raider.
	const Spot grenade{ 0, 0x3C, 1000.0f, 1000.0f, 0.0f };
	const Spot alice{ 0, 0x3C, 1500.0f, 1000.0f, 0.0f };
	for (const float bobAway : { 0.0f, 5000.0f, 6001.0f, 20000.0f }) {
		const Spot bob{ 0, 0x3C, 1000.0f + bobAway, 1000.0f, 0.0f };
		ReportGate gate;
		const bool sent = gate.Pass(Distance(grenade, alice), 500.0f, t);
		const Seen seen{ grenade, 500.0f, sent ? Fate::kShared : Fate::kOurs, t };
		const bool aliceDeals = !DealtThere(seen);  // her copy's blow, reported as her hit's damage
		const bool bobDeals = sent && SetOff(grenade, bob, 500.0f, true) == Blast::kSetOff;  // the raider beside it in his game
		CHECK(sent);
		CHECK(aliceDeals != bobDeals);
	}
	// Past her budget: not sent, so it goes off in no other game, and her game deals the copy's blow.
	ReportGate gate;
	int        kept = 0;
	for (int i = 0; i < 40; ++i) {
		const bool sent = gate.Pass(500.0f, 500.0f, t);
		const Seen seen{ grenade, 500.0f, sent ? Fate::kShared : Fate::kOurs, t };
		CHECK(!DealtThere(seen) != sent);
		kept += sent ? 0 : 1;
	}
	CHECK(kept == 40 - static_cast<int>(ReportGate::BURST));
}

TEST("remote edits: a report is sent only where the server is sure to pass it on: nearer, rarer and smaller than its own limits")
{
	using namespace std::chrono_literals;
	const auto t = std::chrono::steady_clock::time_point{} + 100s;
	static_assert(ReportGate::RANGE < Protocol::REPORTED_EXPLOSION_RANGE);
	static_assert(ReportGate::BURST < Protocol::REPORTED_EXPLOSION_BURST);
	static_assert(ReportGate::PER_SECOND < Protocol::REPORTED_EXPLOSIONS_PER_SECOND);
	{
		ReportGate gate;
		CHECK(gate.Pass(ReportGate::RANGE, 500.0f, t));
		CHECK(!gate.Pass(ReportGate::RANGE + 1.0f, 500.0f, t));  // the server measures from a state a moment old
		CHECK(!gate.Pass(NaN, 500.0f, t));
		CHECK(!gate.Pass(100.0f, 1025.0f, t));  // nobody sets off one that big
		CHECK(!gate.Pass(100.0f, 0.0f, t));
	}
	// Its burst, then its rate: a reliable run of reports arriving at the server closer together by as much as two
	// seconds still finds the server's budget (16, then 4 a second) with room.
	ReportGate  gate;
	TokenBucket server{ Protocol::REPORTED_EXPLOSION_BURST, Protocol::REPORTED_EXPLOSIONS_PER_SECOND };
	std::vector<std::chrono::steady_clock::time_point> sent;
	for (int i = 0; i < 400; ++i) {
		const auto at = t + std::chrono::milliseconds{ 25 * i };  // an explosive minigun: 40 a second for 10 s
		if (gate.Pass(100.0f, 300.0f, at)) {
			sent.push_back(at);
		}
	}
	CHECK(sent.size() < 400u);
	CHECK(sent.size() >= static_cast<std::size_t>(ReportGate::BURST));
	// The first ones held back two seconds by the network, the rest not: they all reach the server at once.
	for (const auto at : sent) {
		const auto arrives = (std::max)(at, t + 2s);
		CHECK(server.Take(arrives));
	}
}

TEST("remote edits: a blow is put down to the explosion its hit names, else the nearest seen lately that reaches it")
{
	using namespace std::chrono_literals;
	const auto t = std::chrono::steady_clock::time_point{} + 100s;
	BlastLog   log;
	CHECK(!log.At({ 0, 0x3C, 0.0f, 0.0f, 0.0f }, t));
	log.Note({ { 0, 0x3C, 0.0f, 0.0f, 0.0f }, 500.0f, Fate::kOurs, t, 0xFF000101 });
	log.Note({ { 0, 0x3C, 1400.0f, 0.0f, 0.0f }, 500.0f, Fate::kShared, t + 10ms, 0xFF000102 });
	log.Note({ { 0x1F00, 0, 100.0f, 0.0f, 0.0f }, 500.0f, Fate::kShared, t, 0xFF000103 });  // another space, same numbers

	const auto first = log.At({ 0, 0x3C, 100.0f, 0.0f, 0.0f }, t + 20ms);
	REQUIRE(first);
	CHECK(first->fate == Fate::kOurs);
	const auto second = log.At({ 0, 0x3C, 1300.0f, 0.0f, 0.0f }, t + 20ms);
	REQUIRE(second);
	CHECK(second->fate == Fate::kShared);
	// Within the radius and an actor's size of it; farther, it is no blow of that explosion's.
	CHECK(log.At({ 0, 0x3C, 1400.0f + 500.0f + BlastLog::REACH, 0.0f, 0.0f }, t + 20ms));
	CHECK(!log.At({ 0, 0x3C, 1400.0f + 500.0f + BlastLog::REACH + 1.0f, 0.0f, 0.0f }, t + 20ms));
	const auto inside = log.At({ 0x1F00, 0, 0.0f, 0.0f, 0.0f }, t);
	REQUIRE(inside);
	CHECK(inside->fate == Fate::kShared);
	// Seen too long ago: none.
	CHECK(!log.At({ 0, 0x3C, 100.0f, 0.0f, 0.0f }, t + BlastLog::KEPT + 11ms));
	log.Prune(t + BlastLog::KEPT + 11ms);
	CHECK(log.Size() == 0u);

	for (std::size_t i = 0; i < BlastLog::MAX + 10; ++i) {
		log.Note({ { 0, 0x3C, static_cast<float>(i) * 2000.0f, 0.0f, 0.0f }, 100.0f, Fate::kOurs, t });
	}
	CHECK(log.Size() == BlastLog::MAX);
	CHECK(!log.At({ 0, 0x3C, 0.0f, 0.0f, 0.0f }, t));  // the oldest went first
	CHECK(log.At({ 0, 0x3C, (BlastLog::MAX + 9) * 2000.0f, 0.0f, 0.0f }, t));
}

TEST("remote edits: of two explosions that reach a blow and fared differently, the one the hit names is taken, never the nearer")
{
	using namespace std::chrono_literals;
	const auto t = std::chrono::steady_clock::time_point{} + 100s;
	// Two grenades a moment apart beside the same raider: the first reported, the second kept ours (past the budget).
	BlastLog   log;
	const Seen reported{ { 0, 0x3C, 0.0f, 0.0f, 0.0f }, 500.0f, Fate::kShared, t, 0xFF000201 };
	const Seen kept{ { 0, 0x3C, 300.0f, 0.0f, 0.0f }, 500.0f, Fate::kOurs, t + 100ms, 0xFF000202 };
	log.Note(reported);
	log.Note(kept);
	const Spot raider{ 0, 0x3C, 250.0f, 0.0f, 0.0f };  // nearer the kept one
	bool       ambiguous = true;
	const auto byFirst = log.At(raider, t + 200ms, reported.source, &ambiguous);
	REQUIRE(byFirst);
	CHECK(byFirst->fate == Fate::kShared);  // the owner's game deals it
	CHECK(!ambiguous);
	const auto bySecond = log.At(raider, t + 200ms, kept.source, &ambiguous);
	REQUIRE(bySecond);
	CHECK(bySecond->fate == Fate::kOurs);
	CHECK(!ambiguous);
	// Neither named (a hit whose source we can't read): not to be told apart, so it is taken as ours, whichever is nearer.
	const auto unnamed = log.At({ 0, 0x3C, 50.0f, 0.0f, 0.0f }, t + 200ms, 0, &ambiguous);
	REQUIRE(unnamed);
	CHECK(unnamed->fate == Fate::kOurs);
	CHECK(ambiguous);
	// A name we never saw: as if none.
	CHECK(log.At({ 0, 0x3C, 50.0f, 0.0f, 0.0f }, t + 200ms, 0xFF0009FF, &ambiguous)->fate == Fate::kOurs);
	// Two that fared alike: the nearest, and no doubt about it.
	log.Clear();
	log.Note(reported);
	log.Note({ { 0, 0x3C, 300.0f, 0.0f, 0.0f }, 300.0f, Fate::kEachOwn, t, 0xFF000203 });  // a mine beside it
	const auto alike = log.At(raider, t + 200ms, 0, &ambiguous);
	REQUIRE(alike);
	CHECK(alike->fate == Fate::kEachOwn);
	CHECK(!ambiguous);
}

TEST("remote edits: a destructible object's watch covers only its own blast radius, for its time")
{
	using namespace std::chrono_literals;
	const auto  t = std::chrono::steady_clock::time_point{} + 100s;
	AreaWatches watches;
	const Spot  car{ 0, 0x3C, 1000.0f, 1000.0f, 0.0f };
	CHECK(!watches.Any(t));
	watches.Arm(car, 900.0f, t + 8s);
	watches.Arm(car, 0.0f, t + 20s);  // no radius: nothing
	CHECK(watches.Any(t));
	CHECK(watches.Covers({ 0, 0x3C, 1900.0f, 1000.0f, 0.0f }, t));
	CHECK(!watches.Covers({ 0, 0x3C, 1901.0f, 1000.0f, 0.0f }, t));  // a raider's grenade across the street
	CHECK(!watches.Covers({ 0x1F00, 0, 1000.0f, 1000.0f, 0.0f }, t));
	CHECK(!watches.Covers(car, t + 8s + 1ms));
	// Armed again (it blew long after the hit): one watch, the longer.
	watches.Arm(car, 128.0f, t + 30s);
	CHECK(watches.Size() == 1u);
	CHECK(watches.Covers({ 0, 0x3C, 1900.0f, 1000.0f, 0.0f }, t + 29s));
	watches.Prune(t + 31s);
	CHECK(!watches.Any(t + 31s));
	CHECK(watches.Size() == 0u);
	for (std::size_t i = 0; i < AreaWatches::MAX + 3; ++i) {
		watches.Arm({ 0, 0x3C, static_cast<float>(i) * 5000.0f, 0.0f, 0.0f }, 300.0f, t + 8s + std::chrono::milliseconds(i));
	}
	CHECK(watches.Size() == AreaWatches::MAX);
	CHECK(!watches.Covers({ 0, 0x3C, 0.0f, 0.0f, 0.0f }, t));  // the soonest to end went first
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

TEST("remote edits: a friend's build is set to its size here only when that changes, and never past the bounds")
{
	Protocol::WorkshopItem item;
	item.scale = 1.0f;
	CHECK(!ScaleToApply(item, 1.0f));  // placed at the game's own size: left alone
	item.scale = 1.005f;
	CHECK(!ScaleToApply(item, 1.0f));
	item.scale = 1.5f;
	CHECK(ScaleToApply(item, 1.0f) == 1.5f);
	CHECK(!ScaleToApply(item, 1.5f));  // moved, the same size
	item.scale = 100.0f;
	CHECK(ScaleToApply(item, 1.0f) == MAX_SCALE);
	CHECK(!ScaleToApply(item, MAX_SCALE));
	item.scale = 0.0001f;
	CHECK(ScaleToApply(item, 1.0f) == MIN_SCALE);
}

TEST("remote edits: a friend's scrap is applied in the build area, waits while its volumes aren't loaded, and is refused outside")
{
	CHECK(ScrapReach(BuildArea::kLoaded, true, 99999.0f) == Verdict::kApply);  // the volumes decide, not the distance
	CHECK(ScrapReach(BuildArea::kLoaded, false, 10.0f) == Verdict::kRefuse);
	CHECK(ScrapReach(BuildArea::kNotLoaded, false, 10.0f) == Verdict::kWait);
	CHECK(ScrapReach(BuildArea::kNotLoaded, false, 99999.0f) == Verdict::kWait);
	// A settlement of several volumes, some not loaded yet: inside a loaded one goes, outside them all waits.
	CHECK(ScrapReach(BuildArea::kPartlyLoaded, true, 99999.0f) == Verdict::kApply);
	CHECK(ScrapReach(BuildArea::kPartlyLoaded, false, 10.0f) == Verdict::kWait);
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
	CHECK(LookToPlace(0, OUR_DEFAULT) == 0);  // nothing asked: our default
	CHECK(LookToPlace(0x0020A57B, OUR_DEFAULT) == 0x0020A57B);
	CHECK(LookToPlace(0x0024A381, OUR_DEFAULT) == 0);
}

TEST("death reports: a report is held against the player the server named, where they were when it came and where they are now")
{
	using namespace std::chrono_literals;
	using DeathReports::Verdict;
	std::map<std::uint32_t, Spot> heard{ { 2, { 0, 0x3C, 0.0f, 0.0f, 0.0f } } };
	DeathReports::Book book{ [&](std::uint32_t a_player) -> std::optional<Spot> {
		const auto it = heard.find(a_player);
		return it == heard.end() ? std::nullopt : std::optional{ it->second };
	} };
	const DeathReports::Clock::time_point t0{ 1h };
	const DeathReports::Copy jared{ JARED, { 0, 0x3C, 100.0f, 0.0f, 0.0f } };
	const DeathReports::Copy farJared{ JARED, { 0, 0x3C, 50000.0f, 0.0f, 0.0f } };

	const auto killed = book.Received({ .refId = 0x1F0B5, .playerId = 2, .killed = true, .x = 5.0f }, t0);
	CHECK(killed.reporter == 2);
	CHECK(killed.killer == 2);
	CHECK(killed.x == 5.0f);
	REQUIRE(killed.reporterThen.has_value());
	CHECK(book.Judge(false, killed, jared, t0) == Verdict::kKill);

	// Far from where they were then, and from where they are now: waits for their position, then is refused.
	CHECK(book.Judge(false, killed, farJared, t0) == Verdict::kWait);
	heard[2] = { 0, 0x3C, 49000.0f, 0.0f, 0.0f };  // they were on their way after all
	CHECK(book.Judge(false, killed, farJared, t0 + 1s) == Verdict::kKill);
	heard[2] = { 0, 0x3C, 0.0f, 0.0f, 0.0f };
	CHECK(book.Judge(false, killed, farJared, t0 + DeathReports::CATCH_UP) == Verdict::kKeepAlive);
	CHECK(book.Judge(true, killed, farJared, t0 + DeathReports::CATCH_UP) == Verdict::kKill);  // a shared story
	CHECK(book.Judge(false, killed, { RAIDER, farJared.at }, t0 + DeathReports::CATCH_UP) == Verdict::kKill);

	// Found dead, or died of something else: nobody's kill, and nobody's stand-in pushes it.
	CHECK(book.Received({ .refId = 0x1F0B5, .playerId = 2 }, t0).killer == 0);
	// A player we never heard from, and a replay from the session: no wait, nobody saw it.
	const auto stranger = book.Received({ .refId = 0x1F0B5, .playerId = 7, .killed = true }, t0);
	CHECK(!stranger.reporterThen);
	CHECK(book.Judge(false, stranger, jared, t0) == Verdict::kWait);
	const auto replay = DeathReports::Book::Replayed(t0);
	CHECK(replay.reporter == 0);
	CHECK(book.Judge(false, replay, jared, t0) == Verdict::kKeepAlive);
	CHECK(book.Judge(false, book.Received({ .refId = 0x1F0B5, .playerId = 0, .x = 5.0f }, t0), jared, t0) == Verdict::kKeepAlive);  // a placed replay
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

TEST("remote edits: each thrower's watch is its own; a settler's grenade never marks our player's kept Molotov as shared")
{
	using RemoteEdits::ThrowWatches;
	using namespace std::chrono_literals;
	const auto       t0 = ThrowWatches::Clock::now();
	constexpr std::uint32_t PLAYER = 0x14, SETTLER = 0x0001A2B3, COMPANION = 0xFF000F10;
	ThrowWatches     watches;
	CHECK(!watches.Shared(PLAYER, t0));
	CHECK(!watches.Shared(0, t0));
	// Our player's Molotov, kept ours (past the budget): its fire burns here alone.
	watches.Arm(PLAYER, t0 + 8s);
	CHECK(watches.Shared(PLAYER, t0));
	watches.Unshare(PLAYER, t0 + 1s);
	CHECK(!watches.Shared(PLAYER, t0 + 1s));
	// A settler we run throws a grenade: its watch, not hers.
	watches.Arm(SETTLER, t0 + 2s + 8s);
	CHECK(watches.Shared(SETTLER, t0 + 2s));
	CHECK(!watches.Shared(PLAYER, t0 + 2s));
	CHECK(!watches.Shared(PLAYER, t0 + 20s, 20s));
	// A hit that names nobody: the last to throw's.
	CHECK(watches.Shared(0, t0 + 2s));
	// After the watch, for a while (a Molotov's fire burns on), then no more.
	CHECK(!watches.Shared(SETTLER, t0 + 11s));
	CHECK(watches.Shared(SETTLER, t0 + 11s, 20s));
	CHECK(!watches.Shared(SETTLER, t0 + 31s, 20s));
	// An explosion that named none, kept: every watch on then is unshared, one already over is not.
	watches.Arm(COMPANION, t0 + 12s + 8s);
	watches.Unshare(0, t0 + 12s);
	CHECK(!watches.Shared(COMPANION, t0 + 12s));
	CHECK(watches.Shared(SETTLER, t0 + 12s, 20s));
	// Armed again: shared again.
	watches.Arm(PLAYER, t0 + 13s + 8s);
	CHECK(watches.Shared(PLAYER, t0 + 13s));
	// Bounded: the one ending first goes.
	for (std::uint32_t i = 0; i < ThrowWatches::MAX; ++i) {
		watches.Arm(0x00100000 + i, t0 + 14s + 8s + std::chrono::seconds{ i });
	}
	CHECK(watches.Shared(0x00100000 + ThrowWatches::MAX - 1, t0 + 14s));
	CHECK(!watches.Shared(SETTLER, t0 + 14s, 20s));  // ended first, gone
}

TEST("remote edits: a death blast is told by its explosion's base alone; every game sets its own off")
{
	CHECK(IsDeathBlast("Fallout4.esm", 0x001A673A, false));  // a Sentry Bot's self-destruct, seen on the rig in both games
	CHECK(IsDeathBlast("Fallout4.esm", 0x001AF075, true));  // a Mr. Gutsy's
	CHECK(IsDeathBlast("Fallout4.esm", 0x0023E912, true));  // a Sentry Bot's leg, in its cascade
	CHECK(IsDeathBlast("DLCCoast.esm", 0x00053DFE, true));  // a Fog Crawler's
	CHECK(!IsDeathBlast("DLCRobot.esm", 0x001A673A, true));  // the plugin counts
	CHECK(!IsDeathBlast("Fallout4.esm", 0x001A7FF2, true));  // FatManExplosion naming its shooter: a Fat Man's, the normal way
	CHECK(IsDeathBlast("Fallout4.esm", 0x001A7FF2, false));  // ... naming no owner: a suicider's nuke, set off in both games on the rig
	CHECK(IsDeathBlast("Fallout4.esm", 0x001A673A, true));   // a self-destruct, owned or not
	CHECK(!IsDeathBlast("Fallout4.esm", 0x000E19EF, true));  // RobotModExplosion: a turret's shot's impact too
	CHECK(!IsDeathBlast("Fallout4.esm", 0x000FD3AA, true));  // a missile
	CHECK(!IsDeathBlast("Fallout4.esm", 0x00216359, true));  // a Sentry Bot's own missile, fired alive
	for (std::size_t i = 0; i < DEATH_BLASTS.size(); ++i) {
		CHECK((DEATH_BLASTS[i].id >> 24) == 0);  // no load-order byte
		for (std::size_t j = i + 1; j < DEATH_BLASTS.size(); ++j) {
			CHECK(DEATH_BLASTS[i].id != DEATH_BLASTS[j].id || DEATH_BLASTS[i].file != DEATH_BLASTS[j].file);
		}
	}
	CHECK(DealtThere(Seen{ {}, 300.0f, Fate::kEachOwn, {} }));  // its blow on a copy is the NPC's own game's
}

TEST("remote edits: an explosion's hit on a friend's stand-in is passed on only when it goes off in our game alone")
{
	const Seen reported{ {}, 300.0f, Fate::kShared, {} };  // an NPC's grenade at them: it goes off beside them there
	const Seen trap{ {}, 300.0f, Fate::kEachOwn, {} };
	const Seen ours{ {}, 300.0f, Fate::kOurs, {} };  // kept: too big, or past the budget
	CHECK(!PassStandInHit(true, &reported));
	CHECK(!PassStandInHit(true, &trap));
	CHECK(PassStandInHit(true, &ours));
	CHECK(PassStandInHit(true, nullptr));  // one we never saw: passed on, as before
	CHECK(PassStandInHit(false, &reported));  // a bullet's
}
