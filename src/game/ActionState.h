#pragma once

#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <string_view>
#include <utility>

// States an actor's animation graph enters by one engine action and leaves by another, and that nothing on the wire
// set off on a stand-in: swimming, and drinking from water (a sink, a pump, a fountain, open water).
//
// Swimming. The engine's graphs have no swim variable: the swim states (tread, forward, sprint, the dive) are a state
// machine entered by the action ActionSwimStart, whose idles pick the event by the actor's graph and weapon (a person's
// "g_SwimStart_MT" or "g_SwimStart", power armor's "..._PA", a dog's and a mirelurk king's "SwimStart"), and left by
// ActionSwimStop ("SwimStop"). The engine performs them as its own actor goes in and out of deep water. A stand-in has
// no AI and is put where its friend is: it got the friend's move mode (the swim bit with it) and the swim flag, and
// nothing ever performed the action, so it walked, ran or stood in the water at swimming height. Now the stand-in, or
// the copy of an NPC another game runs, is asked to start as its owner's state says it swims and to stop as it no
// longer does.
//
// Drinking. Holding the activate key on water has the engine perform ActionActivateLoopingBegin (the idles pick
// "g_drinkStart", or "swimDrinkStart" for a swimmer), and ActionActivateLoopingEnd ("drinkEnd") on release. Nothing
// told the other games. Now the drinker's game says so (Protocol::ShotAction::kDrinkStart, again every RENEW while it
// lasts, and kDrinkStop), and the stand-in performs the same two actions. The say is not reliable (a shot's channel):
// a stand-in drinks for HOLD after the last start it heard, so a lost stop ends by itself and a lost start is made up
// by the next.
//
// Neither state can be read back from a graph, so what was asked is kept (Track): an ask the graph refuses (busy with
// a draw, a landing, a get-up) is asked again every RETRY, MAX_TRIES times, and then let be until the wanted state
// changes; a graph built anew or put back in its base state (Reset) is asked again whatever was asked before. Kept
// free of the game so it can be tested.
namespace ActionState
{
	using Clock = std::chrono::steady_clock;
	using namespace std::chrono_literals;

	// ActorState's move mode bit the engine sets while an actor swims (ActorState::IsSwimming).
	inline constexpr std::uint16_t MOVE_SWIMMING = 0x400;

	// Fallout4.esm's actions (AACT).
	inline constexpr std::uint32_t SWIM_START = 0x0014DB5D;   // ActionSwimStart
	inline constexpr std::uint32_t SWIM_STOP = 0x0014DB5E;    // ActionSwimStop
	inline constexpr std::uint32_t DRINK_START = 0x0002AE5D;  // ActionActivateLoopingBegin
	inline constexpr std::uint32_t DRINK_STOP = 0x0002AE5C;   // ActionActivateLoopingEnd

	inline constexpr auto         RETRY = 500ms;
	inline constexpr std::uint8_t MAX_TRIES = 6;

	enum class Ask : std::uint8_t
	{
		kNothing,
		kEnter,
		kLeave,
	};

	struct Track
	{
		bool              in = false;     // the graph is taken to be in the state: it took the enter, or the tries ran out
		bool              fresh = true;   // nothing was asked of this graph yet
		bool              want = false;   // what the tries are for
		std::uint8_t      tries = 0;      // asks for it so far
		Clock::time_point next{};         // no ask before then
		std::uint32_t     asks = 0;       // dev: every enter and leave asked
		std::uint32_t     taken = 0;      // dev: ... of them, taken by the graph
		std::uint32_t     giveUps = 0;    // dev: wanted states let be after MAX_TRIES refusals
	};

	// The graph was built anew or put back in its base state: what it was told before is nothing to it.
	constexpr void Reset(Track& a_track)
	{
		a_track.fresh = true;
		a_track.tries = 0;
		a_track.next = {};
	}

	// What to ask of the graph now. a_want: its owner is in the state. Answer() says how the ask went.
	constexpr Ask Next(Track& a_track, bool a_want, Clock::time_point a_now)
	{
		if (a_track.fresh) {
			a_track.fresh = false;
			a_track.want = a_want;
			a_track.tries = 0;
			if (a_want) {
				a_track.in = false;  // entered anew
			}
			// Not wanted: left only if it was in before. A graph in its base state refuses that, which costs nothing; one
			// that was never rebuilt (a hold released) leaves the state it still was in.
		} else if (a_track.want != a_want) {
			// The gap to the last ask stays: an owner at the water's edge, in and out by the frame, asks twice a second.
			a_track.want = a_want;
			a_track.tries = 0;
		}
		if (a_track.in == a_want || a_now < a_track.next) {
			return Ask::kNothing;
		}
		if (a_track.tries >= MAX_TRIES) {
			a_track.in = a_want;
			++a_track.giveUps;
			return Ask::kNothing;
		}
		++a_track.tries;
		++a_track.asks;
		a_track.next = a_now + RETRY;
		return a_want ? Ask::kEnter : Ask::kLeave;
	}

