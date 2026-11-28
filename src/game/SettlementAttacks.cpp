#include "game/SettlementAttacks.h"

#include "AttackBook.h"
#include "Config.h"
#include "Text.h"
#include "game/Hud.h"
#include "game/NpcSync.h"
#include "game/Papyrus.h"
#include "game/QuestActors.h"
#include "game/QuestStageLayout.h"
#include "game/RadiantSync.h"
#include "game/Spawns.h"
#include "game/Story.h"
#include "game/WorkshopSync.h"
#include "game/WorldSync.h"

#include <array>
#include <chrono>
#include <format>
#include <optional>
#include <unordered_map>
#include <utility>

namespace SettlementAttacks
{
	namespace
	{
		using namespace std::chrono_literals;
		using Clock = AttackBook::Clock;
		using Phase = Protocol::AttackPhase;
		using Outcome = Protocol::AttackOutcome;

		constexpr auto LOOK_INTERVAL = 500ms;
		// A mirror's start that failed is tried again after this long (its quest runs elsewhere here, or won't fill).
		constexpr auto START_WAIT = 15s;
		constexpr std::uint32_t MAX_START_TRIES = 3;
		// A start the game can't make yet (the quest's last run here is still being shut down) is asked again this soon,
		// and isn't one of the tries, this many times.
		constexpr auto          START_AGAIN = 2s;
		constexpr std::uint32_t MAX_START_WAITS = 10;
		// A stage set or a stop asked is asked again after this, if the game hasn't done it.
		constexpr auto STEP_AGAIN = 5s;
		// The mirror's own WorkshopAttackScript timer (stage 10's StartTimerGameTime(24), timer 0, whose end begins the
		// attack) is pushed this far out, again every PUSH_AGAIN: long past the host's run (24 game hours, a night attack's
		// wait, then at most 15 before its cleanup). The mirror is defused as it runs, so a save that kept one (and a game
		// without this plugin to stop it on load) sees it play out with no damage and end, never stuck.
		constexpr float TIMER_HOURS = 48.0f;
		constexpr auto  PUSH_AGAIN = 10min;
		constexpr std::string_view ATTACK_SCRIPT = "WorkshopAttackScript";
		// The host's attackers, as our copies of friends' NPCs, count as at the settlement within this of its workbench
		// (a settlement's attack markers are at its edges), and for this long after they were last seen there (copies come
		// and go as they move in and out of our loaded cells).
		constexpr float ATTACKERS_RADIUS = 8192.0f;
		constexpr auto  ATTACKERS_LINGER = 10s;
		// A mirror started as the session ends, not seen running yet: any run of its quest that begins this soon after is
		// ours to close down.
		constexpr auto CLOSE_PENDING = START_WAIT + 5s;

		bool                                   hosting = false;
		std::vector<std::vector<std::uint8_t>> outgoing;
		Clock::time_point                      lastLook{};

		// AttackBook::ATTACKS in this game (a quest not found is skipped), and the add-ons not installed here (their attacks
		// aren't looked for).
		std::vector<RE::TESQuest*>    quests;
		std::vector<std::string_view> missingFiles;
		bool                          resolved = false;

		// AttackBook::SIEGES in this game.
		struct SiegeQuest
		{
			RE::TESQuest*             quest = nullptr;
			const AttackBook::Siege* siege = nullptr;
		};
		std::vector<SiegeQuest> siegeQuests;

		// The host's game.
		AttackBook::HostRuns  hostRuns;
		AttackBook::HostWords hostWords;

		// Closing down a run of ours that is the host's to decide (AttackBook::NextClose): when each step was asked.
		struct CloseState
		{
			Clock::time_point defuseAsked{};
			Clock::time_point timerAsked{};
			Clock::time_point stopAsked{};
		};

		// A friend's game.
		AttackBook::View view;
		struct Mirror
		{
			std::uint32_t     workshop = 0;
			std::uint32_t     location = 0;
			Phase             hostPhase = Phase::kRolled;  // the host's run's, kOver once it ended (or the host left)
			Outcome           outcome = Outcome::kNone;
			std::uint32_t     startTries = 0;
			Clock::time_point startAsked{};
			std::uint32_t     startWaits = 0;  // starts the game couldn't make yet (not tries)
			Clock::time_point startAfter{};    // ... the next is asked no sooner
			bool              running = false;  // seen running at the host's settlement
			bool              ended = false;    // it ran and stopped while the host's run went on (its own cleanup): not started again
			bool              gaveUpLogged = false;
			bool              objectiveShown = false;
			bool              sweptOwn = false;  // its own attackers removed (its own timer began it)
			Clock::time_point pushed{};          // its timer pushed out last
			Clock::time_point attackersSeen{};   // the host's attackers last seen at the settlement here
			CloseState        close;
		};
		std::unordered_map<std::uint32_t, Mirror> mirrors;  // by quest
		struct Own
		{
			std::uint32_t     workshop = 0;
			Clock::time_point asked{};
			bool              hosts = false;  // judged the host's to decide (defused, held or stopped)
			bool              holdLogged = false;
			bool              keepLogged = false;
			CloseState        close;
		};
		std::unordered_map<std::uint32_t, Own>  own;          // by quest: our own runs judged
		std::unordered_map<std::uint32_t, bool> toldUnderway;  // by quest: the host's attacks whose start we noticed

		// Runs closed down out of a session's looks: what a session left behind as it ended, and what a load brought that
		// a session left defused (AttackBook::Leftover). Looked at in any frame in the world, in a session or not.
		struct Closing
		{
			std::uint32_t     workshop = 0;
			Clock::time_point anyUntil{};       // a start of ours may still land until then: a run of the quest anywhere is ours
			bool              objectiveShown = false;
			bool              seenRunning = false;
			std::string_view  why;
			CloseState        close;
		};
		std::unordered_map<std::uint32_t, Closing> closing;  // by quest
		bool                                       sweepPending = false;  // a load came: its leftovers are looked for

		// A friend's game: its own run of a siege quest, held back for the host's (AttackBook::NextSiege).
		struct SiegeRun
		{
			bool              held = false;          // we marked its startStage done
			bool              objectiveShown = false;
			bool              hostDefended = false;  // the host's run reached its doneStage
			bool              hostOver = false;      // the host's run was told over (a new one clears both)
			bool              hostSaid = false;      // the host's run, or its word that the settlement is the host's, came in this session
			bool              ownLogged = false;
			Clock::time_point attackersSeen{};
			Clock::time_point endAsked{};
		};
		std::unordered_map<std::uint32_t, SiegeRun> sieges;     // by quest
		std::vector<std::uint32_t>                  releasing;  // held sieges let go once the world is there (the session ended)

		// Counts for `net status`.
		std::uint32_t reported = 0;       // our phases sent (host)
		std::uint32_t answered = 0;       // questions answered (host)
		std::uint32_t ownerChanges = 0;   // changes of owner told (host)
		std::uint32_t asked = 0;          // questions sent (friend)
		std::uint32_t mirrorStarts = 0;   // mirrors started (friend)
		std::uint32_t mirrorsEnded = 0;   // mirrors stopped (friend)
		std::uint32_t ownDefused = 0;     // own runs at the host's settlements defused (friend)
		std::uint32_t ownStopped = 0;     // and stopped
		std::uint32_t timerPushes = 0;    // mirror timers pushed out
		std::uint32_t closedDown = 0;     // runs closed down after a session's end or a load
		std::uint32_t leftovers = 0;      // runs a load brought that a session left defused
		std::uint32_t savedDefused = 0;   // runs defused just before a save
		std::uint32_t siegesHeld = 0;     // our siege fights held back for the host's (friend)
		std::uint32_t siegesEnded = 0;    // our siege end stages set from the host's (friend)
		std::uint32_t siegesReleased = 0; // held fights let go (friend, or a session's end)
		std::uint32_t siegeTimers = 0;    // siege timers started again (a release, or a load)

