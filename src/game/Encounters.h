#pragma once

#include "game/QuestFilter.h"
#include "game/RemoteEdits.h"

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
// An encounter is a quest of the misc type whose editor id starts RE (RESceneLC01, RETravelKMK01, REObjectKMK02,
// RECampLC01, REChokepointSC01, REAssaultSC06, RECheckpoint02_SynthAttackers; the add-ons' after their prefix:
// DLC03RESceneKMK01, DLC04RETravelCT01, DLC01RESceneKMK02). Every actor of one is made at runtime by an alias of the quest
// as it starts (Fallout4.esm: 780 created NPC bases over 236 such quests), so each game's run makes its own, under ids
// that are that game's own (0xFF......): the usual spawn. Spawns shared one only once it was in combat or hostile to its
// player, so a calm scene stood in one game alone (a Nexus report on 0.13.2: the accusation of RESceneLC01, "Art and
// Art", whose accusor only aims). Now one of our runtime NPCs that an alias of a running encounter quest created is ours
// to share calm or not; the server tells it apart from a shared quest's paired creature (QuestActors.h) by the spawn's
// kind (Protocol::SpawnKind::kEncounter): an encounter quest is repeatable and every game's run is its own, so two
// games running RETravelKMK01 at two ends of the map both share theirs, and no twin refusal applies.
//
// Each game rolls encounters for its own player: the trigger references (RETriggerScene, RETriggerTravel, ...) send a
// story event on the player's entering them (RETriggerScript), and the story manager picks one quest for it. Two players
// crossing one trigger together usually roll two different encounters at the same markers, sometimes the same one. So
// where a friend's encounter already runs, ours is stopped (through the quest's own stop, which REScript itself calls on
// REParent's cleanup and which deletes its created actors), and our copies of theirs are placed: one encounter per spot.
// Who was first is the server's order: every spawn message that arrives (a friend's kSpawned, our own grant in
// kActorOwners) is numbered as it comes, the same order in every game since the server sends them in the order it took
// them, so the two games decide alike and exactly one stops. Kept free of the game so it can be tested
// (tests/EncounterTests.cpp).
namespace Encounters
{
	using Clock = std::chrono::steady_clock;
	using RemoteEdits::Spot;

	// TESQuest::data.questType of the game's miscellaneous quests, which every random encounter is filed under (its
	// parents and bookkeeping are of type none, and one vertibird fight is a side quest; none of those makes a calm actor).
	inline constexpr std::int8_t QUEST_TYPE_MISC = 6;

	// A friend's encounter this close to one of our encounter's actors runs at the same spot (an encounter's markers lie
	// within about a thousand units of its trigger; two triggers are seldom closer than this).
	inline constexpr float COLLIDE_RANGE = 3000.0f;
	// Our encounter is stopped for a friend's only this soon after our game started it: past that its player is in it.
	inline constexpr auto STOP_WINDOW = std::chrono::seconds(20);
	// A quest we stopped is left alone this long: the engine winds it down and deletes its actors meanwhile.
	inline constexpr auto STOPPED_FOR = std::chrono::seconds(60);
	// An encounter of ours none of whose actors has been seen for this long is forgotten (it stopped, or its actors
	// unloaded for good).
	inline constexpr auto FORGET_AFTER = std::chrono::minutes(5);
	// Actors of one encounter whose spots are kept (a travelling group is three to eight).
	inline constexpr std::size_t MAX_ACTORS = 16;

	// Whether this is a name the game gives its random encounters: RE and a capital or an underscore (RESceneLC01,
	// REChokepointSC01, RECheckpoint02_SynthAttackers, REScene_VertibirdKMK01), after the add-on's prefix in an add-on's
	// files (DLC03RESceneKMK01, DLC04RETravelCT01). Not ReconScopeQuest or RelayTowerQuest.
	inline bool IsEncounterName(std::string_view a_editorId)
	{
		const auto name = QuestFilter::WithoutAddOnPrefix(a_editorId);
		return name.size() > 2 && name[0] == 'R' && name[1] == 'E' && (name[2] == '_' || std::isupper(static_cast<unsigned char>(name[2])));
	}

	// An encounter quest: a misc quest named so. REParent and RECorpseSwarmCreator (type none) and the RECheckpoint*_Init
	// side quests are not; nor is REAssault_VertibirdKMK01 (a side quest), whose vertibird fight is shared as any fight.
	inline bool IsEncounter(std::int8_t a_type, std::string_view a_editorId)
	{
		return a_type == QUEST_TYPE_MISC && IsEncounterName(a_editorId);
	}

	// A friend's encounter spawn, as the book sees it (Spawns builds these from its copies).
	struct Theirs
	{
		std::uint32_t spawnId = 0;
		std::uint32_t quest = 0;   // the encounter quest their game rolled
		std::uint32_t player = 0;  // whose
		Spot          at;          // where their game has it now
		std::uint64_t seq = 0;     // its number in the server's order (Book::Next as it arrived)
		bool          over = false;  // it died, or their game let it go
	};

