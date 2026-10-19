#include "Test.h"

#include "game/RadiantRules.h"

#include <map>

namespace
{
	using namespace RadiantRules;
	using Protocol::RadiantFill;

	// Fallout4.esm.
	constexpr std::uint32_t MIN_RECRUIT01 = 0x00098136;      // raider troubles
	constexpr std::uint32_t MIN_RECRUIT02 = 0x00099848;      // kidnapping
	constexpr std::uint32_t MIN_OWNED01 = 0x000A1412;        // a settlement attacked
	constexpr std::uint32_t MIN_DEFEND_CASTLE = 0x000BDB04;  // the Castle attacked
	constexpr std::uint32_t BOSR01 = 0x00064EC7;             // Cleansing the Commonwealth
	constexpr std::uint32_t BOSR02 = 0x000C30DC;             // Quartermastery
	constexpr std::uint32_t INSTR01 = 0x000F7933;            // Pest Control
	constexpr std::uint32_t RRR01A = 0x000B1F1D;             // Butcher's Bill (runs once)
	constexpr std::uint32_t RRR02A = 0x000B3E82;             // Concierge
	constexpr std::uint32_t MQ104 = 0x0001F25E;              // Unlikely Valentine (the main story)
	constexpr std::uint32_t MIN01 = 0x0005DEE4;              // When Freedom Calls (a side quest)

	constexpr std::uint32_t SANCTUARY_WORKSHOP = 0x000250FE;
	constexpr std::uint32_t TENPINES_WORKSHOP = 0x0001F0C9;
	constexpr std::uint32_t CORVEGA = 0x0001F228;  // a location
	constexpr std::uint32_t LEXINGTON = 0x0001F22A;
	constexpr std::uint32_t POLICE_STATION = 0x0001FA4A;  // Cambridge Police Station
	constexpr std::uint32_t SPAWN = 0xFF000123;       // an NPC shared under a spawn id
	constexpr std::uint32_t OUR_COPY = 0xFF004567;    // our copy of it
	constexpr std::uint32_t RUNTIME_NPC = Protocol::RADIANT_UNSHARED_REF;  // a settler a game made, under no spawn id

	constexpr auto REF = Kind::kRef;
	constexpr auto LOC = Kind::kLoc;

	// MinRecruit01's aliases (some of them), as the game files them.
	const std::vector<Alias> RECRUIT01{
		{ 19, REF, Fill::kSource, true, "AnyWorkshop" },
		{ 17, REF, Fill::kSource, true, "UnownedWorkshop" },
		{ 20, REF, Fill::kSource, true, "EventWorkshop" },
		{ 9, REF, Fill::kByOthers, false, "Workshop" },
		{ 2, LOC, Fill::kDerived, false, "ActualLocation" },
		{ 4, LOC, Fill::kSource, false, "RaiderDungeon" },
		{ 8, REF, Fill::kDerived, true, "RaiderMapMarker" },
		{ 6, REF, Fill::kByOthers, false, "SettlementSpokesman" },
		{ 7, REF, Fill::kFixed, false, "PrestonGarvey" },
	};

	// This game: the plugin references and locations it has, and our copies of friends' spawns.
	std::optional<std::uint32_t> Resolve(Kind a_kind, std::uint32_t a_form)
	{
		if (a_kind == LOC) {
			return a_form == CORVEGA || a_form == LEXINGTON || a_form == POLICE_STATION ? std::optional{ a_form } : std::nullopt;
		}
		if (a_form == SPAWN) {
			return OUR_COPY;
		}
		if ((a_form >> 24) == 0xFF) {  // a spawn we have no copy of, or one the session doesn't share
			return std::nullopt;
		}
		return a_form;
	}
}

