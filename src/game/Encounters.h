#pragma once

#include "game/QuestFilter.h"
#include "game/RemoteEdits.h"
#include "game/SpawnBook.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// Random encounters shared calm: the game that rolled one owns it, its actors are shared as spawns from the moment they
// stand (a man held at gunpoint and called a synth, travellers, a trader), the others mirror them, and a game that rolls
// its own encounter where a friend's already runs stops its own.
//
// An encounter is a quest of the misc type, started by a story manager event, whose editor id starts RE (RESceneLC01,
// RETravelKMK01, REObjectKMK02, RECampLC01, REChokepointSC01, REAssaultSC06, RECheckpoint02_SynthAttackers; the add-ons'
// after their prefix: DLC03RESceneKMK01, DLC04RETravelCT01, DLC01RESceneKMK02). Every actor of one is made at runtime by
// an alias of the quest as it starts (Fallout4.esm: 780 created NPC bases over 236 such quests), so each game's run makes
// its own, under ids that are that game's own (0xFF......): the usual spawn. Spawns shared one only once it was in combat
// or hostile to its player, so a calm scene stood in one game alone (a Nexus report on 0.13.2: the accusation of
// RESceneLC01, "Art and Art", whose accusor only aims). Now one of our runtime NPCs that an alias of a running encounter
// quest created is ours to share calm or not; the server tells it apart from a shared quest's paired creature
// (QuestActors.h) by the spawn's kind (Protocol::SpawnKind::kEncounter): an encounter quest is repeatable and every
// game's run is its own, so two games running RETravelKMK01 at two ends of the map both share theirs, and no twin
// refusal applies. A receiver takes a friend's word for the kind only when the quest named is an encounter in its own
// game's files. An encounter's run ends when its quest stops (REScript's own stop, once its player is far); an actor of
// it still standing then (a radstag, the surviving Art) is shared on, plain (reported again under its id, which the
// server passes on): no encounter runs at its spot any more.
//
// Each game rolls encounters for its own player: the trigger references (RETriggerScene, RETriggerTravel, ...) send a
// story event on the player's entering them (RETriggerScript), and the story manager picks one quest for it. Two players
// crossing one trigger together usually roll two different encounters at the same markers, sometimes the same one. So
// where a friend's encounter already runs, ours is stopped (through the quest's own stop, which REScript itself calls on
// REParent's cleanup) and its actors removed (the quest's stop leaves some standing: a travelling group's dogs, a
// corpse), and our copies of theirs are placed: one encounter per spot. A spot is where an encounter's actors first
// stood, its markers, and a friend's encounter also holds where its actors stand now: a travelling merchant two cells
// on still holds the trigger he was rolled at, and the road he is on. Who was first is the server's order: every spawn
// message that arrives (a friend's kSpawned, our own grant in kActorOwners) is numbered as it comes, the same order in
// every game since the server sends them in the order it took them, so the two games decide alike and exactly one stops. Only a run just begun is stopped (STOP_WINDOW):
// past that its player is in it. A fight's window is shorter (FIGHT_STOP_WINDOW: an actor of the run seen fighting or
// hostile), and a run one of whose actors has hurt its player is never stopped: that player is in the fight, and a
// friend's encounter beside it runs too (WorldSync's hit watch tells Spawns, OursHurt). The encounter quests already running on the first in-world frame of a session, or
// after a load, are settled (Settle): the book can't tell when they began, and a long-running encounter must never be
// stopped partway for a friend's that came to stand beside it; every run first seen after that is fresh. Two
// encounters both met so run side by side until one ends on its own. Kept free of the game so it can be tested
// (tests/EncounterTests.cpp).
namespace Encounters
{
	using Clock = std::chrono::steady_clock;
	using RemoteEdits::Spot;

	// TESQuest::data.questType of the game's miscellaneous quests, which every random encounter is filed under (its
	// parents and bookkeeping are of type none, and one vertibird fight is a side quest; none of those makes a calm actor).
	inline constexpr std::int8_t QUEST_TYPE_MISC = 6;

