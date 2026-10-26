#pragma once

#include "Protocol.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

// Radiant quests: the ones the game hands out again and again, each time picking its own targets (the
// settlement that asks, the place to clear, the person kidnapped) by the aliases' conditions. Each game
// picks its own, so a stage of one player's run means nothing in another's: the "clear the raiders" stage
// of a friend's run at Corvega would finish yours at Tenpines Bluff. So a radiant is shared by its run:
// when one player's game starts one, the friends' games start the same quest with the same fills (the
// same settlement, place and people), and only then do its stages go between them, under that run's name.
// A fill one game can't match (an NPC made at runtime that isn't shared, a reference that isn't there)
// leaves that run each player's own, said in the log, so neither game's quest is broken.
//
// Which radiants are shared (RADIANTS, all from Fallout4.esm): every one whose targets are the game's own
// references and locations (a settlement, a dungeon, its people, bosses and map marker, a container), with items
// the quest makes (a locket, a holotape, a tracker) made in each game for its own player. The Minutemen's
// settlement requests, Preston's requests from settlements the player has (but their attacks), the Brotherhood's,
// the Institute's and the Railroad's jobs, and Diamond City's wanted posters. Left alone, each player's own: the
// ones whose targets or helpers are NPCs the quest makes at runtime (settlement attacks and the Castle's, a synth
// boss, refugees, a courser, Brotherhood squires and scribes, a patrol, a vertibird, an assault's defenders). One
// game's runtime NPCs are in its friends' games already, as copies (Spawns), placed while both players stand near
// them; a friend's game starting the same quest would make a second set of its own. Those stay unshared even when
// the story is (before, a friend's stage started them here with this game's own targets). Kept free of the game so
// it can be tested; RadiantSync reads the quests.
namespace RadiantRules
{
	enum class Share : std::uint8_t
	{
		kNotRadiant,  // not one of the game's radiants: QuestFilter decides
		kShared,      // shared by its run, with the same fills
		kLeftAlone,   // a radiant each player keeps for themselves
	};

	struct Entry
	{
		std::uint32_t    formId;  // Fallout4.esm (load index 00)
		std::string_view editorId;
		Share            share;
		// A run of it here that this player hasn't been given (no objective shown) may be stopped for a friend's with
		// other targets: the faction's own scripts start these whenever they can (the Institute's when the player joins,
		// the Railroad's at HQ, the Brotherhood's as their giver speaks), so a friend's game nearly always holds one of
		// its own. Only where stopping it before it is given changes nothing else: nothing counted, enabled or hidden
		// by its start, no stage run as it stops, and a quest that can run again (not "run once").
		bool replace = false;
	};