TEST("radiants: the Minutemen's settlement requests are shared by their run; attacks and the factions' other radiants are left alone")
{
	CHECK(ShareOf(MIN_RECRUIT01) == Share::kShared);
	CHECK(ShareOf(MIN_RECRUIT02) == Share::kShared);
	CHECK(ShareOf(0x0003DF95) == Share::kShared);  // MinRadiantOwned02: raiders near a settlement of the player's
	CHECK(ShareOf(0x0003E0C1) == Share::kShared);  // MinRadiantOwned03: kidnapping
	// Each game simulates its own attack: the attackers are made at the settlement.
	CHECK(ShareOf(MIN_OWNED01) == Share::kLeftAlone);
	CHECK(ShareOf(MIN_DEFEND_CASTLE) == Share::kLeftAlone);
	// Not radiants: QuestFilter decides.
	CHECK(ShareOf(MQ104) == Share::kNotRadiant);
	CHECK(ShareOf(MIN01) == Share::kNotRadiant);
	CHECK(!IsRadiant(MIN01));
	CHECK(IsRadiant(MIN_OWNED01));
	// Every entry once, and every one Fallout4.esm's.
	std::set<std::uint32_t> seen;
	for (const auto& entry : RADIANTS) {
		CHECK(seen.insert(entry.formId).second);
		CHECK((entry.formId >> 24) == 0);
		CHECK(!entry.editorId.empty());
		// Only a shared radiant's run is ever stopped for a friend's.
		CHECK((!entry.replace || entry.share == Share::kShared));
	}
}

TEST("radiants: the factions' jobs at a dungeon or a place are shared; those with targets made at runtime, a settlement's workshop or HQ's count are not")
{
	// The Brotherhood's: Cleansing the Commonwealth, Quartermastery.
	CHECK(ShareOf(BOSR01) == Share::kShared);
	CHECK(ShareOf(BOSR02) == Share::kShared);
	CHECK(ShareOf(0x000C8675) == Share::kLeftAlone);  // Leading by Example: a squire made at runtime
	CHECK(ShareOf(0x000CF3E2) == Share::kLeftAlone);  // Learning Curve: a scribe made at runtime
	CHECK(ShareOf(0x000D1EB2) == Share::kLeftAlone);  // Feeding the Troops: a settlement's workshop
	// The Institute's: Pest Control, Hypothesis, Political Leanings.
	CHECK(ShareOf(INSTR01) == Share::kShared);
	CHECK(ShareOf(0x000EB268) == Share::kShared);
	CHECK(ShareOf(0x000F0D8C) == Share::kShared);
	CHECK(ShareOf(0x000EDCEF) == Share::kLeftAlone);  // Appropriation: defenders made at runtime
	CHECK(ShareOf(0x000EDE28) == Share::kLeftAlone);  // Reclamation: the synth made at runtime
	// The Railroad's: Butcher's Bill and Concierge; the others count their offer at HQ or make their targets.
	CHECK(ShareOf(RRR01A) == Share::kShared);
	CHECK(ShareOf(RRR02A) == Share::kShared);
	for (const std::uint32_t railroad : { 0x000B1F1Cu, 0x000B926Bu, 0x000B26C2u, 0x000B926Au, 0x001845F7u, 0x00186C79u, 0x0013A33Du, 0x00145F1Au, 0x0014A34Au, 0x00150577u }) {
		CHECK(ShareOf(railroad) == Share::kLeftAlone);
	}
}

TEST("radiants: a run here its player hasn't been given is stopped for a friend's only where that changes nothing else")
{
	// The Institute's wait at their start until handed out, the Brotherhood's start as their giver speaks.
	CHECK(Replaces(INSTR01, false, false, false));
	CHECK(Replaces(BOSR01, false, false, false));
	CHECK(Replaces(RRR02A, false, false, false));
	// Given to this player (an objective shown): theirs is declined, ours goes on.
	CHECK(!Replaces(INSTR01, false, true, false));
	// A run known here (shared, ours or a friend's): never stopped for another.
	CHECK(!Replaces(INSTR01, true, false, false));
	// A quest that runs once would never run again here.
	CHECK(!Replaces(INSTR01, false, false, true));
	CHECK(!Replaces(RRR01A, false, false, false));  // Butcher's Bill: runs once
	// The Minutemen's (their stop runs a stage of its own), and the radiants left alone or not radiants at all.
	CHECK(!Replaces(MIN_RECRUIT01, false, false, false));
	CHECK(!Replaces(MIN_OWNED01, false, false, false));
	CHECK(!Replaces(MIN01, false, false, false));
}