	// The friend's encounter ours gives way to.
	struct Rival
	{
		std::uint32_t spawnId = 0;
		std::uint32_t quest = 0;
		std::uint32_t player = 0;
		float         distance = 0.0f;  // from the nearest of ours
	};

	class Book
	{
	public:
		// The next number in the server's order: given to every spawn message as it arrives, a friend's spawn or our grant.
		std::uint64_t Next() { return ++seq; }

		// One of our encounter actors (a_local, of quest a_quest) was seen in the scan, standing at a_at. The first sighting
		// of a quest's actor is when our run of it started, as far as the book can tell.
		void OursSeen(std::uint32_t a_quest, std::uint32_t a_local, const Spot& a_at, Clock::time_point a_now)
		{
			auto& run = ours.try_emplace(a_quest, Run{ .firstSeen = a_now }).first->second;
			run.lastSeen = a_now;
			if (const auto it = std::ranges::find(run.actors, a_local, &Actor::local); it != run.actors.end()) {
				it->at = a_at;
			} else if (run.actors.size() < MAX_ACTORS) {
				run.actors.push_back({ a_local, a_at });
			}
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

		// We stopped our run of a_quest: left alone for STOPPED_FOR, and its actors not offered meanwhile.
		void Stopped(std::uint32_t a_quest, Clock::time_point a_now)
		{
			stopped[a_quest] = a_now;
			ours.erase(a_quest);
		}

		[[nodiscard]] bool IsStopped(std::uint32_t a_quest, Clock::time_point a_now) const
		{
			const auto it = stopped.find(a_quest);
			return it != stopped.end() && a_now - it->second < STOPPED_FOR;
		}

		// The friend's encounter our run of a_quest gives way to, if any: a live spawn of theirs within COLLIDE_RANGE of one
		// of our run's actors, whose number in the server's order is below our run's first grant (or our run has none yet),
		// while our run started within STOP_WINDOW. The nearest when several. Nothing for a run the book doesn't know, one
		// past the window, or one stopped already.
		[[nodiscard]] std::optional<Rival> RivalOf(std::uint32_t a_quest, std::span<const Theirs> a_theirs, Clock::time_point a_now) const
		{
			const auto it = ours.find(a_quest);
			if (it == ours.end() || IsStopped(a_quest, a_now) || a_now - it->second.firstSeen >= STOP_WINDOW) {
				return std::nullopt;
			}
			const auto&          run = it->second;
			std::optional<Rival> best;
			for (const auto& theirs : a_theirs) {
				if (theirs.over || theirs.quest == 0 || (run.grant && theirs.seq >= *run.grant)) {
					continue;
				}
				for (const auto& actor : run.actors) {
					const float distance = RemoteEdits::Distance(actor.at, theirs.at);  // NaN in another place
					if (RemoteEdits::Within(distance, COLLIDE_RANGE) && (!best || distance < best->distance)) {
						best = Rival{ theirs.spawnId, theirs.quest, theirs.player, distance };
					}
				}
			}
			return best;
		}

		// Runs none of whose actors has been seen for FORGET_AFTER, and stops past STOPPED_FOR, are forgotten.
		void Expire(Clock::time_point a_now)
		{
			std::erase_if(ours, [&](const auto& a_entry) { return a_now - a_entry.second.lastSeen >= FORGET_AFTER; });
			std::erase_if(stopped, [&](const auto& a_entry) { return a_now - a_entry.second >= STOPPED_FOR; });
		}

		// Whether the book knows our run of a_quest.
		[[nodiscard]] bool Knows(std::uint32_t a_quest) const { return ours.contains(a_quest); }

		// Where our run of a_quest was first seen (for the log), or nothing.
		[[nodiscard]] std::optional<Clock::time_point> StartedAt(std::uint32_t a_quest) const
		{
			const auto it = ours.find(a_quest);
			return it != ours.end() ? std::optional{ it->second.firstSeen } : std::nullopt;
		}

		[[nodiscard]] std::size_t Size() const { return ours.size(); }

		void Clear()
		{
			ours.clear();
			stopped.clear();
		}

	private:
		struct Actor
		{
			std::uint32_t local = 0;
			Spot          at;
		};
		struct Run
		{
			Clock::time_point            firstSeen{};
			Clock::time_point            lastSeen{};
			std::optional<std::uint64_t> grant;  // the number of the first grant of one of its actors
			std::vector<Actor>           actors;
		};
		std::unordered_map<std::uint32_t, Run>               ours;     // our encounter runs, by quest
		std::unordered_map<std::uint32_t, Clock::time_point> stopped;  // quests we stopped, and when
		std::uint64_t                                        seq = 0;
	};
}