	// A friend's encounter this close to where one of our encounter's actors first stood runs at the same spot (an
	// encounter's markers lie within about a thousand units of its trigger; two triggers are seldom closer than this).
	inline constexpr float COLLIDE_RANGE = 3000.0f;
	// Our encounter is stopped for a friend's only this soon after our game started it: past that its player is in it.
	inline constexpr auto STOP_WINDOW = std::chrono::seconds(20);
	// The same for a fight (an actor of the run seen fighting, or hostile to our player): a fight is under way at once,
	// and raiders vanishing from a player's sights twenty seconds in is worse than two fights side by side.
	inline constexpr auto FIGHT_STOP_WINDOW = std::chrono::seconds(10);
	// A quest we stopped is left alone this long: the engine winds it down meanwhile, and its actors still seen are
	// removed, not offered.
	inline constexpr auto STOPPED_FOR = std::chrono::seconds(60);
	// Actors of one encounter whose spots are kept (a travelling group is three to eight).
	inline constexpr std::size_t MAX_ACTORS = 16;
	// Calm encounter actors one game shares at once: half its spawns (Protocol::MAX_SPAWNS_PER_PLAYER), a camp and a
	// travelling group, so a fight's raiders are never refused for a crowd of bystanders. A calm actor that turns hostile
	// or joins a fight is shared as any fight's spawn, past this.
	inline constexpr std::size_t MAX_CALM = 16;

	// Whether this is a name the game gives its random encounters: RE and a capital or an underscore (RESceneLC01,
	// REChokepointSC01, RECheckpoint02_SynthAttackers, REScene_VertibirdKMK01), after the add-on's prefix in an add-on's
	// files (DLC03RESceneKMK01, DLC04RETravelCT01). Not ReconScopeQuest or RelayTowerQuest.
	inline bool IsEncounterName(std::string_view a_editorId)
	{
		const auto name = QuestFilter::WithoutAddOnPrefix(a_editorId);
		return name.size() > 2 && name[0] == 'R' && name[1] == 'E' && (name[2] == '_' || std::isupper(static_cast<unsigned char>(name[2])));
	}

	// What the running game holds in TESQuest::eventID for a quest no story manager event starts.
	inline constexpr std::uint32_t NO_STORY_EVENT = 0xFFFFFFFF;

	// Whether a quest's event (TESQuest::eventID, as the running game holds it) names a story manager event. The record's
	// ENAM is a word ('SCPT', the script event a trigger's RETriggerScript sends; 'OAAT' an actor's attaching; 'CLOC' a
	// change of location), and TESQuest's load reads it in as it is, but the quest's InitItemImpl (1.11.240, +669943 to
	// +669964) turns it into the event's index in BGSStoryEventManager's registered events (a small number), or all
	// ones when the word names no registered event; a quest with no ENAM holds all ones from the start (+668556). So any
	// value but all ones is an event, and that is the engine's own test too: the console's startquest (+5C6E29) and
	// Papyrus's Quest.Start and Quest.SetStage on a stopped quest refuse a quest whose event is not all ones. (Read as a
	// word, as this did until 2026-11-12, no running quest's event ever passed: DLC01Lair_ChangeLocation's CLOC was not
	// named as its refusal's cause, and only a story node's list told encounters.)
	inline bool IsStoryEvent(std::uint32_t a_event)
	{
		return a_event != NO_STORY_EVENT;
	}

	// An encounter quest: a misc quest named so that a story manager event starts (IsStoryEvent of TESQuest::eventID:
	// 'SCPT' for the 205 of Fallout4.esm's a trigger's RETriggerScript sends, 'OAAT' for REActorAttach01 and 02). The
	// system's bookkeeping is no encounter, whatever its name: REFactionShared and the RETreasureHuntSCnn_Reward quests
	// (misc, but nothing starts them: they run from the game's start or are set going by a stage), REParent and
	// RECorpseSwarmCreator (type none), the RECheckpoint*_Init side quests; nor is REAssault_VertibirdKMK01 (a side
	// quest), whose vertibird fight is shared as any fight.
	inline bool IsEncounter(std::int8_t a_type, std::uint32_t a_event, std::string_view a_editorId)
	{
		return a_type == QUEST_TYPE_MISC && IsStoryEvent(a_event) && IsEncounterName(a_editorId);
	}

	// The same, told first by the story manager's own word: a_listed says whether one of its quest nodes lists the quest
	// (BGSStoryManagerQuestNode::children, which the game reads once), which is what "a story event starts it" means;
	// the quest's event decides only when no node could be read (a_listed empty).
	inline bool IsEncounter(std::int8_t a_type, std::optional<bool> a_listed, std::uint32_t a_event, std::string_view a_editorId)
	{
		if (a_listed) {
			return a_type == QUEST_TYPE_MISC && *a_listed && IsEncounterName(a_editorId);
		}
		return IsEncounter(a_type, a_event, a_editorId);
	}