		void Resolve()
		{
			if (resolved) {
				return;
			}
			const auto handler = RE::TESDataHandler::GetSingleton();
			if (!handler) {
				return;
			}
			for (const auto& siege : AttackBook::SIEGES) {
				if (!handler->LookupLoadedModByName(siege.file)) {
					continue;
				}
				if (const auto quest = handler->LookupForm<RE::TESQuest>(siege.quest, siege.file)) {
					siegeQuests.push_back({ quest, &siege });
				} else {
					REX::WARN("SettlementAttacks: {} ({:06X}) is not in {} here; its siege is each game's own", siege.editorId, siege.quest, siege.file);
				}
			}
			for (const auto& attack : AttackBook::ATTACKS) {
				if (std::ranges::contains(missingFiles, attack.file)) {
					continue;
				}
				if (!handler->LookupLoadedModByName(attack.file)) {
					// An add-on this game doesn't have (a friend's game without it drops the host's reports of its attacks, as any
					// message naming a form of a file it lacks).
					missingFiles.push_back(attack.file);
					REX::INFO("SettlementAttacks: {} isn't installed here: its attacks aren't looked for", attack.file);
					continue;
				}
				if (const auto quest = handler->LookupForm<RE::TESQuest>(attack.quest, attack.file)) {
					quests.push_back(quest);
				} else {
					REX::WARN("SettlementAttacks: {} ({:06X}) is not in {} here; its attacks are each game's own", attack.editorId, attack.quest, attack.file);
				}
			}
			resolved = true;
		}

		const char* EditorId(const RE::TESQuest* a_quest)
		{
			const char* name = a_quest ? a_quest->formEditorID.c_str() : nullptr;
			return name && *name ? name : "?";
		}

		bool Running(const RE::TESQuest* a_quest)
		{
			return (a_quest->data.flags & QuestActors::QUEST_RUNNING) != 0;
		}

		std::uint32_t AliasedLocation(RE::TESQuest* a_quest, std::uint32_t a_alias)
		{
			RE::BSAutoReadLock lock{ a_quest->aliasAccessLock };
			const auto         it = a_quest->aliasedLocMap.find(a_alias);
			return it != a_quest->aliasedLocMap.end() && it->second ? it->second->GetFormID() : 0;
		}

		AttackBook::Seen Read(RE::TESQuest* a_quest)
		{
			AttackBook::Seen seen{ a_quest->GetFormID() };
			seen.running = Running(a_quest);
			if (seen.running) {
				const auto workshop = a_quest->GetAliasedRef(AttackBook::WORKSHOP_ALIAS).get();
				seen.workshop = workshop && Protocol::IsShareableRef(workshop->GetFormID()) ? workshop->GetFormID() : 0;
				const auto location = AliasedLocation(a_quest, AttackBook::LOCATION_ALIAS);
				seen.location = Protocol::IsShareableRef(location) ? location : 0;
			}
			seen.begun = QuestStageLayout::IsStageDone(a_quest, AttackBook::START_STAGE);
			seen.done = QuestStageLayout::IsStageDone(a_quest, AttackBook::DONE_STAGE);
			return seen;
		}

		// A siege quest as the attacks' look reads it: begun is its startStage, done its doneStage.
		AttackBook::Seen ReadSiege(const SiegeQuest& a_siege)
		{
			const auto       quest = a_siege.quest;
			AttackBook::Seen seen{ quest->GetFormID() };
			seen.running = Running(quest);
			if (seen.running) {
				const auto workshop = quest->GetAliasedRef(a_siege.siege->workshopAlias).get();
				seen.workshop = workshop && Protocol::IsShareableRef(workshop->GetFormID()) ? workshop->GetFormID() : 0;
				const auto location = AliasedLocation(quest, a_siege.siege->locationAlias);
				seen.location = Protocol::IsShareableRef(location) ? location : 0;
			}
			seen.begun = QuestStageLayout::IsStageDone(quest, a_siege.siege->startStage);
			seen.done = QuestStageLayout::IsStageDone(quest, a_siege.siege->doneStage);
			return seen;
		}

		RE::TESObjectREFR* Workbench(std::uint32_t a_id)
		{
			return a_id != 0 ? RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_id) : nullptr;
		}

		// The workbench's settlement is 3D-loaded here: our player is there (CheckResolveAttack's own test).
		bool LoadedHere(std::uint32_t a_workshop)
		{
			const auto bench = Workbench(a_workshop);
			return bench && bench->Get3D() != nullptr;
		}

		// Whether our player owns the settlement in our save (Storage's test: WorkshopScript.SetOwnedByPlayer makes the
		// player's NPC the workbench's owner; PlayerFaction counts too). RadiantRules::StartsOwned is another question on
		// purpose (a job script's own test, GetActorOwner, which gives no faction): neither is to be made like the other.
		bool OwnBench(RE::TESObjectREFR* a_bench)
		{
			const auto owner = a_bench ? a_bench->GetOwner() : nullptr;
			return owner && (owner->GetFormID() == 0x00000007 || owner->GetFormID() == 0x0001C21C);
		}

		// The host's word on a settlement as our game reads it (the host's game).
		bool HostOwns(RE::TESObjectREFR* a_bench)
		{
			return Config::Get().sharedAttacks && a_bench && WorkshopSync::IsWorkbenchAnchor(a_bench) && OwnBench(a_bench);
		}

		bool HasObjective(const RE::TESQuest* a_quest, std::uint16_t a_index)
		{
			return std::ranges::any_of(a_quest->objectives, [&](const RE::BGSQuestObjective* a_objective) { return a_objective && a_objective->index == a_index; });
		}

		std::string PlaceName(std::uint32_t a_workshop, std::uint32_t a_location)
		{
			const auto named = [](const RE::BGSLocation* a_at) -> std::string {
				const char* name = a_at ? a_at->GetFullName() : nullptr;
				return name && *name ? std::string{ name } : std::string{};
			};
			auto name = named(a_location != 0 ? RE::TESForm::GetFormByID<RE::BGSLocation>(a_location) : nullptr);
			if (name.empty()) {
				const auto bench = Workbench(a_workshop);
				name = named(bench ? bench->GetCurrentLocation() : nullptr);
			}
			return name.empty() ? Text::Get("attack.a_settlement") : name;
		}

		void Console(const std::string& a_command)
		{
			RE::Console::ExecuteCommand(a_command.c_str());
			NpcSync::CountCommands(1);
		}

		// Stage DONE_STAGE: the quest's 1000 then finds its attackers dead as far as it cares, and rolls no damage
		// (CheckResolveAttack), wherever our player is. The console's setstage marks the stage done as it returns (the
		// engine's TESQuest::SetStage; its fragment runs on the VM later).
		void Defuse(const RE::TESQuest* a_quest)
		{
			Console(std::format("setstage {:08X} {}", a_quest->GetFormID(), AttackBook::DONE_STAGE));
		}

