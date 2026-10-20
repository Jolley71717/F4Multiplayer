#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>

// The screen held black: when a fade the session caused is never faded back in, fade it in. The game's quest
// scripts fade the screen to black and leave it so (Game.FadeOutGame with abStayFaded) until a later stage, a
// trigger or a timer of theirs fades it back in. A stage the session sets from a friend's game, or a teleport
// to a friend, can start such a fade where nothing will end it: the player is left in the dark with the HUD
// gone. So once the session has set a stage or moved the player, a fade that comes up soon after (or is up
// then) and stays up for STUCK_AFTER of play is faded back in, at most MAX_TRIES times. A fade the session had
// no hand in is only logged: the game's own scripts may mean it. Free of the game, so it can be tested.
namespace FadeGuard
{
	using Clock = std::chrono::steady_clock;

	// A fade that comes up this long (of play) after the session set a stage or moved the player is the session's.
	inline constexpr auto ARM_FOR = std::chrono::seconds(15);
	// The game's own fades last a few seconds (the longest the quests hold the screen black is about 10 s); one the
	// session caused that stays up this long of play is faded back in.
	inline constexpr auto STUCK_AFTER = std::chrono::seconds(20);
	inline constexpr int  MAX_TRIES = 2;
	// A fade the session didn't cause, up this long of play, is logged (once).
	inline constexpr auto LONG_FADE = std::chrono::seconds(30);
	// One frame counts at most this much: a hitch, a hang or a debugger isn't play.
	inline constexpr auto MAX_STEP = std::chrono::milliseconds(250);

	enum class Act : std::uint8_t
	{
		kNone,
		kFadeIn,    // the session's fade has been up STUCK_AFTER: fade the screen back in
		kLongFade,  // a fade the session didn't cause has been up LONG_FADE (log it)
		kCleared,   // a fade that was up a while (see Guard::Up) has gone
	};

	struct Screen
	{
		bool faded = false;   // the game's fader is up (FaderMenu open)
		bool paused = false;  // no play: a loading screen, a menu that pauses the game, the opening (its own fades)
	};

	class Guard
	{
	public:
		// The session set a stage here or moved the player.
		void Arm()
		{
			armLeft = ARM_FOR;
			if (faded) {
				caused = true;  // up already: the session's step may have broken what was to fade it back in
			}
		}

		Act Step(Clock::time_point a_now, const Screen& a_screen)
		{
			const auto step = last ? std::clamp(a_now - *last, Clock::duration::zero(), Clock::duration(MAX_STEP)) : Clock::duration::zero();
			last = a_now;
			if (!a_screen.paused) {
				armLeft = (std::max)(armLeft - step, Clock::duration::zero());
			}

			if (!a_screen.faded) {
				const bool wasUp = faded;
				faded = false;
				if (wasUp && up >= LONG_ENOUGH_TO_LOG) {
					lastUp = up;
					lastCaused = caused;
					lastTries = tries;
					Reset();
					return Act::kCleared;
				}
				Reset();
				return Act::kNone;
			}

			if (!faded) {
				faded = true;
				caused = armLeft > Clock::duration::zero();
				up = Clock::duration::zero();
				tries = 0;
				longLogged = false;
			}
			if (a_screen.paused) {
				return Act::kNone;
			}
			up += step;
			if (caused) {
				if (tries < MAX_TRIES && up >= STUCK_AFTER * (tries + 1)) {
					++tries;
					++fadedIn;
					return Act::kFadeIn;
				}
			} else if (!longLogged && up >= LONG_FADE) {
				longLogged = true;
				++longFades;
				return Act::kLongFade;
			}
			return Act::kNone;
		}

		// The fade up now: whether it is, for how long of play, whether the session caused it, how often faded in.
		[[nodiscard]] bool              Faded() const { return faded; }
		[[nodiscard]] Clock::duration   Up() const { return up; }
		[[nodiscard]] bool              Caused() const { return caused; }
		[[nodiscard]] int               Tries() const { return tries; }
		[[nodiscard]] bool              Armed() const { return armLeft > Clock::duration::zero(); }
		// The last fade that went (Act::kCleared).
		[[nodiscard]] Clock::duration   LastUp() const { return lastUp; }
		[[nodiscard]] bool              LastCaused() const { return lastCaused; }
		[[nodiscard]] int               LastTries() const { return lastTries; }
		[[nodiscard]] std::uint32_t     FadedIn() const { return fadedIn; }
		[[nodiscard]] std::uint32_t     LongFades() const { return longFades; }

		// A fade up this long of play is logged when it goes.
		static constexpr auto LONG_ENOUGH_TO_LOG = std::chrono::seconds(5);

	private:
		void Reset()
		{
			caused = false;
			up = Clock::duration::zero();
			tries = 0;
			longLogged = false;
		}

		std::optional<Clock::time_point> last;
		Clock::duration                  armLeft = Clock::duration::zero();
		bool                             faded = false;
		bool                             caused = false;
		bool                             longLogged = false;
		Clock::duration                  up = Clock::duration::zero();
		int                              tries = 0;
		Clock::duration                  lastUp = Clock::duration::zero();
		bool                             lastCaused = false;
		int                              lastTries = 0;
		std::uint32_t                    fadedIn = 0;
		std::uint32_t                    longFades = 0;
	};
}