	// A friend's encounter spawn, as the book sees it (Spawns builds these from its copies).
	struct Theirs
	{
		std::uint32_t spawnId = 0;
		std::uint32_t quest = 0;   // the encounter quest their game rolled
		std::uint32_t player = 0;  // whose
		Spot          at;          // where their game had it as it was shared: its encounter's spot (not where it has walked to)
		std::uint64_t seq = 0;     // its number in the server's order (Book::Next as it arrived)
		bool          over = false;  // it died, or their game let it go (its encounter over, it is shared on plain: not an encounter spawn any more)
		std::optional<Spot> now;   // where it stands now (its states), if known: a travelling group counts where it is as well as where it began
	};

	// Friends' encounter spawns as the book judges them, from our records of friends' spawns (Spawns' SpawnBook::Theirs)
	// and the number each arrived as (a_seq, by spawn id): only those still of the encounter kind (one whose encounter
	// ended was shared on plain by its owner and taken so, Theirs::Rekind: no encounter runs at its spot any more), over
	// once dead or let go of, at the spot it was shared at and where its owner's game has it now.
	inline std::vector<Theirs> TheirsOf(const SpawnBook::Theirs& a_book, const std::unordered_map<std::uint32_t, std::uint64_t>& a_seq)
	{
		std::vector<Theirs> out;
		for (const auto id : a_book.Ids()) {
			const auto copy = a_book.Find(id);
			if (!copy || copy->spawn.kind != Protocol::SpawnKind::kEncounter) {
				continue;
			}
			const auto& spawn = copy->spawn;
			const auto  seq = a_seq.find(id);
			out.push_back({ id, spawn.quest, spawn.playerId, Spot{ spawn.cell, spawn.worldspace, spawn.x, spawn.y, spawn.z }, seq != a_seq.end() ? seq->second : 0, copy->dead || copy->gone, copy->at });
		}
		return out;
	}

	// The friend's encounter ours gives way to.
	struct Rival
	{
		std::uint32_t spawnId = 0;
		std::uint32_t quest = 0;
		std::uint32_t player = 0;
		float         distance = 0.0f;  // from the nearest of ours
	};

	// What holds every stop of ours for the moment, whichever run it is: the world's runs are not settled yet (Settle is
	// to come, on Spawns' first in-world frame), or our player is in a conversation or a scene (a stop removes every
	// actor of the run, the speaker among them, whichever of them the scan is looking at). Ours stays meanwhile, judged
	// again next time it is seen while the window lasts; past the window it stays for good.
	struct Holds
	{
		bool unsettled = false;
		bool talking = false;
	};

	class Book
	{
	public:
		// The next number in the server's order: given to every spawn message as it arrives, a friend's spawn or our grant.
		std::uint64_t Next() { return ++seq; }

		// The encounter quests running already as the book first looks (the first in-world frame of a session, or after a
		// load): settled, never stopped, whatever comes to stand beside them. A run the book knows of one is settled too.
		void Settle(std::span<const std::uint32_t> a_quests, Clock::time_point a_now)
		{
			for (const auto quest : a_quests) {
				ours.try_emplace(quest, Run{ .firstSeen = a_now }).first->second.settled = true;
			}
		}

		// One of our encounter actors (a_local, of quest a_quest) was seen in the scan, standing at a_at. The first sighting
		// of a quest's actor is when our run of it started, as far as the book can tell (one running before the book looked
		// was settled by Settle), and where the actor first stood is where it stays for the book: the run's spot is its
		// markers, wherever its actors walk to. True when it is a run's; false for an actor of a quest we stopped lately
		// (IsStopped), which makes no new run and is not offered. Which of those the game removes is the game's call
		// (the stopped run's, which it knows from the stop; not a fresh roll's of the same quest within STOPPED_FOR).
		// a_fight: the actor is fighting or hostile to our player, which makes the run a fight for good (FIGHT_STOP_WINDOW).
		bool OursSeen(std::uint32_t a_quest, std::uint32_t a_local, const Spot& a_at, Clock::time_point a_now, bool a_fight = false)
		{
			if (IsStopped(a_quest, a_now)) {
				return false;
			}
			auto& run = ours.try_emplace(a_quest, Run{ .firstSeen = a_now }).first->second;
			run.fight = run.fight || a_fight;
			if (!std::ranges::contains(run.actors, a_local, &Actor::local) && run.actors.size() < MAX_ACTORS) {
				run.actors.push_back({ a_local, a_at });
			}
			return true;
		}