		// Calls a WorkshopAttackScript method on the quest with a float and an int (timer 0, the attack's).
		bool CallTimer(RE::TESQuest* a_quest, std::string_view a_function, std::optional<float> a_hours)
		{
			const auto self = Papyrus::BoundObject(a_quest, ATTACK_SCRIPT, false);
			if (!self) {
				return false;
			}
			return Papyrus::CallOn(self, a_function, [a_hours](RE::BSScrapArray<RE::BSScript::Variable>& a_out) {
				if (a_hours) {
					a_out = RE::BSScrapArray<RE::BSScript::Variable>{ 2 };
					a_out[0] = *a_hours;
					a_out[1] = static_cast<std::int32_t>(0);
				} else {
					a_out = RE::BSScrapArray<RE::BSScript::Variable>{ 1 };
					a_out[0] = static_cast<std::int32_t>(0);
				}
			});
		}

		// Pushes the quest's WorkshopAttackScript timer out (StartTimerGameTime again under its id 0 replaces it).
		bool PushTimer(RE::TESQuest* a_quest)
		{
			return CallTimer(a_quest, "StartTimerGameTime", TIMER_HOURS);
		}

		// Cancels it (CancelTimerGameTime(0)): a timer that outlived the quest's stop would SetStage(20) on it, which
		// starts a stopped quest again.
		bool CancelTimer(RE::TESQuest* a_quest)
		{
			return CallTimer(a_quest, "CancelTimerGameTime", std::nullopt);
		}

		// The close-down's next step for a run of ours (AttackBook::NextClose), taken when it is due: the step taken
		// (kNone: none now, or the run is closed). a_viaSpawns: stopped through Spawns, its alias-made actors removed
		// first (in a session); otherwise by the console (out of one: Spawns' frame doesn't run).
		AttackBook::CloseStep Close(RE::TESQuest* a_quest, bool a_running, bool a_done, CloseState& a_state, Clock::time_point a_now, bool a_viaSpawns)
		{
			const AttackBook::CloseFacts facts{
				.running = a_running,
				.done = a_done,
				.timerCancelled = a_state.timerAsked != Clock::time_point{} && a_now - a_state.timerAsked >= LOOK_INTERVAL,
			};
			switch (const auto step = AttackBook::NextClose(facts)) {
			case AttackBook::CloseStep::kNone:
				return step;
			case AttackBook::CloseStep::kDefuse:
				if (a_now - a_state.defuseAsked < STEP_AGAIN) {
					return AttackBook::CloseStep::kNone;
				}
				a_state.defuseAsked = a_now;
				Defuse(a_quest);
				return step;
			case AttackBook::CloseStep::kCancelTimer:
				if (a_state.timerAsked != Clock::time_point{}) {
					return AttackBook::CloseStep::kNone;  // asked: the stop comes a look later
				}
				a_state.timerAsked = a_now;
				if (!CancelTimer(a_quest)) {
					REX::WARN("SettlementAttacks: {:08X} '{}': its timer couldn't be cancelled (no script bound); stopped all the same", a_quest->GetFormID(), EditorId(a_quest));
				}
				return step;
			case AttackBook::CloseStep::kStop:
				if (a_now - a_state.stopAsked < STEP_AGAIN) {
					return AttackBook::CloseStep::kNone;
				}
				a_state.stopAsked = a_now;
				if (a_viaSpawns) {
					Spawns::StopQuestRun(a_quest->GetFormID());
				} else {
					Console(std::format("stopquest {:08X}", a_quest->GetFormID()));
				}
				return step;
			}
			return AttackBook::CloseStep::kNone;
		}

		void Send(const Protocol::SettlementAttack& a_attack)
		{
			outgoing.push_back(Protocol::Encode(a_attack));
		}

		void SendOwner(std::uint32_t a_workshop, bool a_owned)
		{
			hostWords.Said(a_workshop, a_owned);
			outgoing.push_back(Protocol::Encode(Protocol::SettlementOwner{ a_workshop, a_owned }));
		}

		// --- the host's game ---

		bool Shares()
		{
			return Config::Get().sharedAttacks;
		}

		void HostLook(Clock::time_point a_now)
		{
			if (!Shares()) {
				return;
			}
			std::vector<AttackBook::Seen> seen;
			seen.reserve(quests.size());
			for (const auto quest : quests) {
				seen.push_back(Read(quest));
			}
			// The Castle's siege is reported as the attacks are: rolled while the quest runs, underway at its fight, defended
			// at its end stage, over as it stops.
			for (const auto& siege : siegeQuests) {
				seen.push_back(ReadSiege(siege));
			}
			for (const auto& report : hostRuns.Look(seen)) {
				const auto quest = RE::TESForm::GetFormByID<RE::TESQuest>(report.quest);
				REX::INFO("SettlementAttacks: our attack {:08X} '{}' on {:08X} ({}): phase {}{}", report.quest, EditorId(quest), report.workshop, PlaceName(report.workshop, report.location),
					std::to_underlying(report.phase), report.phase == Phase::kOver ? std::format(", outcome {}", std::to_underlying(report.outcome)) : std::string{});
				if (report.phase == Phase::kRolled) {
					// Our word on the settlement first: a friend's roll there gives way at once.
					SendOwner(report.workshop, OwnBench(Workbench(report.workshop)));
				}
				Send(report);
				++reported;
			}
			// A settlement we spoke of changed hands (taken through the Minutemen, say): told at once, so no friend's game
			// keeps a word that no longer holds.
			if (hostWords.Due(a_now)) {
				const auto changes = hostWords.Recheck(a_now, [](std::uint32_t a_workshop) -> std::optional<bool> {
					const auto bench = Workbench(a_workshop);
					return bench ? std::optional{ HostOwns(bench) } : std::nullopt;
				});
				for (const auto& change : changes) {
					outgoing.push_back(Protocol::Encode(change));
					++ownerChanges;
					REX::INFO("SettlementAttacks: settlement {:08X} ({}) is {}the host's now: told", change.workshop, PlaceName(change.workshop, 0), change.hostOwned ? "" : "no longer ");
				}
			}
		}

		void Answer(const Protocol::SettlementAttack& a_question)
		{
			const bool owned = HostOwns(Workbench(a_question.workshop));
			SendOwner(a_question.workshop, owned);
			++answered;
			REX::INFO("SettlementAttacks: a friend's game rolled an attack on {:08X} ({}): told it is {}the host's", a_question.workshop, PlaceName(a_question.workshop, a_question.location),
				owned ? "" : "not ");
		}

		// --- a friend's game ---

		void StartMirror(RE::TESQuest* a_quest, Mirror& a_mirror, Clock::time_point a_now)
		{
			auto location = a_mirror.location;
			if (location == 0) {
				const auto bench = Workbench(a_mirror.workshop);
				const auto at = bench ? bench->GetCurrentLocation() : nullptr;
				location = at ? at->GetFormID() : 0;
			}
			std::vector<std::pair<std::uint32_t, std::uint32_t>> aliases{ { AttackBook::WORKSHOP_ALIAS, a_mirror.workshop } };
			if (location != 0) {
				aliases.push_back({ AttackBook::LOCATION_ALIAS, location });
			}
			const auto reply = RadiantSync::StartWithEvent(a_quest, a_mirror.workshop, location, aliases, "a mirror of the host's settlement attack");
			// The quest's last run here still being shut down (our own roll of it, stopped a moment ago) isn't a try.
			const bool waits = reply.starts_with("wait") && a_mirror.startWaits < MAX_START_WAITS;
			if (waits) {
				++a_mirror.startWaits;
				a_mirror.startAfter = a_now + START_AGAIN;
			} else {
				a_mirror.startAsked = a_now;
				++a_mirror.startTries;
			}
			REX::INFO("SettlementAttacks: mirroring the host's attack {:08X} '{}' on {:08X} ({}), try {}{}: {}", a_quest->GetFormID(), EditorId(a_quest), a_mirror.workshop,
				PlaceName(a_mirror.workshop, a_mirror.location), a_mirror.startTries + (waits ? 1 : 0), waits ? " (asked again shortly)" : "", reply);
			if (reply.starts_with("ok")) {
				++mirrorStarts;
			}
		}