TEST("radiants: the Brotherhood's developer shortcut (its 0, which makes the player a member and moves them) is never taken from a friend")
{
	CHECK(!Takes(BOSR01, 0));
	CHECK(!Takes(BOSR02, 0));
	CHECK(Takes(BOSR01, 10));
	CHECK(Takes(BOSR01, 50));  // the reward: once in each game
	CHECK(Takes(MIN_RECRUIT01, 0));
	CHECK(Takes(INSTR01, 0));
}

TEST("radiants: a friend's Cleansing the Commonwealth is started here at their dungeon, the level picks they left empty left empty")
{
	// BoSR01's aliases as the game files them: a dungeon picked by the player's level (each optional, each pushing its
	// pick into Dungeon), its map marker and bosses from it, the Police Station and the paladin's team the same everywhere.
	const std::vector<Alias> bosr01{
		{ 13, LOC, Fill::kFixed, false, "PoliceStation" },
		{ 16, LOC, Fill::kSource, true, "DungeonLevel50" },
		{ 21, LOC, Fill::kSource, true, "DungeonLevel30" },
		{ 20, LOC, Fill::kSource, true, "DungeonPlayerLevel" },
		{ 1, LOC, Fill::kByOthers, false, "Dungeon" },
		{ 3, REF, Fill::kDerived, false, "DungeonMapMarker" },
		{ 4, REF, Fill::kFixed, false, "BoSR01Rhys" },
	};
	const std::vector<RadiantFill> fills{
		{ 13, 1, POLICE_STATION }, { 16, 1, 0 }, { 21, 1, CORVEGA }, { 20, 1, CORVEGA }, { 1, 1, CORVEGA }, { 3, 0, 0x0001A2B3 }, { 4, 0, 0x0005DE40 },
	};
	const auto plan = MakePlan(bosr01, fills, Resolve);
	REQUIRE(plan.Ok());
	const std::vector<Force> want{
		{ 16, LOC, 0 },  // no dungeon at their level 50: none here either
		{ 21, LOC, CORVEGA },
		{ 20, LOC, CORVEGA },
		{ 1, LOC, CORVEGA },
		{ 3, REF, 0x0001A2B3 },
	};
	CHECK(plan.forces == want);

	// Running here already (its giver spoke to this player too) with another dungeon: not the same run.
	std::map<std::pair<Kind, std::uint32_t>, std::uint32_t> here{ { { LOC, 13 }, POLICE_STATION }, { { LOC, 21 }, LEXINGTON }, { { LOC, 20 }, LEXINGTON },
		{ { LOC, 1 }, LEXINGTON }, { { REF, 3 }, 0x0001A2B4 }, { { REF, 4 }, 0x0005DE40 } };
	const auto ours = [&](Kind a_kind, std::uint32_t a_alias) {
		const auto it = here.find({ a_kind, a_alias });
		return it != here.end() ? it->second : 0u;
	};
	CHECK(!SameTargets(Compare(bosr01, fills, Resolve, ours)));
	// The same dungeon here: shared as it runs.
	here[{ LOC, 21 }] = CORVEGA;
	here[{ LOC, 20 }] = CORVEGA;
	here[{ LOC, 1 }] = CORVEGA;
	here[{ REF, 3 }] = 0x0001A2B3;
	CHECK(Compare(bosr01, fills, Resolve, ours).empty());
}

