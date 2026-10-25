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
	constexpr std::uint32_t RRR03 = 0x000B926B;              // Jackpot
	constexpr std::uint32_t RRR04 = 0x000B26C2;              // Mercer Safehouse
	constexpr std::uint32_t RRR05 = 0x000B926A;              // Weathervane
	constexpr std::uint32_t RRR06 = 0x001845F7;              // Randolph Safehouse
	constexpr std::uint32_t BOSR05 = 0x000D1EB2;             // Feeding the Troops
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

TEST("radiants: every faction's jobs at a settlement, a dungeon or a place are shared; those with NPCs made at runtime are not")
{
	// The Minutemen's: the locket, the hostile settlements, Preston's building, generator and synth requests.
	for (const std::uint32_t minutemen : { 0x00106F05u, 0x0015F03Fu, 0x0015F040u, 0x00161D1Du, 0x00162F44u, 0x00109D73u }) {
		CHECK(ShareOf(minutemen) == Share::kShared);
	}
	CHECK(ShareOf(0x00157577) == Share::kLeftAlone);  // a synth boss made at runtime
	CHECK(ShareOf(0x00164167) == Share::kLeftAlone);  // refugees made at runtime
	// Every settlement attack: the attackers are made at runtime.
	for (const std::uint32_t attack : { 0x000A1412u, 0x0005E51Fu, 0x00160409u, 0x00186A08u, 0x000E477Fu, 0x001533A9u, 0x000BDB04u }) {
		CHECK(ShareOf(attack) == Share::kLeftAlone);
	}
	// The Brotherhood's: Cleansing the Commonwealth, Quartermastery, Feeding the Troops.
	CHECK(ShareOf(BOSR01) == Share::kShared);
	CHECK(ShareOf(BOSR02) == Share::kShared);
	CHECK(ShareOf(BOSR05) == Share::kShared);
	CHECK(ShareOf(0x000C8675) == Share::kLeftAlone);  // Leading by Example: a squire made at runtime
	CHECK(ShareOf(0x000CF3E2) == Share::kLeftAlone);  // Learning Curve: a scribe made at runtime
	// The Institute's: Pest Control, Hypothesis, Political Leanings.
	CHECK(ShareOf(INSTR01) == Share::kShared);
	CHECK(ShareOf(0x000EB268) == Share::kShared);
	CHECK(ShareOf(0x000F0D8C) == Share::kShared);
	CHECK(ShareOf(0x000EDCEF) == Share::kLeftAlone);  // Appropriation: defenders made at runtime
	CHECK(ShareOf(0x000EDE28) == Share::kLeftAlone);  // Reclamation: the synth made at runtime
	// The Railroad's: Butcher's Bill (both parts), Concierge, Jackpot, Mercer and Randolph Safehouse, Weathervane.
	for (const std::uint32_t railroad : { RRR01A, 0x000B1F1Cu, RRR02A, RRR03, RRR04, RRR05, RRR06 }) {
		CHECK(ShareOf(railroad) == Share::kShared);
	}
	for (const std::uint32_t railroad : { 0x00186C79u, 0x0013A33Du, 0x00145F1Au, 0x0014A34Au, 0x00150577u }) {
		CHECK(ShareOf(railroad) == Share::kLeftAlone);  // a courser, bosses, the one to rescue, a patrol, a vertibird made at runtime
	}
	// Diamond City's wanted posters (misc quests).
	CHECK(ShareOf(0x0014182C) == Share::kShared);
	CHECK(ShareOf(0x0018127D) == Share::kShared);
	CHECK(ShareOf(0x00181280) == Share::kShared);
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
	// The Railroad's jobs counted on offer are (OFFERS counts them off); Randolph Safehouse's offer hides the place's
	// enemies, which a stop would leave hidden.
	CHECK(Replaces(RRR03, false, false, false));
	CHECK(Replaces(RRR04, false, false, false));
	CHECK(Replaces(RRR05, false, false, false));
	CHECK(!Replaces(RRR06, false, false, false));
	CHECK(Replaces(BOSR05, false, false, false));
	// The Minutemen's (their stop runs a stage of its own), and the radiants left alone or not radiants at all.
	CHECK(!Replaces(MIN_RECRUIT01, false, false, false));
	CHECK(!Replaces(MIN_OWNED01, false, false, false));
	CHECK(!Replaces(MIN01, false, false, false));
}

TEST("radiants: the Brotherhood's developer shortcut (its 0, which makes the player a member and moves them) is never taken from a friend")
{
	CHECK(!Takes(BOSR01, 0));
	CHECK(!Takes(BOSR02, 0));
	CHECK(!Takes(BOSR05, 0));
	CHECK(Takes(BOSR05, 30));
	CHECK(Takes(BOSR01, 10));
	CHECK(Takes(BOSR01, 50));  // the reward: once in each game
	CHECK(Takes(MIN_RECRUIT01, 0));
	CHECK(Takes(INSTR01, 0));
}