		// Whether the host's attackers (our copies of friends' NPCs) stand at the settlement here, lately.
		bool AttackersNear(std::uint32_t a_workshop, Clock::time_point& a_seen, Clock::time_point a_now)
		{
			if (Spawns::CopiesNear(Workbench(a_workshop), ATTACKERS_RADIUS) > 0) {
				a_seen = a_now;
			}
			return a_seen != Clock::time_point{} && a_now - a_seen < ATTACKERS_LINGER;
		}

		bool AttackersHere(Mirror& a_mirror, Clock::time_point a_now)
		{
			return AttackersNear(a_mirror.workshop, a_mirror.attackersSeen, a_now);
		}

		// Our mirror of the host's attack in a_quest (AttackBook::Next). False: it is done with (forget it).
		bool StepMirror(RE::TESQuest* a_quest, const AttackBook::Seen& a_seen, Mirror& a_mirror, Clock::time_point a_now)
		{
			const auto id = a_quest->GetFormID();
			const bool running = a_seen.running && a_seen.workshop == a_mirror.workshop;
			if (running && !a_mirror.running) {
				a_mirror.running = true;
				a_mirror.pushed = {};
				REX::INFO("SettlementAttacks: our mirror {:08X} '{}' runs at {:08X}", id, EditorId(a_quest), a_mirror.workshop);
			}
			if (!running && a_mirror.running) {
				a_mirror.running = false;
				a_mirror.objectiveShown = false;
				++mirrorsEnded;
				REX::INFO("SettlementAttacks: our mirror {:08X} '{}' at {:08X} has stopped{}", id, EditorId(a_quest), a_mirror.workshop,
					a_mirror.hostPhase == Phase::kOver ? "" : " before the host's run ended (not started again)");
				a_mirror.ended = a_mirror.hostPhase != Phase::kOver;
				return a_mirror.ended;  // kept while the host's run goes on, so it isn't started again
			}
			if (!running && a_mirror.hostPhase == Phase::kOver) {
				return false;
			}
			const bool gaveUp = a_mirror.startTries >= MAX_START_TRIES && a_now - a_mirror.startAsked >= START_WAIT;
			const bool tried = a_mirror.ended || gaveUp || a_now < a_mirror.startAfter || (a_mirror.startTries > 0 && a_now - a_mirror.startAsked < START_WAIT);
			if (!running && gaveUp && !a_mirror.gaveUpLogged) {
				a_mirror.gaveUpLogged = true;
				REX::WARN("SettlementAttacks: the host's attack {:08X} '{}' on {:08X} couldn't be mirrored here after {} tries; its notices still come", id, EditorId(a_quest), a_mirror.workshop,
					MAX_START_TRIES);
			}
			const bool loadedHere = running && LoadedHere(a_mirror.workshop);
			const AttackBook::MirrorFacts facts{
				.hostPhase = a_mirror.hostPhase,
				.outcome = a_mirror.outcome,
				.running = running,
				.elsewhere = a_seen.running && !running,
				.begun = a_seen.begun,
				.done = a_seen.done,
				.hasObjective = HasObjective(a_quest, AttackBook::OBJECTIVE),
				.objectiveShown = a_mirror.objectiveShown,
				.loadedHere = loadedHere,
				.hostAttackersHere = loadedHere && a_mirror.hostPhase == Phase::kUnderway && AttackersHere(a_mirror, a_now),
				.startTried = tried,
			};
			// Its own timer, pushed out while it waits for the host's (from its first look running: stage 10 has started it).
			if (running && !a_seen.begun && a_mirror.hostPhase != Phase::kOver && (a_mirror.pushed == Clock::time_point{} || a_now - a_mirror.pushed >= PUSH_AGAIN)) {
				if (PushTimer(a_quest)) {
					++timerPushes;
				}
				a_mirror.pushed = a_now;
			}
			// Its own timer beat the push and began it: its attackers are out, not the host's (the host's are shared), so
			// they are removed.
			if (running && a_seen.begun && !a_mirror.sweptOwn) {
				a_mirror.sweptOwn = true;
				REX::WARN("SettlementAttacks: our mirror {:08X} '{}' began on its own timer: its attackers are removed", id, EditorId(a_quest));
				Spawns::StopQuestRun(id, true);
			}
			switch (AttackBook::Next(facts)) {
			case AttackBook::MirrorStep::kNone:
				break;
			case AttackBook::MirrorStep::kStart:
				StartMirror(a_quest, a_mirror, a_now);
				break;
			case AttackBook::MirrorStep::kDefuse:
				if (a_now - a_mirror.close.defuseAsked >= STEP_AGAIN) {
					a_mirror.close.defuseAsked = a_now;
					REX::INFO("SettlementAttacks: our mirror {:08X} '{}': stage {} (defused as it runs: only the host's run rolls damage)", id, EditorId(a_quest), AttackBook::DONE_STAGE);
					Defuse(a_quest);
				}
				break;
			case AttackBook::MirrorStep::kShowObjective:
				// Stage 100, set as the mirror ran, completed it unseen: shown as the host's fight, open.
				a_mirror.objectiveShown = true;
				Console(std::format("setobjectivecompleted {:08X} {} 0", id, AttackBook::OBJECTIVE));
				Console(std::format("setobjectivedisplayed {:08X} {} 1", id, AttackBook::OBJECTIVE));
				REX::INFO("SettlementAttacks: our player is at {:08X} during the host's attack, the host's attackers there: our mirror's objective {} is shown", a_mirror.workshop,
					AttackBook::OBJECTIVE);
				break;
			case AttackBook::MirrorStep::kHideObjective:
				a_mirror.objectiveShown = false;
				Console(std::format("setobjectivedisplayed {:08X} {} 0", id, AttackBook::OBJECTIVE));
				REX::INFO("SettlementAttacks: our mirror's objective {} at {:08X} is hidden ({})", AttackBook::OBJECTIVE, a_mirror.workshop,
					a_mirror.hostPhase == Phase::kOver ? "the host's run ended undefended" : !loadedHere ? "our player left" : "the host's attackers aren't here");
				break;
			case AttackBook::MirrorStep::kCompleteObjective:
				a_mirror.objectiveShown = false;  // done with: it stays as completed
				Console(std::format("setobjectivecompleted {:08X} {} 1", id, AttackBook::OBJECTIVE));
				REX::INFO("SettlementAttacks: our mirror's objective {} at {:08X} is completed: the host's attackers are dead", AttackBook::OBJECTIVE, a_mirror.workshop);
				break;
			case AttackBook::MirrorStep::kStop:
				if (Close(a_quest, running, a_seen.done, a_mirror.close, a_now, true) == AttackBook::CloseStep::kStop) {
					REX::INFO("SettlementAttacks: our mirror {:08X} '{}' at {:08X} is stopped: the host's run ended", id, EditorId(a_quest), a_mirror.workshop);
				}
				break;
			}
			return true;
		}

