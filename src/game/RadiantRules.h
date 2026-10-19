#pragma once

#include "Protocol.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
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
// Which radiants are shared (RADIANTS, all from Fallout4.esm): the Minutemen's "a settlement needs our help"
// (MinRecruit: raiders, kidnappers, ghouls, a place to clear) and Preston's requests from settlements the
// player has (MinRadiantOwned: raiders, kidnapping); the Brotherhood's dungeon jobs (Cleansing the Commonwealth,
// Quartermastery), the Institute's (Pest Control, Hypothesis, Political Leanings) and two of the Railroad's
// (Butcher's Bill, Concierge): a settlement or a dungeon, the place's people and its map marker, all of them the
// game's own references and locations. Left alone, each player's own: settlement attacks and the Castle's (the
// attackers are made where the settlement stands, and each game runs its own attack and its workshop's defense),
// quests whose targets are made at runtime (a synth boss, refugees, a courser, Brotherhood squires and scribes, a
// patrol, a vertibird, an assault's defenders: each game would make its own, and the session would share both),
// quests that work on a settlement's workshop (Feeding the Troops, Mercer Safehouse), and the Railroad jobs whose
// start counts the job as on offer at HQ or hides a place's enemies (RADIANTS says which). Those stay unshared even
// when the story is (before, a friend's stage started them here with this game's own targets). Kept free of the
// game so it can be tested; RadiantSync reads the quests.
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
		Entry{ 0x00106F05, "MinRecruit03", Share::kLeftAlone },  // a locket made at runtime
		Entry{ 0x0015F03F, "MinRecruit05", Share::kLeftAlone },  // a hostile settlement
		Entry{ 0x0015F040, "MinRecruit06", Share::kLeftAlone },
		Entry{ 0x00157577, "MinRecruit08", Share::kLeftAlone },  // a synth boss made at runtime
		Entry{ 0x00164167, "MinRecruit09", Share::kLeftAlone },  // refugees made at runtime
		// Preston's requests from the player's own settlements.
		Entry{ 0x0003DF95, "MinRadiantOwned02", Share::kShared },               // raiders
		Entry{ 0x0003E0C1, "MinRadiantOwned03ChangeLocOnly", Share::kShared },  // kidnapping
		Entry{ 0x000A1412, "MinRadiantOwned01", Share::kLeftAlone },            // settlement attacks: the attackers are made at the settlement
		Entry{ 0x0005E51F, "MinRadiantOwned09ChangeLocOnly", Share::kLeftAlone },
		Entry{ 0x00160409, "MinRadiantOwned05", Share::kLeftAlone },
		Entry{ 0x00186A08, "MinRadiantOwned08", Share::kLeftAlone },
		Entry{ 0x000E477F, "MinRadiantOwned04_BOS", Share::kLeftAlone },
		Entry{ 0x001533A9, "MinRadiantOwned11", Share::kLeftAlone },  // synth attack
		Entry{ 0x00161D1D, "MinRadiantOwned06ChangeLocOnly", Share::kLeftAlone },  // building defenses: each settlement's own
		Entry{ 0x00162F44, "MinRadiantOwned07ChangeLocOnly", Share::kLeftAlone },
		Entry{ 0x00109D73, "MinRadiantOwned10ChangeLocOnly", Share::kLeftAlone },  // a synth among the settlers
		Entry{ 0x000BDB04, "MinDefendCastle", Share::kLeftAlone },                 // the Castle attacked
		Entry{ 0x00176328, "MinDefendCastleRadiantStarter", Share::kLeftAlone },
		Entry{ 0x0022FD6E, "MinDefendCastle_Attackers", Share::kLeftAlone },
		// The Brotherhood's. Their giver's line sets their 10 (it starts them), the player taking the job their 20, and
		// the reward is their 50: an item, and BoS200's stage for the job done (shared with the story too, set once). The
		// 0 is the developers' shortcut (it makes the player a member and moves them to the giver): never taken (UNTAKEN).
		Entry{ 0x00064EC7, "BoSR01", Share::kShared, true },  // Cleansing the Commonwealth: a dungeon by the player's level, its bosses
		Entry{ 0x000C30DC, "BosR02", Share::kShared, true },  // Quartermastery: a dungeon, the tech in its boss's container (made in each game)
		Entry{ 0x0009A687, "BoSR01PostHello", Share::kLeftAlone },  // dialogue only
		Entry{ 0x000C8675, "BoSR03", Share::kLeftAlone },  // Leading by Example: a squire made at runtime follows the player
		Entry{ 0x000CF3E2, "BoSR04", Share::kLeftAlone },  // Learning Curve: a scribe made at runtime follows the player
		Entry{ 0x000CF3E3, "BoSR04_AO_Scribe", Share::kLeftAlone },
		Entry{ 0x000D1EB2, "BoSR05", Share::kLeftAlone },  // Feeding the Troops: a settlement's food, its happiness changed by the job
		// The Institute's: all five start when the player joins (Inst301) and wait at their 5 until their giver hands one
		// out (its 100); each pays at its turn-in (10 or 20, and 1000).
		Entry{ 0x000F7933, "InstR01", Share::kShared, true },  // Pest Control: a dungeon and its bosses
		Entry{ 0x000EB268, "InstR02", Share::kShared, true },  // Hypothesis: a dungeon, its boss, the data on it (made in each game)
		Entry{ 0x000F0D8C, "InstR05", Share::kShared, true },  // Political Leanings: the same NPC and marker in every game
		Entry{ 0x000E8D19, "InstR01_OLD", Share::kLeftAlone },  // not used by the game
		Entry{ 0x000EC1E9, "InstR03_OLD", Share::kLeftAlone },
		Entry{ 0x000EDCEF, "InstR03NEW", Share::kLeftAlone },  // Appropriation: an assault, its defenders made at runtime
		Entry{ 0x000EDE28, "InstR04", Share::kLeftAlone },     // Reclamation: the synth to bring back is made at runtime
		// The Railroad's. Their HQ starts most of them whenever the player walks in, and their start counts the job as on
		// offer (RRQuestsAvailable, which the job taken counts off again): one stopped before it is taken would leave
		// that count one too high, and the Drummer Boy telling the player of work that isn't there.
		Entry{ 0x000B1F1D, "RRR01a", Share::kShared },  // Butcher's Bill: a dead drop by its conditions; runs once, never replaced
		Entry{ 0x000B3E82, "RRR02a", Share::kShared, true },  // Concierge: a place, its people (nothing counted at its start)
		Entry{ 0x000B1F1C, "RRR01b", Share::kLeftAlone },  // Butcher's Bill 2: a dead captive made at runtime
		Entry{ 0x000B926B, "RRR03", Share::kLeftAlone },   // Jackpot: counted on offer; the tracker it gives is made at runtime
		Entry{ 0x000B26C2, "RRR04", Share::kLeftAlone },   // Mercer Safehouse: builds up a settlement's workshop
		Entry{ 0x000B926A, "RRR05", Share::kLeftAlone },   // Weathervane: counted on offer, and starts again by itself when done
		Entry{ 0x001845F7, "RRR06", Share::kLeftAlone },   // Randolph Safehouse: counted on offer, and hides the place's enemies until taken
		Entry{ 0x00186C79, "RRR07", Share::kLeftAlone },   // Variable Removal: a courser made at runtime
		Entry{ 0x0013A33D, "RRR08", Share::kLeftAlone },   // To the Mattresses: the bosses are made at runtime
		Entry{ 0x00145F1A, "RRR09", Share::kLeftAlone },   // Lost Soul: the one to rescue is made at runtime
		Entry{ 0x0014A34A, "RRR10", Share::kLeftAlone },   // A Clean Equation: a patrol made at runtime
		Entry{ 0x00150577, "RRR11", Share::kLeftAlone },   // High Ground: a vertibird and its crew made at runtime
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
	// (BoSR01's and BosR02's 0 make the player a member of the Brotherhood, set its story's stages and move them to
	// their giver). Their startup stage is another; StageRules lists them as moving the player.
	struct Untaken
	{
		std::uint32_t quest;
		std::uint16_t stage;
	};
	inline constexpr std::array UNTAKEN{
		Untaken{ 0x00064EC7, 0 },  // BoSR01
		Untaken{ 0x000C30DC, 0 },  // BosR02
	};

	inline bool Takes(std::uint32_t a_quest, std::uint16_t a_stage)
	{
		return std::ranges::none_of(UNTAKEN, [&](const Untaken& a_untaken) { return a_untaken.quest == a_quest && a_untaken.stage == a_stage; });
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

	// Aliases' flags (BGSBaseAlias::FLAGS): optional, filled by other aliases.
	inline constexpr std::uint32_t ALIAS_OPTIONAL = 1u << 1;
	inline constexpr std::uint32_t ALIAS_BY_OTHERS = 1u << 11;

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
		std::vector<Force> forces;
		std::string        fallback;

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
	template <class Resolve, class Here>
	std::vector<Mismatch> Compare(std::span<const Alias> a_aliases, std::span<const Protocol::RadiantFill> a_fills, Resolve&& a_resolve, Here&& a_here)
	{
		std::vector<Mismatch> out;
		for (const auto& alias : a_aliases) {
			const auto fill = FillFor(a_fills, alias);
			if (!fill) {
				continue;
			}
			// A place is the run's target whichever way it is filled (a fill type read wrong leaves it checked).
			const bool picked = alias.fill == Fill::kSource || alias.fill == Fill::kByOthers || alias.kind == Kind::kLoc;
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
	};

	// The radiants' runs in this game: which run of each quest is shared here, and with it the rule that pays a run's
	// reward once in each game. The reward is the quest's own stage (MinRecruit's 400), so a game must set each stage
	// of a run at most once and never start a run it has finished: a run's start that comes again (a friend's game
	// resends its runs when someone joins; a repeated packet) is ignored once that run is known, finished or turned
	// down here, and each stage of it is taken or reported once. A stage taken from a friend is never reported back.
	// A new run of the same quest (it is handed out again) is another run, and pays again.
	class Book
	{
	public:
		enum class State : std::uint8_t
		{
			kStarting,  // started here with a friend's fills; not yet seen running
			kShared,
		};

		// This game started the quest in a session (the player has it): a run of ours, with the stages done so far.
		Run Begin(std::uint32_t a_quest, std::uint32_t a_self, std::span<const std::uint16_t> a_done)
		{
			const Run run{ a_self, ++serial };
			auto&     instance = quests[a_quest];
			instance = Instance{};
			instance.state = State::kShared;
			instance.runs.push_back(run);
			instance.reported.insert(a_done.begin(), a_done.end());
			return run;
		}

		[[nodiscard]] Answer OnStart(std::uint32_t a_quest, const Run& a_run, bool a_running) const
		{
			if (finished.contains(a_run) || refused.contains(a_run)) {
				return Answer::kIgnore;
			}
			if (const auto it = quests.find(a_quest); it != quests.end()) {
				if (std::ranges::find(it->second.runs, a_run) != it->second.runs.end() || it->second.state == State::kStarting) {
					return Answer::kIgnore;
				}
				return Answer::kCompare;
			}
			return a_running ? Answer::kCompare : Answer::kStart;
		}

		// Started here from a friend's run, the stages they had done to come.
		void Starting(std::uint32_t a_quest, const Run& a_run)
		{
			auto& instance = quests[a_quest];
			instance = Instance{};
			instance.state = State::kStarting;
			instance.runs.push_back(a_run);
		}

		// The quest runs here with the run's fills (started from it, or found running with them): the run is shared.
		void Join(std::uint32_t a_quest, const Run& a_run)
		{
			auto& instance = quests[a_quest];
			instance.state = State::kShared;
			if (std::ranges::find(instance.runs, a_run) == instance.runs.end()) {
				instance.runs.push_back(a_run);
			}
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

		// The quest stopped here (completed, failed or timed out): its run is over here, and never starts again.
		void Ended(std::uint32_t a_quest)
		{
			if (const auto it = quests.find(a_quest); it != quests.end()) {
				finished.insert(it->second.runs.begin(), it->second.runs.end());
				quests.erase(it);
			}
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
		[[nodiscard]] std::size_t Finished() const { return finished.size(); }

		// The session ended: no run is shared any more (the quests go on, each player's own). The runs finished or
		// turned down here stay known (a player keeps their id in a session when they come back), so a start sent
		// again never starts one here twice.
		void EndSession() { quests.clear(); }

		// Another server, or a save loaded: nothing of the last is known.
		void Clear()
		{
			quests.clear();
			finished.clear();
			refused.clear();
		}

	private:
		struct Instance
		{
			State                   state = State::kShared;
			std::vector<Run>        runs;      // the run (ours or a friend's) and any other found to have the same fills
			std::set<std::uint16_t> taken;     // friends' stages taken for it
			std::set<std::uint16_t> reported;  // stages that need no report from here (reported, sent with the start, or taken)
		};

		std::map<std::uint32_t, Instance> quests;
		std::set<Run>                     finished;
		std::set<Run>                     refused;
		std::uint32_t                     serial = 0;
	};
}