TEST("radiants: a Railroad job stopped for a friend's run while on offer is counted off the jobs on offer, as its take would have")
{
	// Mercer Safehouse: counted at its 50, off at its 75.
	CHECK(LeavesCounted(RRR04, std::vector<std::uint16_t>{ 0, 50 }, 0.0f));
	CHECK(!LeavesCounted(RRR04, std::vector<std::uint16_t>{ 0 }, 0.0f));               // not offered yet
	CHECK(!LeavesCounted(RRR04, std::vector<std::uint16_t>{ 0, 50, 75 }, 0.0f));       // taken: counted off already
	// Jackpot counts its offer only once done before (RRR03Completed above 0).
	CHECK(LeavesCounted(RRR03, std::vector<std::uint16_t>{ 0, 50 }, 1.0f));
	CHECK(!LeavesCounted(RRR03, std::vector<std::uint16_t>{ 0, 50 }, 0.0f));
	CHECK(!LeavesCounted(RRR03, std::vector<std::uint16_t>{ 0, 50, 100 }, 1.0f));
	// Weathervane only while no MILA is placed.
	CHECK(LeavesCounted(RRR05, std::vector<std::uint16_t>{ 0, 50 }, 0.0f));
	CHECK(!LeavesCounted(RRR05, std::vector<std::uint16_t>{ 0, 50 }, 2.0f));
	// Concierge counts nothing; nor do the others.
	CHECK(!LeavesCounted(RRR02A, std::vector<std::uint16_t>{ 0, 50 }, 0.0f));
	CHECK(!LeavesCounted(INSTR01, std::vector<std::uint16_t>{ 5 }, 0.0f));
	// Every quest with an offer is a shared one that may be stopped (and only such a one needs it).
	for (const auto& offer : OFFERS) {
		const auto entry = Find(offer.quest);
		REQUIRE(entry != nullptr);
		CHECK(entry->share == Share::kShared);
		CHECK(entry->replace);
		CHECK(offer.offered < offer.taken);
		CHECK(((offer.counted == Counted::kAlways) == (offer.global == 0)));
	}
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

TEST("radiants: a settler of the friend's settlement that isn't loaded here at the start is left to the others, and only noted if it differs")
{
	// BoSR05's aliases (some of them): the settlement's workshop, owned or not, pushed into Workshop; its settlers from it.
	const std::vector<Alias> bosr05{
		{ 10, REF, Fill::kSource, true, "OwnedWorkshop" },
		{ 11, REF, Fill::kSource, true, "UnownedWorkshop" },
		{ 6, REF, Fill::kByOthers, false, "Workshop" },
		{ 0, LOC, Fill::kDerived, false, "Settlement" },
		{ 4, REF, Fill::kDerived, false, "SettlementBoss" },
		{ 5, REF, Fill::kByOthers, false, "SettlementSpokesman" },
	};
	constexpr std::uint32_t THEIR_SETTLER = 0x0009B1C6;
	constexpr std::uint32_t OUR_SETTLER = 0x0009B1C7;  // the settlement's other one, which this game took
	const std::vector<RadiantFill> fills{ { 10, 0, 0 }, { 11, 0, TENPINES_WORKSHOP }, { 6, 0, TENPINES_WORKSHOP }, { 0, 1, CORVEGA }, { 4, 0, THEIR_SETTLER },
		{ 5, 0, THEIR_SETTLER } };

	bool       loaded = false;  // their settler: in memory here only once the quest runs with the settlement
	const auto resolve = [&](Kind a_kind, std::uint32_t a_form) { return a_form == THEIR_SETTLER && !loaded ? std::nullopt : Resolve(a_kind, a_form); };
	const auto plan = MakePlan(bosr05, fills, resolve);
	REQUIRE(plan.Ok());
	CHECK(plan.forces.size() == 4);  // the workshops, Workshop and the settlement
	CHECK((plan.unforced == std::vector<std::uint32_t>{ 4, 5 }));

	loaded = true;
	std::map<std::pair<Kind, std::uint32_t>, std::uint32_t> here{ { { REF, 11 }, TENPINES_WORKSHOP }, { { REF, 6 }, TENPINES_WORKSHOP }, { { LOC, 0 }, CORVEGA },
		{ { REF, 4 }, OUR_SETTLER }, { { REF, 5 }, OUR_SETTLER } };
	const auto ours = [&](Kind a_kind, std::uint32_t a_alias) {
		const auto it = here.find({ a_kind, a_alias });
		return it != here.end() ? it->second : 0u;
	};
	auto mismatches = Compare(bosr05, fills, resolve, ours, plan.unforced);
	CHECK(mismatches.size() == 2);
	CHECK(SameTargets(mismatches));
	// Without the plan's word (a run compared as it runs here), the spokesman counts.
	CHECK(!SameTargets(Compare(bosr05, fills, resolve, ours)));
	// Another settlement: the targets differ whatever the settlers.
	here[{ LOC, 0 }] = LEXINGTON;
	CHECK(!SameTargets(Compare(bosr05, fills, resolve, ours, plan.unforced)));
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
	CHECK(book.OnStart(MIN_RECRUIT01, theirs, true) == Answer::kTake);  // sent again: its stages taken, each once

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

TEST("radiants: a friend's pick whose conditions are lifted here may take a reference another quest reserves; a needed alias may not")
{
	// Forced, an alias no longer follows the others either way.
	CHECK((ForcedFlags(ALIAS_BY_OTHERS | ALIAS_OPTIONAL, true) & ALIAS_BY_OTHERS) == 0);
	CHECK((ForcedFlags(ALIAS_BY_OTHERS, false) & ALIAS_BY_OTHERS) == 0);
	// The settlement owned here and not in the friend's game: the reservation of this game's settlement quests is passed.
	CHECK((ForcedFlags(ALIAS_OPTIONAL, true) & ALIAS_ALLOW_RESERVED) != 0);
	// A needed target a story or companion quest reserves here fails the start instead of being taken from it.
	CHECK((ForcedFlags(0, false) & ALIAS_ALLOW_RESERVED) == 0);
	// An alias whose own definition allows it keeps it.
	CHECK((ForcedFlags(ALIAS_ALLOW_RESERVED, false) & ALIAS_ALLOW_RESERVED) != 0);
	CHECK((ForcedFlags(ALIAS_OPTIONAL, false) & ALIAS_OPTIONAL) != 0);
}

TEST("radiants: a run finished here and shared again under another number (a friend loaded a save inside it, or came back) is never started here again")
{
	Book                           book;
	const Run                      first{ 3, 7 };
	const std::vector<RadiantFill> fills{ { 20, 0, TENPINES_WORKSHOP }, { 4, 1, CORVEGA }, { 9, 0, RUNTIME_NPC }, { 19, 0, 0 } };
	CHECK(book.OnStart(MIN_RECRUIT01, first, false, fills) == Answer::kStart);
	book.Starting(MIN_RECRUIT01, first, fills);
	book.Join(MIN_RECRUIT01, first, fills);
	CHECK(book.Accept(MIN_RECRUIT01, first, 400));  // the reward, paid here
	book.Ended(MIN_RECRUIT01);

	// Their game loaded an autosave from inside the dungeon and shares it again, a new number, its settler made again.
	const std::vector<RadiantFill> again{ { 4, 1, CORVEGA }, { 9, 0, RUNTIME_NPC }, { 20, 0, TENPINES_WORKSHOP } };
	CHECK((book.OnStart(MIN_RECRUIT01, Run{ 3, 8 }, false, again) == Answer::kDone));
	// After a reconnect, and after this game loads a save too (one from before the reward would pay it twice).
	book.EndSession();
	CHECK((book.OnStart(MIN_RECRUIT01, Run{ 3, 9 }, false, again) == Answer::kDone));
	book.Reloaded();
	CHECK((book.OnStart(MIN_RECRUIT01, Run{ 3, 10 }, false, again) == Answer::kDone));
	CHECK(book.OnStart(MIN_RECRUIT01, first, false, fills) == Answer::kIgnore);
	// Another place is another run, with a reward of its own.
	const std::vector<RadiantFill> lexington{ { 20, 0, TENPINES_WORKSHOP }, { 4, 1, LEXINGTON } };
	CHECK((book.OnStart(MIN_RECRUIT01, Run{ 3, 11 }, false, lexington) == Answer::kStart));
	// Running here, it is compared as ever (it may be this game's own run of it).
	CHECK((book.OnStart(MIN_RECRUIT01, Run{ 3, 12 }, true, again) == Answer::kCompare));
	// Another quest with the same place is its own.
	CHECK((book.OnStart(MIN_RECRUIT02, Run{ 3, 13 }, false, again) == Answer::kStart));

	// This game's own run, finished: the same.
	const std::vector<RadiantFill> ours{ { 2, 1, POLICE_STATION } };
	book.Begin(BOSR01, 5, {}, ours);
	book.Ended(BOSR01);
	CHECK((book.OnStart(BOSR01, Run{ 3, 14 }, false, ours) == Answer::kDone));

	// One joined while it ran here (both games had the same targets), finished: the same.
	const std::vector<RadiantFill> joined{ { 1, 1, LEXINGTON } };
	CHECK((book.OnStart(INSTR01, Run{ 3, 15 }, true, joined) == Answer::kCompare));
	book.Join(INSTR01, Run{ 3, 15 }, joined);
	book.Ended(INSTR01);
	CHECK((book.OnStart(INSTR01, Run{ 3, 16 }, false, joined) == Answer::kDone));
}

TEST("radiants: the same targets are a run shared again only for a while after one finished here; later they are a new run, which pays again")
{
	using namespace std::chrono_literals;
	const Book::Time               t0 = Book::Time{} + 100h;
	Book                           book;
	const Run                      first{ 3, 7 };
	const std::vector<RadiantFill> fills{ { 20, 0, TENPINES_WORKSHOP }, { 4, 1, CORVEGA } };
	book.Starting(MIN_RECRUIT01, first, fills);
	book.Join(MIN_RECRUIT01, first, fills);
	CHECK(book.Accept(MIN_RECRUIT01, first, 400));
	book.Ended(MIN_RECRUIT01, t0);

	// Shared again under a new number soon after (a save from before its share): the same run.
	CHECK((book.OnStart(MIN_RECRUIT01, Run{ 3, 8 }, false, fills, t0 + 5min) == Answer::kDone));
	CHECK((book.OnStart(MIN_RECRUIT01, Run{ 3, 8 }, false, fills, t0 + Book::SAME_RUN_WINDOW - 1s) == Answer::kDone));
	// The quest handed out again an hour later for Corvega once more: a new run, shared and paid.
	CHECK((book.OnStart(MIN_RECRUIT01, Run{ 3, 9 }, false, fills, t0 + 1h) == Answer::kStart));
	// Across sessions and loads the same.
	book.EndSession();
	book.Reloaded();
	CHECK((book.OnStart(MIN_RECRUIT01, Run{ 4, 2 }, false, fills, t0 + Book::SAME_RUN_WINDOW) == Answer::kStart));
	// The run itself is never started again, however late.
	CHECK((book.OnStart(MIN_RECRUIT01, first, false, fills, t0 + 100h) == Answer::kIgnore));

	// That new run finished too: its targets known again from then.
	book.Starting(MIN_RECRUIT01, Run{ 3, 9 }, fills);
	book.Join(MIN_RECRUIT01, Run{ 3, 9 }, fills);
	book.Ended(MIN_RECRUIT01, t0 + 2h);
	CHECK((book.OnStart(MIN_RECRUIT01, Run{ 3, 10 }, false, fills, t0 + 2h + 1min) == Answer::kDone));
}

TEST("radiants: a save keeps the runs shared and finished, so a game that restarts never starts a run it finished, and one that loads a save inside a run shares it as the same run")
{
	constexpr std::uint64_t        SESSION = 0x1234'5678'9ABC'DEF0;
	constexpr std::uint64_t        OTHER = 0x0FED'CBA9'8765'4321;
	const std::vector<RadiantFill> fills{ { 20, 0, TENPINES_WORKSHOP }, { 4, 1, CORVEGA } };
	const auto running = [&](std::uint32_t a_quest) -> std::optional<QuestHere> {
		if (a_quest == MIN_RECRUIT01) {
			return QuestHere{ { 10, 100 }, fills };
		}
		return std::nullopt;  // not running in the loaded world
	};

	// Bob's game shares a run of raider troubles; Alice's starts it, and both play it.
	Book bob{ SerialSeed(1792000000) };
	bob.Bind(SESSION);
	const auto run = bob.Begin(MIN_RECRUIT01, 2, std::vector<std::uint16_t>{ 10 }, fills);
	const auto bos = bob.Begin(BOSR01, 2, {});  // and another of his, over in his save
	const auto bobSave = bob.Save(SESSION);       // he saves inside it
	CHECK(bobSave.session == SESSION);
	CHECK(bobSave.shared.size() == 2);

	Book alice;
	alice.Bind(SESSION);
	CHECK((alice.OnStart(MIN_RECRUIT01, run, false, fills) == Answer::kStart));
	alice.Starting(MIN_RECRUIT01, run, fills);
	alice.Join(MIN_RECRUIT01, run, fills);
	CHECK(alice.Accept(MIN_RECRUIT01, run, 400));  // paid
	alice.Ended(MIN_RECRUIT01);
	const auto aliceSave = alice.Save(SESSION);
	CHECK((aliceSave.finished == std::vector<Saved::Finished>{ { SESSION, { run } } }));
	CHECK(aliceSave.shared.empty());

	// Alice's game restarts, loads her save and joins again: the run is finished there, by its id, however late.
	Book restarted;
	restarted.Remember(aliceSave);
	restarted.Bind(SESSION);
	CHECK(restarted.Finished() == 1);
	CHECK((restarted.OnStart(MIN_RECRUIT01, run, false, fills, Book::Time{} + std::chrono::hours{ 5 }) == Answer::kIgnore));
	// In another session the ids are others' runs.
	restarted.Bind(OTHER);
	CHECK(restarted.Finished() == 0);
	CHECK((restarted.OnStart(MIN_RECRUIT01, run, false) == Answer::kStart));

	// Bob's game loads his save from inside the run (his game restarted too): the same run, shared again as it was, not
	// a new number; his stages done there need no report, and a friend's are taken.
	Book bobAgain{ SerialSeed(1792003600) };
	bobAgain.Bind(SESSION);
	bobAgain.Remember(bobSave);
	CHECK(bobAgain.Restore(bobSave, running) == 1);
	CHECK(bobAgain.Shares(MIN_RECRUIT01));
	CHECK((bobAgain.Ours(2) == std::vector<std::pair<std::uint32_t, Run>>{ { MIN_RECRUIT01, run } }));
	CHECK((bobAgain.Shared() == std::vector<std::pair<std::uint32_t, Run>>{ { MIN_RECRUIT01, run } }));
	CHECK(bobAgain.Report(MIN_RECRUIT01, 100).empty());
	CHECK((bobAgain.Report(MIN_RECRUIT01, 200) == std::vector<Run>{ run }));
	CHECK(bobAgain.Accept(MIN_RECRUIT01, run, 150));
	// The other is over in his world: finished there, never started again.
	CHECK(!bobAgain.Knows(BOSR01));
	CHECK((bobAgain.OnStart(BOSR01, bos, false) == Answer::kIgnore));
	// Its targets are kept with it: finished, a share of it under a new number soon after is known.
	bobAgain.Ended(MIN_RECRUIT01);
	CHECK((bobAgain.OnStart(MIN_RECRUIT01, Run{ 3, 1 }, false, fills) == Answer::kDone));

	// A restore never takes over a quest known here already, nor shares one not asked to, nor one of another session.
	Book busy;
	busy.Bind(SESSION);
	busy.Begin(MIN_RECRUIT01, 2, {}, fills);
	CHECK(busy.Restore(bobSave, running) == 0);
	Book unshared;
	unshared.Bind(SESSION);
	CHECK(unshared.Restore(bobSave, running, false) == 0);
	CHECK(!unshared.Knows(MIN_RECRUIT01));
	Book elsewhere;
	elsewhere.Bind(OTHER);
	CHECK(elsewhere.Restore(bobSave, running) == 0);
	CHECK(!elsewhere.Knows(MIN_RECRUIT01));
	elsewhere.Bind(SESSION);
	CHECK((elsewhere.OnStart(BOSR01, bos, false) == Answer::kIgnore));  // over in that world: finished in its session

	// Kept through the bytes.
	CHECK((Decode(Encode(bobSave)) == std::optional<Saved>{ bobSave }));
	CHECK((Decode(Encode(aliceSave)) == std::optional<Saved>{ aliceSave }));
}

TEST("radiants: a shared run the player finished alone after leaving the session is finished here, so a friend still on it never starts it here again")
{
	constexpr std::uint64_t        SESSION = 77;
	const std::vector<RadiantFill> fills{ { 20, 0, TENPINES_WORKSHOP }, { 4, 1, CORVEGA } };
	const Run                      theirs{ 2, 40 };
	Book                           alice;
	alice.Bind(SESSION);
	alice.Starting(MIN_RECRUIT01, theirs, fills);
	alice.Join(MIN_RECRUIT01, theirs, fills);

	// The connection drops mid-run: what was shared is kept (RadiantSync holds it), and the session's runs go.
	const auto kept = alice.Save(SESSION);
	alice.EndSession();
	CHECK(!alice.Knows(MIN_RECRUIT01));
	// She clears Corvega alone, paid in her own world, and joins again.
	const auto over = [](std::uint32_t) -> std::optional<QuestHere> { return std::nullopt; };
	alice.Bind(SESSION);
	CHECK(alice.Restore(kept, over) == 0);
	// Bob, still on it, sends its start again: ignored, by its id, however late.
	CHECK((alice.OnStart(MIN_RECRUIT01, theirs, false, fills, Book::Time{} + std::chrono::hours{ 3 }) == Answer::kIgnore));
	CHECK((alice.Save(SESSION).finished == std::vector<Saved::Finished>{ { SESSION, { theirs } } }));
}

TEST("radiants: a friend's run taken back from our save takes the stages their game sends again with its start, so ours catches up")
{
	constexpr std::uint64_t        SESSION = 77;
	const std::vector<RadiantFill> fills{ { 20, 0, TENPINES_WORKSHOP }, { 4, 1, CORVEGA } };
	const Run                      theirs{ 2, 40 };  // Bob's run; Alice joined it

	// Alice saved at stage 100; both cleared Corvega since (200). She loads the save: the run is taken back from it,
	// her world's stages done so far.
	Book alice;
	alice.Bind(SESSION);
	alice.Starting(MIN_RECRUIT01, theirs, fills);
	alice.Join(MIN_RECRUIT01, theirs, fills);
	const auto save = alice.Save(SESSION);
	alice.Reloaded();
	const auto loaded = [&](std::uint32_t) -> std::optional<QuestHere> { return QuestHere{ { 10, 100 }, fills }; };
	CHECK(alice.Restore(save, loaded) == 1);

	// Bob's game sends its start again (her load told it), with its stages done: 10, 100 and 200. Their stage packet of 200
	// comes after the start, while it waits its turn here, and goes with it.
	std::vector<std::uint16_t> stages{ 10, 100, 200 };
	stages.push_back(200);
	CHECK((alice.OnStart(MIN_RECRUIT01, theirs, true, fills) == Answer::kTake));
	std::vector<std::uint16_t> set;
	for (const auto stage : stages) {
		if (alice.Accept(MIN_RECRUIT01, theirs, stage)) {
			set.push_back(stage);
		}
	}
	// Only 200 is set here: 10 and 100 are done in her world, and 200 once.
	CHECK((set == std::vector<std::uint16_t>{ 200 }));
	CHECK(alice.Report(MIN_RECRUIT01, 200).empty());  // never reported back
	CHECK((alice.Report(MIN_RECRUIT01, 300) == std::vector<Run>{ theirs }));  // hers from now on go to Bob

	// A run still starting here is ignored as ever, as is one finished here.
	Book starting;
	starting.Starting(MIN_RECRUIT01, theirs, fills);
	CHECK((starting.OnStart(MIN_RECRUIT01, theirs, false, fills) == Answer::kIgnore));
	alice.Ended(MIN_RECRUIT01);
	CHECK((alice.OnStart(MIN_RECRUIT01, theirs, false, fills) == Answer::kIgnore));
}

TEST("radiants: a run of a session the player is not in (or where radiants aren't shared) is kept while it runs, and finished in its session once over")
{
	constexpr std::uint64_t        FIRST = 77;
	constexpr std::uint64_t        SECOND = 88;
	const std::vector<RadiantFill> fills{ { 20, 0, TENPINES_WORKSHOP }, { 4, 1, CORVEGA } };
	const Run                      theirs{ 2, 40 };
	Book                           alice;
	alice.Bind(FIRST);
	alice.Starting(MIN_RECRUIT01, theirs, fills);
	alice.Join(MIN_RECRUIT01, theirs, fills);
	const auto held = alice.Save(FIRST);
	alice.EndSession();

	// She joins another friend's server mid-run: the run still runs here, but isn't that session's. Kept.
	alice.Bind(SECOND);
	const auto running = [&](std::uint32_t) -> std::optional<QuestHere> { return QuestHere{ { 10 }, fills }; };
	std::vector<Saved::Quest> kept;
	CHECK(alice.Restore(held, running, true, {}, &kept) == 0);
	CHECK(!alice.Knows(MIN_RECRUIT01));
	CHECK((kept == held.shared));
	// Radiants not shared in a session of its own: kept too.
	std::vector<Saved::Quest> unshared;
	Book off;
	off.Bind(FIRST);
	CHECK(off.Restore(held, running, false, {}, &unshared) == 0);
	CHECK((unshared == held.shared));

	// She finishes it there: over in this world, finished in the first session.
	const auto over = [](std::uint32_t) -> std::optional<QuestHere> { return std::nullopt; };
	std::vector<Saved::Quest> still;
	CHECK(alice.Restore(Saved{ FIRST, kept, {} }, over, false, {}, &still) == 0);
	CHECK(still.empty());
	CHECK((alice.OnStart(MIN_RECRUIT01, theirs, false) == Answer::kStart));  // in this session, the id is someone else's
	alice.Bind(FIRST);
	CHECK((alice.OnStart(MIN_RECRUIT01, theirs, false, fills, Book::Time{} + std::chrono::hours{ 1 }) == Answer::kIgnore));  // back: never again

	// A kept run goes into the save with its own session.
	const auto bytes = Encode(Saved{ SECOND, kept, {} });
	const auto read = Decode(bytes);
	CHECK(read.has_value());
	CHECK(read->shared.front().session == FIRST);
}

TEST("radiants: a save's run is given back only to the quest running with its targets; one handed out again for another place is a new run")
{
	constexpr std::uint64_t        SESSION = 77;
	const std::vector<RadiantFill> corvega{ { 20, 0, TENPINES_WORKSHOP }, { 4, 1, CORVEGA } };
	const std::vector<RadiantFill> lexington{ { 20, 0, TENPINES_WORKSHOP }, { 4, 1, LEXINGTON } };
	const Run                      theirs{ 2, 40 };
	Book                           alice;
	alice.Bind(SESSION);
	alice.Starting(MIN_RECRUIT01, theirs, corvega);
	alice.Join(MIN_RECRUIT01, theirs, corvega);
	const auto kept = alice.Save(SESSION);
	alice.EndSession();

	// Alone, she finished Corvega and Preston sent her to Lexington: the quest runs, but for another run.
	const auto again = [&](std::uint32_t) -> std::optional<QuestHere> { return QuestHere{ { 10 }, lexington }; };
	alice.Bind(SESSION);
	CHECK(alice.Restore(kept, again) == 0);
	CHECK(!alice.Knows(MIN_RECRUIT01));  // her new run is begun as her own and shared
	CHECK((alice.OnStart(MIN_RECRUIT01, theirs, true, corvega) == Answer::kIgnore));
	CHECK(alice.Report(MIN_RECRUIT01, 100).empty());  // nothing of it goes under Bob's run
	// The same targets: the same run.
	Book bob;
	bob.Bind(SESSION);
	const auto same = [&](std::uint32_t) -> std::optional<QuestHere> { return QuestHere{ { 10 }, corvega }; };
	CHECK(bob.Restore(kept, same) == 1);
	CHECK(bob.Shares(MIN_RECRUIT01));
}

TEST("radiants: runs finished in a session stay known there after a visit to another server")
{
	const std::vector<RadiantFill> fills{ { 4, 1, CORVEGA } };
	const Run                      first{ 2, 40 };
	Book                           alice;
	alice.Bind(1);
	alice.Starting(MIN_RECRUIT01, first, fills);
	alice.Join(MIN_RECRUIT01, first, fills);
	alice.Ended(MIN_RECRUIT01);

	// Another friend's server: its player 2 is someone else, whose runs are new here.
	alice.Clear();
	alice.Bind(2);
	CHECK((alice.OnStart(MIN_RECRUIT01, first, false) == Answer::kStart));
	alice.Starting(MIN_RECRUIT01, first);
	alice.Join(MIN_RECRUIT01, first);
	alice.Ended(MIN_RECRUIT01);

	// Back on the first: its run is still finished here, and so in her save.
	alice.Clear();
	alice.Bind(1);
	CHECK((alice.OnStart(MIN_RECRUIT01, first, false, fills, Book::Time{} + std::chrono::hours{ 2 }) == Answer::kIgnore));
	const auto save = alice.Save(1);
	CHECK((save.finished == std::vector<Saved::Finished>{ { 2, { first } }, { 1, { first } } }));  // the one played last, last

	// A game that restarted and loaded that save knows both, each in its own session.
	Book restarted;
	restarted.Remember(save);
	restarted.Bind(1);
	CHECK((restarted.OnStart(MIN_RECRUIT01, first, false) == Answer::kIgnore));
	restarted.Bind(2);
	CHECK((restarted.OnStart(MIN_RECRUIT01, first, false) == Answer::kIgnore));
	restarted.Bind(3);
	CHECK((restarted.OnStart(MIN_RECRUIT01, first, false) == Answer::kStart));

	// The save keeps the last sessions played.
	Book many;
	for (std::uint64_t session = 1; session <= MAX_SAVED_SESSIONS + 2; ++session) {
		many.Bind(session);
		many.Starting(MIN_RECRUIT01, first);
		many.Join(MIN_RECRUIT01, first);
		many.Ended(MIN_RECRUIT01);
	}
	const auto last = many.Save(0);
	CHECK(last.finished.size() == MAX_SAVED_SESSIONS);
	CHECK(last.finished.front().session == 3);
	CHECK(last.finished.back().session == MAX_SAVED_SESSIONS + 2);
}

TEST("radiants: a save's record of the runs reads back only whole, and within its bounds")
{
	Saved saved{ 7, {}, {} };
	for (std::uint32_t i = 0; i < MAX_SAVED_QUESTS + 5; ++i) {
		Saved::Quest quest{ 0x1000 + i, {}, {} };
		for (std::uint32_t r = 0; r < MAX_SAVED_RUNS + 3; ++r) {
			quest.runs.push_back({ 2, 100 + r });
		}
		quest.targets.push_back({ 4, 1, CORVEGA });
		saved.shared.push_back(std::move(quest));
	}
	for (std::uint64_t session = 1; session <= MAX_SAVED_SESSIONS + 1; ++session) {
		Saved::Finished finished{ session, {} };
		for (std::uint32_t i = 0; i < MAX_SAVED_FINISHED + 10; ++i) {
			finished.runs.push_back({ 3, i });
		}
		saved.finished.push_back(std::move(finished));
	}
	const auto read = Decode(Encode(saved));
	CHECK(read.has_value());
	CHECK(read->session == 7);
	CHECK(read->shared.size() == MAX_SAVED_QUESTS);
	CHECK(read->shared.front().runs.size() == MAX_SAVED_RUNS);
	CHECK((read->shared.front().runs.back() == Run{ 2, 100 + MAX_SAVED_RUNS + 2 }));  // the latest kept
	CHECK(read->finished.size() == MAX_SAVED_SESSIONS);
	CHECK(read->finished.front().session == 2);  // the most recent kept
	CHECK(read->finished.front().runs.size() == MAX_SAVED_FINISHED);
	CHECK((read->finished.front().runs.back() == Run{ 3, MAX_SAVED_FINISHED + 9 }));
	CHECK((read->finished.front().runs.front() == Run{ 3, 10 }));

	// Damaged: cut short, a byte too many, another version.
	auto bytes = Encode(Saved{ 7, { { MIN_RECRUIT01, { { 2, 5 } }, { { 4, 1, CORVEGA } } } }, { { 7, { { 3, 4 } } } } });
	CHECK(Decode(bytes).has_value());
	CHECK(!Decode(std::span{ bytes }.first(bytes.size() - 1)).has_value());
	auto longer = bytes;
	longer.push_back(0);
	CHECK(!Decode(longer).has_value());
	bytes[0] = 2;
	CHECK(!Decode(bytes).has_value());
	CHECK(!Decode({}).has_value());

	// A book that finished more than the bound keeps the latest in its save, and all of them itself.
	Book book;
	for (std::uint32_t i = 0; i < MAX_SAVED_FINISHED + 4; ++i) {
		book.Starting(MIN_RECRUIT01, Run{ 3, i });
		book.Join(MIN_RECRUIT01, Run{ 3, i });
		book.Ended(MIN_RECRUIT01);
	}
	const auto save = book.Save(7);
	CHECK(save.finished.size() == 1);
	CHECK(save.finished.front().runs.size() == MAX_SAVED_FINISHED);
	CHECK((save.finished.front().runs.front() == Run{ 3, 4 }));
	CHECK(book.Finished() == MAX_SAVED_FINISHED + 4);
	CHECK((book.OnStart(MIN_RECRUIT01, Run{ 3, 0 }, false) == Answer::kIgnore));
}

TEST("radiants: a run's targets are its places, or with none its references, but never one made at runtime or empty")
{
	const std::vector<RadiantFill> fills{ { 9, 0, RUNTIME_NPC }, { 4, 1, CORVEGA }, { 20, 0, TENPINES_WORKSHOP }, { 6, 0, SPAWN }, { 3, 1, LEXINGTON }, { 7, 1, 0 } };
	CHECK((TargetsOf(fills) == std::vector<RadiantFill>{ { 3, 1, LEXINGTON }, { 4, 1, CORVEGA } }));
	const std::vector<RadiantFill> people{ { 9, 0, RUNTIME_NPC }, { 20, 0, TENPINES_WORKSHOP }, { 6, 0, SPAWN }, { 2, 0, SANCTUARY_WORKSHOP } };
	CHECK((TargetsOf(people) == std::vector<RadiantFill>{ { 2, 0, SANCTUARY_WORKSHOP }, { 20, 0, TENPINES_WORKSHOP } }));
	// Nothing to know a run by: never taken for one finished here.
	const std::vector<RadiantFill> nameless{ { 9, 0, RUNTIME_NPC }, { 6, 0, SPAWN }, { 7, 1, 0 } };
	CHECK(TargetsOf(nameless).empty());
	Book book;
	book.Begin(MIN_RECRUIT01, 5, {}, nameless);
	book.Ended(MIN_RECRUIT01);
	CHECK((book.OnStart(MIN_RECRUIT01, Run{ 3, 1 }, false, nameless) == Answer::kStart));
}

TEST("radiants: a game's run numbers start at its own start, so one that restarts, its player given back their id, never numbers a run as one of its last")
{
	Book       before{ SerialSeed(1792000000) };
	const auto first = before.Begin(MIN_RECRUIT01, 3, {});
	before.Ended(MIN_RECRUIT01);
	const auto second = before.Begin(MIN_RECRUIT01, 3, {});

	// A friend's game ran the first and finished it, and still runs the second (Tenpines Bluff).
	Book friends;
	friends.Starting(MIN_RECRUIT01, first);
	friends.Join(MIN_RECRUIT01, first);
	friends.Ended(MIN_RECRUIT01);
	friends.Starting(MIN_RECRUIT01, second);
	friends.Join(MIN_RECRUIT01, second);

	// The game crashes and comes back a minute later as player 3 again, and its quest is handed out for Corvega.
	Book       after{ SerialSeed(1792000060) };
	const auto next = after.Begin(MIN_RECRUIT01, 3, {});
	CHECK(next != first);
	CHECK(next != second);
	CHECK(next.serial > second.serial);
	CHECK(!friends.Accept(MIN_RECRUIT01, next, 400));  // its stages aren't the Tenpines run's
	CHECK(friends.Accept(MIN_RECRUIT01, second, 400));
	friends.Ended(MIN_RECRUIT01);
	CHECK(friends.OnStart(MIN_RECRUIT01, next, false) == Answer::kStart);  // nor is it the first, finished there

	// They go on through a load and another server.
	after.Reloaded();
	after.Clear();
	CHECK(after.Begin(MIN_RECRUIT01, 3, {}).serial > next.serial);
}

TEST("radiants: a run shared here and shared again under a new number is joined as the same, and said to be (its game is sent the stages done here)")
{
	Book                           book;
	const std::vector<RadiantFill> fills{ { 20, 0, TENPINES_WORKSHOP }, { 4, 1, CORVEGA } };
	const Run                      first{ 3, 7 };
	CHECK(!book.Join(MIN_RECRUIT01, first, fills));  // found running here with their fills: the first share
	CHECK(book.Report(MIN_RECRUIT01, 100).size() == 1);
	CHECK(!book.Join(MIN_RECRUIT01, first, fills));  // the same run again
	// Their game loaded a save from before this game's 100 and shares it again: this game's stages go to the new number.
	const Run again{ 3, 8 };
	CHECK(book.OnStart(MIN_RECRUIT01, again, true, fills) == Answer::kCompare);
	CHECK(book.Join(MIN_RECRUIT01, again, fills));
	CHECK(book.Report(MIN_RECRUIT01, 200).size() == 2);
	// One started here from a friend's run is a first share.
	CHECK(book.OnStart(MIN_RECRUIT02, again, false, fills) == Answer::kStart);
	book.Starting(MIN_RECRUIT02, again, fills);
	CHECK(!book.Join(MIN_RECRUIT02, again, fills));
}