		void StepOwn(RE::TESQuest* a_quest, const AttackBook::Seen& a_seen, Clock::time_point a_now)
		{
			auto& run = own[a_quest->GetFormID()];
			if (run.workshop != a_seen.workshop) {
				run = Own{ a_seen.workshop };
			}
			const AttackBook::Facts facts{
				.word = view.WordOn(a_seen.workshop, a_now),
				// The host's attack, or the host's "Defend X" job whose attack our game holds back (RadiantSync), is there.
				.hostAttackHere = view.LiveAt(a_seen.workshop) || RadiantSync::HeldAttackAt(a_seen.workshop),
				.begun = a_seen.begun,
				.done = a_seen.done,
				.loadedHere = LoadedHere(a_seen.workshop),
				.askedLately = run.asked != Clock::time_point{} && a_now - run.asked < AttackBook::ASK_AGAIN,
			};
			const auto place = [&] { return PlaceName(a_seen.workshop, a_seen.location); };
			const auto verdict = AttackBook::Judge(facts);
			run.hosts = run.hosts || verdict == AttackBook::Verdict::kDefuse || verdict == AttackBook::Verdict::kHold || verdict == AttackBook::Verdict::kStop;
			switch (verdict) {
			case AttackBook::Verdict::kKeep:
				if (!run.keepLogged && facts.word != AttackBook::Word::kUnknown) {
					run.keepLogged = true;
					REX::INFO("SettlementAttacks: our own attack {:08X} '{}' on {:08X} ({}) is ours: not the host's settlement", a_quest->GetFormID(), EditorId(a_quest), a_seen.workshop, place());
				}
				break;
			case AttackBook::Verdict::kAsk:
				run.asked = a_now;
				++asked;
				Send({ a_seen.workshop, a_quest->GetFormID(), a_seen.location, Phase::kRolled, Outcome::kNone });
				REX::INFO("SettlementAttacks: our own attack {:08X} '{}' on {:08X} ({}): asking whose settlement it is{}", a_quest->GetFormID(), EditorId(a_quest), a_seen.workshop, place(),
					facts.word == AttackBook::Word::kNotHost ? " (again: the host may have taken it since)" : "");
				break;
			case AttackBook::Verdict::kDefuse:
				if (a_now - run.close.defuseAsked >= STEP_AGAIN) {
					run.close.defuseAsked = a_now;
					++ownDefused;
					REX::INFO("SettlementAttacks: our own attack {:08X} '{}' on {:08X} ({}) is at the host's settlement{}: stage {}, so it rolls no damage here", a_quest->GetFormID(),
						EditorId(a_quest), a_seen.workshop, place(), facts.hostAttackHere ? " (their attack is there)" : "", AttackBook::DONE_STAGE);
					Defuse(a_quest);
				}
				break;
			case AttackBook::Verdict::kHold:
				if (!run.holdLogged) {
					run.holdLogged = true;
					REX::INFO("SettlementAttacks: our own attack {:08X} '{}' on {:08X} is defused, its fight on around our player: stopped once they leave", a_quest->GetFormID(), EditorId(a_quest),
						a_seen.workshop);
				}
				break;
			case AttackBook::Verdict::kStop:
				if (Close(a_quest, true, a_seen.done, run.close, a_now, true) == AttackBook::CloseStep::kStop) {
					++ownStopped;
					REX::INFO("SettlementAttacks: our own attack {:08X} '{}' on {:08X} ({}) is stopped, its attackers removed: the host's settlement", a_quest->GetFormID(), EditorId(a_quest),
						a_seen.workshop, place());
				}
				break;
			}
		}

		// --- a siege (AttackBook::SIEGES) ---

		const SiegeQuest* SiegeOf(std::uint32_t a_quest)
		{
			const auto it = std::ranges::find_if(siegeQuests, [&](const SiegeQuest& a_siege) { return a_siege.quest->GetFormID() == a_quest; });
			return it != siegeQuests.end() ? &*it : nullptr;
		}

		// An objective's state here (QUEST_OBJECTIVE_STATE: 1 shown, 2 completed), 0 if it has none.
		std::int32_t ObjectiveState(const RE::TESQuest* a_quest, std::uint16_t a_index)
		{
			for (const auto objective : a_quest->objectives) {
				if (objective && objective->index == a_index) {
					return static_cast<std::int32_t>(objective->state);
				}
			}
			return 0;
		}

		// Starts the siege's fight timer again (the script's own function), for a run whose player has arrived: its fight
		// then comes as the game would have it.
		bool StartSiegeTimer(const SiegeQuest& a_siege)
		{
			const auto self = Papyrus::BoundObject(a_siege.quest, a_siege.siege->script, false);
			return self && Papyrus::CallOn(self, a_siege.siege->timerFunction, [](RE::BSScrapArray<RE::BSScript::Variable>& a_out) { a_out = RE::BSScrapArray<RE::BSScript::Variable>{}; });
		}

		// Our held fight is ours again: our mark on its startStage cleared, its timer started again where our player has
		// arrived (the hold may have let its end go by: the timer sets startStage only if it isn't done).
		void ReleaseSiege(const SiegeQuest& a_siege, bool a_objectiveShown, std::string_view a_why)
		{
			const auto  quest = a_siege.quest;
			const auto& siege = *a_siege.siege;
			if (a_objectiveShown) {
				Console(std::format("setobjectivedisplayed {:08X} {} 0", quest->GetFormID(), siege.objective));
			}
			if (QuestStageLayout::IsStageDone(quest, siege.doneStage) || !QuestStageLayout::ClearDone(quest, siege.startStage)) {
				return;
			}
			++siegesReleased;
			const bool arrived = Running(quest) && QuestStageLayout::IsStageDone(quest, siege.arrivedStage);
			const bool timer = arrived && StartSiegeTimer(a_siege);
			if (timer) {
				++siegeTimers;
			}
			REX::INFO("SettlementAttacks: our siege {:08X} '{}' is ours again ({}): stage {} cleared{}", quest->GetFormID(), EditorId(quest), a_why, siege.startStage,
				timer ? ", its timer started again" : arrived ? "; its timer couldn't be started (no script bound)" : " (its player hasn't arrived: its timer starts as they do)");
		}

