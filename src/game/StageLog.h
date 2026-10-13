#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <utility>
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

	// QUEST_DATA::flags bits: the quest is running, the quest is completed.
	inline constexpr std::uint16_t QUEST_RUNNING = 0x0001;
	inline constexpr std::uint16_t QUEST_COMPLETED = 0x0002;

	// A quest with nothing more to set: completed and stopped. Many quests complete at a stage and keep
	// running, with stages after it that carry the story on (MQ102 completes at 65; its 150 starts MQ103),
	// and TESQuest::SetStage only needs the running bit.
	inline bool Finished(std::uint16_t a_flags)
	{
		return (a_flags & QUEST_COMPLETED) && !(a_flags & QUEST_RUNNING);
	}

	// What a quest in this game says about the stages waiting for it.
	enum class Quest
	{
		kGone,   // nothing more to set: the quest isn't here or shared, or it is finished
		kWait,   // not yet: it isn't running or started here, or the player is in a conversation
		kReady,  // a stage can be set now
	};

	// What became of a stage whose turn came.
	enum class Set
	{
		kSet,         // set; the next once it reads done
		kSkipped,     // dropped: it isn't to be set here
		kLater,       // not now; nothing that came after it is set either
		kQuestWaits,  // not now; its quest waits, the other quests go on
	};

	// Stages from other players, set here in the order they came (the order they were done in): per
	// quest the lowest first, and the next only once that one reads done here (or TIMEOUT after it was
	// set). A stage reads done before its script has run: the game hands the script to Papyrus, which
	// runs it a moment later, and a script that waits (Utility.Wait, moving the player) runs on for
	// seconds. So after a stage with such a script nothing that came after it is set for its hold time,
	// or a later stage would overtake it (MQ206's 1200 stops the quest that 1000 still needs). Several
	// quests at a time, at most MAX_PER_POLL stages per poll; a quest that can't take a stage yet holds
	// nothing up. Only setting the highest stage skipped the ones between, and with them what their
	// scripts do: MQ104's 210 starts MQ105, so a friend who only got MQ104's last stage never got MQ105,
	// and its stages waited for it forever. Scripts don't run while the game is paused (a menu, a
	// loading screen), so a paused poll moves the holds and timeouts on by the time that passed.
	class Queue
	{
	public:
		static constexpr auto        TIMEOUT = std::chrono::seconds(5);
		static constexpr auto        MAX_HOLD = std::chrono::seconds(30);  // the longest wait in StageRules is 25 s
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
			if (!quests[a_quest].stages.try_emplace(a_stage, next).second) {
				return false;
			}
			++next;
			++count;
			return true;
		}

		// One poll. a_game has Quest(quest) -> Quest; Settled(quest, stage): the stage needs nothing
		// more here (done, or the quest is already past it); Set(quest, stage) -> Set; Hold(quest,
		// stage) -> Clock::duration: how long its script may run on once it reads done. A ready quest's
		// settled stages are dropped, and all of a quest that is gone. Returns the stages given up on:
		// set, but not done TIMEOUT later. While the game is paused nothing is set, and the time since
		// the last poll doesn't count against any hold or timeout.
		template <class Game>
		std::vector<Stage> Poll(Clock::time_point a_now, Game& a_game, bool a_paused = false)
		{
			const auto sinceLast = lastPoll ? a_now - *lastPoll : Clock::duration::zero();
			lastPoll = a_now;
			if (a_paused) {
				for (auto& [id, entry] : quests) {
					entry.since += sinceLast;
					entry.holdUntil += sinceLast;
				}
				return {};
			}
			std::vector<Stage>                                    gaveUp;
			std::vector<std::pair<std::uint64_t, std::uint32_t>> ready;  // (when its first stage came, quest)
			std::uint64_t                                         blockedFrom = NONE;  // what came from here on waits
			for (auto it = quests.begin(); it != quests.end();) {
				const auto id = it->first;
				auto&      entry = it->second;
				const auto state = a_game.Quest(id);
				if (state == Quest::kGone) {
					count -= entry.stages.size();
					it = quests.erase(it);
					continue;
				}
				if (entry.setting) {
					if (a_game.Settled(id, entry.setting->stage)) {
						count -= entry.stages.erase(entry.setting->stage);
						entry.holdUntil = a_now + entry.setting->hold;
						entry.holdFrom = entry.setting->from;
						entry.setting.reset();
					} else if (a_now - entry.since >= TIMEOUT) {
						gaveUp.push_back({ id, entry.setting->stage });
						count -= entry.stages.erase(entry.setting->stage);
						entry.setting.reset();
					}
				}
				const bool holding = a_now < entry.holdUntil;
				if (entry.setting && entry.setting->hold > Clock::duration::zero()) {
					blockedFrom = (std::min)(blockedFrom, entry.setting->from);
				} else if (holding) {
					blockedFrom = (std::min)(blockedFrom, entry.holdFrom);
				}
				if (state == Quest::kReady && !entry.setting) {
					count -= std::erase_if(entry.stages, [&](const auto& a_stage) { return a_game.Settled(id, a_stage.first); });
				}
				if (entry.stages.empty() && !holding) {
					it = quests.erase(it);
					continue;
				}
				if (state == Quest::kReady && !entry.setting && !holding && !entry.stages.empty()) {
					ready.emplace_back(First(entry), id);
				}
				++it;
			}

			std::ranges::sort(ready);
			std::size_t set = 0;
			for (const auto& [from, id] : ready) {
				if (from >= blockedFrom || set >= MAX_PER_POLL) {
					break;
				}
				auto&      entry = quests.at(id);
				const auto stage = entry.stages.begin()->first;
				switch (a_game.Set(id, stage)) {
				case Set::kSet:
					{
						const auto hold = (std::min)(Clock::duration(a_game.Hold(id, stage)), Clock::duration(MAX_HOLD));
						entry.setting = InFlight{ stage, from, hold };
						entry.since = a_now;
						++set;
						if (hold > Clock::duration::zero()) {
							blockedFrom = from;
						}
						break;
					}
				case Set::kSkipped:
					entry.stages.erase(entry.stages.begin());
					--count;
					break;
				case Set::kLater:
					blockedFrom = from;
					break;
				case Set::kQuestWaits:
					break;
				}
			}
			std::erase_if(quests, [&](const auto& a_entry) { return a_entry.second.stages.empty() && a_now >= a_entry.second.holdUntil; });
			return gaveUp;
		}

		// The stages waiting for a quest, ascending (the one being set included).
		[[nodiscard]] Stages Waiting(std::uint32_t a_quest) const
		{
			Stages     stages;
			const auto it = quests.find(a_quest);
			if (it != quests.end()) {
				for (const auto& stage : it->second.stages) {
					stages.push_back(stage.first);
				}
			}
			return stages;
		}

		[[nodiscard]] bool Contains(std::uint32_t a_quest) const
		{
			const auto it = quests.find(a_quest);
			return it != quests.end() && !it->second.stages.empty();
		}

		[[nodiscard]] bool Contains(std::uint32_t a_quest, std::uint16_t a_stage) const
		{
			const auto it = quests.find(a_quest);
			return it != quests.end() && it->second.stages.contains(a_stage);
		}

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
		}

		// Drops every quest but these (the one whose stage took the player's controls, and the one
		// whose stage gives them back).
		void Keep(std::initializer_list<std::uint32_t> a_quests)
		{
			for (auto it = quests.begin(); it != quests.end();) {
				if (std::ranges::find(a_quests, it->first) == a_quests.end()) {
					count -= it->second.stages.size();
					it = quests.erase(it);
				} else {
					++it;
				}
			}
		}

	private:
		static constexpr std::uint64_t NONE = (std::numeric_limits<std::uint64_t>::max)();

		struct InFlight
		{
			std::uint16_t   stage = 0;
			std::uint64_t   from = 0;  // its quest's place in the order when it was set
			Clock::duration hold{};
		};

		struct Entry
		{
			std::map<std::uint16_t, std::uint64_t> stages;  // stage -> when it came (a count)
			std::optional<InFlight>                setting;  // set, waiting for it to read done
			Clock::time_point                      since{};
			Clock::time_point                      holdUntil{};  // the last stage's script may still be running
			std::uint64_t                          holdFrom = 0;
		};

		// A quest's place in the order: when the first of its waiting stages came.
		static std::uint64_t First(const Entry& a_entry)
		{
			std::uint64_t first = NONE;
			for (const auto& stage : a_entry.stages) {
				first = (std::min)(first, stage.second);
			}
			return first;
		}

		std::map<std::uint32_t, Entry>   quests;
		std::size_t                      count = 0;
		std::uint64_t                    next = 0;
		std::optional<Clock::time_point> lastPoll;
	};
}