	// Fallout4.esm's radiants (tools/questscan.cs reads them; their aliases were read the same way).
	inline constexpr std::array RADIANTS{
		// The Minutemen's "a settlement needs our help": the settlement (its workshop), the place nearest it and
		// who lives there. Its 400 gives the reward (MinRecruitQuestScript.FinishQuest: MinRadiantReward to the
		// player, the settlement the player's), set once in each game.
		Entry{ 0x0011B36E, "MinRecruit00", Share::kShared },  // raiders (Oberland Station, Tenpines Bluff or another)
		Entry{ 0x00098136, "MinRecruit01", Share::kShared },  // raider troubles
		Entry{ 0x00099848, "MinRecruit02", Share::kShared },  // kidnapping (its 90, paying the ransom, is never set from a friend: StageRules)
		Entry{ 0x00157CA5, "MinRecruit04", Share::kShared },  // clear a place
		Entry{ 0x00186642, "MinRecruit07", Share::kShared },  // ghoul problem
		Entry{ 0x00106F05, "MinRecruit03", Share::kShared },  // Abernathy Farm's locket: the satellite station's container (the locket made in each game's)
		Entry{ 0x0015F03F, "MinRecruit05", Share::kShared },  // a hostile settlement to take, and its bosses
		Entry{ 0x0015F040, "MinRecruit06", Share::kShared },
		Entry{ 0x00157577, "MinRecruit08", Share::kLeftAlone },  // a synth boss made at runtime
		Entry{ 0x00164167, "MinRecruit09", Share::kLeftAlone },  // refugees made at runtime
		// Preston's requests from the player's own settlements.
		Entry{ 0x0003DF95, "MinRadiantOwned02", Share::kShared },               // raiders
		Entry{ 0x0003E0C1, "MinRadiantOwned03ChangeLocOnly", Share::kShared },  // kidnapping
		Entry{ 0x00161D1D, "MinRadiantOwned06ChangeLocOnly", Share::kShared },  // building up a settlement (built in every game)
		Entry{ 0x00162F44, "MinRadiantOwned07ChangeLocOnly", Share::kShared },  // a settlement's generator, a dungeon's part for it
		Entry{ 0x00109D73, "MinRadiantOwned10ChangeLocOnly", Share::kShared },  // a synth among the settlers (one recruited at runtime: each player's own)
		// Settlement attacks and the Castle's: the attackers (and the Castle's waves, its defenders and vertibirds) are
		// made at runtime by the quest, in each game that runs it. One game's attackers are in its friends' games already,
		// as the copies of any NPC made at runtime (Spawns); a friend's game starting the same attack would send a second
		// wave of its own.
		Entry{ 0x000A1412, "MinRadiantOwned01", Share::kLeftAlone },  // raiders
		Entry{ 0x0005E51F, "MinRadiantOwned09ChangeLocOnly", Share::kLeftAlone },
		Entry{ 0x00160409, "MinRadiantOwned05", Share::kLeftAlone },  // super mutants
		Entry{ 0x00186A08, "MinRadiantOwned08", Share::kLeftAlone },  // gunners
		Entry{ 0x000E477F, "MinRadiantOwned04_BOS", Share::kLeftAlone },
		Entry{ 0x001533A9, "MinRadiantOwned11", Share::kLeftAlone },  // synths
		Entry{ 0x000BDB04, "MinDefendCastle", Share::kLeftAlone },    // the Castle attacked
		Entry{ 0x00176328, "MinDefendCastleRadiantStarter", Share::kLeftAlone },
		Entry{ 0x0022FD6E, "MinDefendCastle_Attackers", Share::kLeftAlone },
		// The Brotherhood's. Their giver's line sets their 10 (it starts them), the player taking the job their 20, and
		// the reward is their 50: an item, and BoS200's stage for the job done (shared with the story too, set once). The
		// 0 is the developers' shortcut (it makes the player a member and moves them to the giver): never taken (UNTAKEN).
		// The squire's and the scribe's jobs are left alone: the one who follows the player is made at runtime.
		Entry{ 0x00064EC7, "BoSR01", Share::kShared, true },  // Cleansing the Commonwealth: a dungeon by the player's level, its bosses
		Entry{ 0x000C30DC, "BosR02", Share::kShared, true },  // Quartermastery: a dungeon, the tech in its boss's container (made in each game)
		Entry{ 0x0009A687, "BoSR01PostHello", Share::kLeftAlone },  // dialogue only
		Entry{ 0x000C8675, "BoSR03", Share::kLeftAlone },  // Leading by Example: a squire made at runtime follows the player
		Entry{ 0x000CF3E2, "BoSR04", Share::kLeftAlone },  // Learning Curve: a scribe made at runtime follows the player
		Entry{ 0x000CF3E3, "BoSR04_AO_Scribe", Share::kLeftAlone },
		Entry{ 0x000D1EB2, "BoSR05", Share::kShared, true },  // Feeding the Troops: a settlement (its happiness changed in every game)
		// The Institute's: all five start when the player joins (Inst301) and wait at their 5 until their giver hands one
		// out (its 100); each pays at its turn-in (10 or 20, and 1000).
		Entry{ 0x000F7933, "InstR01", Share::kShared, true },  // Pest Control: a dungeon and its bosses
		Entry{ 0x000EB268, "InstR02", Share::kShared, true },  // Hypothesis: a dungeon, its boss, the data on it (made in each game)
		Entry{ 0x000F0D8C, "InstR05", Share::kShared, true },  // Political Leanings: the same NPC and marker in every game
		Entry{ 0x000E8D19, "InstR01_OLD", Share::kLeftAlone },  // not used by the game
		Entry{ 0x000EC1E9, "InstR03_OLD", Share::kLeftAlone },
		Entry{ 0x000EDCEF, "InstR03NEW", Share::kLeftAlone },  // Appropriation: an assault, its defenders made at runtime
		Entry{ 0x000EDE28, "InstR04", Share::kLeftAlone },     // Reclamation: the synth to bring back is made at runtime
		// The Railroad's. Their HQ and each other's stages start them whenever they can (Butcher's Bill's 100 offers Mercer
		// Safehouse, whose 300 offers Jackpot and Concierge), and most count the job as on offer (RRQuestsAvailable, counted
		// off as the job is taken): one stopped for a friend's run before it was taken is counted off as it stops (OFFERS).
		// Butcher's Bill and its second part run once, and Randolph Safehouse's offer hides the place's enemies until it is
		// taken: those are never stopped (a game holding its own run of one keeps it).
		Entry{ 0x000B1F1D, "RRR01a", Share::kShared },        // Butcher's Bill: a dead drop by its conditions
		Entry{ 0x000B1F1C, "RRR01b", Share::kShared },        // Butcher's Bill 2: a place; the dead captive and what is on them made in each game
		Entry{ 0x000B3E82, "RRR02a", Share::kShared, true },  // Concierge: a place, its people (nothing counted at its start)
		Entry{ 0x000B926B, "RRR03", Share::kShared, true },   // Jackpot: a place and its door; the tracker made in each game
		Entry{ 0x000B26C2, "RRR04", Share::kShared, true },   // Mercer Safehouse: a settlement to build up (built in every game)
		Entry{ 0x000B926A, "RRR05", Share::kShared, true },   // Weathervane: a high point; the MILA made in each game; offered again when done
		Entry{ 0x001845F7, "RRR06", Share::kShared },         // Randolph Safehouse: a dead drop and a place, its bosses
		Entry{ 0x00186C79, "RRR07", Share::kLeftAlone },   // Variable Removal: a courser made at runtime
		Entry{ 0x0013A33D, "RRR08", Share::kLeftAlone },   // To the Mattresses: the bosses are made at runtime
		Entry{ 0x00145F1A, "RRR09", Share::kLeftAlone },   // Lost Soul: the one to rescue is made at runtime
		Entry{ 0x0014A34A, "RRR10", Share::kLeftAlone },   // A Clean Equation: a patrol made at runtime
		Entry{ 0x00150577, "RRR11", Share::kLeftAlone },   // High Ground: a vertibird and its crew made at runtime
		// Diamond City's wanted posters (misc quests): a dungeon and its bosses, the poster's note made in each game. All
		// three start with the game and run once, so a game whose own poster names another dungeon keeps its own.
		Entry{ 0x0014182C, "FFDiamondCityWanted01", Share::kShared },
		Entry{ 0x0018127D, "FFDiamondCityWanted04", Share::kShared },
		Entry{ 0x00181280, "FFDiamondCityWanted07", Share::kShared },
		Entry{ 0x0002C59B, "FFDiamondCity05", Share::kLeftAlone },  // a bloatfly's gland: the bloatfly is made at runtime
	};

	inline const Entry* Find(std::uint32_t a_formId)
	{
		const auto it = std::ranges::find(RADIANTS, a_formId, &Entry::formId);
		return it != RADIANTS.end() ? &*it : nullptr;
	}

	inline Share ShareOf(std::uint32_t a_formId)
	{
		const auto entry = Find(a_formId);
		return entry ? entry->share : Share::kNotRadiant;
	}

	inline bool IsRadiant(std::uint32_t a_formId)
	{
		return ShareOf(a_formId) != Share::kNotRadiant;
	}

	// Stages of a shared radiant never taken from a friend: the developers' shortcuts, which a player never reaches
	// (BoSR01's, BosR02's and BoSR05's 0 make the player a member of the Brotherhood, set its story's stages and move
	// them to their giver). Their startup stage is another; StageRules lists them as moving the player.
	struct Untaken
	{
		std::uint32_t quest;
		std::uint16_t stage;
	};
	inline constexpr std::array UNTAKEN{
		Untaken{ 0x00064EC7, 0 },  // BoSR01
		Untaken{ 0x000C30DC, 0 },  // BosR02
		Untaken{ 0x000D1EB2, 0 },  // BoSR05
	};

	inline bool Takes(std::uint32_t a_quest, std::uint16_t a_stage)
	{
		return std::ranges::none_of(UNTAKEN, [&](const Untaken& a_untaken) { return a_untaken.quest == a_quest && a_untaken.stage == a_stage; });
	}