TEST("radiants: an alias is forced when each game picks it itself (conditions, the start's event), not when it follows the others")
{
	CHECK(RefFill(0, 0) == Fill::kSource);          // conditions
	CHECK(RefFill(3, 0) == Fill::kSource);          // the start's event
	CHECK(RefFill(1, 0) == Fill::kFixed);           // a forced reference
	CHECK(RefFill(6, 0) == Fill::kFixed);           // a unique actor
	CHECK(RefFill(5, 0) == Fill::kFixed);           // another quest's alias
	CHECK(RefFill(2, 0) == Fill::kDerived);         // from another alias
	CHECK(RefFill(4, 0) == Fill::kDerived);         // made at runtime
	CHECK(RefFill(7, 0) == Fill::kDerived);         // near another alias
	CHECK(RefFill(1, ALIAS_BY_OTHERS) == Fill::kByOthers);
	CHECK(LocFill(0, 0) == Fill::kSource);
	CHECK(LocFill(1, 0) == Fill::kFixed);
	CHECK(LocFill(2, 0) == Fill::kDerived);
	CHECK(LocFill(0, ALIAS_BY_OTHERS | ALIAS_OPTIONAL) == Fill::kByOthers);
}

TEST("radiants: a friend's run is started here with their settlement and place; what follows from them is forced where this game has it too")
{
	const std::vector<RadiantFill> fills{
		{ 19, 0, TENPINES_WORKSHOP }, { 17, 0, 0 }, { 20, 0, TENPINES_WORKSHOP }, { 9, 0, TENPINES_WORKSHOP },
		{ 2, 1, LEXINGTON }, { 4, 1, CORVEGA }, { 8, 0, 0x0001A2B3 }, { 6, 0, RUNTIME_NPC }, { 7, 0, 0x00019FD9 },
	};
	const auto plan = MakePlan(RECRUIT01, fills, Resolve);
	REQUIRE(plan.Ok());
	const std::vector<Force> want{
		{ 19, REF, TENPINES_WORKSHOP },
		{ 17, REF, 0 },  // empty in their game, optional: left empty here too
		{ 20, REF, TENPINES_WORKSHOP },
		{ 9, REF, TENPINES_WORKSHOP },  // pushed in by the others there; forced here as well
		{ 2, LOC, LEXINGTON },  // follows from the workshop: forced too, so a pick between two can't differ
		{ 4, LOC, CORVEGA },
		{ 8, REF, 0x0001A2B3 },
		// The spokesman, a settler their game made, is left to the aliases that push it in (as there); Preston is the
		// same everywhere.
	};
	CHECK(plan.forces == want);
}

TEST("radiants: a fill this game can't match leaves the run each player's own, saying which")
{
	// The place to clear isn't a location this game has.
	std::vector<RadiantFill> fills{ { 20, 0, TENPINES_WORKSHOP }, { 9, 0, TENPINES_WORKSHOP }, { 4, 1, 0x0001F999 } };
	auto plan = MakePlan(RECRUIT01, fills, Resolve);
	CHECK(!plan.Ok());
	CHECK(plan.fallback.find("RaiderDungeon") != std::string::npos);

	// A picked NPC their game made at runtime, never shared: no copy here.
	const std::vector<Alias> kidnapping{ { 1, REF, Fill::kSource, false, "Workshop" }, { 38, REF, Fill::kSource, false, "caravanActor" } };
	fills = { { 1, 0, SANCTUARY_WORKSHOP }, { 38, 0, RUNTIME_NPC } };
	plan = MakePlan(kidnapping, fills, Resolve);
	CHECK(!plan.Ok());
	CHECK(plan.fallback.find("made at runtime") != std::string::npos);

	// One shared under a spawn id: our copy of it.
	fills = { { 1, 0, SANCTUARY_WORKSHOP }, { 38, 0, SPAWN } };
	plan = MakePlan(kidnapping, fills, Resolve);
	REQUIRE(plan.Ok());
	CHECK((plan.forces.back() == Force{ 38, REF, OUR_COPY }));

	// A required alias empty in theirs (it can't be: the quest wouldn't have started), or one this quest hasn't.
	CHECK((!MakePlan(RECRUIT01, std::vector<RadiantFill>{ { 9, 0, TENPINES_WORKSHOP } }, Resolve).Ok()));  // no RaiderDungeon
	plan = MakePlan(RECRUIT01, std::vector<RadiantFill>{ { 9, 0, TENPINES_WORKSHOP }, { 4, 1, CORVEGA }, { 99, 0, TENPINES_WORKSHOP } }, Resolve);
	CHECK(!plan.Ok());
}