		// Our own run of a siege quest while the host's game decides it (AttackBook::NextSiege).
		void SiegeLook(const SiegeQuest& a_siege, Clock::time_point a_now)
		{
			const auto  quest = a_siege.quest;
			const auto& siege = *a_siege.siege;
			const auto  id = quest->GetFormID();
			const auto  seen = ReadSiege(a_siege);
			const auto  live = view.LiveOf(id);
			auto&       run = sieges[id];
			const auto word = live || seen.workshop != 0 ? view.WordOn(live ? live->workshop : seen.workshop, a_now) : AttackBook::Word::kUnknown;
			run.hostSaid = (run.hostSaid || live || word == AttackBook::Word::kHost) && word != AttackBook::Word::kNotHost;
			if (live) {
				if (run.hostOver && live->phase < Phase::kDefended) {
					run.hostOver = false;  // a new run of the host's
					run.hostDefended = false;
				}
				run.hostDefended = run.hostDefended || live->phase >= Phase::kDefended;
			}
			if (!seen.running) {
				run.held = false;
				run.objectiveShown = false;
				run.ownLogged = false;
				if (run.hostOver || !live) {
					run.hostDefended = false;  // a run of ours that starts later is a new one
				}
				return;
			}
			const auto workshop = seen.workshop != 0 ? seen.workshop : live ? live->workshop : 0;
			const bool loadedHere = workshop != 0 && LoadedHere(workshop);
			const AttackBook::SiegeFacts facts{
				.running = true,
				.begun = seen.begun,
				.held = run.held,
				.done = seen.done,
				.hostRun = live != nullptr,
				.hostPhase = live ? live->phase : Phase::kOver,
				.hostDefended = run.hostDefended,
				// The host's word expires in minutes and a live run after hours (View), while a siege waits for its fight
				// far longer: once said in this session it holds until the host's game says otherwise or the session ends.
				.hostOwns = workshop != 0 && run.hostSaid,
				.objectiveShown = run.objectiveShown,
				.loadedHere = loadedHere,
				.hostAttackersHere = loadedHere && live && live->phase == Phase::kUnderway && AttackersNear(workshop, run.attackersSeen, a_now),
			};
			if (seen.begun && !run.held && !seen.done && (facts.hostRun || facts.hostOwns) && !run.ownLogged) {
				run.ownLogged = true;
				REX::WARN("SettlementAttacks: our own siege {:08X} '{}' began before the host's word: its fight is left to run (never stopped mid-fight)", id, EditorId(quest));
			}
			switch (AttackBook::NextSiege(facts)) {
			case AttackBook::SiegeStep::kNone:
				break;
			case AttackBook::SiegeStep::kHold:
				if (QuestStageLayout::MarkDone(quest, siege.startStage)) {
					run.held = true;
					++siegesHeld;
					REX::INFO("SettlementAttacks: our siege {:08X} '{}' at {:08X} ({}) is held: the host's game decides it (stage {} marked done, its fight never begins here)", id, EditorId(quest),
						workshop, PlaceName(workshop, seen.location), siege.startStage);
				} else if (!run.ownLogged) {
					run.ownLogged = true;
					REX::WARN("SettlementAttacks: our siege {:08X} '{}' couldn't be held (no stage {})", id, EditorId(quest), siege.startStage);
				}
				break;
			case AttackBook::SiegeStep::kShowObjective:
				run.objectiveShown = true;
				Console(std::format("setobjectivedisplayed {:08X} {} 1", id, siege.objective));
				REX::INFO("SettlementAttacks: our player is at {:08X} during the host's siege, its attackers there: our objective {} is shown", workshop, siege.objective);
				break;
			case AttackBook::SiegeStep::kHideObjective:
				run.objectiveShown = false;
				Console(std::format("setobjectivedisplayed {:08X} {} 0", id, siege.objective));
				REX::INFO("SettlementAttacks: our siege's objective {} at {:08X} is hidden ({})", siege.objective, workshop, !loadedHere ? "our player left" : "the host's attackers aren't here");
				break;
			case AttackBook::SiegeStep::kEnd:
				if (a_now - run.endAsked >= STEP_AGAIN) {
					run.endAsked = a_now;
					// What the startStage our hold skipped does to the objectives before it (completes the "get ready" one,
					// hides the "go there" one), then the end stage, with its own script: our end, our scenes, our reward.
					if (const auto ready = ObjectiveState(quest, siege.beforeObjective); (ready & 1) != 0 && (ready & 2) == 0) {
						Console(std::format("setobjectivecompleted {:08X} {} 1", id, siege.beforeObjective));
					}
					if (const auto go = ObjectiveState(quest, siege.goObjective); (go & 1) != 0 && (go & 2) == 0) {
						Console(std::format("setobjectivedisplayed {:08X} {} 0", id, siege.goObjective));
					}
					if (!run.objectiveShown) {
						Console(std::format("setobjectivedisplayed {:08X} {} 1", id, siege.objective));
					}
					run.objectiveShown = false;
					Console(std::format("setstage {:08X} {}", id, siege.doneStage));
					++siegesEnded;
					REX::INFO("SettlementAttacks: the host's siege at {:08X} ({}) was won: our {:08X} '{}' sets stage {} (its end, once)", workshop, PlaceName(workshop, seen.location), id, EditorId(quest),
						siege.doneStage);
				}
				break;
			case AttackBook::SiegeStep::kRelease:
				ReleaseSiege(a_siege, false, "the host's run ended and the settlement isn't the host's");
				run.held = false;
				break;
			}
		}

		void FriendLook(Clock::time_point a_now)
		{
			for (const auto quest : view.Expire(a_now)) {
				REX::WARN("SettlementAttacks: the host's attack {:08X} was never told over in {} h: forgotten", quest, std::chrono::duration_cast<std::chrono::hours>(AttackBook::View::LIVE_FOR).count());
			}
			for (const auto quest : quests) {
				const auto id = quest->GetFormID();
				const auto seen = Read(quest);
				const auto live = view.LiveOf(id);
				auto       it = mirrors.find(id);
				if (live) {
					if (it == mirrors.end()) {
						it = mirrors.emplace(id, Mirror{ live->workshop, live->location }).first;
					}
					if (it->second.workshop != live->workshop) {
						it->second = Mirror{ live->workshop, live->location };  // the host's quest moved: a new run
					}
					it->second.hostPhase = live->phase;
					if (live->location != 0) {
						it->second.location = live->location;
					}
				} else if (it != mirrors.end() && it->second.hostPhase != Phase::kOver) {
					it->second.hostPhase = Phase::kOver;  // told over, the host left, or forgotten
				}
				if (closing.contains(id)) {
					continue;  // being closed down (CloseOut): then looked at again
				}
				const bool mirrored = it != mirrors.end() && seen.running && seen.workshop == it->second.workshop;
				if (seen.running && seen.workshop != 0 && !mirrored) {
					StepOwn(quest, seen, a_now);
				} else {
					own.erase(id);
				}
				if (it != mirrors.end() && !StepMirror(quest, seen, it->second, a_now)) {
					mirrors.erase(it);
				}
			}
			for (const auto& siege : siegeQuests) {
				SiegeLook(siege, a_now);
			}
		}

		// --- in a session or not ---

		// After a load: every run a session left defused (AttackBook::Leftover) is closed down, but a mirror of the host's
		// attack live now (the save was made in this session).
		void Sweep()
		{
			sweepPending = false;
			// A siege waiting for its fight (its player arrived, the fight not begun): its timer started again, as a session's
			// hold may have let it go by in the save (the timer sets the fight's stage only if it isn't done). At most a
			// timer's length (2 game hours) later than the game's own.
			for (const auto& siege : siegeQuests) {
				const auto quest = siege.quest;
				if (Running(quest) && QuestStageLayout::IsStageDone(quest, siege.siege->arrivedStage) && !QuestStageLayout::IsStageDone(quest, siege.siege->startStage) &&
					!QuestStageLayout::IsStageDone(quest, siege.siege->doneStage) && StartSiegeTimer(siege)) {
					++siegeTimers;
					REX::INFO("SettlementAttacks: the loaded game's siege {:08X} '{}' waits for its fight: its timer is started again", quest->GetFormID(), EditorId(quest));
				}
			}
			for (const auto quest : quests) {
				const auto seen = Read(quest);
				if (!AttackBook::Leftover(seen)) {
					continue;
				}
				const auto mirror = mirrors.find(seen.quest);
				if (mirror != mirrors.end() && mirror->second.workshop == seen.workshop && mirror->second.hostPhase != Phase::kOver) {
					continue;
				}
				++leftovers;
				closing.insert_or_assign(seen.quest, Closing{ .workshop = seen.workshop, .why = "a session left it defused (stage 100 done, 20 not)" });
				REX::INFO("SettlementAttacks: the loaded game holds {:08X} '{}' at {:08X} defused by a session (stage {} done, {} not): it is stopped, its timer cancelled first", seen.quest,
					EditorId(quest), seen.workshop, AttackBook::DONE_STAGE, AttackBook::START_STAGE);
			}
		}