	// ---- The Railroad's jobs on offer ---------------------------------------------------------------

	// RRQuestsAvailable (Fallout4.esm): the Railroad's jobs on offer at HQ, which the Drummer Boy and the others there
	// speak of. A job's offer counts one in (Mod +1) and the job taken counts it off (Mod -1), so a run stopped between
	// the two would leave it one too high for good.
	inline constexpr std::uint32_t RR_QUESTS_AVAILABLE = 0x0016C2A1;

	// When a job's offer counts it: always, or by another global's value then (one that can't change while it is on offer).
	enum class Counted : std::uint8_t
	{
		kAlways,
		kIfPositive,  // the global is above 0
		kIfZero,      // the global is 0
	};

	struct Offer
	{
		std::uint32_t quest;
		std::uint16_t offered;  // the stage that counts it in
		std::uint16_t taken;    // the stage that counts it off
		Counted       counted = Counted::kAlways;
		std::uint32_t global = 0;  // the global kIfPositive and kIfZero read
	};

	// Read off their fragments (the offer's Mod +1, the take's Mod -1, under the same condition).
	inline constexpr std::array OFFERS{
		Offer{ 0x000B926B, 50, 100, Counted::kIfPositive, 0x000E1685 },  // Jackpot: once done before (RRR03Completed)
		Offer{ 0x000B26C2, 50, 75 },                                      // Mercer Safehouse (its 100 and its 300 set 75)
		Offer{ 0x000B926A, 50, 100, Counted::kIfZero, 0x000A59AC },      // Weathervane: while no MILA is placed (RRR05MILAsPlaced)
	};

	inline const Offer* OfferOf(std::uint32_t a_quest)
	{
		const auto it = std::ranges::find(OFFERS, a_quest, &Offer::quest);
		return it != OFFERS.end() ? &*it : nullptr;
	}

	// Whether this game's run of a quest, stopped now with a_done its stages done, leaves its offer counted in
	// RRQuestsAvailable (a_value: the offer's condition global now): counted in, and not off yet.
	inline bool LeavesCounted(std::uint32_t a_quest, std::span<const std::uint16_t> a_done, float a_value)
	{
		const auto offer = OfferOf(a_quest);
		if (!offer || std::ranges::find(a_done, offer->offered) == a_done.end() || std::ranges::find(a_done, offer->taken) != a_done.end()) {
			return false;
		}
		switch (offer->counted) {
		case Counted::kIfPositive:
			return a_value > 0.0f;
		case Counted::kIfZero:
			return a_value == 0.0f;
		default:
			return true;
		}
	}

	// A friend's run of a quest that runs here with other targets: whether this game's run is stopped for theirs. Only one
	// the book doesn't know (no run shared here, ours or a friend's), that this player hasn't been given (a_given: an
	// objective shown), of a quest that may be (Entry::replace) and can run again (a_runOnce: the quest's own flag).
	inline bool Replaces(std::uint32_t a_quest, bool a_known, bool a_given, bool a_runOnce)
	{
		const auto entry = Find(a_quest);
		return entry && entry->share == Share::kShared && entry->replace && !a_known && !a_given && !a_runOnce;
	}

	// ---- Fills ------------------------------------------------------------------------------------

	enum class Kind : std::uint8_t
	{
		kRef = 0,  // a reference alias (a collection's members aren't matched: they come from the others)
		kLoc = 1,  // a location alias
	};

	// How an alias is filled when its quest starts.
	enum class Fill : std::uint8_t
	{
		kSource,    // by its conditions or the start's event: each game picks its own, so it is forced to the friend's
		kByOthers,  // pushed in by other aliases as they fill ("forced by aliases": the quest's Workshop); forced too where it can be
		kFixed,     // the same in every game: a forced reference or location, a unique actor, another quest's alias
		kDerived,   // from another alias (its location's boss, map marker), near one, or made at runtime: follows the rest; forced
		            // too where it can be (MakePlan), never a reason to fall back
	};

	// An alias as the game files it.
	struct Alias
	{
		std::uint32_t    id = 0;
		Kind             kind = Kind::kRef;
		Fill             fill = Fill::kDerived;
		bool             optional = false;
		std::string_view name;
	};

	// Aliases' flags (BGSBaseAlias::FLAGS): optional, filled by other aliases, may take a reference another quest reserves.
	inline constexpr std::uint32_t ALIAS_OPTIONAL = 1u << 1;
	inline constexpr std::uint32_t ALIAS_BY_OTHERS = 1u << 11;
	inline constexpr std::uint32_t ALIAS_ALLOW_RESERVED = 1u << 9;

	// A forced alias's flags for a friend's start: no longer filled by the others, and, only where its conditions are lifted
	// (an optional pick that follows this game's state: a settlement owned here and not there), allowed a reference another
	// quest of this game reserves. A needed alias keeps the reservations: a target a story or companion quest holds here
	// fails the start, and the run stays each player's own.
	constexpr std::uint32_t ForcedFlags(std::uint32_t a_flags, bool a_lifted)
	{
		return (a_flags & ~ALIAS_BY_OTHERS) | (a_lifted ? ALIAS_ALLOW_RESERVED : 0u);
	}

	// How a reference alias is filled, by BGSRefAlias::FILL_TYPE: 0 conditions, 1 forced, 2 from another alias, 3
	// from the start's event, 4 made at runtime, 5 another quest's alias, 6 a unique actor, 7 near another alias.
	inline Fill RefFill(std::uint16_t a_fillType, std::uint32_t a_flags)
	{
		if (a_flags & ALIAS_BY_OTHERS) {
			return Fill::kByOthers;
		}
		switch (a_fillType) {
		case 0:
		case 3:
			return Fill::kSource;
		case 1:
		case 5:
		case 6:
			return Fill::kFixed;
		default:
			return Fill::kDerived;
		}
	}

	// How a location alias is filled, by its fill type (read off the game's own: 0 conditions, 1 a forced location, 2
	// from another alias, 3 from the start's event, 4 another quest's alias).
	inline Fill LocFill(std::uint16_t a_fillType, std::uint32_t a_flags)
	{
		if (a_flags & ALIAS_BY_OTHERS) {
			return Fill::kByOthers;
		}
		switch (a_fillType) {
		case 0:
		case 3:
			return Fill::kSource;
		case 1:
		case 4:
			return Fill::kFixed;
		default:
			return Fill::kDerived;
		}
	}