		// The server granted one of our encounter actors of a_quest its spawn id, the a_seq-th spawn message to arrive. The
		// first grant is the run's place in the order.
		void OursGranted(std::uint32_t a_quest, std::uint64_t a_seq)
		{
			const auto it = ours.find(a_quest);
			if (it != ours.end() && (!it->second.grant || a_seq < *it->second.grant)) {
				it->second.grant = a_seq;
			}
		}

		// One of our run's actors hurt our player: the run is never stopped from now on (MayStop), whatever came before it
		// at the spot. Its player is in that fight; a friend's encounter beside it runs too. True the first time.
		bool OursHurt(std::uint32_t a_quest)
		{
			const auto it = ours.find(a_quest);
			if (it == ours.end() || it->second.hurt) {
				return false;
			}
			it->second.hurt = true;
			it->second.fight = true;
			return true;
		}

		// Our run of a_quest is over (the quest no longer runs here): forgotten. Seen running again later, it is a new run.
		void OursOver(std::uint32_t a_quest)
		{
			ours.erase(a_quest);
		}

		// We stopped our run of a_quest: left alone for STOPPED_FOR, and its actors seen so far are the game's to remove
		// (Doomed): the quest's own stop leaves some standing.
		void Stopped(std::uint32_t a_quest, Clock::time_point a_now)
		{
			stopped[a_quest] = a_now;
			if (const auto it = ours.find(a_quest); it != ours.end()) {
				for (const auto& actor : it->second.actors) {
					Doom(a_quest, actor.local);
				}
				ours.erase(it);
			}
		}

		// An actor of a run we stopped, for the game to remove.
		struct Doomed
		{
			std::uint32_t local = 0;
			std::uint32_t quest = 0;

			bool operator==(const Doomed&) const = default;
		};

		// The actors of runs we stopped that the game is to remove, each once (Stopped).
		std::vector<Doomed> TakeDoomed()
		{
			return std::exchange(doomed, {});
		}

		[[nodiscard]] bool IsStopped(std::uint32_t a_quest, Clock::time_point a_now) const
		{
			const auto it = stopped.find(a_quest);
			return it != stopped.end() && a_now - it->second < STOPPED_FOR;
		}

		// Whether our run of a_quest could give way to a friend's now: known, not settled, none of its actors has hurt our
		// player (OursHurt), within its window of its start (STOP_WINDOW, a fight's FIGHT_STOP_WINDOW), not stopped
		// already, and nothing holding every stop (a_holds). RivalOf says to which, if any; what it needs of theirs is
		// gathered only when this holds.
		[[nodiscard]] bool MayStop(std::uint32_t a_quest, Clock::time_point a_now, Holds a_holds = {}) const
		{
			if (a_holds.unsettled || a_holds.talking) {
				return false;
			}
			const auto it = ours.find(a_quest);
			return it != ours.end() && !it->second.settled && !it->second.hurt && !IsStopped(a_quest, a_now) && a_now - it->second.firstSeen < WindowOf(it->second);
		}

		// Whether our run of a_quest stands for good: known, not stopped, and past giving way to anyone's (settled, an actor
		// of it hurt our player, or its window over). Only such a run is shared as a run (EncounterRuns.h): a friend's game
		// never follows one of ours that may yet be stopped. Holds don't count: they put a stop off, they don't make a run stand.
		[[nodiscard]] bool Stands(std::uint32_t a_quest, Clock::time_point a_now) const
		{
			const auto it = ours.find(a_quest);
			return it != ours.end() && !IsStopped(a_quest, a_now) && (it->second.settled || it->second.hurt || a_now - it->second.firstSeen >= WindowOf(it->second));
		}