		// The runs being closed down, a step each.
		void CloseOut(Clock::time_point a_now)
		{
			std::erase_if(closing, [&](auto& a_entry) {
				auto&      entry = a_entry.second;
				const auto quest = RE::TESForm::GetFormByID<RE::TESQuest>(a_entry.first);
				if (!quest) {
					return true;
				}
				const auto seen = Read(quest);
				const bool pending = entry.anyUntil != Clock::time_point{} && a_now < entry.anyUntil;
				const bool ours = seen.running && (seen.workshop == entry.workshop || pending);
				if (!ours) {
					if (entry.seenRunning && !seen.running) {
						++closedDown;
						REX::INFO("SettlementAttacks: {:08X} '{}' at {:08X} is closed down ({})", a_entry.first, EditorId(quest), entry.workshop, entry.why);
					}
					return !pending;  // a start of ours that may still land is waited for
				}
				entry.seenRunning = true;
				if (entry.objectiveShown) {
					// Shown and not completed, stage 1000 would say the attack was won: hidden first.
					entry.objectiveShown = false;
					Console(std::format("setobjectivedisplayed {:08X} {} 0", a_entry.first, AttackBook::OBJECTIVE));
				}
				switch (Close(quest, true, seen.done, entry.close, a_now, false)) {
				case AttackBook::CloseStep::kDefuse:
					REX::INFO("SettlementAttacks: {:08X} '{}' at {:08X}: stage {} ({})", a_entry.first, EditorId(quest), seen.workshop, AttackBook::DONE_STAGE, entry.why);
					break;
				case AttackBook::CloseStep::kStop:
					REX::INFO("SettlementAttacks: {:08X} '{}' at {:08X} is stopped, defused ({})", a_entry.first, EditorId(quest), seen.workshop, entry.why);
					break;
				default:
					break;
				}
				return false;
			});
		}

		void Notice(const Protocol::SettlementAttack& a_attack)
		{
			auto&      told = toldUnderway[a_attack.quest];
			const auto key = AttackBook::NoticeFor(a_attack.phase, a_attack.outcome, told);
			if (a_attack.phase == Phase::kUnderway) {
				if (told) {
					return;
				}
				told = true;
			}
			if (a_attack.phase == Phase::kOver) {
				toldUnderway.erase(a_attack.quest);
			}
			if (!key.empty()) {
				Hud::Notify(Text::Format(key, { { "place", PlaceName(a_attack.workshop, a_attack.location) } }));
			}
		}

		// The sieges a session's end let go, once the world is there.
		void ReleaseAll()
		{
			for (const auto id : std::exchange(releasing, {})) {
				if (const auto siege = SiegeOf(id)) {
					ReleaseSiege(*siege, false, "the session ended");
				}
			}
		}

