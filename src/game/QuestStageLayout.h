#pragma once

#include "game/StageLog.h"

// CommonLibF4 only declares TESQuestStage; this mirror has the engine's layout (1.11.240), read from
// TESQuest::IsStageDone (+66B700), which finds a stage in TESQuest::stages (+100) by the index at +18
// and tests bit 0 of the flags at +1A, and TESQuestStage::SetDone (+683340), which sets that bit and
// walks the log entries at +00. Read, and written only by MarkDone.
namespace QuestStageLayout
{
	struct Mirror
	{
		RE::BSTArray<void*>  items;  // 00 the stage's log entries (TESQuestStageItem*)
		RE::QUEST_STAGE_DATA data;   // 18 index, flags (kDone = bit 0)
	};
	static_assert(sizeof(RE::BSTArray<void*>) == 0x18);
	static_assert(offsetof(Mirror, data) == 0x18);
	static_assert(offsetof(RE::TESQuest, stages) == 0x100);

	inline const Mirror* Of(const RE::TESQuestStage* a_stage)
	{
		return reinterpret_cast<const Mirror*>(a_stage);
	}

	inline bool IsDone(const Mirror& a_stage)
	{
		return a_stage.data.flags.all(RE::QUEST_STAGE_DATA::Flags::kDone);
	}

	// The quest's stages done in this game, ascending.
	inline StageLog::Stages Done(const RE::TESQuest* a_quest)
	{
		StageLog::Stages done;
		for (const auto stage : a_quest->stages) {
			if (stage && IsDone(*Of(stage))) {
				done.push_back(Of(stage)->data.index);
			}
		}
		std::ranges::sort(done);
		const auto repeats = std::ranges::unique(done);
		done.erase(repeats.begin(), repeats.end());
		return done;
	}

	// The digest of the quest's stages done in this game (StageLog::Digest), read without a list.
	using Digest = StageLog::Digest;
	inline Digest DigestOf(const RE::TESQuest* a_quest)
	{
		Digest digest;
		for (const auto stage : a_quest->stages) {
			if (stage && IsDone(*Of(stage))) {
				digest.Add(Of(stage)->data.index);
			}
		}
		return digest;
	}

	inline bool IsStageDone(const RE::TESQuest* a_quest, std::uint16_t a_stage)
	{
		return std::ranges::any_of(a_quest->stages, [&](const RE::TESQuestStage* a_entry) {
			return a_entry && Of(a_entry)->data.index == a_stage && IsDone(*Of(a_entry));
		});
	}

	// Marks the quest's stage done without setting it: the done bit alone, as IsStageDone and the save read it (the quest's
	// stages are marked changed for the save), not SetDone's walk of the log entries (a Complete Quest entry would complete
	// the quest through the engine, with its quest XP) nor SetStage's script. For a friend's turn-in whose reward was given
	// here (QuestSync::CloseTurnIn). False if the quest has no such stage or it is done already. Main thread only.
	inline bool MarkDone(RE::TESQuest* a_quest, std::uint16_t a_stage)
	{
		bool marked = false;
		for (const auto stage : a_quest->stages) {
			if (!stage) {
				continue;
			}
			auto& mirror = *reinterpret_cast<Mirror*>(stage);
			if (mirror.data.index == a_stage && !IsDone(mirror)) {
				mirror.data.flags.set(RE::QUEST_STAGE_DATA::Flags::kDone);
				marked = true;
			}
		}
		if (marked) {
			a_quest->AddChange(RE::CHANGE_TYPES::kQuestStages);
		}
		return marked;
	}

	// Whether the quest has this stage at all (setting one it doesn't have does nothing: TESQuest::SetStage
	// finds no entry for it in the quest's stage table).
	inline bool Has(const RE::TESQuest* a_quest, std::uint16_t a_stage)
	{
		return std::ranges::any_of(a_quest->stages, [&](const RE::TESQuestStage* a_entry) { return a_entry && Of(a_entry)->data.index == a_stage; });
	}

	// How many stages the quest has.
	inline std::size_t Count(const RE::TESQuest* a_quest)
	{
		return a_quest->stages.size();
	}
}
