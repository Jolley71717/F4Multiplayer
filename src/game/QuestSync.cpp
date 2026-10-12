#include "game/QuestSync.h"
#include "game/Official.h"

#include "Config.h"
#include "game/Achievements.h"
#include "game/Hud.h"
#include "game/QuestStageLayout.h"
#include "game/RemotePlayers.h"
#include "game/StageLog.h"
#include "game/Story.h"
#include "game/WorldSync.h"

namespace QuestSync
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		constexpr auto POLL_INTERVAL = 500ms;
		// After loading, quest scripts settle their stages for a moment; that isn't progress.
		constexpr auto SETTLE_TIME = 5s;

		// QUEST_DATA::questType values that are shared: main quest, the four factions, side quests
		// and DLC quests. 0 is "none" (internal quests) and 6 is "miscellaneous" (radiant/ambient).
		bool IsSharedType(std::int8_t a_type)
		{
			return a_type > 0 && a_type != 6;
		}

		// The factions' last quests, which the game files under no type (0, like its internal quests). They
		// are shared with the rest of the story when the host shares it; their stages award achievements.
		// Fallout4.esm: Inst305 "Powering Up", BoS302 "Blind Betrayal", BoS304 "Ad Victoriam".
		constexpr std::array<std::uint32_t, 3> STORY_FINALES{ 0x000A274B, 0x000B9F9D, 0x00173ED9 };

		bool IsFinale(const RE::TESQuest* a_quest)
		{
			return a_quest->data.questType == 0 && std::ranges::find(STORY_FINALES, a_quest->GetFormID()) != STORY_FINALES.end();
		}

		// Quests whose progress is watched here: the shared types and the factions' finales.
		bool Watched(const RE::TESQuest* a_quest)
		{
			return IsSharedType(a_quest->data.questType) || IsFinale(a_quest);
		}

		// QUEST_DATA::flags bits: the quest is running, the quest is completed.
		constexpr std::uint16_t QUEST_RUNNING = 0x0001;
		constexpr std::uint16_t QUEST_COMPLETED = 0x0002;
		// A quest that completes this soon after a stage from another player was completed by them.
		constexpr auto REMOTE_COMPLETION_WINDOW = 10s;
		// Stages a quest's script sets this soon after another player's stage are its reaction to
		// that stage, not this player's progress: every game that applies it gets there by itself.
		constexpr auto SCRIPT_REACTION_WINDOW = 3s;

		std::unordered_map<std::uint32_t, std::uint16_t> knownStages;  // last current stage seen per quest
		std::unordered_map<std::uint32_t, StageLog::Stages> doneStages;  // the stages seen done per quest
		std::unordered_map<std::uint32_t, bool>          knownDone;    // last completed state seen per quest
		std::unordered_map<std::uint32_t, Clock::time_point> remoteStageAt;  // when another player last moved it
		StageLog::Queue                                  waiting;  // stages from other players, until they are set here
		std::unordered_set<std::uint32_t>                waited;   // quests whose stages waited for the quest to start here
		std::optional<Clock::time_point>                 reportFrom;  // set on the first in-game poll
		std::vector<std::vector<std::uint8_t>>           outgoing;
		Clock::time_point                                nextPoll{};
		std::uint32_t                                    reported = 0;
		std::uint32_t                                    applied = 0;
		std::uint32_t                                    caughtUp = 0;  // ... after waiting for the quest to start here
		std::uint32_t                                    gaveUp = 0;    // set, but not done in time
		std::uint32_t                                    refused = 0;   // more than the queue holds
		std::uint32_t                                    completed = 0;
		std::unordered_map<std::uint32_t, std::uint16_t> miscStages;  // misc quests: stage changes only
		Clock::time_point                                lastStageChange{};

		std::span<RE::TESQuest*> AllQuests()
		{
			const auto handler = RE::TESDataHandler::GetSingleton();
			if (!handler) {
				return {};
			}
			auto& quests = handler->GetFormArray<RE::TESQuest>();
			return { quests.data(), quests.size() };
		}

		// A stage set in the middle of a conversation or scene can break it; wait until it's over.
		bool PlayerBusy()
		{
			const auto ui = RE::UI::GetSingleton();
			const auto player = RE::PlayerCharacter::GetSingleton();
			return (ui && ui->GetMenuOpen("DialogueMenu"sv)) || (player && player->GetCurrentScene());
		}

		// The main story and faction quests (their finales too) are shared only if the host chose a
		// shared story. Quests from other mods are never shared (see Official.h).
		bool Shared(const RE::TESQuest* a_quest)
		{
			if (!Official::IsOfficialForm(a_quest)) {
				return false;
			}
			if (IsFinale(a_quest)) {
				return Story::Shared();
			}
			return IsSharedType(a_quest->data.questType) && (Story::Shared() || !Story::IsStoryType(a_quest->data.questType));
		}

		// Whether stages from other players can be set on this quest now. They wait until this player
		// has started the quest too (an objective is shown; the game runs some quests in the background
		// long before that), possibly because another quest's stage starts it (MQ104's 210 starts MQ105).
		StageLog::Quest QuestState(std::uint32_t a_quest)
		{
			const auto quest = RE::TESForm::GetFormByID<RE::TESQuest>(a_quest);
			if (!quest || !Shared(quest) || (quest->data.flags & QUEST_COMPLETED)) {
				return StageLog::Quest::kGone;
			}
			// Quest scripts must not run at the main menu or during a loading screen.
			if (!WorldSync::InWorld() || Story::InOpening() || !(quest->data.flags & QUEST_RUNNING) || !PlayerStarted(quest)) {
				waited.insert(a_quest);
				return StageLog::Quest::kWait;
			}
			return PlayerBusy() ? StageLog::Quest::kWait : StageLog::Quest::kReady;
		}

		// Stages only move forward: never undo progress a player already has. A stage this quest is
		// already past here is left alone, done or not (it may be a branch this player didn't take).
		bool Settled(std::uint32_t a_quest, std::uint16_t a_stage)
		{
			const auto quest = RE::TESForm::GetFormByID<RE::TESQuest>(a_quest);
			return !quest || a_stage <= quest->currentStage || QuestStageLayout::IsStageDone(quest, a_stage);
		}

		// Sets another player's stage here. That runs the stage's script, as it did in their game (an
		// achievement in it too, where the game awards them), and starts a quest from wherever the player
		// is; it can pull a new character out of the opening.
		bool Set(std::uint32_t a_quest, std::uint16_t a_stage)
		{
			const auto quest = RE::TESForm::GetFormByID<RE::TESQuest>(a_quest);
			if (!quest) {
				return false;
			}
			RE::Console::ExecuteCommand(std::format("setstage {:08X} {}", a_quest, a_stage).c_str());
			const char* name = quest->GetFullName();
			REX::INFO("QuestSync: set {:08X} '{}' stage {} (was {})", a_quest, name ? name : "", a_stage, quest->currentStage);
			// That stage came from another player; don't report it back. (The console sets it a moment later.)
			knownStages[a_quest] = std::max(quest->currentStage, a_stage);
			if (const auto known = doneStages.find(a_quest); known != doneStages.end()) {
				StageLog::Insert(known->second, a_stage);
			}
			remoteStageAt[a_quest] = Clock::now();
			lastStageChange = Clock::now();  // its quest XP is ours, not kill XP
			++applied;
			if (waited.erase(a_quest) > 0) {
				++caughtUp;
				if (name && *name) {
					Hud::Notify(std::format("Caught up with your friends: {}", name));
				}
			}
			Achievements::AfterStageSet(a_quest, a_stage);
			return true;
		}
	}

	bool PlayerStarted(const RE::TESQuest* a_quest)
	{
		return a_quest && std::ranges::any_of(a_quest->objectives, [](const RE::BGSQuestObjective* a_objective) {
			return a_objective && a_objective->state != static_cast<char>(RE::QUEST_OBJECTIVE_STATE::kDormant);
		});
	}

	void Apply(const Protocol::QuestStage& a_stage)
	{
		if (!Config::Get().syncQuests) {
			return;
		}
		const auto quest = RE::TESForm::GetFormByID<RE::TESQuest>(a_stage.quest);
		if (!quest || !Shared(quest) || (quest->data.flags & QUEST_COMPLETED) || Settled(a_stage.quest, a_stage.stage)) {
			return;
		}
		// Set by Frame, in order: a session's stages arrive in the order they were reported (all of them
		// for a player who joins late or loads a save), and each stage's script runs before the next.
		if (!waiting.Add(a_stage.quest, a_stage.stage) && waiting.Size() >= StageLog::Queue::MAX_WAITING && refused++ == 0) {
			REX::WARN("QuestSync: more than {} stages waiting; dropping some", StageLog::Queue::MAX_WAITING);
		}
	}

	std::vector<std::uint16_t> Waiting(std::uint32_t a_quest)
	{
		return waiting.Waiting(a_quest);
	}

	void ApplyDone(const Protocol::QuestDone& a_done)
	{
		const auto quest = RE::TESForm::GetFormByID<RE::TESQuest>(a_done.quest);
		const char* name = quest ? quest->GetFullName() : nullptr;
		if (!quest || !Watched(quest) || !Official::IsOfficialForm(quest) || !name || !*name) {
			return;
		}
		Hud::Notify(std::format("{} completed {}", RemotePlayers::NameOf(a_done.playerId), name));
	}

	void Frame()
	{
		const auto now = Clock::now();
		if (now < nextPoll) {
			return;
		}
		nextPoll = now + POLL_INTERVAL;

		if (!WorldSync::InWorld()) {
			return;
		}
		// Misc quests (radiant ones give XP too) are only watched for stage changes.
		for (const auto quest : AllQuests()) {
			if (quest && quest->data.questType == 6) {
				const auto [it, inserted] = miscStages.try_emplace(quest->GetFormID(), quest->currentStage);
				if (!inserted && it->second != quest->currentStage) {
					it->second = quest->currentStage;
					lastStageChange = now;
				}
			}
		}
		if (!Config::Get().syncQuests) {
			for (const auto quest : AllQuests()) {
				if (quest && Watched(quest)) {
					const auto [it, inserted] = knownStages.try_emplace(quest->GetFormID(), quest->currentStage);
					if (!inserted && it->second != quest->currentStage) {
						it->second = quest->currentStage;
						lastStageChange = now;
					}
				}
			}
			return;
		}

		for (const auto& stage : waiting.Poll(now, QuestState, Settled, Set)) {
			++gaveUp;
			REX::WARN("QuestSync: {:08X} stage {} still not done {} s after setting it; going on with the next", stage.quest, stage.stage,
				std::chrono::duration_cast<std::chrono::seconds>(StageLog::Queue::TIMEOUT).count());
		}
		std::erase_if(waited, [](std::uint32_t a_quest) { return !waiting.Contains(a_quest); });

		if (!reportFrom) {
			reportFrom = now + SETTLE_TIME;
		}
		const bool report = now >= *reportFrom;

		for (const auto quest : AllQuests()) {
			if (!quest || !Watched(quest)) {
				continue;
			}
			const auto id = quest->GetFormID();

			// Completed here, not because of another player's stage: tell the others who did it.
			const bool done = (quest->data.flags & QUEST_COMPLETED) != 0;
			const auto [doneIt, firstLook] = knownDone.try_emplace(id, done);
			if (!firstLook && doneIt->second != done) {
				doneIt->second = done;
				const auto remote = remoteStageAt.find(id);
				const bool byOther = remote != remoteStageAt.end() && now - remote->second < REMOTE_COMPLETION_WINDOW;
				const char* name = quest->GetFullName();
				if (done && report && !byOther && name && *name && (id >> 24) != 0xFF) {
					outgoing.push_back(Protocol::Encode(Protocol::QuestDone{ 0, id }, Protocol::MessageType::kReportQuestDone));
					++completed;
				}
			}

			// Any stage change gives quest XP, not kill XP (see Party).
			const auto [it, inserted] = knownStages.try_emplace(id, quest->currentStage);
			if (!inserted && it->second != quest->currentStage) {
				it->second = quest->currentStage;
				lastStageChange = now;
			}

			// Every stage done since the last look is progress, not just the current stage: a stage whose
			// script moves the quest on at once (MQ104's 210 stops it) is never seen as the current one,
			// and the others then never ran it, nor what it does (starting MQ105, an achievement).
			auto       stages = QuestStageLayout::Done(quest);
			const auto [known, firstLookStages] = doneStages.try_emplace(id);
			if (firstLookStages) {
				known->second = std::move(stages);
				continue;
			}
			const auto fresh = StageLog::NewlyDone(known->second, std::move(stages));
			if (fresh.empty()) {
				continue;
			}
			lastStageChange = now;
			// Only of quests this player has started: the game moves some quests along in the background,
			// and a new game sets up many while the player is still in the opening.
			const auto remote = remoteStageAt.find(id);
			const bool reaction = remote != remoteStageAt.end() && now - remote->second < SCRIPT_REACTION_WINDOW;
			if (!report || reaction || (id >> 24) == 0xFF || !Shared(quest) || !PlayerStarted(quest) || Story::InOpening()) {
				continue;
			}
			for (const auto stage : fresh) {
				outgoing.push_back(Protocol::Encode(Protocol::QuestStage{ id, stage }, Protocol::MessageType::kReportQuestStage));
				++reported;
			}
		}
	}

	std::vector<std::vector<std::uint8_t>> TakeOutgoing()
	{
		return std::exchange(outgoing, {});
	}

	void Rebaseline()
	{
		knownStages.clear();
		doneStages.clear();
		miscStages.clear();
		knownDone.clear();
		remoteStageAt.clear();
		reportFrom.reset();
		waiting.Clear();
		waited.clear();
	}

	Clock::time_point LastStageChange()
	{
		return lastStageChange;
	}

	std::string Describe()
	{
		return std::format("quests: reported={} applied={} caughtUp={} waiting={} setting={} gaveUp={} refused={} completed={}", reported, applied, caughtUp,
			waiting.Size(), waiting.Setting(), gaveUp, refused, completed);
	}
}