	// An alias forced to a fill of this game's for the start (form 0: left empty).
	struct Force
	{
		std::uint32_t alias = 0;
		Kind          kind = Kind::kRef;
		std::uint32_t form = 0;

		bool operator==(const Force&) const = default;
	};

	// How this game starts a friend's run: the aliases to force, or why it can't (fallback set: the run stays each
	// player's own).
	struct Plan
	{
		std::vector<Force>         forces;
		std::string                fallback;
		// Reference aliases that follow the others whose friend's fill this game didn't have loaded at the start (a settler of a
		// settlement far away): left to the aliases they follow, and only noted if they differ once it runs (Compare).
		std::vector<std::uint32_t> unforced;

		[[nodiscard]] bool Ok() const { return fallback.empty(); }
	};

	inline const Protocol::RadiantFill* FillFor(std::span<const Protocol::RadiantFill> a_fills, const Alias& a_alias)
	{
		const auto it = std::ranges::find_if(a_fills, [&](const Protocol::RadiantFill& a_fill) { return a_fill.alias == a_alias.id && a_fill.kind == static_cast<std::uint8_t>(a_alias.kind); });
		return it != a_fills.end() ? &*it : nullptr;
	}

	inline std::string Why(const Alias& a_alias, std::uint32_t a_form)
	{
		if (a_alias.kind == Kind::kLoc) {
			return std::format("'{}' is the location {:08X}, which this game doesn't have", a_alias.name, a_form);
		}
		if (a_form == Protocol::RADIANT_UNSHARED_REF) {
			return std::format("'{}' is an NPC the friend's game made at runtime, which the session doesn't share", a_alias.name);
		}
		if (Protocol::IsSpawnRef(a_form)) {
			return std::format("'{}' is an NPC the friend's game made at runtime ({:08X}) that this game has no copy of", a_alias.name, a_form);
		}
		return std::format("'{}' is {:08X}, which this game doesn't have (or not loaded)", a_alias.name, a_form);
	}

	// The fills the friend's game picked for the aliases that each game picks for itself, as this game's: a_resolve(kind,
	// form) gives this game's form for theirs (the same plugin reference or location if it is here, our NPC under a
	// spawn id) or nothing. The ones pushed in by other aliases, or that follow from others, are forced too where they can
	// be (a settlement has two settlers with its boss's location reference type, and each game may take either; the boss is
	// then pushed into the spokesman), and otherwise left to the aliases they follow, as in the friend's game. Every fill
	// they sent must be one of this quest's aliases (their game's files and ours are the same).
	template <class Resolve>
	Plan MakePlan(std::span<const Alias> a_aliases, std::span<const Protocol::RadiantFill> a_fills, Resolve&& a_resolve)
	{
		Plan plan;
		for (const auto& fill : a_fills) {
			const auto known = std::ranges::any_of(a_aliases, [&](const Alias& a_alias) { return a_alias.id == fill.alias && static_cast<std::uint8_t>(a_alias.kind) == fill.kind; });
			if (!known) {
				plan.fallback = std::format("the friend's quest has an alias {} this game's hasn't", fill.alias);
				return plan;
			}
		}
		for (const auto& alias : a_aliases) {
			if (alias.fill == Fill::kFixed) {
				continue;
			}
			const auto          fill = FillFor(a_fills, alias);
			const std::uint32_t form = fill ? fill->form : 0;
			if (form == 0) {
				if (alias.fill != Fill::kSource) {
					continue;
				}
				if (!alias.optional) {
					plan.fallback = std::format("'{}' is empty in the friend's game", alias.name);
					return plan;
				}
				plan.forces.push_back({ alias.id, alias.kind, 0 });
				continue;
			}
			const std::optional<std::uint32_t> local = a_resolve(alias.kind, form);
			if (!local || *local == 0) {
				if (alias.fill != Fill::kSource) {
					if (alias.kind == Kind::kRef) {
						plan.unforced.push_back(alias.id);
					}
					continue;
				}
				plan.fallback = Why(alias, form);
				return plan;
			}
			plan.forces.push_back({ alias.id, alias.kind, *local });
		}
		return plan;
	}

	// A fill this game has that the friend's doesn't.
	struct Mismatch
	{
		std::uint32_t    alias = 0;
		std::string_view name;
		bool             decisive = false;  // one each game picks for itself: the targets differ
		std::uint32_t    theirs = 0;        // as this game's form (0: empty)
		std::uint32_t    ours = 0;
	};

	// After the start, or for a quest that already runs here: the fills of this game (a_here(kind, alias), 0 when empty)
	// that differ from the friend's. Only those this game can name (a fill made at runtime in each game is each game's).
	// a_unforced: after a start, the plan's (Plan::unforced): one of them that differs while a place of the run is the
	// friend's is another settler of the same place, only noted.
	template <class Resolve, class Here>
	std::vector<Mismatch> Compare(std::span<const Alias> a_aliases, std::span<const Protocol::RadiantFill> a_fills, Resolve&& a_resolve, Here&& a_here,
		std::span<const std::uint32_t> a_unforced = {})
	{
		const bool samePlace = std::ranges::any_of(a_aliases, [&](const Alias& a_alias) {
			const auto fill = FillFor(a_fills, a_alias);
			if (a_alias.kind != Kind::kLoc || !fill || fill->form == 0) {
				return false;
			}
			const std::optional<std::uint32_t> local = a_resolve(a_alias.kind, fill->form);
			return local && *local != 0 && *local == a_here(a_alias.kind, a_alias.id);
		});
		std::vector<Mismatch> out;
		for (const auto& alias : a_aliases) {
			const auto fill = FillFor(a_fills, alias);
			if (!fill) {
				continue;
			}
			// A place is the run's target whichever way it is filled (a fill type read wrong leaves it checked).
			const bool unforced = alias.kind == Kind::kRef && alias.fill != Fill::kSource && samePlace && std::ranges::find(a_unforced, alias.id) != a_unforced.end();
			const bool picked = (alias.fill == Fill::kSource || alias.fill == Fill::kByOthers || alias.kind == Kind::kLoc) && !unforced;
			std::uint32_t theirs = 0;
			if (fill->form != 0) {
				const std::optional<std::uint32_t> local = a_resolve(alias.kind, fill->form);
				if (!local || *local == 0) {
					if (alias.fill == Fill::kSource || alias.kind == Kind::kLoc) {
						out.push_back({ alias.id, alias.name, true, fill->form, a_here(alias.kind, alias.id) });
					}
					continue;
				}
				theirs = *local;
			}
			const std::uint32_t ours = a_here(alias.kind, alias.id);
			if (ours != theirs) {
				out.push_back({ alias.id, alias.name, picked, theirs, ours });
			}
		}
		return out;
	}