		bool InWorldNow()
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			return player && player->GetParentCell() && WorldSync::InWorld();
		}
	}

	void OnWelcome(bool a_hosting)
	{
		hosting = a_hosting;
		lastLook = {};
	}

	void Frame(bool a_inSession)
	{
		const auto now = Clock::now();
		if (now - lastLook < LOOK_INTERVAL || Story::InOpening() || !WorldSync::InWorld() || !Hud::IsSettled() || Hud::LoadingScreenUp()) {
			return;
		}
		if (!a_inSession && !sweepPending && closing.empty() && releasing.empty()) {
			return;
		}
		lastLook = now;
		Resolve();
		if (sweepPending) {
			Sweep();
		}
		if (!closing.empty()) {
			CloseOut(now);
		}
		if (!releasing.empty()) {
			ReleaseAll();
		}
		if (!a_inSession) {
			return;
		}
		if (hosting) {
			HostLook(now);
		} else {
			FriendLook(now);
		}
	}

	void OnAttack(const Protocol::SettlementAttack& a_attack)
	{
		if (hosting) {
			if (a_attack.phase == Phase::kRolled) {
				Answer(a_attack);  // a friend's question
			}
			return;
		}
		if (a_attack.quest == 0) {
			return;  // the host's roll of an add-on's attack this game lacks: nothing here to mirror (its word on the settlement comes on its own)
		}
		const auto before = view.OnAttack(a_attack, Clock::now());
		if (!before || *before != a_attack.phase) {
			const auto quest = RE::TESForm::GetFormByID<RE::TESQuest>(a_attack.quest);
			REX::INFO("SettlementAttacks: the host's attack {:08X} '{}' on {:08X} ({}): phase {}{}", a_attack.quest, EditorId(quest), a_attack.workshop, PlaceName(a_attack.workshop, a_attack.location),
				std::to_underlying(a_attack.phase), a_attack.phase == Phase::kOver ? std::format(", outcome {}", std::to_underlying(a_attack.outcome)) : std::string{});
			Notice(a_attack);
		}
		if (a_attack.phase == Phase::kOver) {
			if (const auto it = mirrors.find(a_attack.quest); it != mirrors.end()) {
				it->second.hostPhase = Phase::kOver;
				it->second.outcome = a_attack.outcome;
			}
			if (SiegeOf(a_attack.quest)) {
				auto& run = sieges[a_attack.quest];
				run.hostOver = true;
				run.hostDefended = run.hostDefended || a_attack.outcome == Outcome::kDefended;
			}
		} else if (a_attack.phase == Phase::kDefended && SiegeOf(a_attack.quest)) {
			sieges[a_attack.quest].hostDefended = true;
		}
		lastLook = {};  // looked at again next frame
	}

	void OnOwner(const Protocol::SettlementOwner& a_owner)
	{
		if (hosting) {
			return;
		}
		view.OnOwner(a_owner, Clock::now());
		if (a_owner.workshop == 0) {
			REX::INFO("SettlementAttacks: the host's game left: nothing it said of settlements stands; our mirrors end, our held sieges are let go");
			toldUnderway.clear();
			for (auto& [id, run] : sieges) {
				run.hostSaid = false;
			}
		} else {
			REX::INFO("SettlementAttacks: settlement {:08X} is {}the host's", a_owner.workshop, a_owner.hostOwned ? "" : "not ");
		}
		lastLook = {};
	}

	std::vector<std::vector<std::uint8_t>> TakeOutgoing()
	{
		return std::exchange(outgoing, {});
	}

	void BeforeSave()
	{
		// A held siege's mark never goes into a save: one loaded without a session (or without this plugin) has its fight
		// to come. Held again at the next look.
		for (auto& [id, run] : sieges) {
			const auto siege = run.held ? SiegeOf(id) : nullptr;
			if (!siege) {
				continue;
			}
			run.held = false;
			if (run.objectiveShown) {
				run.objectiveShown = false;
				Console(std::format("setobjectivedisplayed {:08X} {} 0", id, siege->siege->objective));
			}
			if (!QuestStageLayout::IsStageDone(siege->quest, siege->siege->doneStage) && QuestStageLayout::ClearDone(siege->quest, siege->siege->startStage)) {
				REX::INFO("SettlementAttacks: before the save: our held siege {:08X} '{}' has its stage {} cleared (held again after)", id, EditorId(siege->quest), siege->siege->startStage);
			}
		}
		if (hosting || (mirrors.empty() && own.empty() && closing.empty())) {
			return;  // the host's runs are the session's real attacks; with nothing of a session's, nothing to defuse
		}
		Resolve();
		const auto now = Clock::now();
		for (const auto quest : quests) {
			const auto seen = Read(quest);
			if (!seen.running || seen.done || seen.workshop == 0) {
				continue;
			}
			const auto mirror = mirrors.find(seen.quest);
			const auto run = own.find(seen.quest);
			const auto closed = closing.find(seen.quest);
			const bool hostDecides = (mirror != mirrors.end() && mirror->second.workshop == seen.workshop) || (run != own.end() && run->second.hosts) ||
			                         (closed != closing.end() && (closed->second.workshop == seen.workshop || now < closed->second.anyUntil)) || view.LiveAt(seen.workshop) ||
			                         view.WordOn(seen.workshop, now) == AttackBook::Word::kHost;
			if (!hostDecides) {
				continue;
			}
			Defuse(quest);
			++savedDefused;
			if (QuestStageLayout::IsStageDone(quest, AttackBook::DONE_STAGE)) {
				REX::INFO("SettlementAttacks: before the save: {:08X} '{}' at {:08X}, the host's to decide, is defused (stage {})", seen.quest, EditorId(quest), seen.workshop, AttackBook::DONE_STAGE);
			} else {
				REX::WARN("SettlementAttacks: before the save: {:08X} '{}' at {:08X}, the host's to decide, didn't show stage {} done: the save may hold it undefused (a load doesn't stop it: only a defused run is swept)",
					seen.quest, EditorId(quest), seen.workshop, AttackBook::DONE_STAGE);
			}
		}
	}

	void OnGameLoaded()
	{
		// Our runs are the loaded save's: read afresh. The host's game reports any it still runs as over (its quest stopped
		// in the save left behind) and those of the loaded save as new; a friend's mirrors are looked for again. What was
		// being closed down was the world before the load; what the loaded one holds of a session is swept (Sweep).
		own.clear();
		closing.clear();
		releasing.clear();  // the save has no mark of ours (BeforeSave)
		for (auto& [id, run] : sieges) {
			run.held = false;
			run.objectiveShown = false;
			run.ownLogged = false;
			run.attackersSeen = {};
			run.endAsked = {};
		}
		sweepPending = true;
		for (auto& [id, mirror] : mirrors) {
			mirror.running = false;
			mirror.ended = false;
			mirror.gaveUpLogged = false;
			mirror.startTries = 0;
			mirror.objectiveShown = false;
			mirror.sweptOwn = false;
			mirror.pushed = {};
			mirror.attackersSeen = {};
			mirror.close = {};
		}
		lastLook = {};
	}

	void Reset()
	{
		// Every mirror, and every roll of our own judged the host's, is closed down: defused, its timer cancelled, stopped,
		// here and now if the world is there, or once it is back (CloseOut, from any frame). A mirror started and not
		// yet seen running: any run of its quest that begins shortly is closed down too.
		const auto now = Clock::now();
		for (const auto& [id, mirror] : mirrors) {
			Closing entry{ .workshop = mirror.workshop, .objectiveShown = mirror.objectiveShown, .why = "the session ended: a mirror of the host's attack" };
			const bool startWaiting = !mirror.running && mirror.startTries > 0 && now - mirror.startAsked < START_WAIT;
			if (startWaiting) {
				entry.anyUntil = now + CLOSE_PENDING;
			}
			closing.insert_or_assign(id, entry);
		}
		for (const auto& [id, run] : own) {
			if (run.hosts) {
				closing.insert_or_assign(id, Closing{ .workshop = run.workshop, .why = "the session ended: our own roll at the host's settlement" });
			}
		}
		// Every held siege's fight is ours again: no player is left waiting on a host who left.
		for (const auto& [id, run] : sieges) {
			const auto siege = SiegeOf(id);
			if (!run.held || !siege) {
				continue;
			}
			if (run.objectiveShown && InWorldNow()) {
				Console(std::format("setobjectivedisplayed {:08X} {} 0", id, siege->siege->objective));
			}
			releasing.push_back(id);
		}
		sieges.clear();
		if (!releasing.empty() && InWorldNow()) {
			ReleaseAll();
		}
		if (!closing.empty()) {
			REX::INFO("SettlementAttacks: the session ended: {} run(s) of the host's to close down{}", closing.size(), InWorldNow() ? "" : ", once the world is back");
			if (InWorldNow()) {
				Resolve();
				CloseOut(now);  // stage 100 at once where it isn't done; the timers and stops follow in the next frames
			}
		}
		hosting = false;
		outgoing.clear();
		hostRuns.Reset();
		hostWords.Reset();
		view.Reset();
		mirrors.clear();
		own.clear();
		toldUnderway.clear();
		lastLook = {};
	}

	std::string Describe()
	{
		return std::format(
			"attacks: hosting={} live={} mirrors={} reported={} answered={} ownerChanges={} asked={} mirrorStarts={} mirrorsEnded={} ownDefused={} ownStopped={} timerPushes={} closing={} "
			"closedDown={} leftovers={} savedDefused={} siegesHeld={} siegesEnded={} siegesReleased={} siegeTimers={}",
			hosting, hosting ? hostRuns.Size() : view.LiveCount(), mirrors.size(), reported, answered, ownerChanges, asked, mirrorStarts, mirrorsEnded, ownDefused, ownStopped, timerPushes,
			closing.size(), closedDown, leftovers, savedDefused, siegesHeld, siegesEnded, siegesReleased, siegeTimers);
	}

	std::string List()
	{
		Resolve();
		std::string out;
		const auto  now = Clock::now();
		for (const auto quest : quests) {
			const auto seen = Read(quest);
			if (!seen.running) {
				continue;
			}
			const auto mirror = mirrors.find(seen.quest);
			const bool mirrored = mirror != mirrors.end() && mirror->second.workshop == seen.workshop;
			out += std::format("{:08X} '{}' workshop={:08X} location={:08X} begun={} done={} loaded={} {} ", seen.quest, EditorId(quest), seen.workshop, seen.location, seen.begun, seen.done,
				LoadedHere(seen.workshop), mirrored ? std::format("mirror(host phase {})", std::to_underlying(mirror->second.hostPhase)) : closing.contains(seen.quest) ? std::string{ "closing" } : std::string{ "own" });
			if (!hosting && !mirrored) {
				const auto word = view.WordOn(seen.workshop, now);
				out += std::format("word={} ", word == AttackBook::Word::kHost ? "host" : word == AttackBook::Word::kNotHost ? "notHost" : "unknown");
			}
			out += "; ";
		}
		for (const auto& siege : siegeQuests) {
			const auto seen = ReadSiege(siege);
			if (!seen.running) {
				continue;
			}
			const auto run = sieges.find(seen.quest);
			out += std::format("{:08X} '{}' siege workshop={:08X} arrived={} begun={} done={} held={} hostDefended={}; ", seen.quest, EditorId(siege.quest), seen.workshop,
				QuestStageLayout::IsStageDone(siege.quest, siege.siege->arrivedStage), seen.begun, seen.done, run != sieges.end() && run->second.held,
				run != sieges.end() && run->second.hostDefended);
		}
		if (out.empty()) {
			out = "no attack running here; ";
		}
		if (!missingFiles.empty()) {
			out += "add-ons not installed here (their attacks aren't looked for):";
			for (const auto file : missingFiles) {
				out += std::format(" {}", file);
			}
			out += "; ";
		}
		for (const auto& live : view.AllLive()) {
			out += std::format("host's {:08X} at {:08X} phase {}; ", live.quest, live.workshop, std::to_underlying(live.phase));
		}
		return out + Describe();
	}
}
