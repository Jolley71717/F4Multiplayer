#pragma once

#include <chrono>
#include <cstdint>

// A stand-in's weapon drawn or put away to match its friend's (Puppets::MatchWeaponDrawn). Each draw and each holster
// plays the weapon's own draw or holster sound. It was asked again every second for as long as the stand-in's state
// differed from the friend's, with no end: a stand-in whose graph never takes the draw, or that something puts away
// again each time (an equip that is still going on, a graph that is not set up), clicked once a second for the rest of
// the session. Now a wanted state gets MAX_TRIES asks; past them it is let be until the friend draws or holsters
// again, or the stand-in's gear changes (a redraw); only a holster is still asked, every SLOW_HOLSTER, so a weapon
// never stays out on a friend who put theirs away. A state that held for STABLE gives the next mismatch its asks
// back. Kept free of the game so it can be tested.
namespace DrawMatch
{
	using Clock = std::chrono::steady_clock;
	using namespace std::chrono_literals;

	inline constexpr auto         RETRY = 1s;   // drawing takes a moment, and fails while the equipment is being changed
	inline constexpr std::uint8_t MAX_TRIES = 4;
	inline constexpr auto         STABLE = 10s;
	inline constexpr auto         SLOW_HOLSTER = 10s;   // once given up, a holster the friend wants is still asked this often

	enum class Do : std::uint8_t
	{
		kNothing,
		kDraw,
		kHolster,
	};

	struct Track
	{
		bool              known = false;
		bool              want = false;     // the friend's state the tries are for
		std::uint8_t      tries = 0;        // asks for it so far
		bool              gaveUp = false;   // MAX_TRIES asked and still not so: let be
		Clock::time_point next{};           // no ask before then
		Clock::time_point matchedSince{};   // the stand-in has matched since then (zero while it doesn't)
		std::uint32_t     asks = 0;         // dev: every draw and holster asked
		std::uint32_t     giveUps = 0;      // dev: states given up on
	};

	// What to ask of the stand-in now. a_want: the friend's weapon is out; a_drawn: the stand-in's is; a_redraw: its gear
	// changed, so it holsters (if drawn) and draws again; a_notBefore: no ask before then (an equip or a get-up playing).
	constexpr Do Next(Track& a_track, bool a_want, bool a_drawn, bool a_redraw, Clock::time_point a_notBefore, Clock::time_point a_now)
	{
		if (!a_track.known || a_track.want != a_want || a_redraw) {
			a_track.known = true;
			a_track.want = a_want;
			a_track.tries = 0;
			if (a_track.gaveUp) {
				a_track.next = {};  // the slow wait was for the state given up on
			}
			a_track.gaveUp = false;
		}
		const bool holster = a_redraw && a_drawn;
		if (a_want == a_drawn && !holster) {
			if (a_track.matchedSince == Clock::time_point{}) {
				a_track.matchedSince = a_now;
			} else if (!a_track.gaveUp && a_now - a_track.matchedSince >= STABLE) {
				a_track.tries = 0;
			}
			return Do::kNothing;
		}
		a_track.matchedSince = {};
		if (a_now < a_notBefore || a_now < a_track.next) {
			return Do::kNothing;
		}
		if (a_track.gaveUp) {
			// A weapon left out on a friend who put theirs away is seen: it is asked for again, slowly.
			if (a_want) {
				return Do::kNothing;
			}
			++a_track.asks;
			a_track.next = a_now + SLOW_HOLSTER;
			return Do::kHolster;
		}
		if (a_track.tries >= MAX_TRIES) {
			a_track.gaveUp = true;
			++a_track.giveUps;
			a_track.next = a_now + SLOW_HOLSTER;
			return Do::kNothing;
		}
		++a_track.tries;
		++a_track.asks;
		a_track.next = a_now + RETRY;
		return a_want && !holster ? Do::kDraw : Do::kHolster;
	}
}
