#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <unordered_map>

// A fix-up tried on an NPC our game copies whenever its owner's states say it is needed: switching on a copy that
// is off here (NpcSync::SwitchOn) or bringing one in from another cell (BringIn). Each runs a console command,
// and the states come ten or twenty times a second, so it is tried at most every a_interval. One that never takes
// (a copy a quest keeps off in our game, or one that can't be moved) used to be tried forever, and logged each
// time; it gets TRIES now, and is left alone until the copy loads or the session starts over. Pure, so it can be
// tested without the game.
namespace RetryBudget
{
	using Clock = std::chrono::steady_clock;

	constexpr std::uint32_t TRIES = 3;
	constexpr std::size_t   MAX_TRACKED = 1024;  // NPCs remembered; past that it starts over (a guard)

	enum class Verdict
	{
		kWait,   // tried moments ago, or given up: nothing now
		kFirst,  // try (and say so)
		kAgain,  // try quietly
		kLast,   // try, and say it is the last time
	};

	[[nodiscard]] constexpr bool Go(Verdict a_verdict) { return a_verdict != Verdict::kWait; }

	class Budget
	{
	public:
		explicit Budget(Clock::duration a_interval) :
			interval(a_interval)
		{}

		// Asked each time the fix-up looks needed.
		Verdict Ask(std::uint32_t a_id, Clock::time_point a_now)
		{
			auto it = entries.find(a_id);
			if (it == entries.end()) {
				if (entries.size() >= MAX_TRACKED) {
					entries.clear();
				}
				it = entries.emplace(a_id, Entry{}).first;
			}
			auto& entry = it->second;
			if (entry.tries >= TRIES || (entry.tries > 0 && a_now - entry.last < interval)) {
				return Verdict::kWait;
			}
			entry.last = a_now;
			++entry.tries;
			if (entry.tries == TRIES) {
				++gaveUp;
				return Verdict::kLast;
			}
			return entry.tries == 1 ? Verdict::kFirst : Verdict::kAgain;
		}

		// It worked (the copy loaded where it should be): a later need starts afresh.
		void Forget(std::uint32_t a_id) { entries.erase(a_id); }

		void Clear() { entries.clear(); }

		[[nodiscard]] std::uint32_t GaveUp() const { return gaveUp; }

	private:
		struct Entry
		{
			Clock::time_point last{};
			std::uint32_t     tries = 0;
		};

		Clock::duration                             interval;
		std::unordered_map<std::uint32_t, Entry>    entries;
		std::uint32_t                               gaveUp = 0;  // NPCs given their last try
	};

	// Our copy of a friend's NPC as one of its owner's states finds it (NpcSync::ApplyStates reads it from the game).
	struct Copy
	{
		bool exists = false;    // the NPC is in our game (a shareable ref)
		bool off = false;       // switched off here (disabled)
		bool unusable = false;  // deleted, dead or the player: never switched on
		bool here = false;      // loaded, in the cell or world the owner's copy is in
		bool movable = false;   // BringIn may move it: the owner's copy is outdoors near us in our world, and ours is on,
		                        // alive, not our companion, not in a scene here and not fighting us
	};

	// What to do about one of the owner's states.
	struct Fixes
	{
		Verdict switchOn = Verdict::kWait;
		Verdict bringIn = Verdict::kWait;
	};

	// The fix-ups one of the owner's states calls for, asked of each budget. One that took (the copy is on, or here)
	// is forgotten, so a later need (a script switches it off again, the companion is left behind again) starts
	// with fresh tries; one with nothing to do (a copy that can't be switched on or moved) leaves its budget as it is.
	inline Fixes Plan(Budget& a_switchOns, Budget& a_bringIns, std::uint32_t a_id, const Copy& a_copy, Clock::time_point a_now)
	{
		Fixes fixes;
		if (a_copy.exists && !a_copy.off) {
			a_switchOns.Forget(a_id);
		} else if (a_copy.exists && !a_copy.unusable) {
			fixes.switchOn = a_switchOns.Ask(a_id, a_now);
		}
		if (a_copy.here) {
			a_bringIns.Forget(a_id);
		} else if (a_copy.movable) {
			fixes.bringIn = a_bringIns.Ask(a_id, a_now);
		}
		return fixes;
	}
}