	inline bool SameTargets(std::span<const Mismatch> a_mismatches)
	{
		return std::ranges::none_of(a_mismatches, &Mismatch::decisive);
	}

	// ---- Runs -------------------------------------------------------------------------------------

	// One run of a radiant: the game that started it (its player id in the session) and its number there.
	struct Run
	{
		std::uint32_t origin = 0;
		std::uint32_t serial = 0;

		auto operator<=>(const Run&) const = default;
	};

	// What becomes of a friend's start here.
	enum class Answer : std::uint8_t
	{
		kIgnore,   // a run known here already, one finished or turned down here, or a start under way
		kStart,    // start the quest here with their fills
		kCompare,  // the quest runs here: share it if its fills are theirs
		kDone,     // a run with its targets finished here (shared again under another number): never started again
		kTake,     // the run is shared here already (taken back from a save, or after a reconnect): its start's stages are taken, each once
	};

	// The fills that name a run's targets, to know the run again under another number (a game that loaded a save from
	// inside it, or came back, shares it again as a new run): its locations, or where it has none its references, but
	// those no other game names (empty, made at runtime). In order, so two runs' compare equal.
	inline std::vector<Protocol::RadiantFill> TargetsOf(std::span<const Protocol::RadiantFill> a_fills)
	{
		const auto named = [](const Protocol::RadiantFill& a_fill) { return a_fill.form != 0 && (a_fill.form >> 24) != 0xFF; };
		const auto place = [](const Protocol::RadiantFill& a_fill) { return a_fill.kind == static_cast<std::uint8_t>(Kind::kLoc); };
		const bool places = std::ranges::any_of(a_fills, [&](const Protocol::RadiantFill& a_fill) { return named(a_fill) && place(a_fill); });
		std::vector<Protocol::RadiantFill> out;
		for (const auto& fill : a_fills) {
			if (named(fill) && place(fill) == places) {
				out.push_back(fill);
			}
		}
		std::ranges::sort(out, {}, [](const Protocol::RadiantFill& a_fill) { return std::tuple{ a_fill.kind, a_fill.alias, a_fill.form }; });
		return out;
	}

	// Where a game's run numbers start: a number of its own start (the seconds since 1970 then), so a game that restarts,
	// whose player the server gives back their id, never numbers a run as one of its last (its friends may still hold
	// that one, running or finished). A game makes far fewer runs than one a second.
	constexpr std::uint32_t SerialSeed(std::uint64_t a_unixSeconds)
	{
		return static_cast<std::uint32_t>(a_unixSeconds);
	}

	// What a save keeps of the book (Book::Save; the co-save's record): the session it was saved in, the runs its world
	// shares, each with its session (so a game that loads it shares its run again under the same run, which a friend who finished it
	// knows, and not as a new one), and the runs finished here in each of its last sessions (so one is never started
	// again after the game restarts, or comes back from another server). Run ids are a session's: the runs shared are
	// shared again only in that session, and a run finished is known only in its own.
	struct Saved
	{
		struct Quest
		{
			std::uint32_t                      quest = 0;
			std::vector<Run>                   runs;
			std::vector<Protocol::RadiantFill> targets;
			std::uint64_t                      session = 0;  // the session the runs are of

			bool operator==(const Quest&) const = default;
		};

		struct Finished
		{
			std::uint64_t    session = 0;
			std::vector<Run> runs;  // the most recent last

			bool operator==(const Finished&) const = default;
		};

		std::uint64_t         session = 0;  // the one it was saved in
		std::vector<Quest>    shared;
		std::vector<Finished> finished;  // by session, the one played most recently last

		bool operator==(const Saved&) const = default;
	};

	inline constexpr std::uint16_t SAVED_VERSION = 1;
	inline constexpr std::size_t   MAX_SAVED_QUESTS = 64;     // RADIANTS has fewer
	inline constexpr std::size_t   MAX_SAVED_RUNS = 8;        // of one quest's (the latest kept)
	inline constexpr std::size_t   MAX_SAVED_FINISHED = 256;  // of one session's (the oldest forgotten)
	inline constexpr std::size_t   MAX_SAVED_SESSIONS = 4;    // the one played longest ago forgotten

	// The co-save's bytes: the version, the session, each shared quest's runs and targets, then each session's finished
	// runs.
	inline std::vector<std::uint8_t> Encode(const Saved& a_saved)
	{
		std::vector<std::uint8_t> out;
		const auto                put = [&](const auto& a_value) {
			const auto* bytes = reinterpret_cast<const std::uint8_t*>(&a_value);
			out.insert(out.end(), bytes, bytes + sizeof(a_value));
		};
		const auto putRun = [&](const Run& a_run) {
			put(a_run.origin);
			put(a_run.serial);
		};
		const auto quests = (std::min)(a_saved.shared.size(), MAX_SAVED_QUESTS);
		put(SAVED_VERSION);
		put(a_saved.session);
		put(static_cast<std::uint8_t>(quests));
		for (std::size_t i = 0; i < quests; ++i) {
			const auto& quest = a_saved.shared[i];
			const auto  runs = (std::min)(quest.runs.size(), MAX_SAVED_RUNS);
			const auto  targets = (std::min)(quest.targets.size(), Protocol::MAX_RADIANT_FILLS);
			put(quest.quest);
			put(quest.session);
			put(static_cast<std::uint8_t>(runs));
			for (auto it = quest.runs.end() - static_cast<std::ptrdiff_t>(runs); it != quest.runs.end(); ++it) {
				putRun(*it);
			}
			put(static_cast<std::uint8_t>(targets));
			for (std::size_t j = 0; j < targets; ++j) {
				put(quest.targets[j].alias);
				put(quest.targets[j].kind);
				put(quest.targets[j].form);
			}
		}
		const auto sessions = (std::min)(a_saved.finished.size(), MAX_SAVED_SESSIONS);
		put(static_cast<std::uint8_t>(sessions));
		for (auto session = a_saved.finished.end() - static_cast<std::ptrdiff_t>(sessions); session != a_saved.finished.end(); ++session) {
			const auto finished = (std::min)(session->runs.size(), MAX_SAVED_FINISHED);
			put(session->session);
			put(static_cast<std::uint16_t>(finished));
			for (auto it = session->runs.end() - static_cast<std::ptrdiff_t>(finished); it != session->runs.end(); ++it) {
				putRun(*it);
			}
		}
		return out;
	}

