#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <vector>

// Quest stages: which ones are done, and the stages from other players waiting to be set here. Kept
// free of the game so it can be tested.
namespace StageLog
{
	using Clock = std::chrono::steady_clock;
	using Stages = std::vector<std::uint16_t>;  // ascending, no repeats

	// The stages done now that were not done at the last look, ascending; a_known becomes a_now. A stage
	// is only progress once: a stage's script often sets an earlier stage too (MQ104's 17 sets 12), and
	// the next look must not count that again. A stage no longer done (its quest was reset) is forgotten.
	inline Stages NewlyDone(Stages& a_known, Stages a_now)
	{
		std::ranges::sort(a_now);
		const auto repeats = std::ranges::unique(a_now);
		a_now.erase(repeats.begin(), repeats.end());
		Stages fresh;
		std::ranges::set_difference(a_now, a_known, std::back_inserter(fresh));
		a_known = std::move(a_now);
		return fresh;
	}

	// Adds a stage to a list (one set here from another player, so the next look doesn't report it).
	inline void Insert(Stages& a_stages, std::uint16_t a_stage)
	{
		const auto at = std::ranges::lower_bound(a_stages, a_stage);
		if (at == a_stages.end() || *at != a_stage) {
			a_stages.insert(at, a_stage);
		}
	}

	// What a quest in this game says about the stages waiting for it.
	enum class Quest
	{
		kGone,   // nothing more to set: the quest isn't here or shared, or it is completed
		kWait,   // not yet: it isn't running or started here, or the player is in a conversation
		kReady,  // a stage can be set now
	};

	// Stages from other players, set here in order: per quest the lowest first, and the next only once
	// that one reads done here (or TIMEOUT after it was set). One stage at a time per quest, at most
	// MAX_PER_POLL in all per poll, the quests taking turns. Only setting the highest stage skipped the
	// ones between, and with them what their scripts do: MQ104's 210 starts MQ105, so a friend who only
	// got MQ104's last stage never got MQ105, and its stages waited for it forever.
	class Queue
	{
	public:
		static constexpr auto        TIMEOUT = std::chrono::seconds(5);
		static constexpr std::size_t MAX_PER_POLL = 4;
		static constexpr std::size_t MAX_WAITING = 16384;  // stages in all; more are refused

		struct Stage
		{
			std::uint32_t quest = 0;
			std::uint16_t stage = 0;

			bool operator==(const Stage&) const = default;
		};

		// A stage from another player. False if it is already waiting, or the queue is full.
		bool Add(std::uint32_t a_quest, std::uint16_t a_stage)
		{
			if (count >= MAX_WAITING) {
				return false;
			}
			if (!quests[a_quest].stages.insert(a_stage).second) {
				return false;
			}
			++count;
			return true;
		}

		// One poll. a_quest(quest) -> Quest; a_settled(quest, stage): the stage needs nothing more here
		// (done, or the quest is already past it); a_set(quest, stage): sets it, true if it did. A ready
		// quest's settled stages are dropped, and all of a quest that is gone. Returns the stages given up
		// on: set, but not done TIMEOUT later.
		template <class QuestFn, class SettledFn, class SetFn>
		std::vector<Stage> Poll(Clock::time_point a_now, QuestFn&& a_quest, SettledFn&& a_settled, SetFn&& a_set)
		{
			std::vector<Stage> gaveUp;
			std::size_t        set = 0;
			for (const auto id : InTurn()) {
				const auto it = quests.find(id);
				auto&      entry = it->second;
				const auto state = a_quest(id);
				if (state == Quest::kGone) {
					count -= entry.stages.size();
					quests.erase(it);
					continue;
				}
				if (entry.setting) {
					if (a_settled(id, *entry.setting)) {
						count -= entry.stages.erase(*entry.setting);
						entry.setting.reset();
					} else if (a_now - entry.since >= TIMEOUT) {
						gaveUp.push_back({ id, *entry.setting });
						count -= entry.stages.erase(*entry.setting);
						entry.setting.reset();
					} else {
						continue;  // still waiting for it to be done
					}
				}
				if (state == Quest::kReady) {
					count -= std::erase_if(entry.stages, [&](std::uint16_t a_stage) { return a_settled(id, a_stage); });
				}
				if (entry.stages.empty()) {
					quests.erase(it);
					continue;
				}
				// A quest that can't take a stage yet is left as it is: it may have many waiting.
				if (state != Quest::kReady || set >= MAX_PER_POLL) {
					continue;
				}
				const auto stage = *entry.stages.begin();
				if (a_set(id, stage)) {
					entry.setting = stage;
					entry.since = a_now;
					++set;
					last = id;
				}
			}
			return gaveUp;
		}

		// The stages waiting for a quest, ascending (the one being set included).
		[[nodiscard]] Stages Waiting(std::uint32_t a_quest) const
		{
			const auto it = quests.find(a_quest);
			return it == quests.end() ? Stages{} : Stages(it->second.stages.begin(), it->second.stages.end());
		}

		[[nodiscard]] bool Contains(std::uint32_t a_quest) const { return quests.contains(a_quest); }

		[[nodiscard]] std::size_t Size() const { return count; }

		// Stages set and not yet done.
		[[nodiscard]] std::size_t Setting() const
		{
			return static_cast<std::size_t>(std::ranges::count_if(quests, [](const auto& a_entry) { return a_entry.second.setting.has_value(); }));
		}

		void Clear()
		{
			quests.clear();
			count = 0;
			last = 0;
		}

	private:
		struct Entry
		{
			std::set<std::uint16_t>      stages;
			std::optional<std::uint16_t> setting;  // set, waiting for it to read done
			Clock::time_point            since{};
		};

		// The quests, starting after the one that last had a stage set, so each gets its turn.
		std::vector<std::uint32_t> InTurn() const
		{
			std::vector<std::uint32_t> ids;
			ids.reserve(quests.size());
			const auto from = quests.upper_bound(last);
			for (auto it = from; it != quests.end(); ++it) {
				ids.push_back(it->first);
			}
			for (auto it = quests.begin(); it != from; ++it) {
				ids.push_back(it->first);
			}
			return ids;
		}

		std::map<std::uint32_t, Entry> quests;
		std::size_t                    count = 0;
		std::uint32_t                  last = 0;
	};
}
