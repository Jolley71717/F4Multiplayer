#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <tuple>
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

	// Whether other players' stages of a quest are set here: a quest the session shares (the story's only
	// while the host shares it), or one of a lock's (Queue::Lock), whatever the story is now. A stage set
	// here took the player's controls and only its release gives them back, so a teleport begun in a
	// session finishes after leaving it, when the story is the player's own again.
	inline bool Settable(bool a_shared, bool a_lockQuest)
	{
		return a_shared || a_lockQuest;
	}

	// Reports on their way to the server (the stages done here, the quests completed). A stage is seen
	// done only once, so a report made while the connection is down in a session (the welcome still to
	// come, a reconnect) is kept for when it is back: dropped, what its script starts (MQ104's 210 starts
	// MQ105) would wait forever in the others' games. That holds for the story's stages too, although the
	// story is the player's own while the connection is down (Story::SetShared(false)): they are kept
	// marked as the story's, and go only if the welcome says the session shares the story.
	//
	// One made in no session, playing alone, is no session's: it is dropped, so a session joined later
	// doesn't take the player's solo progress for a friend's and set it in everyone's game. So is what was
	// kept for a session the player then left for another (NewSession: a friend's invite accepted while
	// the last host was being retried), and what a connection down for RECONNECT_GRACE kept: past that,
	// the player is playing alone (their host quit), and nothing of it goes when the host is back.
	//
	// They go at most MAX_PER_SECOND a second, oldest first: the server drops whatever a player sends past
	// its per-second budget of events, and a stage dropped there is never reported again.
	template <class Report>
	class Outbox
	{
	public:
		static constexpr std::size_t MAX_UNSENT = 1024;  // kept, the oldest first (what came first starts what came after)
		static constexpr auto        RECONNECT_GRACE = std::chrono::minutes(5);
		static constexpr std::size_t MAX_PER_SECOND = 200;  // the server's budget is 500 events a second, all kinds together

		// The connection, as the reports see it.
		struct Link
		{
			bool sending = false;      // connected and welcomed
			bool inSession = false;    // in a session, or joining or rejoining one (a host's own game hosting one)
			bool sharedStory = false;  // the session shares the story (the welcome says)
		};

		// a_story: a stage of the main story or of a faction's quest, for a session that shares the story only.
		void Add(Report a_report, bool a_story = false)
		{
			if (reports.size() < MAX_UNSENT) {
				reports.push_back({ std::move(a_report), a_story });
			}
		}

		// The reports to send now.
		std::vector<Report> Take(Clock::time_point a_now, const Link& a_link)
		{
			if (!a_link.sending) {
				if (!a_link.inSession) {
					reports.clear();
					downSince.reset();
				} else if (!downSince) {
					downSince = a_now;
				} else if (a_now - *downSince >= RECONNECT_GRACE) {
					reports.clear();
				}
				return {};
			}
			downSince.reset();
			if (!a_link.sharedStory) {
				std::erase_if(reports, [](const Entry& a_entry) { return a_entry.story; });
			}
			if (a_now - windowStart >= std::chrono::seconds(1)) {
				windowStart = a_now;
				sentThisSecond = 0;
			}
			const auto          count = (std::min)(reports.size(), MAX_PER_SECOND - sentThisSecond);
			std::vector<Report> out;
			out.reserve(count);
			for (std::size_t i = 0; i < count; ++i) {
				out.push_back(std::move(reports[i].report));
			}
			reports.erase(reports.begin(), reports.begin() + static_cast<std::ptrdiff_t>(count));
			sentThisSecond += count;
			return out;
		}

		// Progress of the game before a load.
		void Clear() { reports.clear(); }

		// The server address changed (an invite accepted, hosting started or stopped, leaving): nothing
		// kept for the last session goes to the next.
		void NewSession()
		{
			reports.clear();
			downSince.reset();
		}

		[[nodiscard]] std::size_t Size() const { return reports.size(); }

	private:
		struct Entry
		{
			Report report;
			bool   story = false;
		};

		std::vector<Entry>               reports;
		std::optional<Clock::time_point> downSince;  // not sending since, in a session
		Clock::time_point                windowStart{};
		std::size_t                      sentThisSecond = 0;
	};

	// Stages a quest reached here shortly before the player started it (before its first objective was
	// shown). Only a started quest's stages are reported, since the game moves many quests on in the
	// background long before the player comes across them. But the stage whose script shows the first
	// objective reads done before that script has run, and the conversation that starts a quest sets
	// stages before it: Vault 81's intercom talk sets its stages, then 55 opens the vault door and shows
	// the objective. Dropped, they never reached the friends, and the vault door stayed shut in their
	// games. They are kept for WINDOW, and reported with the quest's next stages if it is started by then.
	class Unstarted
	{
	public:
		static constexpr auto WINDOW = std::chrono::seconds(60);

		void Add(std::uint32_t a_quest, const Stages& a_stages, Clock::time_point a_now)
		{
			auto& kept = quests[a_quest];
			for (const auto stage : a_stages) {
				kept.push_back({ stage, a_now });
			}
		}

		// The quest has started: the stages kept for it (those within WINDOW) and a_fresh, ascending.
		Stages Take(std::uint32_t a_quest, Stages a_fresh, Clock::time_point a_now)
		{
			const auto it = quests.find(a_quest);
			if (it == quests.end()) {
				return a_fresh;
			}
			for (const auto& kept : it->second) {
				if (a_now - kept.at < WINDOW) {
					Insert(a_fresh, kept.stage);
				}
			}
			quests.erase(it);
			return a_fresh;
		}

		// Drops what is older than WINDOW: those quests weren't started in time.
		void Expire(Clock::time_point a_now)
		{
			for (auto it = quests.begin(); it != quests.end();) {
				std::erase_if(it->second, [&](const Kept& a_kept) { return a_now - a_kept.at >= WINDOW; });
				it = it->second.empty() ? quests.erase(it) : std::next(it);
			}
		}

		void Clear() { quests.clear(); }

		[[nodiscard]] bool Has(std::uint32_t a_quest) const { return quests.contains(a_quest); }

		[[nodiscard]] std::size_t Size() const
		{
			std::size_t size = 0;
			for (const auto& [quest, kept] : quests) {
				size += kept.size();
			}
			return size;
		}

	private:
		struct Kept
		{
			std::uint16_t     stage = 0;
			Clock::time_point at{};
		};

		std::map<std::uint32_t, std::vector<Kept>> quests;
	};

	// What a quest in this game says about the stages waiting for it.
	enum class Quest
	{
		kGone,   // nothing more to set: the quest isn't here or shared, or it is finished
		kWait,   // not yet: it isn't running or started here, or the player is in a conversation
		kReady,  // a stage can be set now
	};

	// Why other players' stages of a quest wait here (kNone: they don't).
	enum class Wait
	{
		kNone,
		kLoading,     // a loading screen or the main menu: no quest script may run
		kOpening,     // a new character still in the opening
		kNotRunning,  // the quest isn't running here
		kNotStarted,  // the player hasn't started it here: none of its objectives has been shown
		kBusy,        // the player is in a conversation or a scene
	};

	// A quest with stages from other players waiting, as this game has it.
	struct QuestHere
	{
		bool inWorld = true;
		bool opening = false;
		bool running = false;
		bool started = false;  // one of its objectives has been shown
		bool busy = false;
	};

	// Friends' stages wait until the player has started the quest too (the game runs many quests in the
	// background long before the player comes across them), and not in a conversation or scene. Quest
	// scripts must not run at the main menu or during a loading screen. The reason is logged: a friend
	// whose quest waits silently sees nothing happen (Vault 81's door stayed shut for one who hadn't
	// started the quest).
	inline Wait WaitsFor(const QuestHere& a_quest)
	{
		if (!a_quest.inWorld) {
			return Wait::kLoading;
		}
		if (a_quest.opening) {
			return Wait::kOpening;
		}
		if (!a_quest.running) {
			return Wait::kNotRunning;
		}
		if (!a_quest.started) {
			return Wait::kNotStarted;
		}
		return a_quest.busy ? Wait::kBusy : Wait::kNone;
	}

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
	//
	// A stage set here can take the player's controls away until a later stage gives them back (MQ206's
	// 500 until its 1200, StageRules::LOCKS). While it does (Lock), its quests' stages go first and
	// nothing of other quests holds them up, and if they do nothing for LOCK_TIMEOUT of play (nothing
	// being set, no script of theirs running on, the game not paused) the lock is overdue: its release
	// is to be set outright rather than leave the player unable to move.
	class Queue
	{
	public:
		static constexpr auto        TIMEOUT = std::chrono::seconds(5);
		static constexpr auto        MAX_HOLD = std::chrono::seconds(30);  // the longest wait in StageRules is 25 s
		static constexpr std::size_t MAX_PER_POLL = 4;
		static constexpr std::size_t MAX_WAITING = 16384;  // stages in all; more are refused
		// MQ206's teleport between its 500 and 1200 takes about 10 s, its loading screen not counted.
		static constexpr auto LOCK_TIMEOUT = std::chrono::seconds(90);

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
		// the last poll doesn't count against any hold or timeout, nor the lock's.
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
			std::vector<Stage>                                                gaveUp;
			std::vector<std::tuple<bool, std::uint64_t, std::uint32_t>>      ready;  // (not the lock's, when its first stage came, quest)
			std::uint64_t                                                     blockedFrom = NONE;  // what came from here on waits
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
					ready.emplace_back(!Locks(id), First(entry), id);
				}
				++it;
			}

			std::ranges::sort(ready);
			std::size_t set = 0;
			for (const auto& [other, from, id] : ready) {
				// The lock's quests come first, and nothing of another quest holds them up: the player can't
				// move until its release is set (a combat hold of a quest that came before it, say).
				if ((other && from >= blockedFrom) || set >= MAX_PER_POLL) {
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
							blockedFrom = (std::min)(blockedFrom, from);
						}
						break;
					}
				case Set::kSkipped:
					entry.stages.erase(entry.stages.begin());
					--count;
					break;
				case Set::kLater:
					blockedFrom = (std::min)(blockedFrom, from);  // a lock's stage, sorted first, can have come after a hold that still runs
					break;
				case Set::kQuestWaits:
					break;
				}
			}
			std::erase_if(quests, [&](const auto& a_entry) { return a_entry.second.stages.empty() && a_now >= a_entry.second.holdUntil; });

			// The lock's time runs only while nothing of its quests is being set or running on.
			if (lock) {
				const bool trying = std::ranges::any_of(quests, [&](const auto& a_entry) {
					return Locks(a_entry.first) && (a_entry.second.setting || a_now < a_entry.second.holdUntil);
				});
				lock->idle = trying ? Clock::duration::zero() : lock->idle + sinceLast;
			}
			return gaveUp;
		}

		// A stage of a_quest set here took the player's controls away; a stage of a_release gives them
		// back. Until Unlock.
		void Lock(std::uint32_t a_quest, std::uint32_t a_release) { lock = LockState{ a_quest, a_release }; }

		void Unlock() { lock.reset(); }

		// The lock's quests have done nothing for LOCK_TIMEOUT of play: set its release outright.
		[[nodiscard]] bool LockOverdue() const { return lock && lock->idle >= LOCK_TIMEOUT; }

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
			lock.reset();
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

		struct LockState
		{
			std::uint32_t   quest = 0;
			std::uint32_t   release = 0;
			Clock::duration idle{};  // play since a stage of its quests was last being set or running on
		};

		[[nodiscard]] bool Locks(std::uint32_t a_quest) const { return lock && (a_quest == lock->quest || a_quest == lock->release); }

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
		std::optional<LockState>         lock;
	};
}