	// A save's record (nothing if it can't be read: a damaged record shares no run).
	inline std::optional<Saved> Decode(std::span<const std::uint8_t> a_bytes)
	{
		std::size_t offset = 0;
		const auto  take = [&](auto& a_value) {
			if (offset + sizeof(a_value) > a_bytes.size()) {
				return false;
			}
			std::memcpy(&a_value, a_bytes.data() + offset, sizeof(a_value));
			offset += sizeof(a_value);
			return true;
		};
		const auto takeRun = [&](Run& a_run) { return take(a_run.origin) && take(a_run.serial); };
		std::uint16_t version = 0;
		Saved         saved;
		std::uint8_t  quests = 0;
		if (!take(version) || version != SAVED_VERSION || !take(saved.session) || !take(quests) || quests > MAX_SAVED_QUESTS) {
			return std::nullopt;
		}
		for (std::uint8_t i = 0; i < quests; ++i) {
			Saved::Quest quest;
			std::uint8_t runs = 0;
			if (!take(quest.quest) || !take(quest.session) || !take(runs) || runs > MAX_SAVED_RUNS) {
				return std::nullopt;
			}
			quest.runs.resize(runs);
			for (auto& run : quest.runs) {
				if (!takeRun(run)) {
					return std::nullopt;
				}
			}
			std::uint8_t targets = 0;
			if (!take(targets) || targets > Protocol::MAX_RADIANT_FILLS) {
				return std::nullopt;
			}
			quest.targets.resize(targets);
			for (auto& fill : quest.targets) {
				if (!take(fill.alias) || !take(fill.kind) || !take(fill.form)) {
					return std::nullopt;
				}
			}
			saved.shared.push_back(std::move(quest));
		}
		std::uint8_t sessions = 0;
		if (!take(sessions) || sessions > MAX_SAVED_SESSIONS) {
			return std::nullopt;
		}
		for (std::uint8_t i = 0; i < sessions; ++i) {
			Saved::Finished session;
			std::uint16_t   finished = 0;
			if (!take(session.session) || !take(finished) || finished > MAX_SAVED_FINISHED) {
				return std::nullopt;
			}
			session.runs.resize(finished);
			for (auto& run : session.runs) {
				if (!takeRun(run)) {
					return std::nullopt;
				}
			}
			saved.finished.push_back(std::move(session));
		}
		if (offset != a_bytes.size()) {
			return std::nullopt;
		}
		return saved;
	}

	// A quest running in this world, for Book::Restore: its stages done, and its fills (FillsOf), which name its targets.
	struct QuestHere
	{
		std::vector<std::uint16_t>         done;
		std::vector<Protocol::RadiantFill> fills;
	};

	// The radiants' runs in this game: which run of each quest is shared here, and with it the rule that pays a run's
	// reward once in each game. The reward is the quest's own stage (MinRecruit's 400), so a game must set each stage
	// of a run at most once and never start a run it has finished: a run's start that comes again (a friend's game
	// resends its runs when someone joins; a repeated packet) is ignored once that run is known, finished or turned
	// down here, and each stage of it is taken or reported once. A stage taken from a friend is never reported back.
	// A new run of the same quest (it is handed out again) is another run, and pays again. A run is known by its id: a
	// game that loads a save from inside a run, or comes back to the session, shares it again under the same run (the
	// save's record, Save and Restore, or the one kept at the session's end), so a friend who finished it ignores it, and
	// the runs finished here are kept with the save. A run shared again under a new number all the same (a save from
	// before its share, or one made before this was kept) is known by its targets, but only for a while after a run with
	// them finished here (SAME_RUN_WINDOW): later, the same targets are a new run handed out again (kDone otherwise).
	// That window is the only guard for such a save: one older than the run's share, loaded more than 15 minutes after
	// a friend finished the run (or after their game restarted, as the window isn't saved), starts it there again.
	// The runs finished here are kept through loads, sessions and servers, each known in its own session (Bind).
	class Book
	{
	public:
		using Time = std::chrono::steady_clock::time_point;

		// How long after a run finished here a run of the quest with its targets is taken for it shared again.
		static constexpr auto SAME_RUN_WINDOW = std::chrono::minutes{ 15 };

		Book() = default;
		// a_seed: where this game's run numbers start (SerialSeed).
		explicit Book(std::uint32_t a_seed) :
			serial(a_seed)
		{}

		enum class State : std::uint8_t
		{
			kStarting,  // started here with a friend's fills; not yet seen running
			kShared,
		};

		// This game started the quest in a session (the player has it): a run of ours, with the stages done so far.
		// a_fills: its fills here (FillsOf), which name its targets.
		Run Begin(std::uint32_t a_quest, std::uint32_t a_self, std::span<const std::uint16_t> a_done, std::span<const Protocol::RadiantFill> a_fills = {})
		{
			const Run run{ a_self, ++serial };
			auto&     instance = quests[a_quest];
			instance = Instance{};
			instance.state = State::kShared;
			instance.runs.push_back(run);
			instance.reported.insert(a_done.begin(), a_done.end());
			instance.targets = TargetsOf(a_fills);
			return run;
		}

		// a_fills: the start's.
		[[nodiscard]] Answer OnStart(std::uint32_t a_quest, const Run& a_run, bool a_running, std::span<const Protocol::RadiantFill> a_fills = {}, Time a_now = {}) const
		{
			if (FinishedRun(a_run) || refused.contains(a_run)) {
				return Answer::kIgnore;
			}
			if (const auto it = quests.find(a_quest); it != quests.end()) {
				const bool known = std::ranges::find(it->second.runs, a_run) != it->second.runs.end();
				if (known && it->second.state == State::kShared) {
					return Answer::kTake;
				}
				if (known || it->second.state == State::kStarting) {
					return Answer::kIgnore;
				}
				return Answer::kCompare;
			}
			if (a_running) {
				return Answer::kCompare;
			}
			return FinishedHere(a_quest, a_fills, a_now) ? Answer::kDone : Answer::kStart;
		}

