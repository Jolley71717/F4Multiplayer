#include "Test.h"

#include "game/RadiantRules.h"
#include "game/StageRules.h"

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

TEST("radiants: the Minutemen's settlement requests are shared by their run; the Castle's siege (the host's, as the attacks) and the factions' other radiants are left alone")
{
	CHECK(ShareOf(0, MIN_RECRUIT01) == Share::kShared);
	CHECK(ShareOf(0, MIN_RECRUIT02) == Share::kShared);
	CHECK(ShareOf(0, 0x0003DF95) == Share::kShared);  // MinRadiantOwned02: raiders near a settlement of the player's
	CHECK(ShareOf(0, 0x0003E0C1) == Share::kShared);  // MinRadiantOwned03: kidnapping
	// A settlement attacked: the host's run shared (Entry::attack). The Castle's siege: not a shared run (AttackBook::SIEGES).
	CHECK(ShareOf(0, MIN_OWNED01) == Share::kShared);
	CHECK(ShareOf(0, MIN_DEFEND_CASTLE) == Share::kLeftAlone);
	// Not radiants: QuestFilter decides.
	CHECK(ShareOf(0, MQ104) == Share::kNotRadiant);
	CHECK(ShareOf(0, MIN01) == Share::kNotRadiant);
	CHECK(!IsRadiant(0, MIN01));
	CHECK(IsRadiant(0, MIN_OWNED01));
	// Every entry once (by its file and form ID there), of a file the rules know, with a form ID that has no load index.
	std::set<std::pair<std::uint8_t, std::uint32_t>> seen;
	for (const auto& entry : RADIANTS) {
		CHECK(seen.insert({ entry.file, entry.quest }).second);
		CHECK(entry.file <= 3);
		CHECK((entry.quest >> 24) == 0);
		CHECK(!entry.editorId.empty());
		// Only a shared radiant's run is ever stopped for a friend's.
		CHECK((!entry.replace || entry.share == Share::kShared));
	}
}

TEST("radiants: every faction's jobs at a settlement, a dungeon or a place are shared; those with NPCs made at runtime are not")
{
	// The Minutemen's: the locket, the hostile settlements, Preston's building, generator and synth requests.
	for (const std::uint32_t minutemen : { 0x00106F05u, 0x0015F03Fu, 0x0015F040u, 0x00161D1Du, 0x00162F44u, 0x00109D73u }) {
		CHECK(ShareOf(0, minutemen) == Share::kShared);
	}
	// The synth boss and the refugees made at runtime are the owner's (Entry::spawned), never started again here.
	for (const std::uint32_t spawned : { 0x00157577u, 0x00164167u }) {
		const auto entry = Find(0, spawned);
		REQUIRE(entry != nullptr);
		CHECK(entry->share == Share::kShared);
		CHECK(entry->spawned);
		CHECK(!entry->replace);
		CHECK(!entry->attack);
		CHECK(!Restarts(spawned));
	}
	// Preston's settlement attacks: the host's run (Entry::attack); its attackers are copies in the friends' games already,
	// so none is followed or forced (not spawned), and a friend's own run is never stopped for it.
	for (const std::uint32_t attack : { 0x000A1412u, 0x0005E51Fu, 0x00160409u, 0x00186A08u, 0x000E477Fu, 0x001533A9u }) {
		const auto entry = Find(0, attack);
		REQUIRE(entry != nullptr);
		CHECK(entry->share == Share::kShared);
		CHECK(entry->attack);
		CHECK(!entry->spawned);
		CHECK(!entry->replace);
	}
	// The Castle's siege, its starter and its waves' helper: no shared run (the host decides the siege: AttackBook::SIEGES).
	for (const std::uint32_t castle : { 0x000BDB04u, 0x00176328u, 0x0022FD6Eu }) {
		CHECK(ShareOf(0, castle) == Share::kLeftAlone);
	}
	// Only those six are attacks.
	CHECK(std::ranges::count_if(RADIANTS, [](const Entry& a_entry) { return a_entry.attack; }) == 6);
	// The Brotherhood's: Cleansing the Commonwealth, Quartermastery, Feeding the Troops.
	CHECK(ShareOf(0, BOSR01) == Share::kShared);
	CHECK(ShareOf(0, BOSR02) == Share::kShared);
	CHECK(ShareOf(0, BOSR05) == Share::kShared);
	// Leading by Example and Learning Curve: the squire and the scribe made at the airport walk with the owner (Entry::spawned);
	// the scribe's reading event (BoSR04_AO_Scribe) only sets Learning Curve's stages, which go by that run: left alone.
	for (const std::uint32_t brotherhood : { 0x000C8675u, 0x000CF3E2u }) {
		const auto entry = Find(0, brotherhood);
		REQUIRE(entry != nullptr);
		CHECK(entry->share == Share::kShared);
		CHECK(entry->spawned);
		CHECK(entry->replace);
	}
	CHECK(ShareOf(0, 0x000CF3E3) == Share::kLeftAlone);
	// The Institute's: Pest Control, Hypothesis, Political Leanings.
	CHECK(ShareOf(0, INSTR01) == Share::kShared);
	CHECK(ShareOf(0, 0x000EB268) == Share::kShared);
	CHECK(ShareOf(0, 0x000F0D8C) == Share::kShared);
	// Appropriation and Reclamation: the assault's defenders and the synth made at the start are the owner's (Entry::spawned).
	for (const std::uint32_t institute : { 0x000EDCEFu, 0x000EDE28u }) {
		const auto entry = Find(0, institute);
		REQUIRE(entry != nullptr);
		CHECK(entry->share == Share::kShared);
		CHECK(entry->spawned);
		CHECK(entry->replace);
	}
	CHECK(ShareOf(0, 0x000E8D19) == Share::kLeftAlone);  // InstR01_OLD, InstR03_OLD: not used by the game
	CHECK(ShareOf(0, 0x000EC1E9) == Share::kLeftAlone);
	// The Railroad's: Butcher's Bill (both parts), Concierge, Jackpot, Mercer and Randolph Safehouse, Weathervane.
	for (const std::uint32_t railroad : { RRR01A, 0x000B1F1Cu, RRR02A, RRR03, RRR04, RRR05, RRR06 }) {
		CHECK(ShareOf(0, railroad) == Share::kShared);
	}
	// Variable Removal, To the Mattresses, Lost Soul, A Clean Equation: a courser, a boss, a captive, a patrol made at runtime
	// at the place: shared by their run, the owner's actors standing in every game (Entry::spawned).
	for (const std::uint32_t railroad : { 0x00186C79u, 0x0013A33Du, 0x00145F1Au, 0x0014A34Au }) {
		const auto entry = Find(0, railroad);
		REQUIRE(entry != nullptr);
		CHECK(entry->share == Share::kShared);
		CHECK(entry->spawned);
		CHECK(entry->replace);
	}
	CHECK(ShareOf(0, 0x00150577) == Share::kLeftAlone);  // High Ground: a vertibird and its crew made at runtime, flown by a scene
	// Only a shared radiant's actors are the owner's; the ones shared before have none made at runtime that matter.
	for (const auto& entry : RADIANTS) {
		CHECK((!entry.spawned || entry.share == Share::kShared));
	}
	CHECK(!Find(0, RRR03)->spawned);
	CHECK(!Find(0, MIN_RECRUIT01)->spawned);
	// Diamond City's wanted posters (misc quests).
	CHECK(ShareOf(0, 0x0014182C) == Share::kShared);
	CHECK(ShareOf(0, 0x0018127D) == Share::kShared);
	CHECK(ShareOf(0, 0x00181280) == Share::kShared);
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
	// The jobs with actors made at runtime are offered the same way (counted at 50, off at their take).
	CHECK(Replaces(0x00186C79, false, false, false));   // Variable Removal
	CHECK(Replaces(0x0013A33D, false, false, false));   // To the Mattresses
	CHECK(!Replaces(0x00186C79, false, true, false));   // given here: ours goes on
	CHECK(!Replaces(0x00150577, false, false, false));  // High Ground: left alone
	CHECK(Replaces(BOSR05, false, false, false));
	// The Minutemen's (their stop runs a stage of its own), and the radiants left alone or not radiants at all.
	CHECK(!Replaces(MIN_RECRUIT01, false, false, false));
	CHECK(!Replaces(MIN_OWNED01, false, false, false));
	CHECK(!Replaces(MIN01, false, false, false));
}

TEST("radiants: the developers' shortcuts (the Brotherhood's 0, which makes the player a member and moves them; Appropriation's, which puts them in the Institute) are never taken from a friend")
{
	constexpr std::uint32_t BOSR03 = 0x000C8675;      // Leading by Example
	constexpr std::uint32_t BOSR04 = 0x000CF3E2;      // Learning Curve
	constexpr std::uint32_t INSTR03NEW = 0x000EDCEF;  // Appropriation
	constexpr std::uint32_t INSTR04 = 0x000EDE28;     // Reclamation
	CHECK(!Takes(BOSR01, 0));
	CHECK(!Takes(BOSR02, 0));
	CHECK(!Takes(BOSR05, 0));
	// The squire's and the scribe's jobs: their 0 sets the Brotherhood's story and the main quest's stages too, and moves
	// the player to Kells or Quinlan. Their start-up stage (BoSR03's 5; Quinlan's line sets BoSR04's 10) and the rest are taken.
	CHECK(!Takes(BOSR03, 0));
	CHECK(!Takes(BOSR04, 0));
	CHECK(Takes(BOSR03, 5));
	CHECK(Takes(BOSR04, 10));
	CHECK(Takes(BOSR03, 50));
	CHECK(Takes(BOSR04, 70));
	// Appropriation's 0 puts the player in the Institute's faction; its 10 is its start-up stage. Reclamation's 0 only sets
	// its 5, as Pest Control's does.
	CHECK(!Takes(INSTR03NEW, 0));
	CHECK(Takes(INSTR03NEW, 10));
	CHECK(Takes(INSTR03NEW, 100));
	CHECK(Takes(INSTR04, 0));
	CHECK(Takes(BOSR05, 30));
	CHECK(Takes(BOSR01, 10));
	CHECK(Takes(BOSR01, 50));  // the reward: once in each game
	CHECK(Takes(MIN_RECRUIT01, 0));
	CHECK(Takes(INSTR01, 0));
	// Every untaken stage is a shared radiant's.
	for (const auto& untaken : UNTAKEN) {
		const auto entry = Find(0, untaken.quest);
		REQUIRE(entry != nullptr);
		CHECK(entry->share == Share::kShared);
	}
}

TEST("radiants: Learning Curve and the Minutemen's synth boss, offered only while they aren't running, are never started again by this game; the others are")
{
	constexpr std::uint32_t BOSR03 = 0x000C8675;
	constexpr std::uint32_t BOSR04 = 0x000CF3E2;
	CHECK(!Restarts(BOSR04));
	CHECK(!Restarts(0x00157577));  // MinRecruit08: Preston's story event starts it; one started by hand sits mute
	CHECK(!Restarts(0x00164167));  // MinRecruit09: the same event
	CHECK(Restarts(BOSR03));      // Kells offers it at its start-up stage 5
	CHECK(Restarts(0x000EDCEF));  // Appropriation
	CHECK(Restarts(0x000EDE28));  // Reclamation
	CHECK(Restarts(0x00186C79));  // Variable Removal
	CHECK(Restarts(RRR04));
	// Only a shared radiant whose actors are made at runtime is ever run again (replaced, or hollow): the exception is one.
	for (const auto quest : NOT_RESTARTED) {
		const auto entry = Find(0, quest);
		REQUIRE(entry != nullptr);
		CHECK(entry->share == Share::kShared);
		CHECK(entry->spawned);
	}
}

