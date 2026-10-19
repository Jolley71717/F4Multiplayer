#pragma once

#include <chrono>
#include <optional>

// Whether the key reads (GetAsyncKeyState) reach the plugin at all. A player who walks around with the keyboard holds
// W, A, S or D, Space or a mouse button down sooner or later; when the game has been in play (its window in front,
// no menu) for LIMIT and not one of them, nor a hotkey, was ever seen down, the reads come back empty: on Wine, an
// input path that keeps keys from the async key state (old Wine's DirectInput eats exclusive-mode keys in a hook) or
// a desktop that never puts the game's window in front. The log says so once, and once again if a key is seen after
// all. Pure, so it is unit tested; Hotkeys.cpp feeds it every frame.
namespace KeyWatch
{
	using Clock = std::chrono::steady_clock;

	enum class Event
	{
		kNone,
		kNothingSeen,   // LIMIT of play and no key ever seen down
		kSeenAfterAll,  // a key seen down after kNothingSeen was told
	};

	class Watch
	{
	public:
		static constexpr auto LIMIT = std::chrono::seconds(180);
		// A frame further apart than this from the last (a load, the game paused in the background) counts only this
		// much: the limit is about time in play, not time the game was running.
		static constexpr auto LONGEST_STEP = std::chrono::seconds(1);

		// Once a frame: whether the game is in play (window in front, no menu), and whether any watched key is down.
		Event Tick(Clock::time_point a_now, bool a_inPlay, bool a_anyDown)
		{
			const auto step = last ? std::min<Clock::duration>(a_now - *last, LONGEST_STEP) : Clock::duration::zero();
			last = a_now;
			if (a_anyDown) {
				const bool after = !seen && warned;
				seen = true;
				return after ? Event::kSeenAfterAll : Event::kNone;
			}
			if (seen || warned || !a_inPlay) {
				return Event::kNone;
			}
			inPlay += step;
			if (inPlay >= LIMIT) {
				warned = true;
				return Event::kNothingSeen;
			}
			return Event::kNone;
		}

		[[nodiscard]] bool Seen() const { return seen; }

	private:
		std::optional<Clock::time_point> last;
		Clock::duration                  inPlay{};
		bool                             seen = false;
		bool                             warned = false;
	};
}
