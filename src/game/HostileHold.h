#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>

// A friend's stage that turns a faction on the player (StageRules::HOSTILE: a kick-out, an ending, a story stage that
// sets a place's people on the player) waits in this game until none of that faction is near, and is then set; it is
// never skipped (the maintainer's rule, 2026-11-11, in place of "set only beside the friend, else skipped"). What
// "near" is, and the wait's bookkeeping, kept free of the game so it can be tested: QuestSync reads the actors, judges
// the stage (StageRules::Judge, Verdict::kHostileWaits) and writes the log.
namespace HostileHold
{
	using Clock = std::chrono::steady_clock;

	// A member of the faction is near when it is alive, loaded and fully simulated (the game's high process: the loaded
	// cells around the player outdoors, or the interior they stand in) and within RANGE game units of the player (two
	// cells, about 115 m). One that near comes at the player the moment the faction turns; one farther, or in a cell that
	// isn't loaded, doesn't know. A dead one, a disabled one or a friend's stand-in doesn't count (the caller leaves them
	// out). The player in combat counts as near too (QuestSync: nothing that starts a fight is set mid-fight).
	inline constexpr float RANGE = 8192.0f;

	inline bool Near(float a_dx, float a_dy, float a_dz, bool a_alive)
	{
		return a_alive && a_dx * a_dx + a_dy * a_dy + a_dz * a_dz <= RANGE * RANGE;
	}

	// How long a held stage has waited, in play (time paused or loading doesn't count: no actor moves then), and when to
	// say so: a line as the hold begins, one every LOG_EVERY of play after (a player standing among the faction for an
	// hour sees in the log why the quest doesn't move), and the whole wait when the stage lands. A hold has no end of its
	// own: the maintainer's rule is that the stage waits, however long, and is never skipped.
	inline constexpr auto LOG_EVERY = std::chrono::minutes(5);

	class Waits
	{
	public:
		// A stage judged held now.
		struct Judged
		{
			bool            begun = false;   // the first time: the hold begins
			bool            logDue = false;  // a line is due (begun, or LOG_EVERY of play since the last)
			Clock::duration played{};        // how long it has waited so far
		};

		// Each poll: the play since the last one is added to every held stage, unless the game was paused meanwhile.
		void Tick(Clock::time_point a_now, bool a_paused)
		{
			const auto gap = last ? a_now - *last : Clock::duration::zero();
			last = a_now;
			if (a_paused) {
				return;
			}
			for (auto& [key, held] : stages) {
				held.played += gap;
			}
		}

		Judged Hold(std::uint64_t a_key)
		{
			const auto [it, inserted] = stages.try_emplace(a_key);
			auto&      held = it->second;
			const bool due = inserted || held.played - held.logged >= LOG_EVERY;
			if (due) {
				held.logged = held.played;
			}
			return { inserted, due, held.played };
		}

		// The stage was set, skipped or dropped: how long it waited, if it was held at all.
		std::optional<Clock::duration> End(std::uint64_t a_key)
		{
			const auto it = stages.find(a_key);
			if (it == stages.end()) {
				return std::nullopt;
			}
			const auto played = it->second.played;
			stages.erase(it);
			return played;
		}

		// Drops the held stages a_stillWaiting(key) says no (their quest went away: dropped, done by the player, finished).
		template <class StillWaiting>
		void Keep(StillWaiting a_stillWaiting)
		{
			std::erase_if(stages, [&](const auto& a_entry) { return !a_stillWaiting(a_entry.first); });
		}

		[[nodiscard]] bool        Holds(std::uint64_t a_key) const { return stages.contains(a_key); }
		[[nodiscard]] std::size_t Size() const { return stages.size(); }

		void Clear()
		{
			stages.clear();
			last.reset();
		}

	private:
		struct Held
		{
			Clock::duration played{};
			Clock::duration logged{};  // played as of the last line
		};

		std::unordered_map<std::uint64_t, Held> stages;
		std::optional<Clock::time_point>        last;
	};
}
