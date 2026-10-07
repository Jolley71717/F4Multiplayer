#include "game/QuestSync.h"

#include "Config.h"
#include "game/Hud.h"
#include "game/RemotePlayers.h"
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

		// QUEST_DATA::flags bits: the quest is running, the quest is completed.
		constexpr std::uint16_t QUEST_RUNNING = 0x0001;
		constexpr std::uint16_t QUEST_COMPLETED = 0x0002;
		// A quest that completes this soon after a stage from another player was completed by them.
		constexpr auto REMOTE_COMPLETION_WINDOW = 10s;

		std::unordered_map<std::uint32_t, std::uint16_t> knownStages;  // last stage seen per quest
		std::unordered_map<std::uint32_t, bool>          knownDone;    // last completed state seen per quest
		std::unordered_map<std::uint32_t, Clock::time_point> remoteStageAt;  // when another player last moved it
		std::unordered_map<std::uint32_t, std::uint16_t> pending;  // highest stage from other players per quest, until it can be set here
		std::optional<Clock::time_point>                 reportFrom;  // set on the first in-game poll
		std::vector<std::vector<std::uint8_t>>           outgoing;
		Clock::time_point                                nextPoll{};
		std::uint32_t                                    reported = 0;
		std::uint32_t                                    applied = 0;
		std::uint32_t                                    caughtUp = 0;  // ... after waiting for the quest to start here
		std::uint32_t                                    completed = 0;
		std::unordered_map<std::uint32_t, std::uint16_t> miscStages;  // misc quests: stage changes only
		Clock::time_point                                lastStageChange{};

		std::span<RE::TESQuest*> AllQuests()
		{
			auto& quests = RE::TESDataHandler::GetSingleton()->GetFormArray<RE::TESQuest>();
			return { quests.data(), quests.size() };
		}

		// A stage set in the middle of a conversation or scene can break it; wait until it's over.
		bool PlayerBusy()
		{
			const auto ui = RE::UI::GetSingleton();
			const auto player = RE::PlayerCharacter::GetSingleton();
			return (ui && ui->GetMenuOpen("DialogueMenu"sv)) || (player && player->GetCurrentScene());
		}

		enum class Outcome
		{
			kDone,     // set, or nothing to do
			kWait,     // try again later
		};

		// The main story and faction quests are shared only if the host chose a shared story.
		bool Shared(const RE::TESQuest* a_quest)
		{
			return IsSharedType(a_quest->data.questType) && (Story::Shared() || !Story::IsStoryType(a_quest->data.questType));
		}

		// Sets another player's stage here once this player has started the quest too (an objective
		// is shown; the game runs some quests in the background long before that). Setting a stage
		// starts a quest from wherever the player is, skips the stages before it, and can pull a new
		// character out of the opening.
		Outcome TrySet(std::uint32_t a_quest, std::uint16_t a_stage, bool a_waited)
		{
			const auto quest = RE::TESForm::GetFormByID<RE::TESQuest>(a_quest);
			// Stages only move forward: never undo progress a player already has.
			if (!quest || !Shared(quest) || (quest->data.flags & QUEST_COMPLETED) || a_stage <= quest->currentStage) {
				return Outcome::kDone;
			}
			// Quest scripts must not run at the main menu or during a loading screen.
			if (!WorldSync::InWorld() || Story::InOpening() || !(quest->data.flags & QUEST_RUNNING) || !PlayerStarted(quest) || PlayerBusy()) {
				return Outcome::kWait;
			}
			RE::Console::ExecuteCommand(std::format("setstage {:08X} {}", a_quest, a_stage).c_str());
			const char* name = quest->GetFullName();
			REX::INFO("QuestSync: set {:08X} '{}' stage {} (was {})", a_quest, name ? name : "", a_stage, quest->currentStage);
			// That stage came from another player; don't report it back. (The console sets it a moment later.)
			knownStages[a_quest] = std::max(quest->currentStage, a_stage);
			remoteStageAt[a_quest] = Clock::now();
			lastStageChange = Clock::now();  // its quest XP is ours, not kill XP
			++applied;
			if (a_waited) {
				++caughtUp;
				if (name && *name) {
					Hud::Notify(std::format("Caught up with your friends: {}", name));
				}
			}
			return Outcome::kDone;
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
		if (TrySet(a_stage.quest, a_stage.stage, false) == Outcome::kWait) {
			auto& stage = pending[a_stage.quest];
			stage = std::max(stage, a_stage.stage);
		}
	}

	void ApplyDone(const Protocol::QuestDone& a_done)
	{
		const auto quest = RE::TESForm::GetFormByID<RE::TESQuest>(a_done.quest);
		const char* name = quest ? quest->GetFullName() : nullptr;
		if (!quest || !IsSharedType(quest->data.questType) || !name || !*name) {
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
				if (quest && IsSharedType(quest->data.questType)) {
					const auto [it, inserted] = knownStages.try_emplace(quest->GetFormID(), quest->currentStage);
					if (!inserted && it->second != quest->currentStage) {
						it->second = quest->currentStage;
						lastStageChange = now;
					}
				}
			}
			return;
		}
		std::erase_if(pending, [](const auto& a_entry) { return TrySet(a_entry.first, a_entry.second, true) == Outcome::kDone; });

		if (!reportFrom) {
			reportFrom = now + SETTLE_TIME;
		}
		const bool report = now >= *reportFrom;

		for (const auto quest : AllQuests()) {
			if (!quest || !IsSharedType(quest->data.questType)) {
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

			const auto [it, inserted] = knownStages.try_emplace(id, quest->currentStage);
			if (inserted || it->second == quest->currentStage) {
				continue;
			}
			const bool forward = quest->currentStage > it->second;
			it->second = quest->currentStage;
			lastStageChange = now;
			// Only progress is shared (a quest that restarts or resets goes back to a lower stage),
			// and only of quests this player has started: the game moves some quests along in the
			// background, and a new game sets up many while the player is still in the opening.
			if (report && forward && (id >> 24) != 0xFF && Shared(quest) && PlayerStarted(quest) && !Story::InOpening()) {
				outgoing.push_back(Protocol::Encode(Protocol::QuestStage{ id, quest->currentStage }, Protocol::MessageType::kReportQuestStage));
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
		miscStages.clear();
		knownDone.clear();
		remoteStageAt.clear();
		reportFrom.reset();
		pending.clear();
	}

	Clock::time_point LastStageChange()
	{
		return lastStageChange;
	}

	std::string Describe()
	{
		return std::format("quests: reported={} applied={} caughtUp={} waiting={} completed={}", reported, applied, caughtUp, pending.size(), completed);
	}
}
