#pragma once

#include "game/StageLog.h"

#include <array>
#include <cstring>
#include <format>
#include <string>
#include <vector>

// CommonLibF4 only declares TESQuestStage; this mirror has the engine's layout (1.11.240), read from
// TESQuest::IsStageDone (+66B700), which finds a stage in TESQuest::stages (+100) by the index at +18
// and tests bit 0 of the flags at +1A, and TESQuestStage::SetDone (+683340), which sets that bit and
// walks the log entries at +00. Read, and written only by MarkDone and LogEntries.
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

	// A stage's log entry (TESQuestStageItem, 0x28 bytes; CommonLibF4 only declares it), as its constructor (+683430) and
	// the quest record's loader leave it (+6693DC: a QSDT makes one, +683490 reads its flags and its NAM0, the loader
	// numbers it within its stage and adds it to the stage's array; +6691B3: a CNAM gives it its text and sets +16;
	// +683540 links it to its quest and stage), and as TESQuestStageItem::Run (+683860) reads it.
	struct Item
	{
		enum Flag : std::uint8_t
		{
			kCompletes = 1 << 0,  // Complete Quest
			kFails = 1 << 1,      // Fail Quest
		};

		RE::TESCondition   conditions;  // 00 the entry counts only when these hold for the player
		RE::TESQuest*      nextQuest;   // 08 NAM0: the quest a completing entry starts (a form ID until the forms are linked)
		std::uint32_t      text;        // 10 CNAM: the entry's text, a localized string's ID
		std::uint8_t       flags;       // 14 QSDT
		std::uint8_t       index;       // 15 its number within its stage
		bool               hasText;     // 16 it has a CNAM
		std::uint8_t       pad17;       // 17
		RE::TESQuest*      quest;       // 18 the quest it is of
		RE::TESQuestStage* stage;       // 20 the stage it is of
	};
	static_assert(sizeof(Item) == 0x28);
	static_assert(offsetof(Item, nextQuest) == 0x08);
	static_assert(offsetof(Item, text) == 0x10);
	static_assert(offsetof(Item, flags) == 0x14);
	static_assert(offsetof(Item, index) == 0x15);
	static_assert(offsetof(Item, hasText) == 0x16);
	static_assert(offsetof(Item, quest) == 0x18);
	static_assert(offsetof(Item, stage) == 0x20);
	// What the journal's code below reads besides: the player's quest log, the quest's instances (the journal's place in
	// the quest is the current instance's) and what the game scopes an entry's conditions by.
	static_assert(offsetof(RE::PlayerCharacter, questLog) == 0x7C8);
	static_assert(offsetof(RE::TESQuest, instanceData) == 0x38);
	static_assert(offsetof(RE::TESQuest, currentInstanceID) == 0x50);
	static_assert(offsetof(RE::TESQuest, startEventData) == 0x2C8);
	static_assert(offsetof(RE::BGSQuestInstanceText, journalStage) == 0x38);
	static_assert(offsetof(RE::BGSQuestInstanceText, journalStageItem) == 0x3A);
	static_assert(offsetof(RE::ConditionCheckParams, scopeQuest) == 0x10);
	static_assert(offsetof(RE::ConditionCheckParams, scopeEvent) == 0x18);
	static_assert(offsetof(RE::ConditionCheckParams, outDispFailure) == 0x64);

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
	// The contract, for every caller: the done bit and nothing else. currentStage is left as it is (a caller that wants it
	// moved does that itself: CloseTurnIn does, for an ordered quest only, since a dialogue quest's stages aren't steps), and
	// true means newly marked here, not merely that the stage exists (a caller that needs the latter asks Has).
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

	// ---- the journal ----
	// The player's add of a log entry to the journal (1.11.240: +D60440, ID 2232994; PlayerCharacter, the entry), what
	// TESQuestStageItem::Run calls for an entry with text: it leaves an entry the journal holds already alone (unless its
	// quest repeats), otherwise puts it at the front of PlayerCharacter::questLog (a node from the game's own heap), and
	// for a completing entry with a next quest starts that quest (LogPlan leaves those out). It takes no lock: the game's
	// thread only. Called only if it begins as read.
	inline constexpr REL::ID                      ADD_TO_LOG{ 2232994 };
	inline constexpr std::array<std::uint8_t, 16> ADD_TO_LOG_BEGINS{ 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20, 0x41 };
	// TESCondition::IsTrue(ConditionCheckParams&) (1.11.240: +769000, ID 2211990), what Run asks of an entry's conditions,
	// with the parameters it is given there (+7685A0: the player the subject, no target, the entry's quest and that
	// quest's start event the scope). Called only if it begins as read.
	inline constexpr REL::ID                      CONDITIONS_HOLD{ 2211990 };
	inline constexpr std::array<std::uint8_t, 16> CONDITIONS_HOLD_BEGINS{ 0x40, 0x53, 0x55, 0x57, 0x48, 0x83, 0xEC, 0x60, 0x48, 0x8B, 0x19, 0x48, 0x8B, 0xEA, 0xC6, 0x42 };

	inline bool BeginsAsRead(const REL::ID& a_id, const std::array<std::uint8_t, 16>& a_begins, const char* a_what)
	{
		const auto at = a_id.address();
		const bool same = at != 0 && std::memcmp(reinterpret_cast<const void*>(at), a_begins.data(), a_begins.size()) == 0;
		if (!same) {
			REX::WARN("QuestStageLayout: {} does not begin as read; a stage marked done without its script leaves the journal as it is", a_what);
		}
		return same;
	}

	inline bool AddToLogAsRead()
	{
		static const bool asRead = BeginsAsRead(ADD_TO_LOG, ADD_TO_LOG_BEGINS, "the player's add of a log entry (ID 2232994)");
		return asRead;
	}

	inline bool ConditionsHoldAsRead()
	{
		static const bool asRead = BeginsAsRead(CONDITIONS_HOLD, CONDITIONS_HOLD_BEGINS, "TESCondition::IsTrue (ID 2211990)");
		return asRead;
	}

	// Whether a log entry's conditions hold for our player now, asked as the game asks when it sets the stage. One with no
	// conditions holds; one with conditions that can't be asked (the game's function is not as read) does not.
	inline bool Holds(const Item& a_item, RE::PlayerCharacter* a_player, const RE::TESQuest* a_quest)
	{
		if (!a_item.conditions.head) {
			return true;
		}
		if (!ConditionsHoldAsRead()) {
			return false;
		}
		RE::ConditionCheckParams params;
		params.actionRef = a_player;
		params.scopeQuest = a_quest;
		params.scopeEvent = a_quest->startEventData;
		using holds_t = bool(const RE::TESCondition*, RE::ConditionCheckParams&);
		static REL::Relocation<holds_t> holds{ CONDITIONS_HOLD };
		return holds(&a_item.conditions, params);
	}

	// The quest's stage's log entries that are whole (the entry's own quest and stage are these), in the stage's order.
	inline std::vector<Item*> ItemsOf(RE::TESQuest* a_quest, std::uint16_t a_stage)
	{
		std::vector<Item*> items;
		for (const auto stage : a_quest->stages) {
			if (!stage || Of(stage)->data.index != a_stage) {
				continue;
			}
			for (const auto entry : Of(stage)->items) {
				const auto item = static_cast<Item*>(entry);
				if (item && item->quest == a_quest && item->stage == stage) {
					items.push_back(item);
				}
			}
		}
		return items;
	}

	struct Journal
	{
		std::size_t entries = 0;  // the stage's log entries
		std::size_t logged = 0;   // of them, in the journal now
		std::size_t fresh = 0;    // of those, added by this call
	};

	// Puts the log entries of a stage marked done without its script (MarkDone) in our player's journal, as the game
	// does when it sets the stage (TESQuestStageItem::Run's journal half, in the stage's order): each entry with text
	// whose conditions hold here goes into the player's quest log through the game's own add (one there already stays
	// one), and the quest's current instance's journal stage and entry, what the Pip-Boy shows as the quest's
	// description, become this one's. The rest of Run is left out on purpose: no Complete or Fail Quest, no stage-item
	// event (that is what runs the stage's script). StageLog::LogPlan says which entries. Nothing is done if the game's
	// add does not begin as read. Main thread only.
	inline Journal LogEntries(RE::TESQuest* a_quest, std::uint16_t a_stage)
	{
		Journal    journal;
		const auto player = RE::PlayerCharacter::GetSingleton();
		if (!a_quest || !player || !AddToLogAsRead()) {
			return journal;
		}
		const auto                     items = ItemsOf(a_quest, a_stage);
		std::vector<StageLog::LogItem> plain;
		for (const auto item : items) {
			plain.push_back({ item->hasText, item->hasText && Holds(*item, player, a_quest), (item->flags & Item::kCompletes) != 0, item->nextQuest != nullptr });
		}
		journal.entries = items.size();
		using add_t = bool(RE::PlayerCharacter*, Item*);
		static REL::Relocation<add_t> add{ ADD_TO_LOG };
		for (const auto at : StageLog::LogPlan(plain)) {
			const auto item = items[at];
			if (add(player, item)) {
				++journal.fresh;
			}
			++journal.logged;
			for (const auto instance : a_quest->instanceData) {
				if (instance && instance->id == a_quest->currentInstanceID) {
					instance->journalStage = a_stage;
					*reinterpret_cast<std::uint8_t*>(&instance->journalStageItem) = item->index;  // a byte, as the game writes it (+628040)
					break;
				}
			}
		}
		return journal;
	}

	// What our player's journal holds of a quest, for the rig: its entries as stage:entry, newest first, the quest's
	// current instance's journal stage and entry (the Pip-Boy's description of it), and how many entries the journal
	// holds in all. Main thread only.
	inline std::string JournalText(RE::TESQuest* a_quest)
	{
		const auto player = RE::PlayerCharacter::GetSingleton();
		if (!a_quest || !player) {
			return "error: no player";
		}
		std::string held;
		std::size_t count = 0, total = 0;
		for (const auto entry : player->questLog) {  // through the non-const list: CommonLibF4's const begin() returns nothing
			const auto item = reinterpret_cast<const Item*>(entry);
			if (!item) {
				continue;
			}
			++total;
			if (item->quest == a_quest && item->stage) {
				held += std::format("{}{}:{}", held.empty() ? "" : ",", Of(item->stage)->data.index, item->index);
				++count;
			}
		}
		std::string description = "none";
		for (const auto instance : a_quest->instanceData) {
			if (instance && instance->id == a_quest->currentInstanceID) {
				description = std::format("{}:{}", instance->journalStage, instance->journalStageItem & 0xFF);
				break;
			}
		}
		return std::format("entries({})={} description={} journal={}", count, held.empty() ? "none" : held, description, total);
	}

	// Whether the quest has this stage at all (setting one it doesn't have does nothing: TESQuest::SetStage
	// finds no entry for it in the quest's stage table).
	inline bool Has(const RE::TESQuest* a_quest, std::uint16_t a_stage)
	{
		return std::ranges::any_of(a_quest->stages, [&](const RE::TESQuestStage* a_entry) { return a_entry && Of(a_entry)->data.index == a_stage; });
	}

	// The quest's start-up stages, ascending: those its record flags so (QUEST_STAGE_DATA::kStartUp, the INDX flag of the
	// game's files), which the game runs as the quest starts. A quest may have none (MQ105: setting its 10 starts it, and
	// its 0 never runs) or one that isn't its 0 (MQ205's 10).
	inline std::vector<std::uint16_t> StartUps(const RE::TESQuest* a_quest)
	{
		std::vector<std::uint16_t> startUps;
		for (const auto stage : a_quest->stages) {
			if (stage && Of(stage)->data.flags.all(RE::QUEST_STAGE_DATA::Flags::kStartUp)) {
				startUps.push_back(Of(stage)->data.index);
			}
		}
		std::ranges::sort(startUps);
		const auto repeats = std::ranges::unique(startUps);
		startUps.erase(repeats.begin(), repeats.end());
		return startUps;
	}

	// How many stages the quest has.
	inline std::size_t Count(const RE::TESQuest* a_quest)
	{
		return a_quest->stages.size();
	}
}
