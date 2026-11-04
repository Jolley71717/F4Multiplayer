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
// A stand-in whose holster stalled half-way kept its gun out all the same (bead e4m9). Asked to put its gun away, the
// engine sets the weapon state to "wants to sheathe" and has the graph play the holster; the gun goes to the holster
// at that animation's own event, which also ends the state. A graph that does not take the holster (busy with a
// replayed throw, a reload, a stagger) leaves the state wanting to sheathe with the gun still in the hand. That state
// read as holstered (StateOf says why), so nothing was asked again, the slow holster above included. Now the state is
// read as the engine keeps it, and one still on its way to the friend's after STUCK is set back to where it started,
// where the gun still is, and the same asked again as the first time: an ask on top of one the engine already has
// would be no news to it. And a replayed throw holds the asks off until it has played out (THROW_HOLD).
namespace DrawMatch
{
	using Clock = std::chrono::steady_clock;
	using namespace std::chrono_literals;

	inline constexpr auto         RETRY = 1s;   // drawing takes a moment, and fails while the equipment is being changed
	inline constexpr std::uint8_t MAX_TRIES = 4;
	inline constexpr auto         STABLE = 10s;
	inline constexpr auto         SLOW_HOLSTER = 10s;   // once given up, a holster the friend wants is still asked this often
	inline constexpr auto         STUCK = 3s;           // a draw or holster still on its way this long has stalled (one plays in under a second)
	inline constexpr auto         THROW_HOLD = 2s;      // no ask this long after a replayed throw starts: its wind-up and release play first

	// The stand-in's weapon state as the engine keeps it (RE::WEAPON_STATE, the same values: Puppets checks them).
	enum class State : std::uint8_t
	{
		kSheathed,
		kWantToDraw,
		kDrawing,
		kDrawn,
		kWantToSheathe,
		kSheathing,
	};

	// The state from the actor's 3-bit weaponState field as read in C++ (ActorState). The field is declared with the enum's
	// signed type, and MSVC reads such a bit-field signed: "wants to sheathe" (4) comes back as -4 and "sheathing" (5) as
	// -3, so CommonLib's GetWeaponMagicDrawn (weaponState >= kDrawn) says a weapon on its way to the holster is already
	// away. A stand-in whose holster stalled there, gun in hand, read as holstered and was never asked again. Its low
	// three bits are the engine's value whichever way it was read; the two values it has no name for count as drawn.
	constexpr State StateOf(std::int32_t a_field)
	{
		const auto bits = a_field & 0x7;
		return bits <= static_cast<std::int32_t>(State::kSheathing) ? static_cast<State>(bits) : State::kDrawn;
	}

	// As the engine means it (kDrawn and after): a weapon on its way to the holster is still out.
	constexpr bool Drawn(State a_state)
	{
		return a_state >= State::kDrawn;
	}

	// A draw or a holster asked and not done yet.
	constexpr bool OnTheWay(State a_state)
	{
		return a_state != State::kSheathed && a_state != State::kDrawn;
	}

	// Whether a state on its way goes to drawn (a draw) rather than to sheathed (a holster).
	constexpr bool ToDrawn(State a_state)
	{
		return a_state == State::kWantToDraw || a_state == State::kDrawing;
	}

	// The state a draw or holster on its way started from: where the weapon is until its animation's event moves it.
	constexpr State From(State a_state)
	{
		if (!OnTheWay(a_state)) {
			return a_state;
		}
		return ToDrawn(a_state) ? State::kSheathed : State::kDrawn;
	}

	enum class Do : std::uint8_t
	{
		kNothing,
		kDraw,
		kHolster,
		kRestart,  // the draw or holster on its way stalled: the state is set back to From(), and the friend's asked again if that is not it
	};

	struct Track
	{
		bool              known = false;
		bool              want = false;     // the friend's state the tries are for
		std::uint8_t      tries = 0;        // asks for it so far
		bool              gaveUp = false;   // MAX_TRIES asked and still not so: let be
		Clock::time_point next{};           // no ask before then
		Clock::time_point matchedSince{};   // the stand-in has matched since then (zero while it doesn't)
		State             seen = State::kSheathed;  // the stand-in's weapon state when last looked at
		Clock::time_point seenSince{};      // since when it has been in that state (zero: never looked at)
		std::uint32_t     asks = 0;         // dev: every draw and holster asked
		std::uint32_t     giveUps = 0;      // dev: states given up on
		std::uint32_t     restarts = 0;     // dev: stalled draws and holsters set back and asked again
	};

	// What to ask of the stand-in now. a_want: the friend's weapon is out; a_state: the stand-in's weapon state; a_redraw:
	// its gear changed, so it holsters (if drawn) and draws again; a_notBefore: no ask before then (an equip, a get-up or a
	// replayed throw playing).
	constexpr Do Next(Track& a_track, bool a_want, State a_state, bool a_redraw, Clock::time_point a_notBefore, Clock::time_point a_now)
	{
		if (a_track.seenSince == Clock::time_point{} || a_track.seen != a_state) {
			a_track.seen = a_state;
			a_track.seenSince = a_now;
		}
		const bool drawn = Drawn(a_state);
		if (!a_track.known || a_track.want != a_want || a_redraw) {
			a_track.known = true;
			a_track.want = a_want;
			a_track.tries = 0;
			if (a_track.gaveUp) {
				a_track.next = {};  // the slow wait was for the state given up on
			}
			a_track.gaveUp = false;
		}
		const bool holster = a_redraw && drawn;
		if (a_want == drawn && !holster) {
			if (a_track.matchedSince == Clock::time_point{}) {
				a_track.matchedSince = a_now;
			} else if (!a_track.gaveUp && a_now - a_track.matchedSince >= STABLE) {
				a_track.tries = 0;
			}
			// On its way away from the friend's state and stalled there (a redraw's holster the graph never took): set back
			// to where the weapon still is, which is where theirs is; nothing to ask.
			if (OnTheWay(a_state) && a_now - a_track.seenSince >= STUCK && a_now >= a_notBefore && a_now >= a_track.next) {
				++a_track.restarts;
				a_track.seenSince = a_now;
				a_track.next = a_now + RETRY;
				return Do::kRestart;
			}
			return Do::kNothing;
		}
		a_track.matchedSince = {};
		if (a_now < a_notBefore || a_now < a_track.next) {
			return Do::kNothing;
		}
		// On its way to the friend's state: let it get there; still on its way after STUCK, it is set back and asked again.
		const bool stalled = OnTheWay(a_state) && ToDrawn(a_state) == a_want && !holster;
		if (stalled && a_now - a_track.seenSince < STUCK) {
			return Do::kNothing;
		}
		const auto ask = [&] {
			if (stalled) {
				++a_track.restarts;
				a_track.seenSince = a_now;  // set back, it waits STUCK again before the next restart
				return Do::kRestart;
			}
			return a_want && !holster ? Do::kDraw : Do::kHolster;
		};
		if (a_track.gaveUp) {
			// A weapon left out on a friend who put theirs away is seen: it is asked for again, slowly.
			if (a_want) {
				return Do::kNothing;
			}
			++a_track.asks;
			a_track.next = a_now + SLOW_HOLSTER;
			return ask();
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
		return ask();
	}

	// The same for a stand-in whose weapon is only known to be out or away, never on its way.
	constexpr Do Next(Track& a_track, bool a_want, bool a_drawn, bool a_redraw, Clock::time_point a_notBefore, Clock::time_point a_now)
	{
		return Next(a_track, a_want, a_drawn ? State::kDrawn : State::kSheathed, a_redraw, a_notBefore, a_now);
	}
}