		// The friend's encounter our run of a_quest gives way to, if any (MayStop): a live spawn of theirs whose spot, or
		// the place it stands now, is within COLLIDE_RANGE of where one of our run's actors first stood, whose number in the
		// server's order is below our run's first grant (or our run has none yet). The nearest when several. Where theirs
		// stands now counts too: a travelling merchant two cells on holds his trigger and the road he is on, so a second
		// merchant is not rolled beside him (the rig, PR 337). Only the later run ever gives way, whatever each game sees of
		// the other's positions, so the two games never both stop.
		[[nodiscard]] std::optional<Rival> RivalOf(std::uint32_t a_quest, std::span<const Theirs> a_theirs, Clock::time_point a_now, Holds a_holds = {}) const
		{
			if (!MayStop(a_quest, a_now, a_holds)) {
				return std::nullopt;
			}
			const auto&          run = ours.at(a_quest);
			std::optional<Rival> best;
			for (const auto& theirs : a_theirs) {
				if (theirs.over || theirs.quest == 0 || (run.grant && theirs.seq >= *run.grant)) {
					continue;
				}
				const auto consider = [&](const Spot& a_ours, const Spot& a_theirs) {
					const float distance = RemoteEdits::Distance(a_ours, a_theirs);  // NaN in another place
					if (RemoteEdits::Within(distance, COLLIDE_RANGE) && (!best || distance < best->distance)) {
						best = Rival{ theirs.spawnId, theirs.quest, theirs.player, distance };
					}
				};
				for (const auto& actor : run.actors) {
					consider(actor.at, theirs.at);
					if (theirs.now) {
						consider(actor.at, *theirs.now);
					}
				}
			}
			return best;
		}

		// Stops past STOPPED_FOR are forgotten. Runs are kept until OursOver (the quest stopped running) or Clear.
		void Expire(Clock::time_point a_now)
		{
			std::erase_if(stopped, [&](const auto& a_entry) { return a_now - a_entry.second >= STOPPED_FOR; });
		}

		// Whether the book knows our run of a_quest.
		[[nodiscard]] bool Knows(std::uint32_t a_quest) const { return ours.contains(a_quest); }

		// Whether our run of a_quest is a fight (an actor of it seen fighting or hostile), and whether an actor of it hurt
		// our player.
		[[nodiscard]] bool Fight(std::uint32_t a_quest) const
		{
			const auto it = ours.find(a_quest);
			return it != ours.end() && it->second.fight;
		}
		[[nodiscard]] bool Hurt(std::uint32_t a_quest) const
		{
			const auto it = ours.find(a_quest);
			return it != ours.end() && it->second.hurt;
		}

		// Whether our run of a_quest is settled: met already under way, never stopped.
		[[nodiscard]] bool Settled(std::uint32_t a_quest) const
		{
			const auto it = ours.find(a_quest);
			return it != ours.end() && it->second.settled;
		}

		// Where our run of a_quest was first seen (for the log), or nothing.
		[[nodiscard]] std::optional<Clock::time_point> StartedAt(std::uint32_t a_quest) const
		{
			const auto it = ours.find(a_quest);
			return it != ours.end() ? std::optional{ it->second.firstSeen } : std::nullopt;
		}

		// The quests of our runs (for the check that each still runs).
		[[nodiscard]] std::vector<std::uint32_t> Quests() const
		{
			std::vector<std::uint32_t> out;
			out.reserve(ours.size());
			for (const auto& [quest, run] : ours) {
				out.push_back(quest);
			}
			return out;
		}

		[[nodiscard]] std::size_t Size() const { return ours.size(); }

		// A load, a session's end: what the book knew is of another world (Spawns settles what runs as it next looks).
		void Clear()
		{
			ours.clear();
			stopped.clear();
			doomed.clear();
		}

	private:
		void Doom(std::uint32_t a_quest, std::uint32_t a_local)
		{
			if (!std::ranges::contains(doomed, a_local, &Doomed::local)) {
				doomed.push_back({ a_local, a_quest });
			}
		}

		struct Actor
		{
			std::uint32_t local = 0;
			Spot          at;  // where it first stood
		};
		struct Run
		{
			Clock::time_point            firstSeen{};
			bool                         settled = false;  // met already under way: never stopped
			bool                         fight = false;    // an actor of it seen fighting or hostile: FIGHT_STOP_WINDOW
			bool                         hurt = false;     // an actor of it hurt our player: never stopped
			std::optional<std::uint64_t> grant;            // the number of the first grant of one of its actors
			std::vector<Actor>           actors;
		};
		static Clock::duration WindowOf(const Run& a_run)
		{
			return a_run.fight ? Clock::duration{ FIGHT_STOP_WINDOW } : Clock::duration{ STOP_WINDOW };
		}
		std::unordered_map<std::uint32_t, Run>               ours;     // our encounter runs, by quest
		std::unordered_map<std::uint32_t, Clock::time_point> stopped;  // quests we stopped, and when
		std::vector<Doomed>                                  doomed;   // actors of stopped runs for the game to remove (TakeDoomed)
		std::uint64_t                                        seq = 0;
	};
}