		// Whether a run of the quest with these fills' targets finished here less than SAME_RUN_WINDOW before a_now.
		[[nodiscard]] bool FinishedHere(std::uint32_t a_quest, std::span<const Protocol::RadiantFill> a_fills, Time a_now = {}) const
		{
			const auto it = done.find(a_quest);
			if (it == done.end()) {
				return false;
			}
			const auto targets = TargetsOf(a_fills);
			const auto entry = std::ranges::find(it->second, targets, &Done::targets);
			return !targets.empty() && entry != it->second.end() && a_now - entry->at < SAME_RUN_WINDOW;
		}

		// Started here from a friend's run, the stages they had done to come (a_fills: the start's).
		void Starting(std::uint32_t a_quest, const Run& a_run, std::span<const Protocol::RadiantFill> a_fills = {})
		{
			auto& instance = quests[a_quest];
			instance = Instance{};
			instance.state = State::kStarting;
			instance.runs.push_back(a_run);
			instance.targets = TargetsOf(a_fills);
		}

		// The quest runs here with the run's fills (started from it, or found running with them): the run is shared.
		// Whether it was shared here already, under another run (one shared again under a new number).
		bool Join(std::uint32_t a_quest, const Run& a_run, std::span<const Protocol::RadiantFill> a_fills = {})
		{
			auto&      instance = quests[a_quest];
			const bool again = instance.state == State::kShared && !instance.runs.empty() && std::ranges::find(instance.runs, a_run) == instance.runs.end();
			instance.state = State::kShared;
			if (std::ranges::find(instance.runs, a_run) == instance.runs.end()) {
				instance.runs.push_back(a_run);
			}
			if (instance.targets.empty()) {
				instance.targets = TargetsOf(a_fills);
			}
			return again;
		}

		// The run can't be shared here (its fills can't be matched, the start failed): each player's own. Its start is
		// ignored from now on.
		void Refuse(std::uint32_t a_quest, const Run& a_run)
		{
			refused.insert(a_run);
			if (const auto it = quests.find(a_quest); it != quests.end() && it->second.state == State::kStarting && std::ranges::find(it->second.runs, a_run) != it->second.runs.end()) {
				quests.erase(it);
			}
		}

		// Whether the quest's run here is shared now: its stages go between the games.
		[[nodiscard]] bool Shares(std::uint32_t a_quest) const
		{
			const auto it = quests.find(a_quest);
			return it != quests.end() && it->second.state == State::kShared;
		}

		// Whether a run of the quest is known here (shared, or starting).
		[[nodiscard]] bool Knows(std::uint32_t a_quest) const
		{
			return quests.contains(a_quest);
		}

		[[nodiscard]] bool IsStarting(std::uint32_t a_quest) const
		{
			const auto it = quests.find(a_quest);
			return it != quests.end() && it->second.state == State::kStarting;
		}

		// A friend's stage of a run: to be set here only if that run is the one shared here and the stage is new to it.
		// A stage that comes while the run is still starting is kept for it (the caller queues it once started).
		bool Accept(std::uint32_t a_quest, const Run& a_run, std::uint16_t a_stage)
		{
			const auto it = quests.find(a_quest);
			if (it == quests.end() || std::ranges::find(it->second.runs, a_run) == it->second.runs.end()) {
				return false;
			}
			auto& instance = it->second;
			if (instance.reported.contains(a_stage) || !instance.taken.insert(a_stage).second) {
				return false;
			}
			instance.reported.insert(a_stage);  // done here because of them: never reported back
			return true;
		}

		// A stage done here by this player (not a friend's set here): the runs to report it under, once.
		std::vector<Run> Report(std::uint32_t a_quest, std::uint16_t a_stage)
		{
			const auto it = quests.find(a_quest);
			if (it == quests.end() || it->second.state != State::kShared || !it->second.reported.insert(a_stage).second) {
				return {};
			}
			return it->second.runs;
		}

		// The quest stopped here (completed, failed or timed out) at a_now: its run is over here, and never starts again.
		void Ended(std::uint32_t a_quest, Time a_now = {})
		{
			if (const auto it = quests.find(a_quest); it != quests.end()) {
				for (const auto& run : it->second.runs) {
					Finish(run, current);
				}
				NoteDone(a_quest, std::move(it->second.targets), a_now);
				quests.erase(it);
			}
		}

		// What a save keeps of the book (Saved): the runs shared now, as runs of a_session, and those finished here in the
		// last sessions.
		[[nodiscard]] Saved Save(std::uint64_t a_session) const
		{
			Saved out{ a_session, {}, {} };
			for (const auto& [quest, instance] : quests) {
				if (instance.state == State::kShared && out.shared.size() < MAX_SAVED_QUESTS) {
					out.shared.push_back({ quest, instance.runs, instance.targets, a_session });
				}
			}
			for (const auto& past : pasts) {
				if (!past.recent.empty()) {
					out.finished.push_back({ past.session, past.recent });
				}
			}
			if (out.finished.size() > MAX_SAVED_SESSIONS) {
				out.finished.erase(out.finished.begin(), out.finished.end() - MAX_SAVED_SESSIONS);
			}
			return out;
		}

		// A save's record (or the one kept when the session ended), its world here now: each run it shared is shared
		// again, if a_share and the run is of the session bound now, where its quest runs with the run's targets; one
		// whose quest runs so but that isn't shared here (another session's, or a_share false) goes to a_kept, to be kept
		// until it is over (and then finished in its session) or the player is back in its session.
		// a_here(quest): the quest in this world (QuestHere), or nothing if it doesn't run. Its stages done count as
		// reported (they went under the run when they were done, or with its start). A run whose quest doesn't run here
		// now, or runs with other targets (handed out again), is over in this world: finished here at a_now, in its
		// session, whichever it is (one finished while the player was out of the session, or on another server, must
		// never start here again when they come back). Returns the runs shared again.
		template <class Here>
		std::size_t Restore(const Saved& a_saved, Here&& a_here, bool a_share = true, Time a_now = {}, std::vector<Saved::Quest>* a_kept = nullptr)
		{
			std::size_t shared = 0;
			for (const auto& saved : a_saved.shared) {
				const std::optional<QuestHere> here = a_here(saved.quest);
				if (!here || TargetsOf(here->fills) != saved.targets) {
					for (const auto& run : saved.runs) {
						Finish(run, saved.session);
					}
					NoteDone(saved.quest, saved.targets, a_now);
					continue;
				}
				if (!a_share || saved.session != current) {
					if (a_kept) {
						a_kept->push_back(saved);
					}
					continue;
				}
				if (saved.runs.empty() || quests.contains(saved.quest)) {
					continue;
				}
				auto& instance = quests[saved.quest];
				instance.state = State::kShared;
				instance.runs = saved.runs;
				instance.reported.insert(here->done.begin(), here->done.end());
				instance.targets = saved.targets;
				++shared;
			}
			return shared;
		}