TEST("radiants: every radiant this game starts again itself was read with no alias filled from the story event (RESTARTED_WITHOUT_EVENT), so its start with no event can fill it")
{
	// Sorted, no row twice, each one a shared radiant of Fallout4.esm that is started again (replaced or hollow, and Restarts).
	for (std::size_t i = 1; i < RESTARTED_WITHOUT_EVENT.size(); ++i) {
		CHECK(RESTARTED_WITHOUT_EVENT[i - 1] < RESTARTED_WITHOUT_EVENT[i]);
	}
	for (const auto quest : RESTARTED_WITHOUT_EVENT) {
		const auto entry = Find(0, quest);
		REQUIRE(entry != nullptr);
		CHECK(entry->share == Share::kShared);
		CHECK(entry->replace || entry->spawned);
		CHECK(Restarts(quest));
	}
	// Every radiant RestartOwn can start again is in it: a new one must be read first (or kept from it: NOT_RESTARTED).
	std::size_t restarted = 0;
	for (const auto& entry : RADIANTS) {
		if (entry.share != Share::kShared || !(entry.replace || entry.spawned) || !Restarts(entry.quest)) {
			continue;
		}
		++restarted;
		CHECK(entry.file == 0);
		CHECK(std::ranges::binary_search(RESTARTED_WITHOUT_EVENT, entry.quest));
	}
	CHECK(restarted == RESTARTED_WITHOUT_EVENT.size());
	// The two never started again (NOT_RESTARTED) aren't in it: the synth boss job (Preston's story event) and Learning Curve.
	CHECK(!std::ranges::binary_search(RESTARTED_WITHOUT_EVENT, 0x00157577u));
	CHECK(!std::ranges::binary_search(RESTARTED_WITHOUT_EVENT, 0x000CF3E2u));
	CHECK(std::ranges::binary_search(RESTARTED_WITHOUT_EVENT, BOSR01));
	CHECK(std::ranges::binary_search(RESTARTED_WITHOUT_EVENT, RRR04));
}

TEST("radiants: a save's run held for its session counts as a friend's only in that session: in another the player had another id")
{
	constexpr std::uint32_t RRR07 = 0x00186C79;
	constexpr std::uint32_t RRR09 = 0x00145F1A;
	constexpr std::uint64_t SESSION = 0x1111, OTHER = 0x2222;
	const Run               bobs{ 2, 40 };
	const Run               ours{ 1, 7 };
	const Saved             held{ SESSION, { { RRR07, { bobs }, {}, SESSION }, { RRR09, { ours }, {}, SESSION }, { 0x0013A33D, { ours }, {}, OTHER } }, {} };
	// Bob's run of this session, held: a friend's. Ours (our id in this session): not. A run of another session, whoever
	// started it (our id there was 2, say): never a friend's.
	CHECK(HeldFriendsRun(held, RRR07, 1, SESSION));
	CHECK(!HeldFriendsRun(held, RRR09, 1, SESSION));
	CHECK(!HeldFriendsRun(held, 0x0013A33D, 1, SESSION));
	CHECK(HeldFriendsRun(held, 0x0013A33D, 2, OTHER));  // back in that session, where our id is 2: player 1's run there is a friend's
	CHECK(!HeldFriendsRun(held, RRR07, 1, OTHER));
	// Out of a session, or with no id yet, the record alone can't say whose a run was.
	CHECK(!HeldFriendsRun(held, RRR07, 0, SESSION));
	CHECK(!HeldFriendsRun(held, RRR07, 1, 0));
	// A run of ours in another session, where our id was 2, held after a reconnect under id 1: not a friend's.
	const Saved another{ OTHER, { { RRR07, { bobs }, {}, OTHER } }, {} };
	CHECK(!HeldFriendsRun(another, RRR07, 1, SESSION));
	CHECK(HeldFriendsRun(another, RRR07, 1, OTHER));
}

TEST("radiants: the Brotherhood's escort goes home (the squire's 45, the scribe's 65) on the owner's word alone; every other stage is reported by whoever reaches it")
{
	constexpr std::uint32_t BOSR03 = 0x000C8675;  // Leading by Example
	constexpr std::uint32_t BOSR04 = 0x000CF3E2;  // Learning Curve
	// Our own run: every stage of ours is reported, the escort's going home too.
	CHECK(Reports(BOSR03, 45, true));
	CHECK(Reports(BOSR04, 65, true));
	// A friend's run we follow: its player walking into the Prydwen first sends its own 45 or 65 nowhere (the owner's escort
	// stays with the owner), and reports the rest (the dungeon cleared, the reward) as any shared radiant's.
	CHECK(!Reports(BOSR03, 45, false));
	CHECK(!Reports(BOSR04, 65, false));
	for (const std::uint16_t stage : { 20, 25, 30, 40, 50, 255 }) {
		CHECK(Reports(BOSR03, stage, false));
	}
	for (const std::uint16_t stage : { 10, 30, 40, 50, 60, 70, 80, 255 }) {
		CHECK(Reports(BOSR04, stage, false));
	}
	// The other jobs with actors made at runtime have no such stage.
	CHECK(Reports(0x000EDCEF, 1000, false));  // Appropriation's clean-up
	CHECK(Reports(0x000EDE28, 1000, false));  // Reclamation's turn-in
	CHECK(Reports(0x00186C79, 200, false));   // Variable Removal's kill
	// Every owner-only stage belongs to a shared job whose actors are the owner's: made at runtime and followed, or an attack
	// (the host's, its attackers copies in the friends' games).
	for (const auto& owner : OWNER_STAGES) {
		const auto entry = Find(0, owner.quest);
		REQUIRE(entry != nullptr);
		CHECK(entry->share == Share::kShared);
		CHECK((entry->spawned || entry->attack));
	}
}

namespace
{
	// Preston's settlement attacks (Entry::attack).
	constexpr std::array<std::uint32_t, 6> ATTACKS{ 0x000A1412u, 0x0005E51Fu, 0x00160409u, 0x00186A08u, 0x000E477Fu, 0x001533A9u };
}

TEST("radiants: a settlement attack Preston hands out is shared only from the host's game; a friend's own stays the friend's")
{
	const auto attack = Find(0, MIN_OWNED01);
	const auto request = Find(0, MIN_RECRUIT01);
	const auto castle = Find(0, MIN_DEFEND_CASTLE);
	REQUIRE(attack != nullptr);
	REQUIRE(request != nullptr);
	REQUIRE(castle != nullptr);
	// The host's run of it is shared; a friend's game keeps its own (and on a dedicated server, where no game hosts, every
	// player keeps theirs).
	CHECK(Begins(*attack, true));
	CHECK(!Begins(*attack, false));
	// Any other shared job is shared by whoever is given it; one left alone by nobody.
	CHECK(Begins(*request, true));
	CHECK(Begins(*request, false));
	CHECK(!Begins(*castle, true));
	CHECK(!Begins(*castle, false));
	// A friend's attack sent to the host's game is turned down there (only the host's is shared); a friend's game takes the
	// host's. Other jobs are taken wherever they come from.
	CHECK(!Joins(*attack, true));
	CHECK(Joins(*attack, false));
	CHECK(Joins(*request, true));
	CHECK(Joins(*request, false));
}

TEST("radiants: a friend's game holds its own attack back while the host's run of it is shared there, and lets it go after")
{
	const auto attack = Find(0, 0x001533A9);  // MinRadiantOwned11: synths (WorkshopSynthAttackScript, extending the attack script)
	const auto squire = Find(0, 0x000C8675);  // Leading by Example: followed, but no attack
	REQUIRE(attack != nullptr);
	REQUIRE(squire != nullptr);
	// The host's run, shared here and running: held.
	CHECK(HoldsAttack(attack, true, false, true));
	// Our own (we host): our attack is the one.
	CHECK(!HoldsAttack(attack, true, true, true));
	// Not shared here (out of the session, or a friend's own run of it): the game's own attack, as in a game of one.
	CHECK(!HoldsAttack(attack, false, false, true));
	// Stopped here: nothing to hold.
	CHECK(!HoldsAttack(attack, true, false, false));
	// Not an attack, or not a radiant.
	CHECK(!HoldsAttack(squire, true, false, true));
	CHECK(!HoldsAttack(nullptr, true, false, true));
	// The timer is pushed out past the longest wait (24 hours), and well within it again: every few seconds of play is a
	// few game minutes.
	CHECK(ATTACK_TIMER_HELD > 24.0f);
	CHECK(ATTACK_TIMER_EVERY <= std::chrono::seconds{ 30 });
	CHECK(ATTACK_TIMER == 0);
	CHECK(ATTACK_SCRIPT == "WorkshopAttackScript");
	CHECK(ATTACK_WAITING == "PlayerIsWaiting");
}

TEST("radiants: an attack let go starts its clock again only where it would be running alone: the player there or the defences built, and the attack not begun")
{
	using Stages = std::vector<std::uint16_t>;
	CHECK(!ResumesAttackClock(Stages{ 10, 20, 50, 100 }));            // taken, nobody there yet: the game starts it on arrival
	CHECK(ResumesAttackClock(Stages{ 10, 20, 50, 100, 70 }));         // the player arrived (CheckLocation's hour)
	CHECK(ResumesAttackClock(Stages{ 10, 20, 50, 100, 120 }));        // the defences built
	CHECK(!ResumesAttackClock(Stages{ 10, 20, 50, 100, 70, 130 }));   // begun (the host's 130 is never taken, so only ours)
	CHECK(!ResumesAttackClock(Stages{ 10, 20, 50, 100, 120, 130 }));
	CHECK(!ResumesAttackClock(Stages{}));
}

TEST("radiants: an attack the host already ended is never fought again in the friend's game: its clock isn't started again once 200, 290, 300 or 310 is done")
{
	using Stages = std::vector<std::uint16_t>;
	// The host's end of it is taken here (its 130 never is): the session over before the turn-in, the attack is over here too.
	for (const std::uint16_t ended : { ATTACK_WON, ATTACK_SETTLERS_DEAD, ATTACK_RESOLVED, ATTACK_FAILED }) {
		CHECK(!ResumesAttackClock(Stages{ 10, 20, 50, 100, 70, ended }));        // the player had arrived
		CHECK(!ResumesAttackClock(Stages{ 10, 20, 50, 100, 120, ended }));       // the defences built
		CHECK(!ResumesAttackClock(Stages{ 10, 20, 50, 100, 70, 120, ended }));
		CHECK(!ResumesAttackClock(Stages{ 10, 20, 50, 100, ended }));
		CHECK(AttackBegun(Stages{ 10, 20, 50, 100, 70, ended }));
	}
	CHECK(!ResumesAttackClock(Stages{ 10, 20, 50, 100, 70, 200, 300 }));
	CHECK(!ResumesAttackClock(Stages{ 10, 20, 50, 100, 70, 120, 290, 300, 310 }));
	CHECK(ATTACK_WON == 200);
	CHECK(ATTACK_SETTLERS_DEAD == 290);
	CHECK(ATTACK_RESOLVED == 300);
	CHECK(ATTACK_FAILED == 310);
	// Begun or over: this game's own start, or any end. Nothing before that.
	CHECK(AttackBegun(Stages{ 10, 20, 50, 100, 130 }));
	CHECK(!AttackBegun(Stages{ 10, 20, 50, 100, 70, 120 }));
	CHECK(!AttackBegun(Stages{}));
	// The reward's stages alone say nothing of the attack (they follow a 200 in the game).
	CHECK(!AttackBegun(Stages{ 10, 20, 50, 100, 400, 450, 500 }));
}