TEST("radiants: the targets are the same when the fills each game picks are; a settler that differs is only noted")
{
	const std::vector<RadiantFill> fills{ { 20, 0, TENPINES_WORKSHOP }, { 9, 0, TENPINES_WORKSHOP }, { 4, 1, CORVEGA }, { 6, 0, 0x0001A001 }, { 8, 0, 0x0001A2B3 } };
	std::map<std::pair<Kind, std::uint32_t>, std::uint32_t> here{
		{ { REF, 20 }, TENPINES_WORKSHOP }, { { REF, 9 }, TENPINES_WORKSHOP }, { { LOC, 4 }, CORVEGA }, { { REF, 6 }, 0x0001A001 }, { { REF, 8 }, 0x0001A2B3 }
	};
	const auto ours = [&](Kind a_kind, std::uint32_t a_alias) {
		const auto it = here.find({ a_kind, a_alias });
		return it != here.end() ? it->second : 0u;
	};
	CHECK(Compare(RECRUIT01, fills, Resolve, ours).empty());

	// Another raider camp here: not the same run.
	here[{ LOC, 4 }] = LEXINGTON;
	auto mismatches = Compare(RECRUIT01, fills, Resolve, ours);
	REQUIRE(mismatches.size() == 1);
	CHECK(mismatches[0].decisive);
	CHECK(!SameTargets(mismatches));

	// The same camp, another map marker (follows the camp): only noted.
	here[{ LOC, 4 }] = CORVEGA;
	here[{ REF, 8 }] = 0x0001A2B4;
	mismatches = Compare(RECRUIT01, fills, Resolve, ours);
	REQUIRE(mismatches.size() == 1);
	CHECK(!mismatches[0].decisive);
	CHECK(SameTargets(mismatches));

	// A spokesman their game made at runtime: each game's own, not compared.
	here[{ REF, 8 }] = 0x0001A2B3;
	const std::vector<RadiantFill> runtime{ { 20, 0, TENPINES_WORKSHOP }, { 9, 0, TENPINES_WORKSHOP }, { 4, 1, CORVEGA }, { 6, 0, RUNTIME_NPC } };
	CHECK(SameTargets(Compare(RECRUIT01, runtime, Resolve, ours)));

	// Another place where it follows from the others (the settlement's location): a place differs, so the targets do.
	const std::vector<RadiantFill> withPlace{ { 9, 0, TENPINES_WORKSHOP }, { 2, 1, LEXINGTON }, { 4, 1, CORVEGA } };
	here[{ LOC, 2 }] = CORVEGA;
	CHECK(!SameTargets(Compare(RECRUIT01, withPlace, Resolve, ours)));
}

TEST("radiants: a friend's run starts here once; its stages are set once and never reported back, so its reward is paid once in each game")
{
	Book       book;
	const Run  theirs{ 3, 7 };
	CHECK(book.OnStart(MIN_RECRUIT01, theirs, false) == Answer::kStart);
	CHECK(!book.Knows(MIN_RECRUIT01));
	book.Starting(MIN_RECRUIT01, theirs);
	CHECK(book.IsStarting(MIN_RECRUIT01));
	CHECK(book.Knows(MIN_RECRUIT01));
	CHECK(!book.Shares(MIN_RECRUIT01));
	CHECK(book.OnStart(MIN_RECRUIT01, theirs, false) == Answer::kIgnore);  // sent again while it starts
	book.Join(MIN_RECRUIT01, theirs);
	CHECK(book.Shares(MIN_RECRUIT01));
	CHECK(book.OnStart(MIN_RECRUIT01, theirs, true) == Answer::kIgnore);

	// Their stages: each once.
	CHECK(book.Accept(MIN_RECRUIT01, theirs, 50));
	CHECK(!book.Accept(MIN_RECRUIT01, theirs, 50));
	CHECK(book.Accept(MIN_RECRUIT01, theirs, 400));  // the reward
	CHECK(!book.Accept(MIN_RECRUIT01, theirs, 400));
	// Another run's stage isn't this one's.
	CHECK((!book.Accept(MIN_RECRUIT01, Run{ 3, 8 }, 100)));
	CHECK((!book.Accept(MIN_RECRUIT01, Run{ 4, 7 }, 100)));
	// Set here because of them, the stage is done here too: not reported back (nor would it pay them twice).
	CHECK(book.Report(MIN_RECRUIT01, 400).empty());
	// One done by this player is reported under the run, once.
	CHECK((book.Report(MIN_RECRUIT01, 200) == std::vector<Run>{ theirs }));
	CHECK(book.Report(MIN_RECRUIT01, 200).empty());
	// And a friend's 200 after that changes nothing here.
	CHECK(!book.Accept(MIN_RECRUIT01, theirs, 200));

	// Completed and stopped: the run is over here. Its start sent again (someone joined) never starts it again.
	book.Ended(MIN_RECRUIT01);
	CHECK(!book.Shares(MIN_RECRUIT01));
	CHECK(book.OnStart(MIN_RECRUIT01, theirs, false) == Answer::kIgnore);
	CHECK(!book.Accept(MIN_RECRUIT01, theirs, 500));
	// Nor after a reconnect (the friend keeps their id in the session).
	book.EndSession();
	CHECK(book.OnStart(MIN_RECRUIT01, theirs, false) == Answer::kIgnore);
	// The quest handed out again is another run, with a reward of its own.
	CHECK((book.OnStart(MIN_RECRUIT01, Run{ 3, 9 }, false) == Answer::kStart));
}