		// The runs shared here now, each with its quest (to send the stages done here under them again).
		[[nodiscard]] std::vector<std::pair<std::uint32_t, Run>> Shared() const
		{
			std::vector<std::pair<std::uint32_t, Run>> out;
			for (const auto& [quest, instance] : quests) {
				if (instance.state == State::kShared) {
					for (const auto& run : instance.runs) {
						out.emplace_back(quest, run);
					}
				}
			}
			return out;
		}

		// This game's own runs going now (to send again to a player who joins).
		[[nodiscard]] std::vector<std::pair<std::uint32_t, Run>> Ours(std::uint32_t a_self) const
		{
			std::vector<std::pair<std::uint32_t, Run>> out;
			for (const auto& [quest, instance] : quests) {
				for (const auto& run : instance.runs) {
					if (run.origin == a_self && instance.state == State::kShared) {
						out.emplace_back(quest, run);
					}
				}
			}
			return out;
		}

		[[nodiscard]] std::vector<std::uint32_t> Quests() const
		{
			std::vector<std::uint32_t> out;
			for (const auto& [quest, instance] : quests) {
				out.push_back(quest);
			}
			return out;
		}

		[[nodiscard]] std::size_t Size() const { return quests.size(); }
		[[nodiscard]] std::size_t Finished() const
		{
			const auto* past = PastOf(current);
			return past ? past->runs.size() : 0;
		}

		// The session ended: no run is shared any more (the quests go on, each player's own). The runs finished or
		// turned down here stay known (a player keeps their id in a session when they come back), so a start sent
		// again never starts one here twice.
		void EndSession() { quests.clear(); }

		// A save loaded: the runs going are the loaded world's (its record's, Restore; each game shares its own again). Those
		// turned down here may be taken now; those finished here never start here again, nor for a while do their targets
		// (a save from before the reward would pay it twice).
		void Reloaded()
		{
			quests.clear();
			refused.clear();
		}

		// Another server: nothing of the last is going here. The runs finished in each session stay known in it (a player who
		// comes back to the first server finds them there), as do the targets finished here, and run numbers go on.
		void Clear()
		{
			quests.clear();
			refused.clear();
		}

		// The session the runs are of now (its welcome): a run finished is known in its own session only.
		void Bind(std::uint64_t a_session)
		{
			current = a_session;
			if (const auto it = std::ranges::find(pasts, a_session, &Past::session); it != pasts.end()) {
				std::rotate(it, it + 1, pasts.end());  // the one played most recently last
			} else {
				pasts.push_back(Past{ a_session, {}, {} });
			}
		}

		// A loaded save's runs finished (its record), in their sessions: known here from now on, with those known already.
		void Remember(const Saved& a_saved)
		{
			for (auto session = a_saved.finished.rbegin(); session != a_saved.finished.rend(); ++session) {
				for (const auto& run : session->runs) {
					Finish(run, session->session);
				}
			}
		}

	private:
		struct Done
		{
			std::vector<Protocol::RadiantFill> targets;
			Time                               at;  // when its run finished here
		};

		// The runs finished here in one session.
		struct Past
		{
			std::uint64_t    session = 0;
			std::set<Run>    runs;
			std::vector<Run> recent;  // the latest of them, the most recent last (saved)
		};

		[[nodiscard]] const Past* PastOf(std::uint64_t a_session) const
		{
			const auto it = std::ranges::find(pasts, a_session, &Past::session);
			return it != pasts.end() ? &*it : nullptr;
		}

		[[nodiscard]] bool FinishedRun(const Run& a_run) const
		{
			const auto* past = PastOf(current);
			return past && past->runs.contains(a_run);
		}

		// A run of a_session over here. A session not known here yet is taken for one played before the others (a save's).
		void Finish(const Run& a_run, std::uint64_t a_session)
		{
			auto it = std::ranges::find(pasts, a_session, &Past::session);
			if (it == pasts.end()) {
				it = pasts.insert(pasts.begin(), Past{ a_session, {}, {} });
			}
			if (it->runs.insert(a_run).second) {
				if (it->recent.size() >= MAX_SAVED_FINISHED) {
					it->recent.erase(it->recent.begin());
				}
				it->recent.push_back(a_run);
			}
		}

		// A run of the quest with these targets finished here at a_now (FinishedHere).
		void NoteDone(std::uint32_t a_quest, std::vector<Protocol::RadiantFill> a_targets, Time a_now)
		{
			if (a_targets.empty()) {
				return;
			}
			auto& targets = done[a_quest];
			std::erase_if(targets, [&](const Done& a_done) { return a_done.targets == a_targets; });
			if (targets.size() >= MAX_DONE) {
				targets.erase(targets.begin());
			}
			targets.push_back({ std::move(a_targets), a_now });
		}

		struct Instance
		{
			State                   state = State::kShared;
			std::vector<Run>        runs;      // the run (ours or a friend's) and any other found to have the same fills
			std::set<std::uint16_t> taken;     // friends' stages taken for it
			std::set<std::uint16_t> reported;  // stages that need no report from here (reported, sent with the start, or taken)
			std::vector<Protocol::RadiantFill> targets;  // TargetsOf its fills
		};

		static constexpr std::size_t MAX_DONE = 32;  // targets kept for each quest, the oldest forgotten

		std::map<std::uint32_t, Instance>                                        quests;
		std::vector<Past>                                                        pasts;  // the runs finished here, by session, the one played most recently last
		std::uint64_t                                                            current = 0;  // the session bound (Bind)
		std::set<Run>                                                            refused;
		std::map<std::uint32_t, std::vector<Done>>                               done;  // the targets of runs finished here, by quest
		std::uint32_t                                                            serial = 0;
	};
}