TEST("radiants: settlers group held/unheld sizes: a held job's settler count is raised out of reach once, and given back exactly")
{
	// A settlement's own size (its settlers) is never held; a raised one is.
	CHECK(!IsSettlersHeld(0));
	CHECK(!IsSettlersHeld(3));
	CHECK(!IsSettlersHeld(ATTACK_SETTLERS_HELD - 1));
	CHECK(IsSettlersHeld(ATTACK_SETTLERS_HELD));
	CHECK(IsSettlersHeld(ATTACK_SETTLERS_HELD + 7));
	// Held: raised once; a size already held (a save made while held, its run shared again) stays as it is.
	for (const std::int32_t size : { 0, 1, 3, 12, 40 }) {
		const auto held = SettlersHeldSize(size, true);
		CHECK(held == size + ATTACK_SETTLERS_HELD);
		CHECK(IsSettlersHeld(held));
		CHECK(SettlersHeldSize(held, true) == held);
		// Given back: the size it was, and no longer held.
		CHECK(SettlersHeldSize(held, false) == size);
		CHECK(!IsSettlersHeld(SettlersHeldSize(held, false)));
		// A settler the script added while held (its AdjustDeadCounts) is kept as the size is given back.
		CHECK(SettlersHeldSize(held + 2, false) == size + 2);
		// Two raises a call apart (the size read again before the first call ran) give back the same.
		CHECK(SettlersHeldSize(held + ATTACK_SETTLERS_HELD, false) == size);
		// A size that isn't held is given back as it is.
		CHECK(SettlersHeldSize(size, false) == size);
	}
	// Never negative.
	CHECK(SettlersHeldSize(-5, false) == 0);
	CHECK(SettlersHeldSize(-5, true) == ATTACK_SETTLERS_HELD);
	CHECK(ATTACK_SETTLERS_HELD > 1000);  // more than any settlement's settlers
	CHECK(RE_SCRIPT == "REScript");
	CHECK(RE_DEAD_GROUPS == "DeadCountGroups");
}

TEST("radiants: a hold let go before its settlers' raise has run gives them back only once the raise has run")
{
	// The order the game can take: the hold raises (the call queued, the size not yet changed), the hold is let go on the
	// next poll, and only then does the VM run the raise.
	const std::int32_t size = 3;
	std::int32_t       script = size;  // the group's size as the script holds it
	bool               queued = true;  // the raise is queued, not yet run
	// Let go now: the size read isn't held yet, but nothing is given back (that would leave the raise to land for good).
	CHECK(GiveBackSettlers(queued, IsSettlersHeld(script)) == SettlersGiveBack::kAfterRaise);
	// The raise runs.
	script = SettlersHeldSize(script, true);
	queued = false;
	// Given back now, to the size it had.
	REQUIRE(GiveBackSettlers(queued, IsSettlersHeld(script)) == SettlersGiveBack::kNow);
	script = SettlersHeldSize(script, false);
	CHECK(script == size);
	CHECK(GiveBackSettlers(false, IsSettlersHeld(script)) == SettlersGiveBack::kNothing);
	// A raise the VM cancelled leaves the size as it was: nothing to give back once it is told.
	CHECK(GiveBackSettlers(false, IsSettlersHeld(size)) == SettlersGiveBack::kNothing);
	// A held size with no raise queued (a loaded save's) is given back at once.
	CHECK(GiveBackSettlers(false, IsSettlersHeld(size + ATTACK_SETTLERS_HELD)) == SettlersGiveBack::kNow);
	// A raise queued wins over a size read held (raised again before the last one ran): wait for it, then give all back.
	CHECK(GiveBackSettlers(true, true) == SettlersGiveBack::kAfterRaise);
	CHECK(SettlersHeldSize(size + 2 * ATTACK_SETTLERS_HELD, false) == size);
}

TEST("radiants: a held job's settlers given back recheck the count only on a running job whose attack neither began nor ended here")
{
	using Stages = std::vector<std::uint16_t>;
	CHECK(SettlersRecheck(true, Stages{ 10, 20, 50, 100 }));
	CHECK(SettlersRecheck(true, Stages{ 10, 20, 50, 100, 70, 120 }));
	// A stopped quest: a SetStage would start it again.
	CHECK(!SettlersRecheck(false, Stages{ 10, 20, 50, 100, 70 }));
	CHECK(!SettlersRecheck(false, Stages{}));
	// Begun or over here: a recheck could fail a job already won or lost.
	for (const std::uint16_t ended : { ATTACK_STARTED, ATTACK_WON, ATTACK_SETTLERS_DEAD, ATTACK_RESOLVED, ATTACK_FAILED }) {
		CHECK(!SettlersRecheck(true, Stages{ 10, 20, 50, 100, 70, ended }));
	}
	CHECK(ATTACK_WORKSHOP_ALIAS == 9);
}

TEST("radiants: an attack let go never keeps the hold's timer: started again for an hour where its clock resumes, cancelled everywhere else")
{
	using Stages = std::vector<std::uint16_t>;
	const Stages taken{ 10, 20, 50, 100 }, arrived{ 10, 20, 50, 100, 70 }, defended{ 10, 20, 50, 100, 120 };
	const Stages won{ 10, 20, 50, 100, 70, 200 }, turnedIn{ 10, 20, 50, 100, 70, 120, 200, 400, 450, 500 };
	for (const bool waiting : { true, false }) {
		// Held by this game until now: let go whatever PlayerIsWaiting says (a wait that just ended cleared it).
		// The session over with the player there, or the defences built: the clock's hour, as alone.
		CHECK(LetGo(true, waiting, true, arrived) == AttackClock::kResumed);
		CHECK(LetGo(true, waiting, true, defended) == AttackClock::kResumed);
		// The session over before the friend's player ever arrived: the game starts the timer on arrival, so none is left.
		CHECK(LetGo(true, waiting, true, taken) == AttackClock::kCancelled);
		// The host's attack over (its 200 taken here), the session over before the turn-in: no second attack.
		CHECK(LetGo(true, waiting, true, won) == AttackClock::kCancelled);
		CHECK(LetGo(true, waiting, true, Stages{ 10, 20, 50, 100, 70, 290, 300 }) == AttackClock::kCancelled);
		CHECK(LetGo(true, waiting, true, Stages{ 10, 20, 50, 100, 120, 310 }) == AttackClock::kCancelled);
		// The job turned in (the host's 400) and the quest stopped: a timer left would start the stopped quest again.
		CHECK(LetGo(true, waiting, false, turnedIn) == AttackClock::kCancelled);
		// Stopped any other way, the player there or not: never resumed on a quest that doesn't run.
		CHECK(LetGo(true, waiting, false, arrived) == AttackClock::kCancelled);
		CHECK(LetGo(true, waiting, false, taken) == AttackClock::kCancelled);
		CHECK(LetGo(true, waiting, false, Stages{}) == AttackClock::kCancelled);
	}
	// A loaded world's (not known held here): one a save kept held has PlayerIsWaiting set, and is let go the same way.
	CHECK(LetGo(false, true, true, arrived) == AttackClock::kResumed);
	CHECK(LetGo(false, true, true, taken) == AttackClock::kCancelled);
	CHECK(LetGo(false, true, true, won) == AttackClock::kCancelled);
	CHECK(LetGo(false, true, false, turnedIn) == AttackClock::kCancelled);
	// One that never was held (the host's own, a game of one's): its flag clear, its own timer is never touched.
	CHECK(LetGo(false, false, true, arrived) == AttackClock::kLeft);
	CHECK(LetGo(false, false, true, taken) == AttackClock::kLeft);
	CHECK(LetGo(false, false, true, Stages{ 10, 20, 50, 100, 70, 130 }) == AttackClock::kLeft);
	CHECK(LetGo(false, false, false, turnedIn) == AttackClock::kLeft);
	CHECK(LetGo(false, false, false, Stages{}) == AttackClock::kLeft);
}

TEST("radiants: the host's attack isn't joined by a friend's game whose own attack of it has started already (no second wave)")
{
	using Stages = std::vector<std::uint16_t>;
	const auto request = Find(0, MIN_RECRUIT01);
	REQUIRE(request != nullptr);
	for (const auto quest : ATTACKS) {
		const auto attack = Find(0, quest);
		REQUIRE(attack != nullptr);
		// Running here with the same settlement, not begun: joined, and held.
		CHECK(JoinsRunning(*attack, Stages{ 10, 20, 50, 100 }));
		CHECK(JoinsRunning(*attack, Stages{ 10, 20, 50, 100, 70, 120 }));
		CHECK(JoinsRunning(*attack, Stages{}));
		// This game's own attackers are out (130 is only ever its own): each player's own.
		CHECK(!JoinsRunning(*attack, Stages{ 10, 20, 50, 100, 70, 130 }));
		CHECK(!JoinsRunning(*attack, Stages{ 10, 20, 50, 100, 120, 130, 200 }));
		// The host's end of an earlier sharing of the same run taken here, our own never begun (the host loaded a save
		// and shares it again under a new number): joined again.
		CHECK(JoinsRunning(*attack, Stages{ 10, 20, 50, 100, 70, 200 }));
	}
	// Any other job: its stages are no attack's.
	CHECK(JoinsRunning(*request, Stages{ 10, 130 }));
}

