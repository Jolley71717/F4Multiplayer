#include "game/QuestSync.h"

#include "Config.h"
#include "game/Hud.h"
#include "game/RemotePlayers.h"
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

		// QUEST_DATA::flags bit set when a quest is completed.
		constexpr std::uint16_t QUEST_COMPLETED = 0x0002;
		// A quest that completes this soon after a stage from another player was completed by them.
		constexpr auto REMOTE_COMPLETION_WINDOW = 10s;

		std::unordered_map<std::uint32_t, std::uint16_t> knownStages;  // last stage seen per quest
		std::unordered_map<std::uint32_t, bool>          knownDone;    // last completed state seen per quest
		std::unordered_map<std::uint32_t, Clock::time_point> remoteStageAt;  // when another player last moved it
		std::vector<Protocol::QuestStage>                waiting;  // from other players, until we're in the world
		std::optional<Clock::time_point>                 reportFrom;  // set on the first in-game poll
		std::vector<std::vector<std::uint8_t>>           outgoing;
		Clock::time_point                                nextPoll{};
		std::uint32_t                                    reported = 0;
		std::uint32_t                                    applied = 0;
		std::uint32_t                                    completed = 0;
		std::unordered_map<std::uint32_t, std::uint16_t> miscStages;  // misc quests: stage changes only
		Clock::time_point                                lastStageChange{};

		std::span<RE::TESQuest*> AllQuests()
		{
			auto& quests = RE::TESDataHandler::GetSingleton()->GetFormArray<RE::TESQuest>();
			return { quests.data(), quests.size() };
		}
	}

	void Apply(const Protocol::QuestStage& a_stage)
	{
		if (!Config::Get().syncQuests) {
			return;
		}
		// Quest scripts must not run at the main menu or during a loading screen.
		if (!WorldSync::InWorld()) {
			if (std::ranges::find(waiting, a_stage) == waiting.end()) {
				waiting.push_back(a_stage);
			}
			return;
		}
		const auto quest = RE::TESForm::GetFormByID<RE::TESQuest>(a_stage.quest);
		// Stages only move forward: never undo progress a player already has.
		if (!quest || !IsSharedType(quest->data.questType) || a_stage.stage <= quest->currentStage) {
			return;
		}
		// The console command also starts the quest if it isn't running (TESQuest::SetStage doesn't).
		RE::Console::ExecuteCommand(std::format("setstage {:08X} {}", a_stage.quest, a_stage.stage).c_str());
		REX::INFO("QuestSync: set {:08X} '{}' stage {} -> now {}", a_stage.quest, quest->GetFullName() ? quest->GetFullName() : "", a_stage.stage, quest->currentStage);
		// Whatever stage it's at now came from another player; don't report it back.
		knownStages[a_stage.quest] = quest->currentStage;
		remoteStageAt[a_stage.quest] = Clock::now();
		lastStageChange = Clock::now();  // its quest XP is ours, not kill XP
		++applied;
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
		for (const auto& stage : std::exchange(waiting, {})) {
			Apply(stage);
		}

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
			// Only progress is shared (a quest that restarts or resets goes back to a lower stage).
			if (report && forward && (id >> 24) != 0xFF) {
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
		waiting.clear();
	}

	Clock::time_point LastStageChange()
	{
		return lastStageChange;
	}

	std::string Describe()
	{
		return std::format("quests: reported={} applied={} completed={}", reported, applied, completed);
	}
}
