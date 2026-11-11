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
		// A mirror's start that doesn't run in this long is given up on (its quest runs elsewhere here, or won't fill).
		constexpr auto START_WAIT = 15s;
		constexpr std::uint32_t MAX_START_TRIES = 3;
		// A stage set or a stop asked is asked again after this, if the game hasn't done it.
		constexpr auto STEP_AGAIN = 5s;
		// The mirror's own WorkshopAttackScript timer (stage 10's StartTimerGameTime(24), timer 0, whose end begins the
		// attack) is pushed this far out, again every PUSH_AGAIN: long past the host's run (24 game hours, a night attack's
		// wait, then at most 15 before its cleanup), and still running out one day if a save keeps a mirror past its
		// session, so that quest is never stuck in that save (it plays out as the save's own attack then).
		constexpr float TIMER_HOURS = 48.0f;
		constexpr auto  PUSH_AGAIN = 10min;
		constexpr std::string_view ATTACK_SCRIPT = "WorkshopAttackScript";

		bool                                   hosting = false;
		std::vector<std::vector<std::uint8_t>> outgoing;
		Clock::time_point                      lastLook{};

		// AttackBook::ATTACKS in this game (a quest not found is skipped).
		std::vector<RE::TESQuest*> quests;
		bool                       resolved = false;

		// The host's game.
		AttackBook::HostRuns hostRuns;

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
			bool              running = false;  // seen running at the host's settlement
			bool              ended = false;    // it ran and stopped while the host's run went on (its own cleanup): not started again
			bool              gaveUpLogged = false;
			bool              objectiveShown = false;
			bool              sweptOwn = false;  // its own attackers removed (its own timer began it)
			Clock::time_point pushed{};          // its timer pushed out last
			Clock::time_point stepAsked{};       // a stage or stop asked last
		};
		std::unordered_map<std::uint32_t, Mirror> mirrors;  // by quest
		struct Own
		{
			std::uint32_t     workshop = 0;
			Clock::time_point asked{};
			Clock::time_point stepAsked{};
			bool              holdLogged = false;
			bool              keepLogged = false;
		};
		std::unordered_map<std::uint32_t, Own>  own;          // by quest: our own runs judged
		std::unordered_map<std::uint32_t, bool> toldUnderway;  // by quest: the host's attacks whose start we noticed

		// Counts for `net status`.
		std::uint32_t reported = 0;       // our phases sent (host)
		std::uint32_t answered = 0;       // questions answered (host)
		std::uint32_t asked = 0;          // questions sent (friend)
		std::uint32_t mirrorStarts = 0;   // mirrors started (friend)
		std::uint32_t mirrorsEnded = 0;   // mirrors stopped (friend)
		std::uint32_t ownDefused = 0;     // own runs at the host's settlements defused (friend)
		std::uint32_t ownStopped = 0;     // and stopped
		std::uint32_t timerPushes = 0;    // mirror timers pushed out

		void Resolve()
		{
			if (resolved) {
				return;
			}
			const auto handler = RE::TESDataHandler::GetSingleton();
			if (!handler) {
				return;
			}
			for (const auto& attack : AttackBook::ATTACKS) {
				if (const auto quest = handler->LookupForm<RE::TESQuest>(attack.quest, "Fallout4.esm")) {
					quests.push_back(quest);
				} else {
					REX::WARN("SettlementAttacks: {} ({:06X}) is not in Fallout4.esm here; its attacks are each game's own", attack.editorId, attack.quest);
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
		// player's NPC the workbench's owner; PlayerFaction counts too).
		bool OwnBench(RE::TESObjectREFR* a_bench)
		{
			const auto owner = a_bench ? a_bench->GetOwner() : nullptr;
			return owner && (owner->GetFormID() == 0x00000007 || owner->GetFormID() == 0x0001C21C);
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
		// (CheckResolveAttack), wherever our player is.
		void Defuse(const RE::TESQuest* a_quest)
		{
			Console(std::format("setstage {:08X} {}", a_quest->GetFormID(), AttackBook::DONE_STAGE));
		}

		// Pushes the quest's WorkshopAttackScript timer out (StartTimerGameTime again under its id 0 replaces it).
		bool PushTimer(RE::TESQuest* a_quest)
		{
			const auto self = Papyrus::BoundObject(a_quest, ATTACK_SCRIPT, false);
			if (!self) {
				return false;
			}
			return Papyrus::CallOn(self, "StartTimerGameTime", [](RE::BSScrapArray<RE::BSScript::Variable>& a_out) {
				a_out = RE::BSScrapArray<RE::BSScript::Variable>{ 2 };
				a_out[0] = TIMER_HOURS;
				a_out[1] = static_cast<std::int32_t>(0);
			});
		}

		void Send(const Protocol::SettlementAttack& a_attack)
		{
			outgoing.push_back(Protocol::Encode(a_attack));
		}

		// --- the host's game ---

		bool Shares()
		{
			return Config::Get().sharedAttacks;
		}

		void HostLook()
		{
			if (!Shares()) {
				return;
			}
			std::vector<AttackBook::Seen> seen;
			seen.reserve(quests.size());
			for (const auto quest : quests) {
				seen.push_back(Read(quest));
			}
			for (const auto& report : hostRuns.Look(seen)) {
				const auto quest = RE::TESForm::GetFormByID<RE::TESQuest>(report.quest);
				REX::INFO("SettlementAttacks: our attack {:08X} '{}' on {:08X} ({}): phase {}{}", report.quest, EditorId(quest), report.workshop, PlaceName(report.workshop, report.location),
					std::to_underlying(report.phase), report.phase == Phase::kOver ? std::format(", outcome {}", std::to_underlying(report.outcome)) : std::string{});
				if (report.phase == Phase::kRolled) {
					// Our word on the settlement first: a friend's roll there gives way at once.
					outgoing.push_back(Protocol::Encode(Protocol::SettlementOwner{ report.workshop, OwnBench(Workbench(report.workshop)) }));
				}
				Send(report);
				++reported;
			}
		}

		void Answer(const Protocol::SettlementAttack& a_question)
		{
			const auto bench = Workbench(a_question.workshop);
			const bool owned = Shares() && bench && WorkshopSync::IsWorkbenchAnchor(bench) && OwnBench(bench);
			outgoing.push_back(Protocol::Encode(Protocol::SettlementOwner{ a_question.workshop, owned }));
			++answered;
			REX::INFO("SettlementAttacks: a friend's game rolled an attack on {:08X} ({}): told it is {}the host's", a_question.workshop, PlaceName(a_question.workshop, a_question.location),
				owned ? "" : "not ");
		}

		// --- a friend's game ---

		void StartMirror(RE::TESQuest* a_quest, Mirror& a_mirror, Clock::time_point a_now)
		{
			a_mirror.startAsked = a_now;
			++a_mirror.startTries;
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
			REX::INFO("SettlementAttacks: mirroring the host's attack {:08X} '{}' on {:08X} ({}), try {}: {}", a_quest->GetFormID(), EditorId(a_quest), a_mirror.workshop,
				PlaceName(a_mirror.workshop, a_mirror.location), a_mirror.startTries, reply);
			if (reply.starts_with("ok") || reply.starts_with("pending")) {
				++mirrorStarts;
			}
		}

		// Our mirror of the host's attack in a_quest (AttackBook::Next). False: it is done with (forget it).
		bool StepMirror(RE::TESQuest* a_quest, const AttackBook::Seen& a_seen, Mirror& a_mirror, Clock::time_point a_now)
		{
			const bool running = a_seen.running && a_seen.workshop == a_mirror.workshop;
			if (running && !a_mirror.running) {
				a_mirror.running = true;
				a_mirror.pushed = {};
				REX::INFO("SettlementAttacks: our mirror {:08X} '{}' runs at {:08X}", a_quest->GetFormID(), EditorId(a_quest), a_mirror.workshop);
			}
			if (!running && a_mirror.running) {
				a_mirror.running = false;
				++mirrorsEnded;
				REX::INFO("SettlementAttacks: our mirror {:08X} '{}' at {:08X} has stopped{}", a_quest->GetFormID(), EditorId(a_quest), a_mirror.workshop,
					a_mirror.hostPhase == Phase::kOver ? "" : " before the host's run ended (not started again)");
				a_mirror.ended = a_mirror.hostPhase != Phase::kOver;
				return a_mirror.ended;  // kept while the host's run goes on, so it isn't started again
			}
			if (!running && a_mirror.hostPhase == Phase::kOver) {
				return false;
			}
			const bool gaveUp = a_mirror.startTries >= MAX_START_TRIES && a_now - a_mirror.startAsked >= START_WAIT;
			const bool tried = a_mirror.ended || gaveUp || (a_mirror.startTries > 0 && a_now - a_mirror.startAsked < START_WAIT);
			if (!running && gaveUp && !a_mirror.gaveUpLogged) {
				a_mirror.gaveUpLogged = true;
				REX::WARN("SettlementAttacks: the host's attack {:08X} '{}' on {:08X} couldn't be mirrored here after {} tries; its notices still come", a_quest->GetFormID(), EditorId(a_quest),
					a_mirror.workshop, MAX_START_TRIES);
			}
			const AttackBook::MirrorFacts facts{
				.hostPhase = a_mirror.hostPhase,
				.running = running,
				.elsewhere = a_seen.running && !running,
				.begun = a_seen.begun,
				.done = a_seen.done,
				.objectiveShown = a_mirror.objectiveShown,
				.loadedHere = running && LoadedHere(a_mirror.workshop),
				.startTried = tried,
			};
			// Its own timer, pushed out while it waits for the host's (from its first look running: stage 10 has started it).
			if (running && !a_seen.begun && !a_seen.done && a_mirror.hostPhase != Phase::kOver && (a_mirror.pushed == Clock::time_point{} || a_now - a_mirror.pushed >= PUSH_AGAIN)) {
				if (PushTimer(a_quest)) {
					++timerPushes;
				}
				a_mirror.pushed = a_now;
			}
			switch (AttackBook::Next(facts)) {
			case AttackBook::MirrorStep::kNone:
				break;
			case AttackBook::MirrorStep::kStart:
				StartMirror(a_quest, a_mirror, a_now);
				break;
			case AttackBook::MirrorStep::kDefuse:
				if (a_now - a_mirror.stepAsked >= STEP_AGAIN) {
					a_mirror.stepAsked = a_now;
					REX::INFO("SettlementAttacks: our mirror {:08X} '{}': stage {} ({})", a_quest->GetFormID(), EditorId(a_quest), AttackBook::DONE_STAGE,
						a_mirror.hostPhase == Phase::kOver ? "the host's run ended" : a_mirror.hostPhase == Phase::kDefended ? "the host's attackers are dead" : "its own timer began it");
					Defuse(a_quest);
				}
				if (a_seen.begun && !a_mirror.sweptOwn) {
					a_mirror.sweptOwn = true;  // its own attackers are out: not the host's, so removed (the host's are shared)
					Spawns::StopQuestRun(a_quest->GetFormID(), true);
				}
				break;
			case AttackBook::MirrorStep::kShowObjective:
				a_mirror.objectiveShown = true;
				Console(std::format("setobjectivedisplayed {:08X} {} 1", a_quest->GetFormID(), AttackBook::OBJECTIVE));
				REX::INFO("SettlementAttacks: our player is at {:08X} during the host's attack: our mirror's objective {} is shown", a_mirror.workshop, AttackBook::OBJECTIVE);
				break;
			case AttackBook::MirrorStep::kStop:
				if (a_now - a_mirror.stepAsked >= STEP_AGAIN) {
					a_mirror.stepAsked = a_now;
					REX::INFO("SettlementAttacks: our mirror {:08X} '{}' at {:08X} is stopped: the host's run ended", a_quest->GetFormID(), EditorId(a_quest), a_mirror.workshop);
					Spawns::StopQuestRun(a_quest->GetFormID());
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
				.word = view.WordOn(a_seen.workshop),
				.hostAttackHere = view.LiveAt(a_seen.workshop),
				.begun = a_seen.begun,
				.done = a_seen.done,
				.loadedHere = LoadedHere(a_seen.workshop),
				.askedLately = run.asked != Clock::time_point{} && a_now - run.asked < AttackBook::ASK_AGAIN,
			};
			const auto place = [&] { return PlaceName(a_seen.workshop, a_seen.location); };
			switch (AttackBook::Judge(facts)) {
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
				REX::INFO("SettlementAttacks: our own attack {:08X} '{}' on {:08X} ({}): asking whose settlement it is", a_quest->GetFormID(), EditorId(a_quest), a_seen.workshop, place());
				break;
			case AttackBook::Verdict::kDefuse:
				if (a_now - run.stepAsked >= STEP_AGAIN) {
					run.stepAsked = a_now;
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
				if (a_now - run.stepAsked >= STEP_AGAIN) {
					run.stepAsked = a_now;
					++ownStopped;
					REX::INFO("SettlementAttacks: our own attack {:08X} '{}' on {:08X} ({}) is stopped, its attackers removed: the host's settlement", a_quest->GetFormID(), EditorId(a_quest),
						a_seen.workshop, place());
					Spawns::StopQuestRun(a_quest->GetFormID());
				}
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

		// The session is over for our mirrors: each defused and stopped now (Spawns' stops wait for a session's frame).
		void EndMirrors()
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			if (!player || !player->GetParentCell() || !WorldSync::InWorld()) {
				if (!mirrors.empty()) {
					REX::WARN("SettlementAttacks: the session ended out of the world: {} mirror(s) left as they are (their timers run out on their own)", mirrors.size());
				}
				return;
			}
			for (const auto& [id, mirror] : mirrors) {
				const auto quest = RE::TESForm::GetFormByID<RE::TESQuest>(id);
				if (!quest || !Running(quest) || !mirror.running) {
					continue;
				}
				if (!QuestStageLayout::IsStageDone(quest, AttackBook::DONE_STAGE)) {
					Defuse(quest);
				}
				// The stage's done mark is the engine's, set as the stage is (its fragment runs later on the VM, and so does
				// the shutdown stage's, which reads the mark): stopped only once it shows.
				if (!QuestStageLayout::IsStageDone(quest, AttackBook::DONE_STAGE)) {
					REX::WARN("SettlementAttacks: the session ended; our mirror {:08X} '{}' didn't take stage {}: left running (its timer runs out on its own)", id, EditorId(quest),
						AttackBook::DONE_STAGE);
					continue;
				}
				Console(std::format("stopquest {:08X}", id));
				++mirrorsEnded;
				REX::INFO("SettlementAttacks: the session ended: our mirror {:08X} '{}' at {:08X} is stopped, defused", id, EditorId(quest), mirror.workshop);
			}
		}
	}

	void OnWelcome(bool a_hosting)
	{
		hosting = a_hosting;
		lastLook = {};
	}

	void Frame()
	{
		const auto now = Clock::now();
		if (now - lastLook < LOOK_INTERVAL || Story::InOpening() || !WorldSync::InWorld() || !Hud::IsSettled() || Hud::LoadingScreenUp()) {
			return;
		}
		lastLook = now;
		Resolve();
		if (hosting) {
			HostLook();
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
		}
		lastLook = {};  // looked at again next frame
	}

	void OnOwner(const Protocol::SettlementOwner& a_owner)
	{
		if (hosting) {
			return;
		}
		view.OnOwner(a_owner);
		if (a_owner.workshop == 0) {
			REX::INFO("SettlementAttacks: the host's game left: nothing it said of settlements stands; our mirrors end");
			toldUnderway.clear();
		} else {
			REX::INFO("SettlementAttacks: settlement {:08X} is {}the host's", a_owner.workshop, a_owner.hostOwned ? "" : "not ");
		}
		lastLook = {};
	}

	std::vector<std::vector<std::uint8_t>> TakeOutgoing()
	{
		return std::exchange(outgoing, {});
	}

	void OnGameLoaded()
	{
		// Our runs are the loaded save's: read afresh. The host's game reports any it still runs as over (its quest stopped
		// in the save left behind) and those of the loaded save as new; a friend's mirrors are looked for again.
		own.clear();
		for (auto& [id, mirror] : mirrors) {
			mirror.running = false;
			mirror.ended = false;
			mirror.gaveUpLogged = false;
			mirror.startTries = 0;
			mirror.objectiveShown = false;
			mirror.sweptOwn = false;
			mirror.pushed = {};
		}
		lastLook = {};
	}

	void Reset()
	{
		EndMirrors();
		hosting = false;
		outgoing.clear();
		hostRuns.Reset();
		view.Reset();
		mirrors.clear();
		own.clear();
		toldUnderway.clear();
		lastLook = {};
	}

	std::string Describe()
	{
		return std::format("attacks: hosting={} live={} mirrors={} reported={} answered={} asked={} mirrorStarts={} mirrorsEnded={} ownDefused={} ownStopped={} timerPushes={}", hosting,
			hosting ? hostRuns.Size() : view.LiveCount(), mirrors.size(), reported, answered, asked, mirrorStarts, mirrorsEnded, ownDefused, ownStopped, timerPushes);
	}

	std::string List()
	{
		Resolve();
		std::string out;
		for (const auto quest : quests) {
			const auto seen = Read(quest);
			if (!seen.running) {
				continue;
			}
			const auto mirror = mirrors.find(seen.quest);
			const bool mirrored = mirror != mirrors.end() && mirror->second.workshop == seen.workshop;
			out += std::format("{:08X} '{}' workshop={:08X} location={:08X} begun={} done={} loaded={} {} ", seen.quest, EditorId(quest), seen.workshop, seen.location, seen.begun, seen.done,
				LoadedHere(seen.workshop), mirrored ? std::format("mirror(host phase {})", std::to_underlying(mirror->second.hostPhase)) : std::string{ "own" });
			if (!hosting && !mirrored) {
				const auto word = view.WordOn(seen.workshop);
				out += std::format("word={} ", word == AttackBook::Word::kHost ? "host" : word == AttackBook::Word::kNotHost ? "notHost" : "unknown");
			}
			out += "; ";
		}
		if (out.empty()) {
			out = "no attack running here; ";
		}
		for (const auto& live : view.AllLive()) {
			out += std::format("host's {:08X} at {:08X} phase {}; ", live.quest, live.workshop, std::to_underlying(live.phase));
		}
		return out + Describe();
	}
}