TEST("radiants: the attack's start and its damage roll are never taken from the host; the arrival, the dead and the failure are the host's to report")
{
	for (const auto quest : ATTACKS) {
		// Its 130 starts this game's own attack, its 300 rolls the settlement's damage: never from a friend.
		CHECK(!Takes(quest, ATTACK_STARTED));
		CHECK(!Takes(quest, 300));
		// The offer, the take, the defences built, the attackers dead, the failure and the reward come from the host.
		for (const std::uint16_t stage : { 10, 20, 50, 70, 100, 120, 150, 200, 290, 310, 400, 450, 500 }) {
			CHECK(Takes(quest, stage));
		}
		// A friend's arrival never starts the host's attack's clock, and a friend's own count never ends it: the host's.
		for (const std::uint16_t stage : { ATTACK_ARRIVED, std::uint16_t{ 200 }, std::uint16_t{ 290 }, std::uint16_t{ 310 } }) {
			CHECK(!Reports(quest, stage, false));
			CHECK(Reports(quest, stage, true));
		}
		// The rest is reported by whoever reaches it: the defences built, the reward (either player's turn-in).
		for (const std::uint16_t stage : { 50, 100, 120, 150, 400, 450, 500 }) {
			CHECK(Reports(quest, stage, false));
		}
		// Never run again here (never stopped for a friend's run, never hollow), so not listed with those that aren't.
		CHECK(Restarts(quest));
	}
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
	// Variable Removal, Lost Soul and A Clean Equation count at their 50 and off at their 100; To the Mattresses off at its 75
	// (which its 100 sets too).
	CHECK(LeavesCounted(0x00186C79, std::vector<std::uint16_t>{ 0, 50 }, 0.0f));
	CHECK(!LeavesCounted(0x00186C79, std::vector<std::uint16_t>{ 0, 50, 100 }, 0.0f));
	CHECK(LeavesCounted(0x0013A33D, std::vector<std::uint16_t>{ 1, 50 }, 0.0f));
	CHECK(!LeavesCounted(0x0013A33D, std::vector<std::uint16_t>{ 1, 50, 75, 100 }, 0.0f));
	CHECK(LeavesCounted(0x00145F1A, std::vector<std::uint16_t>{ 0, 50 }, 0.0f));
	CHECK(LeavesCounted(0x0014A34A, std::vector<std::uint16_t>{ 0, 50 }, 0.0f));
	// Concierge counts nothing; nor do the others.
	CHECK(!LeavesCounted(RRR02A, std::vector<std::uint16_t>{ 0, 50 }, 0.0f));
	CHECK(!LeavesCounted(INSTR01, std::vector<std::uint16_t>{ 5 }, 0.0f));
	// Every quest with an offer is a shared one that may be stopped (and only such a one needs it).
	for (const auto& offer : OFFERS) {
		const auto entry = Find(0, offer.quest);
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

TEST("radiants: a friend's run whose start failed on a needed alias this game can't force (the host's spokesman, no settler here) is asked again with that alias alone loosened")
{
	// MinRadiantOwned01's aliases (some of them), as Fallout4.esm 1.11.240 files them: SettlementSpokesman (6, flags 0xC08)
	// must fill and is pushed in from SettlementBoss or SettlementLeader, both optional.
	const std::vector<Alias> owned01{
		{ 24, REF, Fill::kSource, true, "EventWorkshop" },
		{ 9, REF, Fill::kByOthers, false, "Workshop" },
		{ 2, LOC, Fill::kDerived, false, "ActualLocation" },
		{ 7, REF, Fill::kFixed, false, "PrestonGarvey" },
		{ 21, REF, Fill::kDerived, true, "SettlementBoss" },
		{ 20, REF, Fill::kDerived, true, "SettlementLeader" },
		{ 6, REF, Fill::kByOthers, false, "SettlementSpokesman" },
	};
	constexpr std::uint32_t HOST_SETTLER = 0xFF000777;  // the host's settler, shared as a spawn whose copy isn't placed here
	const std::vector<RadiantFill> fills{ { 24, 0, SANCTUARY_WORKSHOP }, { 9, 0, SANCTUARY_WORKSHOP }, { 2, 1, CORVEGA }, { 21, 0, HOST_SETTLER },
		{ 6, 0, HOST_SETTLER } };
	const auto plan = MakePlan(owned01, fills, Resolve);
	REQUIRE(plan.Ok());
	CHECK((plan.unforced == std::vector<std::uint32_t>{ 21, 6 }));

	// Of the aliases left to the others, only the one that must fill is loosened: an optional one can't be why it failed.
	CHECK((LoosenOnRetry(plan.unforced, owned01) == std::vector<std::uint32_t>{ 6 }));
	// Each once, only the quest's own, in the order named.
	const std::vector<std::uint32_t> named{ 6, 99, 9, 6, 24 };
	CHECK((LoosenOnRetry(named, owned01) == std::vector<std::uint32_t>{ 6, 9 }));
	CHECK(LoosenOnRetry({}, owned01).empty());

	// Loosened: optional, every other flag kept (the spokesman's 0xC08 becomes 0xC0A).
	CHECK(LoosenedFlags(0xC08) == 0xC0A);
	CHECK(LoosenedFlags(0x2) == 0x2);
	for (const std::uint32_t flags : { 0x0u, 0x8u, 0x101u, 0x801u, 0x88000u, 0x200200u }) {
		CHECK((LoosenedFlags(flags) & ~ALIAS_OPTIONAL) == (flags & ~ALIAS_OPTIONAL));
		CHECK((LoosenedFlags(flags) & ALIAS_OPTIONAL) != 0);
	}

	// Asked again only when the start failed on a fill, with something to loosen, and the quest can be started now: any
	// other failure (a stop under way, a start that ran) is as before.
	CHECK(RetriesLoosened(true, 1, true));
	CHECK(!RetriesLoosened(false, 1, true));
	CHECK(!RetriesLoosened(true, 0, true));
	CHECK(!RetriesLoosened(true, 1, false));

	// Started so, it runs here with the spokesman (and the optional boss) empty: the same targets, the run shared.
	std::map<std::pair<Kind, std::uint32_t>, std::uint32_t> here{ { { REF, 24 }, SANCTUARY_WORKSHOP }, { { REF, 9 }, SANCTUARY_WORKSHOP }, { { LOC, 2 }, CORVEGA } };
	const auto ours = [&](Kind a_kind, std::uint32_t a_alias) {
		const auto it = here.find({ a_kind, a_alias });
		return it != here.end() ? it->second : 0u;
	};
	CHECK(SameTargets(Compare(owned01, fills, Resolve, ours, plan.unforced)));
	// Another settlement is still another target.
	here[{ REF, 24 }] = TENPINES_WORKSHOP;
	CHECK(!SameTargets(Compare(owned01, fills, Resolve, ours, plan.unforced)));
}

TEST("radiants: a start from the story manager's place (StartWithEvent) loosens only the needed aliases its caller names, once each")
{
	// A checkpoint's aliases (some of them): its trigger from the event, Defender01 a defender at the location (which only a
	// faction's _Init quest places), an optional second defender. The caller (a followed encounter) names the defenders the
	// owner filled with what this game can't name.
	const std::vector<Alias> checkpoint{
		{ 0, REF, Fill::kSource, false, "Trigger" },
		{ 1, LOC, Fill::kSource, false, "Location" },
		{ 2, REF, Fill::kSource, false, "Defender01" },
		{ 3, REF, Fill::kSource, true, "Defender02" },
	};
	const std::vector<std::uint32_t> named{ 2, 3 };
	CHECK((LoosenOnRetry(named, checkpoint) == std::vector<std::uint32_t>{ 2 }));
	// Nothing named (every other caller: the developer's start, the attack's mirror): never asked again.
	CHECK(LoosenOnRetry({}, checkpoint).empty());
	CHECK(!RetriesLoosened(true, LoosenOnRetry({}, checkpoint).size(), true));
	// Only optional ones named: nothing to loosen.
	const std::vector<std::uint32_t> optionalOnly{ 3 };
	CHECK(!RetriesLoosened(true, LoosenOnRetry(optionalOnly, checkpoint).size(), true));
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

TEST("radiants: a friend's run with actors made at runtime is followed with their newest fills until its owner leaves or loads a save")
{
	constexpr std::uint32_t RRR07 = 0x00186C79;
	constexpr std::uint32_t RRR09 = 0x00145F1A;
	const Run               bobs{ 2, 40 };
	const Run               bobsNext{ 2, 41 };
	const Run               carols{ 3, 7 };
	const std::vector<RadiantFill> first{ { 2, 1, CORVEGA }, { 5, 0, RUNTIME_NPC } };  // the courser not shared yet
	const std::vector<RadiantFill> shared{ { 2, 1, CORVEGA }, { 5, 0, SPAWN } };       // shared since: its spawn id
	Followed                       followed;

	// Bob's Variable Removal shared here: followed with its fills; sent again with the courser's spawn id, the newest kept.
	CHECK(followed.Follow(RRR07, bobs, first));
	REQUIRE(followed.Follows(RRR07));
	REQUIRE(followed.Find(RRR07) != nullptr);
	CHECK((followed.Find(RRR07)->fills == first));
	CHECK(!followed.Follow(RRR07, bobs, shared));  // the same run: what was forced for it stands
	CHECK((followed.Find(RRR07)->fills == shared));
	CHECK(followed.Find(RRR07)->run == bobs);
	// Another run of the quest (Bob loaded a save from before it and rolled it again): a fresh entry.
	CHECK(followed.Follow(RRR07, bobsNext, first));
	CHECK(followed.Find(RRR07)->run == bobsNext);
	CHECK(followed.Follow(RRR09, carols, first));
	CHECK(followed.Size() == 2);

	// Bob leaves (or loads a save): his runs are followed no more; Carol's goes on. His send of the run when he is back
	// follows it again.
	CHECK((followed.OwnerGone(2) == std::vector<std::uint32_t>{ RRR07 }));
	CHECK(!followed.Follows(RRR07));
	CHECK(followed.Follows(RRR09));
	CHECK(followed.OwnerGone(2).empty());
	CHECK(followed.Follow(RRR07, bobsNext, shared));
	// The quest stops here: over. The session ends: all over.
	followed.Ended(RRR07);
	CHECK(!followed.Follows(RRR07));
	CHECK(followed.Follows(RRR09));
	followed.Clear();
	CHECK(followed.Size() == 0);
	CHECK(!followed.Follows(RRR09));
}

TEST("radiants: a quest whose run was a friend's stays known as such after the owner or the session goes, until it stops here or another world is loaded")
{
	constexpr std::uint32_t RRR07 = 0x00186C79;
	constexpr std::uint32_t RRR08 = 0x00186C7A;
	constexpr std::uint32_t RRR09 = 0x00145F1A;
	const Run               bobs{ 2, 40 };
	const Run               carols{ 3, 7 };
	const std::vector<RadiantFill> fills{ { 2, 1, CORVEGA }, { 5, 0, SPAWN } };
	Followed                       followed;
	CHECK(!followed.Once(RRR07));  // never followed: a run of this game's own, whatever it looks like
	followed.Follow(RRR07, bobs, fills);
	followed.Follow(RRR09, carols, fills);
	CHECK(followed.Once(RRR07));
	CHECK(!followed.Once(RRR08));
	// Bob goes: his run is followed no more, but the quest is known to have been his.
	followed.OwnerGone(2);
	CHECK(!followed.Follows(RRR07));
	CHECK(followed.Once(RRR07));
	// The session ends: nothing is followed, both are still known.
	followed.Clear();
	CHECK(followed.Once(RRR07));
	CHECK(followed.Once(RRR09));
	// The quest stops here (or is run again as this game's own): forgotten.
	followed.Ended(RRR07);
	CHECK(!followed.Once(RRR07));
	CHECK(followed.Once(RRR09));
	// Another world loaded: nothing of this one's is known.
	followed.Reset();
	CHECK(!followed.Once(RRR09));
}

TEST("radiants: the word of an owner's load doesn't drop a run they sent again just before it")
{
	using namespace std::chrono_literals;
	constexpr std::uint32_t RRR07 = 0x00186C79;
	constexpr std::uint32_t RRR09 = 0x00145F1A;
	const Run               bobs{ 2, 40 };
	const std::vector<RadiantFill> fills{ { 2, 1, CORVEGA }, { 5, 0, SPAWN } };
	const Followed::Time           t0{};
	Followed                       followed;
	// Bob loads twice within two seconds: the server holds the word of the second load, and his send of Variable Removal
	// from that load comes before it. The word of the first load drops his Concierge, sent long before.
	followed.Follow(RRR09, bobs, fills, t0 - 1min);
	followed.Follow(RRR07, bobs, fills, t0 - 1s);
	CHECK((followed.OwnerGone(2, t0, RELOAD_GRACE) == std::vector<std::uint32_t>{ RRR09 }));
	CHECK(followed.Follows(RRR07));
	// The word of a load with no send within the grace drops the run; so does the word that he left, whenever he sent it.
	CHECK((followed.OwnerGone(2, t0 + RELOAD_GRACE, RELOAD_GRACE) == std::vector<std::uint32_t>{ RRR07 }));
	followed.Follow(RRR07, bobs, fills, t0);
	CHECK((followed.OwnerGone(2, t0) == std::vector<std::uint32_t>{ RRR07 }));
	CHECK(RELOAD_GRACE > RELOAD_TOLD_EVERY);
}

TEST("radiants: a run's start sent again while its first waits its turn keeps the newer fills and every stage")
{
	constexpr std::uint32_t RRR07 = 0x00186C79;
	Protocol::RadiantStart  waiting{ 2, RRR07, 40, { { 2, 1, CORVEGA }, { 5, 0, RUNTIME_NPC } }, { 10, 50 } };
	waiting.stages.push_back(100);  // a stage packet that came meanwhile, kept with it
	const Protocol::RadiantStart newer{ 2, RRR07, 40, { { 2, 1, CORVEGA }, { 5, 0, SPAWN } }, { 10, 50, 100, 200 } };
	REQUIRE(Supersedes(waiting, newer));
	CHECK((waiting.fills == newer.fills));
	CHECK((waiting.stages == std::vector<std::uint16_t>{ 10, 50, 100, 200 }));  // each once, in the order they came
	// Another run of the quest, another player's, or another quest: not this one's; nothing changes.
	const auto before = waiting;
	CHECK(!Supersedes(waiting, Protocol::RadiantStart{ 2, RRR07, 41, {}, { 300 } }));
	CHECK(!Supersedes(waiting, Protocol::RadiantStart{ 3, RRR07, 40, {}, { 300 } }));
	CHECK(!Supersedes(waiting, Protocol::RadiantStart{ 2, RRR03, 40, {}, { 300 } }));
	CHECK((waiting == before));
}

TEST("radiants: a Railroad job whose target is gone for good is run again as this game's own, after a wait, unless a friend's run of it stands in")
{
	using namespace std::chrono_literals;
	using Objectives = std::vector<ObjectiveHere>;
	// BGSQuestObjective::state: shown and not done is open; done (completed or failed), or not shown yet, is not.
	CHECK(ObjectiveOpen(1));
	CHECK(!ObjectiveOpen(0));
	CHECK(!ObjectiveOpen(2));
	CHECK(!ObjectiveOpen(3));
	CHECK(!ObjectiveOpen(4));
	CHECK(!ObjectiveOpen(5));

	// On offer (no objective shown yet), with its courser made: never hollow. Taken, the courser standing in the alias the
	// objective points at: not hollow, whether the target alias is forced yet (the created one holds it either way).
	CHECK(!HollowJob(Objectives{ { false, 1, 1 } }, true));
	CHECK(!HollowJob(Objectives{ { true, 1, 1 } }, true));
	CHECK(!HollowJob(Objectives{ { true, 1, 0 } }, true));
	// Followed, then the owner's copy gone (the session over, the owner gone, our save loaded alone): the objective points
	// at nothing, and our own courser was removed: hollow.
	CHECK(HollowJob(Objectives{ { true, 1, 0 } }, false));
	// The courser killed: that objective is done, its corpse in the alias or cleaned up since; the next points at the
	// giver, who stands. Never hollow.
	CHECK(!HollowJob(Objectives{ { false, 1, 0 }, { true, 1, 1 } }, true));
	CHECK(!HollowJob(Objectives{ { false, 1, 0 }, { true, 1, 1 } }, false));
	// An objective shown with no target at all (a text one) says nothing.
	CHECK(!HollowJob(Objectives{ { true, 0, 0 } }, false));
	// A patrol of three, one still standing: not hollow.
	CHECK(!HollowJob(Objectives{ { true, 3, 1 } }, false));
	CHECK(HollowJob(Objectives{ { true, 3, 0 } }, false));
	CHECK(!HollowJob(Objectives{}, false));

	// What an objective's target says: an alias that isn't a reference alias of the quest (a keyword, or none) says
	// nothing; so does one that holds something that isn't an actor (the place's marker, which stands whatever became of
	// the actors). An actor alias is a target: standing with its actor here (alive or dead), gone when it holds nothing
	// or a disabled or deleted one.
	CHECK(TargetOf(false, false, false, false) == Target::kNone);
	CHECK(TargetOf(false, true, true, true) == Target::kNone);
	CHECK(TargetOf(true, true, false, true) == Target::kNone);
	CHECK(TargetOf(true, false, false, false) == Target::kGone);
	CHECK(TargetOf(true, true, true, false) == Target::kGone);
	CHECK(TargetOf(true, true, true, true) == Target::kStanding);
	// The courser's alias empty, the place's marker standing: judged by the courser alone.
	ObjectiveHere judged{ true, 0, 0 };
	judged.Add(Target::kGone);
	judged.Add(Target::kNone);
	judged.Add(Target::kNone);
	CHECK(judged.targets == 1);
	CHECK(judged.standing == 0);
	CHECK(HollowJob(Objectives{ judged }, false));
	judged.Add(Target::kStanding);
	CHECK(!HollowJob(Objectives{ judged }, false));

	// Run again once hollow for HOLLOW_JOB_WAIT with nothing standing in all the while: not while a friend's run of it is
	// followed (their actors stand in) or a friend's start of it waits its turn or is under way (it fills the aliases
	// itself), and never when no friend's run of it is known (a player's own run, whatever it looks like).
	constexpr std::uint32_t RRR07 = 0x00186C79;
	constexpr std::uint32_t RRR08 = 0x00186C7A;
	const HollowClock::Time t0{};
	HollowClock             clock;
	CHECK(!clock.Poll(RRR07, true, false, true, t0));
	CHECK(clock.HollowFor(RRR07, t0 + 1s) == 1s);
	CHECK(!clock.Poll(RRR07, true, false, true, t0 + HOLLOW_JOB_WAIT - 1s));
	CHECK(clock.Poll(RRR07, true, false, true, t0 + HOLLOW_JOB_WAIT));
	clock.Forget(RRR07);  // run again
	CHECK(!clock.HollowFor(RRR07, t0 + HOLLOW_JOB_WAIT).has_value());
	// No longer hollow (its target stands again): counted from the start when it is hollow again.
	CHECK(!clock.Poll(RRR07, true, false, true, t0));
	CHECK(!clock.Poll(RRR07, false, false, true, t0 + 4min));
	CHECK(!clock.Poll(RRR07, true, false, true, t0 + 4min + 30s));
	CHECK(!clock.Poll(RRR07, true, false, true, t0 + 5min));
	CHECK(clock.Poll(RRR07, true, false, true, t0 + 9min + 30s));
	clock.Forget(RRR07);

	// The blocker of the second review: a followed job is often hollow (our copy of the owner's courser stands only while
	// their game has it loaded). Hollow for an hour while followed, then the owner drops: nothing happens before
	// HOLLOW_JOB_WAIT from the drop, so the owner's return within it (followed again) keeps the job, and a later drop
	// counts from the start again. The wait is a wait for the owner's return.
	CHECK(!clock.Poll(RRR07, true, true, true, t0));
	CHECK(!clock.Poll(RRR07, true, true, true, t0 + 1h));
	CHECK(!clock.HollowFor(RRR07, t0 + 1h).has_value());
	const auto dropped = t0 + 1h;
	CHECK(!clock.Poll(RRR07, true, false, true, dropped));  // the owner drops
	CHECK(!clock.Poll(RRR07, true, false, true, dropped + HOLLOW_JOB_WAIT - 1s));
	CHECK(!clock.Poll(RRR07, true, true, true, dropped + HOLLOW_JOB_WAIT - 1s));  // back just in time: followed again
	CHECK(!clock.Poll(RRR07, true, false, true, dropped + HOLLOW_JOB_WAIT));      // and gone again: a fresh wait
	CHECK(clock.HollowFor(RRR07, dropped + HOLLOW_JOB_WAIT) == 0s);
	CHECK(!clock.Poll(RRR07, true, false, true, dropped + 2 * HOLLOW_JOB_WAIT - 1s));
	CHECK(clock.Poll(RRR07, true, false, true, dropped + 2 * HOLLOW_JOB_WAIT));
	clock.Forget(RRR07);
	// A friend's start of it waiting its turn or under way stands in just the same.
	CHECK(!clock.Poll(RRR07, true, false, true, t0));
	CHECK(!clock.Poll(RRR07, true, true, true, t0 + 4min));
	CHECK(!clock.Poll(RRR07, true, false, true, t0 + 5min));
	CHECK(!clock.Poll(RRR07, true, false, true, t0 + 9min + 59s));
	CHECK(clock.Poll(RRR07, true, false, true, t0 + 10min));
	clock.Forget(RRR07);

	// A run of this game's own (no friend's run of the quest known): never, however long.
	CHECK(!clock.Poll(RRR07, true, false, false, t0));
	CHECK(!clock.Poll(RRR07, true, false, false, t0 + 1h));
	CHECK(!clock.HollowFor(RRR07, t0 + 1h).has_value());

	// Two hollow jobs at once (two givers' jobs followed, the session over): each on its own clock.
	CHECK(!clock.Poll(RRR07, true, false, true, t0));
	CHECK(!clock.Poll(RRR08, true, false, true, t0 + 1min));
	CHECK(clock.Poll(RRR07, true, false, true, t0 + HOLLOW_JOB_WAIT));
	CHECK(!clock.Poll(RRR08, true, false, true, t0 + HOLLOW_JOB_WAIT));
	CHECK(clock.Poll(RRR08, true, false, true, t0 + HOLLOW_JOB_WAIT + 1min));
	clock.Clear();  // another world loaded: counted from the load
	CHECK(!clock.Poll(RRR07, true, false, true, t0 + 1h));
	CHECK(clock.HollowFor(RRR07, t0 + 1h) == 0s);
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
	bytes[0] = 3;
	CHECK(!Decode(bytes).has_value());
	bytes[0] = static_cast<std::uint8_t>(SAVED_VERSION_INTROS);  // that version's record goes on after these bytes
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

TEST("radiants: the add-ons' radiants are named by their file and form ID there, so one's form ID is no other file's")
{
	// The four QuestFilter once listed as each game's own, whose aliases are the game's own references and locations.
	struct Dlc
	{
		std::uint8_t  file;
		std::uint32_t quest;
	};
	for (const auto [file, quest] : { Dlc{ 0, 0x026340 }, Dlc{ 0, 0x0482CD }, Dlc{ 1, 0x00EB51 }, Dlc{ 1, 0x040A86 } }) {
		const auto entry = Find(file, quest);
		REQUIRE(entry != nullptr);
		CHECK(entry->share == Share::kShared);
		CHECK(!entry->replace);  // each is handed out by one game's conversation or event: a friend's run with other targets is not stopped
		CHECK(ShareOf(file, quest) == Share::kShared);
		CHECK(IsRadiant(file, quest));
	}
	// Far Harbor's settlement request and Fallout4.esm's share a form ID digit for digit with none of this file's.
	CHECK(Find(1, 0x00EB51) != nullptr);
	CHECK(Find(0, 0x00EB51) == nullptr);
	CHECK(Find(2, 0x00EB51) == nullptr);
	CHECK(ShareOf(0, 0x00EB51) == Share::kNotRadiant);
	CHECK(Find(0, 0x019088) == nullptr);
	CHECK(Find(3, 0x017F46) == nullptr);  // DLC04_RQ_KillRivalBoss: kept in PER_GAME
	CHECK(Find(3, 0x026340) == nullptr);  // Saugus Ironworks is Fallout4.esm's
	CHECK(Find(0, 0x11B36E) != nullptr);  // MinRecruit00
	CHECK(Find(1, 0x11B36E) == nullptr);
	// The add-ons' runs that make NPCs at runtime stay each player's own (QuestFilter::PER_GAME), so they are not listed here.
	CHECK(Find(1, 0x00EB4C) == nullptr);  // DLC03WorkshopRadiantOwned01
	CHECK(Find(3, 0x00DB37) == nullptr);  // DLC04RaidWipeOut
	// The replacement rules, keyed by runtime ID, never match an add-on's quest (its ID carries its load index).
	CHECK(!Replaces(0x0200EB51u, false, false, false));
	CHECK(!Replaces(0x0000EB51u, false, false, false));
	CHECK(Takes(0x0200EB51u, 0));
	CHECK(OfferOf(0x0200EB51u) == nullptr);
	// A radiant is looked up by form ID before its file (RadiantSync::EntryOf): every entry is found that way.
	for (const auto& entry : RADIANTS) {
		CHECK(MayBeRadiant(entry.quest));
	}
	CHECK(!MayBeRadiant(0x000001));
}

TEST("radiants: StageRules names the game files in the order questscan reads them (tools/questscan.cs MASTERS)")
{
	using StageRules::File;
	CHECK(StageRules::FileName(File::kFallout4) == "Fallout4.esm");
	CHECK(StageRules::FileName(File::kCoast) == "DLCCoast.esm");
	CHECK(StageRules::FileName(File::kRobot) == "DLCRobot.esm");
	CHECK(StageRules::FileName(File::kNukaWorld) == "DLCNukaWorld.esm");
	// RADIANTS' file numbers are StageRules::File's: FileOf reads each name back as its own number.
	for (std::uint8_t file = 0; file <= 3; ++file) {
		const auto back = StageRules::FileOf(StageRules::FileName(static_cast<File>(file)));
		REQUIRE(back.has_value());
		CHECK(static_cast<std::uint8_t>(*back) == file);
	}
	CHECK(StageRules::FileOf("dlccoast.ESM") == File::kCoast);  // the game compares names without regard to case
	CHECK(!StageRules::FileOf("DLCworkshop01.esm"));
}

TEST("radiants: how a Minutemen job was given is two bits with its run, and a friend's game sets only those it lacks")
{
	constexpr auto PRESTON = Protocol::RADIANT_INTRO_PRESTON;
	constexpr auto RADIO = Protocol::RADIANT_INTRO_RADIO;
	constexpr auto BOTH = Protocol::RADIANT_INTRO_MASK;
	// MinRecruitQuestScript.OnStoryScript: aiValue1 == 1 sets PrestonIntroOn, 2 sets RadioIntroOn.
	CHECK(Intro(false, false) == 0);
	CHECK(Intro(true, false) == PRESTON);
	CHECK(Intro(false, true) == RADIO);
	CHECK(Intro(true, true) == BOTH);
	CHECK(PRESTON != RADIO);
	CHECK((PRESTON & RADIO) == 0);
	// A friend's game sets what the owner's holds and its own doesn't (what it clears again: IntroSet, below).
	CHECK(IntroToSet(PRESTON, 0) == PRESTON);
	CHECK(IntroToSet(PRESTON, PRESTON) == 0);
	CHECK(IntroToSet(0, PRESTON) == 0);
	CHECK(IntroToSet(BOTH, RADIO) == PRESTON);
	CHECK(IntroToSet(RADIO, PRESTON) == RADIO);
	CHECK(IntroToSet(0xFF, 0) == BOTH);  // a bit this build doesn't know is no variable
	CHECK(IntroText(0) == "neither Preston nor Radio Freedom");
	CHECK(IntroText(PRESTON) == "Preston");
	CHECK(IntroText(RADIO) == "Radio Freedom");
	CHECK(IntroText(BOTH) == "Preston and Radio Freedom");
	// The script and its variables, by the names they are found under (questscan -Function, 1.11.240).
	CHECK(INTRO_SCRIPT == "MinRecruitQuestScript");
	CHECK(INTRO_PRESTON == "PrestonIntroOn");
	CHECK(INTRO_RADIO == "RadioIntroOn");
	CHECK(INTRO_SCRIPT == OWNED_SCRIPTS[0]);
}

TEST("radiants: our Minutemen job sent before its script knew how it was given is read again every two seconds, until it is sent with one")
{
	using namespace std::chrono_literals;
	constexpr auto        PRESTON = Protocol::RADIANT_INTRO_PRESTON;
	const IntroSent::Time t0{};
	IntroSent             sent;
	CHECK(sent.Size() == 0);
	// The handler runs after the quest is seen started: Tenpines' job goes out with nothing, the radio's with its own.
	sent.Sent(MIN_RECRUIT01, 0);
	sent.Sent(MIN_RECRUIT02, Protocol::RADIANT_INTRO_RADIO);
	CHECK(sent.Size() == 2);
	CHECK((sent.Due(t0) == std::vector<std::uint32_t>{ MIN_RECRUIT01 }));
	CHECK(sent.Due(t0 + 1s).empty());  // not before INTRO_READ_EVERY
	CHECK(INTRO_READ_EVERY == 2s);
	// Still nothing in the script: nothing to send. Then Preston's: the run is sent again, and read no more.
	CHECK(!sent.Changed(MIN_RECRUIT01, 0));
	CHECK(sent.Changed(MIN_RECRUIT01, PRESTON));
	CHECK(!sent.Changed(MIN_OWNED01, PRESTON));  // not a run we sent
	sent.Sent(MIN_RECRUIT01, PRESTON);
	CHECK(!sent.Changed(MIN_RECRUIT01, PRESTON));
	CHECK(sent.Due(t0 + INTRO_READ_EVERY).empty());
	// A run that ended is forgotten; the session's end forgets them all, and the wait with them.
	sent.Sent(MIN_OWNED01, 0);
	CHECK((sent.Due(t0 + 2 * INTRO_READ_EVERY) == std::vector<std::uint32_t>{ MIN_OWNED01 }));
	sent.Forget(MIN_OWNED01);
	CHECK(sent.Due(t0 + 3 * INTRO_READ_EVERY).empty());
	CHECK(sent.Size() == 2);
	sent.Clear();
	CHECK(sent.Size() == 0);
	sent.Sent(MIN_OWNED01, 0);
	CHECK((sent.Due(t0) == std::vector<std::uint32_t>{ MIN_OWNED01 }));
}

TEST("radiants: how the job was given is the last byte of a run's start, and a start without it or with a bit unknown is refused")
{
	using MT = Protocol::MessageType;
	const Protocol::RadiantStart start{ 2, MIN_RECRUIT01, 7, { { 20, 0, TENPINES_WORKSHOP } }, { 10 }, Protocol::RADIANT_INTRO_PRESTON };
	for (const auto type : { MT::kReportRadiantStart, MT::kRadiantStarted }) {
		auto       packet = Protocol::Encode(start, type);
		const auto back = Protocol::DecodeRadiantStart(packet);
		REQUIRE(back.has_value());
		CHECK((*back == start));
		CHECK(back->intro == Protocol::RADIANT_INTRO_PRESTON);
		REQUIRE(!packet.empty());
		CHECK(packet.back() == Protocol::RADIANT_INTRO_PRESTON);
		packet.back() = Protocol::RADIANT_INTRO_MASK;
		CHECK(Protocol::DecodeRadiantStart(packet).has_value());
		packet.back() = 4;  // no such way
		CHECK(!Protocol::DecodeRadiantStart(packet).has_value());
		packet.pop_back();  // protocol 77's start
		CHECK(!Protocol::DecodeRadiantStart(packet).has_value());
	}
	// Every other quest's start says nothing.
	const auto plain = Protocol::DecodeRadiantStart(Protocol::Encode(Protocol::RadiantStart{ 2, RRR03, 7, {}, {} }, MT::kRadiantStarted));
	REQUIRE(plain.has_value());
	CHECK(plain->intro == 0);
}

TEST("radiants: a run's start sent again keeps how the job was given from both sends")
{
	constexpr auto         PRESTON = Protocol::RADIANT_INTRO_PRESTON;
	Protocol::RadiantStart waiting{ 2, MIN_RECRUIT01, 40, {}, { 10 } };
	// The owner's script said so after its first send: the send that follows brings it.
	REQUIRE(Supersedes(waiting, Protocol::RadiantStart{ 2, MIN_RECRUIT01, 40, {}, { 10 }, PRESTON }));
	CHECK(waiting.intro == PRESTON);
	// It is only ever set: a later send with nothing (there is none in the owner's game) clears nothing.
	REQUIRE(Supersedes(waiting, Protocol::RadiantStart{ 2, MIN_RECRUIT01, 40, {}, { 10 } }));
	CHECK(waiting.intro == PRESTON);
	// Another run's is not this one's.
	CHECK(!Supersedes(waiting, Protocol::RadiantStart{ 2, MIN_RECRUIT01, 41, {}, {}, Protocol::RADIANT_INTRO_RADIO }));
	CHECK(waiting.intro == PRESTON);
}

TEST("radiants: Preston's offer is held in a friend's game only until the job is taken, so only the owner's game asks for his next job (3 players)")
{
	constexpr auto    PRESTON = Protocol::RADIANT_INTRO_PRESTON;
	const IntroStages marks{ 20, 200, 300 };  // MinRecruit01's activeStage, successStage, failureStage (its script's own)
	// Each game's script of the job: the owner's handler set Preston's from the story event, which the two friends'
	// games never saw.
	const std::uint8_t owner = Intro(true, false);
	std::uint8_t       theirs[2]{ 0, 0 };
	IntroSet           kept[2];
	// The run starts in each friend's game with the stages done so far (10: started, Preston's to offer): no take.
	const std::uint16_t offered[]{ 10 };
	for (int i = 0; i < 2; ++i) {
		const auto set = IntroToSet(owner, theirs[i], IntroTaken(marks, offered));
		CHECK(set == PRESTON);
		theirs[i] = static_cast<std::uint8_t>(theirs[i] | set);
		kept[i].Set(MIN_RECRUIT01, set, marks);
	}
	// Untaken, every game's daily update leaves the job alone (PrestonIntroOn in all three).
	CHECK(theirs[0] == PRESTON);
	CHECK(theirs[1] == PRESTON);
	// A stage that is none of the three clears nothing (80: the settlement's spokesman spoken to).
	const std::uint16_t spoken[]{ 80 };
	CHECK(kept[0].Taken(MIN_RECRUIT01, spoken) == 0);
	CHECK(kept[0].Of(MIN_RECRUIT01) == PRESTON);
	// The owner takes the job (20: in the list): each friend's game clears what it set, before the stage is set there.
	const std::uint16_t take[]{ 20 };
	for (int i = 0; i < 2; ++i) {
		theirs[i] = static_cast<std::uint8_t>(theirs[i] & ~kept[i].Taken(MIN_RECRUIT01, take));
		CHECK(kept[i].Empty());
	}
	// The job ends (FinishQuest asks for Preston's next job where PrestonIntroOn holds): asked for once, by the owner's.
	const int asking = ((owner & PRESTON) ? 1 : 0) + ((theirs[0] & PRESTON) ? 1 : 0) + ((theirs[1] & PRESTON) ? 1 : 0);
	CHECK(asking == 1);
	CHECK((owner & PRESTON) != 0);
	CHECK(theirs[0] == 0);
	CHECK(theirs[1] == 0);
	// The end's own stage (400, whose FinishQuest asks) clears nothing more, and the owner's game never clears: it set
	// nothing itself.
	const std::uint16_t end[]{ 400 };
	IntroSet            owners;
	CHECK(owners.Taken(MIN_RECRUIT01, end) == 0);
	CHECK(kept[0].Taken(MIN_RECRUIT01, end) == 0);
	CHECK(kept[1].Release(MIN_RECRUIT01) == 0);
}

TEST("radiants: a job is taken or over at its script's own three stages, and one that comes so is not set as Preston's to offer")
{
	constexpr auto    PRESTON = Protocol::RADIANT_INTRO_PRESTON;
	constexpr auto    RADIO = Protocol::RADIANT_INTRO_RADIO;
	constexpr auto    BOTH = Protocol::RADIANT_INTRO_MASK;
	const IntroStages marks{ 20, 200, 300 };  // MinRecruit01's (its script's own)
	const std::uint16_t offered[]{ 10, 80 }, take[]{ 20 }, dead[]{ 200 }, failed[]{ 80, 300 }, done[]{ 20, 100, 200, 400 }, zero[]{ 0 };
	CHECK(!IntroTaken(marks, offered));
	CHECK(IntroTaken(marks, take));
	CHECK(IntroTaken(marks, dead));    // its targets dealt with: over
	CHECK(IntroTaken(marks, failed));  // failed before it was taken: over
	CHECK(IntroTaken(marks, done));
	CHECK(!IntroTaken(marks, {}));
	// A stage the script doesn't say (0 or below) is none: stage 0 is no take.
	CHECK(!IntroTaken(IntroStages{}, zero));
	CHECK(!IntroTaken(IntroStages{ -1, 0, 0 }, zero));
	// Taken or over as the run comes: Preston's is not set. Radio Freedom's, which nothing reads, is set as the owner's.
	CHECK(IntroToSet(PRESTON, 0, false) == PRESTON);
	CHECK(IntroToSet(PRESTON, 0, true) == 0);
	CHECK(IntroToSet(BOTH, 0, true) == RADIO);
	CHECK(IntroToSet(BOTH, RADIO, true) == 0);
	CHECK(IntroToSet(BOTH, 0, false) == BOTH);
	// The properties, by the names they are found under (questscan -Function, 1.11.240).
	REQUIRE(INTRO_STAGES.size() == 3);
	CHECK(INTRO_STAGES[0] == "activeStage");
	CHECK(INTRO_STAGES[1] == "successStage");
	CHECK(INTRO_STAGES[2] == "failureStage");
}

TEST("radiants: the settlement refusing the help leaves Preston's offer to be cleared as the job's take, which the refusal sets itself, and asks nobody for a next job")
{
	constexpr auto    PRESTON = Protocol::RADIANT_INTRO_PRESTON;
	const IntroStages recruit01{ 20, 200, 300 };
	const IntroStages recruit02{ 20, 200, 250 };
	// The refusal's own stage is none of the three: it ends nothing (its fragment calls no FinishQuest, no Stop and
	// sets no failure; questscan -Companion, 1.11.240).
	const std::uint16_t refusal01[]{ 90 }, refusal02[]{ 85 };
	CHECK(!IntroTaken(recruit01, refusal01));
	CHECK(!IntroTaken(recruit02, refusal02));
	// Its fragment's first call sets 20, the take: taken from then on, wherever the refusal ran.
	const std::uint16_t refused01[]{ 90, 20 }, refused02[]{ 85, 20 };
	CHECK(IntroTaken(recruit01, refused01));
	CHECK(IntroTaken(recruit02, refused02));

	// The usual order: Preston's offer was taken (20) before the settlement was asked, so a friend's game cleared what it
	// set then; the refusal that follows finds nothing to clear, and the copy holds no Preston's at the job's end.
	IntroSet kept;
	kept.Set(MIN_RECRUIT01, PRESTON, recruit01);
	const std::uint16_t take[]{ 20 };
	CHECK(kept.Taken(MIN_RECRUIT01, take) == PRESTON);
	CHECK(kept.Taken(MIN_RECRUIT01, refusal01) == 0);
	CHECK(kept.Empty());
	// The settlement asked before Preston's offer was taken: the refusal sets 20 there itself, and the poll that follows
	// it (SweepIntros, with the stages done) clears Preston's. Nothing between reads it: the daily update reads it only
	// while 20 is not done, and only the job's 400 and failure stage ask for the next job.
	kept.Set(MIN_RECRUIT02, PRESTON, recruit02);
	CHECK(kept.Taken(MIN_RECRUIT02, refusal02) == 0);
	CHECK(kept.Of(MIN_RECRUIT02) == PRESTON);
	const std::uint16_t doneAfter[]{ 10, 80, 85, 20 };
	CHECK(kept.Taken(MIN_RECRUIT02, doneAfter) == PRESTON);
	CHECK(kept.Empty());
	// Its timeout's failure, later, is one of the three: cleared before it is set, had nothing cleared it yet.
	kept.Set(MIN_RECRUIT02, PRESTON, recruit02);
	const std::uint16_t failed[]{ 250 };
	CHECK(kept.Taken(MIN_RECRUIT02, failed) == PRESTON);
}

TEST("radiants: a job whose script leaves one of its three stages at 0 or below is not set as Preston's to offer in a friend's game")
{
	CHECK(IntroMarksUsable(IntroStages{ 20, 200, 300 }));
	CHECK(IntroMarksUsable(IntroStages{ 1, 1, 1 }));
	// One left at 0 or below would never clear Preston's at that stage, so the friend's copy could reach its end, which
	// asks for the next job, still holding it.
	CHECK(!IntroMarksUsable(IntroStages{ 0, 200, 300 }));
	CHECK(!IntroMarksUsable(IntroStages{ 20, 0, 300 }));
	CHECK(!IntroMarksUsable(IntroStages{ 20, 200, -1 }));
	CHECK(!IntroMarksUsable(IntroStages{}));
	// Not usable reads as unreadable (SetIntro: taken, so Preston's is not set; Radio Freedom's still is).
	CHECK(IntroToSet(Protocol::RADIANT_INTRO_PRESTON, 0, true) == 0);
	CHECK(IntroToSet(Protocol::RADIANT_INTRO_MASK, 0, true) == Protocol::RADIANT_INTRO_RADIO);
}

TEST("radiants: what a friend's game set of how a job was given is remembered until the job is taken, and all of it is cleared when the run is shared no more")
{
	constexpr auto    PRESTON = Protocol::RADIANT_INTRO_PRESTON;
	constexpr auto    RADIO = Protocol::RADIANT_INTRO_RADIO;
	constexpr auto    BOTH = Protocol::RADIANT_INTRO_MASK;
	const IntroStages marks{ 20, 200, 300 };
	const IntroStages others{ 20, 200, 250 };  // MinRecruit02's: its failure is 250
	const std::uint16_t take[]{ 20 }, fail[]{ 300 };
	IntroSet set;
	CHECK(set.Empty());
	set.Set(MIN_RECRUIT01, 0, marks);  // nothing set here (this game's own handler had): nothing to clear later
	CHECK(set.Empty());
	set.Set(MIN_RECRUIT01, PRESTON, marks);
	set.Set(MIN_RECRUIT02, BOTH, others);
	set.Set(MIN_OWNED01, RADIO, marks);
	CHECK(set.Size() == 3);
	CHECK(set.Quests().size() == 3);
	CHECK(set.Of(MIN_RECRUIT01) == PRESTON);
	CHECK(set.Of(MIN_RECRUIT02) == BOTH);
	CHECK(set.Of(RRR03) == 0);
	// Set again with the other (the owner's script said so later, and its run was sent again): both remembered.
	set.Set(MIN_RECRUIT01, RADIO, marks);
	CHECK(set.Of(MIN_RECRUIT01) == BOTH);
	// The first's job is taken: Preston's to clear, Radio Freedom's left as set, the quest remembered no more.
	CHECK(set.Taken(MIN_RECRUIT01, take) == PRESTON);
	CHECK(set.Of(MIN_RECRUIT01) == 0);
	CHECK(set.Taken(MIN_RECRUIT01, take) == 0);  // once
	// One set only as Radio Freedom's: nothing to clear at its take.
	CHECK(set.Taken(MIN_OWNED01, take) == 0);
	CHECK(set.Of(MIN_OWNED01) == 0);
	// The session ends with the second's job untaken: all this game set there is to clear, once. Nothing of a taken one.
	CHECK(set.Release(MIN_RECRUIT02) == BOTH);
	CHECK(set.Release(MIN_RECRUIT02) == 0);
	CHECK(set.Release(MIN_RECRUIT01) == 0);
	CHECK(set.Empty());
	// A quest that stopped is forgotten with nothing to clear.
	set.Set(MIN_RECRUIT01, PRESTON, marks);
	set.Forget(MIN_RECRUIT01);
	CHECK(set.Release(MIN_RECRUIT01) == 0);

	// A save keeps it, with each quest's stages; a load takes the save's in place of what was known.
	set.Set(MIN_RECRUIT01, PRESTON, marks);
	set.Set(MIN_RECRUIT02, RADIO, others);
	const auto saved = set.Save();
	REQUIRE(saved.size() == 2);
	IntroSet loaded;
	loaded.Set(RRR03, PRESTON, marks);  // of the world before the load
	loaded.Load(saved);
	CHECK(loaded.Size() == 2);
	CHECK(loaded.Of(RRR03) == 0);
	CHECK(loaded.Of(MIN_RECRUIT01) == PRESTON);
	CHECK(loaded.Taken(MIN_RECRUIT02, fail) == 0);  // its own failure is 250
	CHECK(loaded.Of(MIN_RECRUIT02) == RADIO);
	CHECK(loaded.Taken(MIN_RECRUIT01, fail) == PRESTON);
	// A record's entry with nothing set, or with bits this build doesn't know, is none.
	const IntroKept odd[]{ { RRR03, 0, marks }, { CORVEGA, 0xFC, marks } };
	loaded.Load(odd);
	CHECK(loaded.Empty());
	set.Clear();
	CHECK(set.Empty());
	CHECK(set.Save().empty());
}

TEST("radiants: a save's record keeps what this game set of how friends' jobs were given, under a version of its own")
{
	const IntroStages marks{ 20, 200, 300 };
	Saved             saved{ 7, { { MIN_RECRUIT01, { { 2, 5 } }, { { 4, 1, CORVEGA } }, 7 } }, { { 7, { { 3, 4 } } } } };
	// None set: the record every build reads.
	auto bytes = Encode(saved);
	CHECK(bytes[0] == SAVED_VERSION);
	const auto plain = Decode(bytes);
	REQUIRE(plain.has_value());
	CHECK(*plain == saved);
	CHECK(plain->intros.empty());

	saved.intros = { { MIN_RECRUIT01, Protocol::RADIANT_INTRO_PRESTON, marks }, { MIN_RECRUIT02, Protocol::RADIANT_INTRO_MASK, IntroStages{ 20, 200, 250 } } };
	bytes = Encode(saved);
	CHECK(bytes[0] == SAVED_VERSION_INTROS);
	const auto read = Decode(bytes);
	REQUIRE(read.has_value());
	CHECK(*read == saved);
	REQUIRE(read->intros.size() == 2);
	CHECK(read->intros[0].quest == MIN_RECRUIT01);
	CHECK(read->intros[0].set == Protocol::RADIANT_INTRO_PRESTON);
	CHECK(read->intros[1].marks[2] == 250);
	// Damaged: cut short, a byte too many, the older version's number over these bytes.
	CHECK(!Decode(std::span{ bytes }.first(bytes.size() - 1)).has_value());
	auto longer = bytes;
	longer.push_back(0);
	CHECK(!Decode(longer).has_value());
	auto older = bytes;
	older[0] = static_cast<std::uint8_t>(SAVED_VERSION);
	CHECK(!Decode(older).has_value());

	// More than a record holds: the first MAX_SAVED_INTROS are kept.
	for (std::uint32_t i = 0; i < MAX_SAVED_INTROS + 4; ++i) {
		saved.intros.push_back({ 0x2000 + i, Protocol::RADIANT_INTRO_PRESTON, marks });
	}
	const auto capped = Decode(Encode(saved));
	REQUIRE(capped.has_value());
	CHECK(capped->intros.size() == MAX_SAVED_INTROS);
	CHECK(capped->intros.front().quest == MIN_RECRUIT01);
	CHECK(capped->shared == saved.shared);
	// A count past the bound is no record: the version, the session, no quest, no session, then the count.
	auto small = Encode(Saved{ 7, {}, {}, { { MIN_RECRUIT01, Protocol::RADIANT_INTRO_PRESTON, marks } } });
	REQUIRE(small.size() == 2 + 8 + 1 + 1 + 1 + 4 + 1 + 12);
	CHECK(Decode(small).has_value());
	small[12] = static_cast<std::uint8_t>(MAX_SAVED_INTROS + 1);
	CHECK(!Decode(small).has_value());
}

TEST("radiants: a friend's settlement job started here is the player's own settlement's only when the player's NPC owns its workbench")
{
	// MinRecruitQuestScript.OnStoryScript's own test (GetActorOwner() == Game.GetPlayer().GetActorBase()), which a
	// friend's run started with no story event never runs.
	CHECK(StartsOwned(0x00000007));   // the player's NPC
	CHECK(!StartsOwned(0));           // nobody's, or a faction's (GetActorOwner gives none)
	CHECK(!StartsOwned(0x0001C21C));  // PlayerFaction owns a workbench the handler doesn't count
	CHECK(!StartsOwned(0x00000014));  // the player's reference is no owner
	// The script whose stages read it (questscan -StoryEvents, 1.11.240), and the names it is found by. Far Harbor's
	// handler sets it too, and nothing reads it there: not written.
	REQUIRE(OWNED_SCRIPTS.size() == 1);
	CHECK(OWNED_SCRIPTS[0] == "MinRecruitQuestScript");
	CHECK(OWNED_VARIABLE == "startsOwned");
	CHECK(OWNED_WORKSHOP == "Workshop");
}

namespace
{
	constexpr std::uint32_t COURSER_JOB = 0x00186C79;       // Variable Removal: the courser (alias 5) made at SynthBoss (alias 4)
	constexpr std::uint32_t GANG_JOB = 0x0013A33D;          // To the Mattresses: seven made at CurrentBoss (alias 1)
	constexpr std::uint32_t BOSS_INDOORS = 0x000A7C11;      // the owner's reference: indoors
	constexpr std::uint32_t BOSS_OUTDOORS = 0x000B2D40;     // this game's own pick, of the same location
	constexpr std::uint32_t UNIVERSITY_POINT = 0x0002BE7F;
}

TEST("radiant places: a followed courser job whose courser this game made at another reference of the place is to be put at the owner's")
{
	const std::vector<RadiantFill> fills{ { 2, 1, UNIVERSITY_POINT }, { 4, 0, BOSS_INDOORS }, { 5, 0, SPAWN } };
	const auto                     loaded = [](std::uint32_t) { return true; };
	const auto                     places = PlacesOf(0, COURSER_JOB, fills, [](std::uint32_t a_alias) { return a_alias == 4 ? BOSS_OUTDOORS : 0u; }, loaded);
	REQUIRE(places.size() == 1);
	CHECK((places[0] == Place{ 4, BOSS_INDOORS, BOSS_OUTDOORS, PlaceState::kDiffers, { 5 } }));
	// The same pick in both games: nothing to do.
	const auto same = PlacesOf(0, COURSER_JOB, fills, [](std::uint32_t) { return BOSS_INDOORS; }, loaded);
	REQUIRE(same.size() == 1);
	CHECK(same[0].state == PlaceState::kSame);
	// The owner's reference not in memory here: known, and nothing to force or move onto. This game's alias left empty is
	// no different.
	const auto unloaded = PlacesOf(0, COURSER_JOB, fills, [](std::uint32_t) { return 0u; }, [](std::uint32_t) { return false; });
	REQUIRE(unloaded.size() == 1);
	CHECK((unloaded[0] == Place{ 4, BOSS_INDOORS, 0, PlaceState::kNotLoaded, { 5 } }));
	// Whether it is loaded is asked of the owner's reference, only when the two differ.
	std::vector<std::uint32_t> asked;
	(void)PlacesOf(0, COURSER_JOB, fills, [](std::uint32_t) { return BOSS_OUTDOORS; }, [&](std::uint32_t a_form) {
		asked.push_back(a_form);
		return true;
	});
	CHECK(asked == std::vector<std::uint32_t>{ BOSS_INDOORS });
	asked.clear();
	(void)PlacesOf(0, COURSER_JOB, fills, [](std::uint32_t) { return BOSS_INDOORS; }, [&](std::uint32_t a_form) {
		asked.push_back(a_form);
		return true;
	});
	CHECK(asked.empty());
}

TEST("radiant places: a place the owner's game left empty or filled with what this game can't name is this game's own")
{
	const auto here = [](std::uint32_t) { return BOSS_OUTDOORS; };
	const auto loaded = [](std::uint32_t) { return true; };
	CHECK(PlacesOf(0, COURSER_JOB, std::vector<RadiantFill>{ { 4, 0, 0 } }, here, loaded).empty());
	CHECK(PlacesOf(0, COURSER_JOB, std::vector<RadiantFill>{ { 4, 0, SPAWN } }, here, loaded).empty());
	CHECK(PlacesOf(0, COURSER_JOB, std::vector<RadiantFill>{ { 4, 0, RUNTIME_NPC } }, here, loaded).empty());
	CHECK(PlacesOf(0, COURSER_JOB, std::vector<RadiantFill>{ { 2, 1, UNIVERSITY_POINT } }, here, loaded).empty());  // not sent
	CHECK(PlacesOf(0, COURSER_JOB, std::vector<RadiantFill>{ { 4, 1, UNIVERSITY_POINT } }, here, loaded).empty());  // a location's fill of that number
	// A quest that makes nothing at a place the game picks, and another file's quest of the same number.
	const std::vector<RadiantFill> fills{ { 4, 0, BOSS_INDOORS } };
	CHECK(PlacesOf(0, RRR03, fills, here, loaded).empty());
	CHECK(PlacesOf(1, COURSER_JOB, fills, here, loaded).empty());
}

TEST("radiant places: only the courser's job is listed, and another quest of the kind has no place to force or move to")
{
	REQUIRE(MADE_AT.size() == 1);
	CHECK((MADE_AT[0] == MadeAt{ COURSER_JOB, 5, 4 }));
	// To the Mattresses makes seven at CurrentBoss, which the quest fills anew with each boss: not listed, so its alias
	// is never forced and none of its seven is ever moved, whatever the two games hold.
	const std::vector<RadiantFill> fills{ { 0, 1, CORVEGA }, { 1, 0, BOSS_INDOORS } };
	CHECK(PlacesOf(0, GANG_JOB, fills, [](std::uint32_t) { return BOSS_OUTDOORS; }, [](std::uint32_t) { return true; }).empty());
}

TEST("radiant places: every listed quest is one whose actors are made at runtime, and no actor is listed twice or as a place")
{
	for (const auto& row : MADE_AT) {
		const auto entry = Find(0, row.quest);
		REQUIRE(entry != nullptr);
		CHECK(entry->spawned);
		CHECK(entry->share == Share::kShared);
		CHECK(row.actor != row.at);
		CHECK(std::ranges::count_if(MADE_AT, [&](const MadeAt& a_other) { return a_other.quest == row.quest && a_other.actor == row.actor; }) == 1);
		// A place is no actor made at another.
		CHECK(std::ranges::none_of(MADE_AT, [&](const MadeAt& a_other) { return a_other.quest == row.quest && a_other.actor == row.at; }));
	}
}

TEST("radiant places: the alias is asked onto the owner's reference a few times, then given up on once")
{
	using namespace std::chrono_literals;
	using Step = PlaceBook::Step;
	PlaceBook  book;
	const auto t0 = PlaceBook::Time{} + 1h;
	CHECK(!book.Asked(COURSER_JOB, 4, BOSS_INDOORS));
	CHECK(book.Same(COURSER_JOB, 4, BOSS_INDOORS) == 0);  // this game's own pick: nothing to tell
	CHECK(book.Differs(COURSER_JOB, 4, BOSS_INDOORS, t0) == Step::kAsk);
	CHECK(book.Asked(COURSER_JOB, 4, BOSS_INDOORS));
	CHECK(!book.Asked(COURSER_JOB, 4, BOSS_OUTDOORS));
	CHECK(book.Differs(COURSER_JOB, 4, BOSS_INDOORS, t0 + 1s) == Step::kWait);  // the script VM has not run the force yet
	CHECK(book.Differs(COURSER_JOB, 4, BOSS_INDOORS, t0 + PLACE_ASK_EVERY) == Step::kAsk);
	CHECK(book.Differs(COURSER_JOB, 4, BOSS_INDOORS, t0 + 2 * PLACE_ASK_EVERY) == Step::kAsk);
	CHECK(PLACE_ASKS == 3);
	// The last ask has its time too, then it is given up on, told once.
	CHECK(book.Differs(COURSER_JOB, 4, BOSS_INDOORS, t0 + 2 * PLACE_ASK_EVERY + 1s) == Step::kWait);
	CHECK(book.Differs(COURSER_JOB, 4, BOSS_INDOORS, t0 + 3 * PLACE_ASK_EVERY) == Step::kGiveUp);
	CHECK(book.Differs(COURSER_JOB, 4, BOSS_INDOORS, t0 + 4 * PLACE_ASK_EVERY) == Step::kWait);
	CHECK(book.Differs(COURSER_JOB, 4, BOSS_INDOORS, t0 + 1h) == Step::kWait);
	// Another reference of the owner's (their run's fills sent again) is asked afresh; another quest's is its own.
	CHECK(book.Differs(COURSER_JOB, 4, BOSS_OUTDOORS, t0 + 1h) == Step::kAsk);
	CHECK(book.Differs(GANG_JOB, 1, BOSS_INDOORS, t0 + 1h) == Step::kAsk);
}

TEST("radiant places: an alias that holds the owner's reference after an ask is told once, with the asks it took")
{
	using namespace std::chrono_literals;
	PlaceBook  book;
	const auto t0 = PlaceBook::Time{} + 1h;
	CHECK(book.Differs(COURSER_JOB, 4, BOSS_INDOORS, t0) == PlaceBook::Step::kAsk);
	CHECK(book.Differs(COURSER_JOB, 4, BOSS_INDOORS, t0 + PLACE_ASK_EVERY) == PlaceBook::Step::kAsk);
	CHECK(book.Same(COURSER_JOB, 4, BOSS_OUTDOORS) == 0);  // not the reference it was asked onto
	CHECK(book.Same(COURSER_JOB, 4, BOSS_INDOORS) == 2);
	CHECK(book.Same(COURSER_JOB, 4, BOSS_INDOORS) == 0);
	CHECK(book.Asked(COURSER_JOB, 4, BOSS_INDOORS));  // what was made at the other reference is still to be moved
}

TEST("radiant places: the owner's reference not in memory here is said once, and a run followed no more is forgotten")
{
	using namespace std::chrono_literals;
	PlaceBook  book;
	const auto t0 = PlaceBook::Time{} + 1h;
	CHECK(book.NotLoaded(COURSER_JOB, 4, BOSS_INDOORS));
	CHECK(!book.NotLoaded(COURSER_JOB, 4, BOSS_INDOORS));
	CHECK(!book.Asked(COURSER_JOB, 4, BOSS_INDOORS));  // waiting is no ask
	// In memory later (our player came by): asked then.
	CHECK(book.Differs(COURSER_JOB, 4, BOSS_INDOORS, t0) == PlaceBook::Step::kAsk);
	CHECK(book.Differs(GANG_JOB, 1, BOSS_INDOORS, t0) == PlaceBook::Step::kAsk);
	book.Forget(COURSER_JOB);
	CHECK(!book.Asked(COURSER_JOB, 4, BOSS_INDOORS));
	CHECK(book.NotLoaded(COURSER_JOB, 4, BOSS_INDOORS));
	CHECK(book.Asked(GANG_JOB, 1, BOSS_INDOORS));  // the other quest's is kept
	book.Clear();
	CHECK(!book.Asked(GANG_JOB, 1, BOSS_INDOORS));
}

TEST("radiant places: an actor is moved only in a run this game's own start just made, once for the job, and never one that was loaded here")
{
	PlaceBook book;
	// A run followed with no start of ours in this session (a save loaded inside the job): whom our player met is not
	// known, so nothing of it is moved.
	CHECK(!book.Moves(COURSER_JOB, 5, OUR_COPY + 1));
	book.Started(COURSER_JOB);
	CHECK(book.Moves(COURSER_JOB, 5, OUR_COPY + 1));
	CHECK(!book.Moves(COURSER_JOB, 5, OUR_COPY + 1));  // once, wherever the owner's reference is by then
	CHECK(!book.Moves(GANG_JOB, 7, OUR_COPY + 1));     // another quest's run, not started here

	// An escort our player walked with: loaded once, so it stays where it is though it is unloaded again later.
	book.Started(GANG_JOB);
	book.Loaded(GANG_JOB, OUR_COPY + 2);
	CHECK(!book.Moves(GANG_JOB, 7, OUR_COPY + 2));
	CHECK(!book.Moves(GANG_JOB, 7, OUR_COPY + 2));
	CHECK(book.Moves(GANG_JOB, 8, OUR_COPY + 3));  // one beside it that never was
	// Loaded after its one move: that was its move.
	book.Loaded(GANG_JOB, OUR_COPY + 3);
	CHECK(!book.Moves(GANG_JOB, 8, OUR_COPY + 3));

	// The run is followed no more: the next run of the quest starts with nothing known, and is moved only once this
	// game's start has made it.
	book.Forget(COURSER_JOB);
	CHECK(!book.Moves(COURSER_JOB, 5, OUR_COPY + 1));
	book.Started(COURSER_JOB);
	CHECK(book.Moves(COURSER_JOB, 5, OUR_COPY + 1));
	CHECK(!book.Moves(GANG_JOB, 8, OUR_COPY + 3));  // the other quest's is kept
	CHECK(!book.Moves(GANG_JOB, 7, OUR_COPY + 2));
	book.Clear();
	CHECK(!book.Moves(GANG_JOB, 9, OUR_COPY + 4));  // no session: nothing started
	book.Started(GANG_JOB);
	CHECK(book.Moves(GANG_JOB, 7, OUR_COPY + 2));   // and nothing remembered as loaded
}

TEST("radiant places: none of ours is moved to a reference our player is near")
{
	CHECK(PlaceIsNear(true, false, 0.0f));               // in the same room
	CHECK(PlaceIsNear(false, true, PLACE_NEAR - 1.0f));  // outdoors, within what is loaded around our player
	CHECK(!PlaceIsNear(false, true, PLACE_NEAR));
	CHECK(!PlaceIsNear(false, false, 100.0f));  // our player outdoors by the door, the reference indoors
	CHECK(PLACE_NEAR >= 2 * 6000.0f);           // SpawnBook::PLACE_RANGE: our copy of theirs is placed well within it
}