	// How the ask Next() returned went: whether the graph took it.
	constexpr void Answer(Track& a_track, bool a_taken)
	{
		if (a_taken) {
			a_track.in = a_track.want;
			++a_track.taken;
		}
	}

	// Whether an actor's state on the wire says it swims: a player's state carries the flag (Protocol::kSwimming) and the
	// move mode, an NPC's only the move mode.
	constexpr bool Swims(bool a_swimFlag, std::uint16_t a_moveMode)
	{
		return a_swimFlag || (a_moveMode & MOVE_SWIMMING) != 0;
	}

	// The event the swim start's idles would pick, sent bare when the action itself is not taken (a graph the idles'
	// conditions miss): by its graph, and for a person by its weapon.
	constexpr const char* SwimStartEvent(bool a_person, bool a_powerArmor, bool a_weaponOut)
	{
		if (!a_person) {
			return "SwimStart";
		}
		if (a_powerArmor) {
			return a_weaponOut ? "g_SwimStart_PA" : "g_SwimStart_MT_PA";
		}
		return a_weaponOut ? "g_SwimStart" : "g_SwimStart_MT";
	}

	inline constexpr const char* SWIM_STOP_EVENT = "SwimStop";

	// The same for a drink: a swimmer's is its own.
	constexpr const char* DrinkStartEvent(bool a_swims)
	{
		return a_swims ? "swimDrinkStart" : "g_drinkStart";
	}

	inline constexpr const char* DRINK_STOP_EVENT = "drinkEnd";

	// --- Drinking, on the drinker's side -------------------------------------------------------------------------------

	inline constexpr auto RENEW = 1s;       // a drink that lasts is said again this often
	inline constexpr auto HOLD = 2500ms;    // a stand-in drinks this long after the last start it heard: two may be lost
	inline constexpr auto UNHEARD = 30s;    // a drink whose graph said nothing for this long is taken to be over

	// What a tag our player's graph announces says about a drink.
	enum class Drink : std::uint8_t
	{
		kNone,
		kBegin,  // the drink starts, or goes round again
		kEnd,
	};

	constexpr Drink DrinkOf(std::string_view a_tag)
	{
		for (const std::string_view begin : { "g_drinkStart", "drinkStart", "swimDrinkStart", "g_drinkFountainStart", "LoopingActivate" }) {
			if (a_tag == begin) {
				return Drink::kBegin;
			}
		}
		return a_tag == "drinkEnd" ? Drink::kEnd : Drink::kNone;
	}

	// Our own player's drink, as its graph told it.
	struct Drinking
	{
		bool              on = false;
		Clock::time_point heard{};  // when its graph last said it drinks
		Clock::time_point sent{};   // when the start was last sent
	};

	enum class Say : std::uint8_t
	{
		kNothing,
		kStart,
		kStop,
	};

	// A tag of our player's graph came.
	constexpr Say Heard(Drinking& a_drinking, Drink a_tag, Clock::time_point a_now)
	{
		switch (a_tag) {
		case Drink::kBegin: {
			const bool was = std::exchange(a_drinking.on, true);
			a_drinking.heard = a_now;
			if (!was || a_now - a_drinking.sent >= RENEW) {
				a_drinking.sent = a_now;
				return Say::kStart;
			}
			return Say::kNothing;
		}
		case Drink::kEnd:
			return std::exchange(a_drinking.on, false) ? Say::kStop : Say::kNothing;
		default:
			return Say::kNothing;
		}
	}

	// Each frame. a_moving: our player walks, which no drink survives (an end its graph never announced).
	constexpr Say Tick(Drinking& a_drinking, bool a_moving, Clock::time_point a_now)
	{
		if (!a_drinking.on) {
			return Say::kNothing;
		}
		if (a_moving || a_now - a_drinking.heard >= UNHEARD) {
			a_drinking.on = false;
			return Say::kStop;
		}
		if (a_now - a_drinking.sent >= RENEW) {
			a_drinking.sent = a_now;
			return Say::kStart;
		}
		return Say::kNothing;
	}

	// --- Drinking, on the stand-in's side ------------------------------------------------------------------------------

	// Whether a stand-in told to drink until a_until still does: not once it walks or is in the air (its friend's drink
	// is over, whatever was heard).
	constexpr bool Drinks(Clock::time_point a_until, bool a_moving, bool a_inAir, Clock::time_point a_now)
	{
		return a_now < a_until && !a_moving && !a_inAir;
	}
}