TEST("radiants: this player's own run is reported with its stages so far, and friends' stages of it are taken")
{
	Book                             book;
	const std::vector<std::uint16_t> done{ 10, 20, 50 };
	const auto                       ours = book.Begin(MIN_RECRUIT02, 5, done);
	CHECK(ours.origin == 5);
	CHECK(book.Shares(MIN_RECRUIT02));
	CHECK(book.Report(MIN_RECRUIT02, 50).empty());  // went with the start
	CHECK((book.Report(MIN_RECRUIT02, 100) == std::vector<Run>{ ours }));
	CHECK(book.Accept(MIN_RECRUIT02, ours, 150));  // the friend freed the captive
	CHECK(book.Ours(5).size() == 1);
	CHECK(book.Ours(6).empty());
	// A second run of ours is numbered apart.
	book.Ended(MIN_RECRUIT02);
	CHECK((book.Begin(MIN_RECRUIT02, 5, {}).serial != ours.serial));
}

TEST("radiants: a friend's run of a quest already running here is shared only if the fills match; one turned down stays so")
{
	Book      book;
	const Run theirs{ 2, 1 };
	// Running here (started before the session, or by the player alone): compare.
	CHECK(book.OnStart(MIN_RECRUIT01, theirs, true) == Answer::kCompare);
	book.Refuse(MIN_RECRUIT01, theirs);  // another settlement here
	CHECK(!book.Shares(MIN_RECRUIT01));
	CHECK(book.OnStart(MIN_RECRUIT01, theirs, true) == Answer::kIgnore);
	CHECK(!book.Accept(MIN_RECRUIT01, theirs, 100));

	// Both players have their own run with the same targets: joined, the stages of either go.
	const auto ours = book.Begin(MIN_RECRUIT02, 9, std::vector<std::uint16_t>{ 10 });
	const Run  friends{ 2, 4 };
	CHECK(book.OnStart(MIN_RECRUIT02, friends, true) == Answer::kCompare);
	book.Join(MIN_RECRUIT02, friends);
	CHECK(book.Accept(MIN_RECRUIT02, friends, 100));
	CHECK(!book.Accept(MIN_RECRUIT02, ours, 100));  // the same stage under the other run: once
	CHECK(book.Report(MIN_RECRUIT02, 150).size() == 2);

	// A start that failed here: refused, and the run not left starting.
	book.Starting(MIN_RECRUIT01, Run{ 2, 2 });
	book.Refuse(MIN_RECRUIT01, Run{ 2, 2 });
	CHECK(!book.IsStarting(MIN_RECRUIT01));
	CHECK((book.OnStart(MIN_RECRUIT01, Run{ 2, 2 }, false) == Answer::kIgnore));
}
