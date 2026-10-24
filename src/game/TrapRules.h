#pragma once

#include "Protocol.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

// World traps shared (src/game/Traps.h): what a trap's state says, what a game reports of it, and what another game does
// with a report. Kept free of the game so the rules can be tested.
//
// A trap is each game's own: its script runs only in the game it is set off in. Those this covers, all placed in the world
// by the game's data (one made at runtime, a mine a raider threw or a trap our player built in a settlement, is in no
// other game):
// - a mine (a placed grenade projectile, PGRE, of a plugin's: a frag mine, a bottlecap mine, a pulse mine...): armed while
//   it is there, gone once it went off, or once it was picked up (disarmed);
// - a tripwire (script TrapTripwire): Active while armed, Busy a moment, then Inactive once tripped (it activates what it
//   is linked to: a grenade bouquet, a gun trap, a swinging log) or Disarmed;
// - a tension trigger on a door or a container (TrapTrigTension): Waiting, Busy, then triggered or Disarmed (disabled);
// - a trap proper, set off by a trigger or shot (TrapBase and its children: a grenade bouquet, a gun trap, an IED, a
//   bear trap of the base game's...): Idle while armed, warn then TrapFired once set off, Disarm once disarmed;
// - a bear trap of Far Harbor's (DLC03:TrapBear): Open while armed, Busy while it snaps, Closed after (it snapped on
//   someone, or was sprung by hand, Disarming), then gone once taken (it goes to the taker's inventory).
// A pressure plate (TrapTrigBase) is not here: it is set again once nothing stands on it, so it is no one-off.
namespace TrapRules
{
	using Clock = std::chrono::steady_clock;

	enum class Kind : std::uint8_t
	{
		kNone,
		kMine,
		kTripwire,
		kTension,
		kTrap,
		kBearTrap,
	};

	struct Script
	{
		Kind             kind;
		std::string_view name;  // as the game's VM knows it
	};

	// The scripts, the most specific first: a reference is of the first kind whose script (or one that extends it) is
	// bound to it.
	inline constexpr std::array SCRIPTS{
		Script{ Kind::kTripwire, "TrapTripwire" },
		Script{ Kind::kTension, "TrapTrigTension" },
		Script{ Kind::kBearTrap, "DLC03:TrapBear" },
		Script{ Kind::kTrap, "TrapBase" },
	};

	constexpr std::string_view ScriptOf(Kind a_kind)
	{
		for (const auto& script : SCRIPTS) {
			if (script.kind == a_kind) {
				return script.name;
			}
		}
		return {};
	}

	constexpr std::string_view NameOf(Kind a_kind)
	{
		switch (a_kind) {
		case Kind::kMine:
			return "mine";
		case Kind::kTripwire:
			return "tripwire";
		case Kind::kTension:
			return "tension trigger";
		case Kind::kTrap:
			return "trap";
		case Kind::kBearTrap:
			return "bear trap";
		case Kind::kNone:
			break;
		}
		return "nothing";
	}

	enum class Phase : std::uint8_t
	{
		kUnknown,   // its script can't be read (yet), or is in a state not listed here
		kArmed,     // set, waiting
		kGoing,     // on its way: tripped or being disarmed this moment (the next state says which)
		kFired,     // went off
		kDisarmed,  // disarmed (or sprung by hand)
		kGone,      // no longer in the world: a mine that went off or was picked up, a bear trap taken
	};

	constexpr bool SameName(std::string_view a_a, std::string_view a_b)
	{
		if (a_a.size() != a_b.size()) {
			return false;
		}
		const auto lower = [](char a_c) { return (a_c >= 'A' && a_c <= 'Z') ? static_cast<char>(a_c - 'A' + 'a') : a_c; };
		for (std::size_t i = 0; i < a_a.size(); ++i) {
			if (lower(a_a[i]) != lower(a_b[i])) {
				return false;
			}
		}
		return true;
	}

	struct StateName
	{
		Kind             kind;
		std::string_view state;
		Phase            phase;
	};

	// The scripts' states (from their compiled scripts on 1.11.240; the VM ignores a state name's case).
	inline constexpr std::array STATES{
		StateName{ Kind::kTripwire, "Active", Phase::kArmed },
		StateName{ Kind::kTripwire, "Busy", Phase::kGoing },
		StateName{ Kind::kTripwire, "Inactive", Phase::kFired },
		StateName{ Kind::kTripwire, "Disarmed", Phase::kDisarmed },
		StateName{ Kind::kTension, "Waiting", Phase::kArmed },
		StateName{ Kind::kTension, "Busy", Phase::kGoing },
		StateName{ Kind::kTension, "triggered", Phase::kFired },
		StateName{ Kind::kTension, "Disarmed", Phase::kDisarmed },
		StateName{ Kind::kTrap, "Idle", Phase::kArmed },
		StateName{ Kind::kTrap, "warn", Phase::kFired },
		StateName{ Kind::kTrap, "TrapFired", Phase::kFired },
		StateName{ Kind::kTrap, "Disarm", Phase::kDisarmed },
		StateName{ Kind::kTrap, "Disarmed", Phase::kDisarmed },
		StateName{ Kind::kBearTrap, "Open", Phase::kArmed },
		StateName{ Kind::kBearTrap, "Busy", Phase::kFired },
		StateName{ Kind::kBearTrap, "Closed", Phase::kFired },
		StateName{ Kind::kBearTrap, "Disarming", Phase::kDisarmed },
	};

	// A scripted trap's phase, from its script's state (a mine has no script: Traps reads it from whether it is there).
	constexpr Phase PhaseOf(Kind a_kind, std::string_view a_state)
	{
		for (const auto& entry : STATES) {
			if (entry.kind == a_kind && SameName(entry.state, a_state)) {
				return entry.phase;
			}
		}
		return Phase::kUnknown;
	}

	// How long after our player activated a trap its going counts as their taking it (a mine picked up: the mine is gone
	// a frame or two later, and the watcher looks four times a second).
	inline constexpr auto TAKE_WINDOW = std::chrono::seconds(3);

	// What a game reports of a trap it watches, from the phase it saw last (a_was) and the one it sees now; nullopt for
	// nothing. Only a trap seen armed is reported going off or disarmed: one first seen spent was spent before we came by
	// (its state is in the session, or it was spent before the session). A mine gone is one that went off, unless our
	// player had just activated it (picked it up); a bear trap gone after it snapped was taken, if by our player.
	// a_known: what the session already holds of it; a report that adds nothing to that isn't sent (it is how what a
	// friend's report did in our game isn't reported back).
	constexpr std::optional<std::uint8_t> Change(Kind a_kind, Phase a_was, Phase a_now, bool a_takenByUs, std::optional<std::uint8_t> a_known)
	{
		using Protocol::TrapState;
		std::optional<std::uint8_t> state;
		const bool wasSet = a_was == Phase::kArmed || a_was == Phase::kGoing;
		const bool wasSpent = a_was == Phase::kFired || a_was == Phase::kDisarmed;
		if (wasSet && a_now == Phase::kFired) {
			state = TrapState::kFired;
		} else if (wasSet && a_now == Phase::kDisarmed) {
			state = TrapState::kDisarmed;
		} else if (a_now == Phase::kGone && (wasSet || wasSpent)) {
			if (a_takenByUs) {
				state = TrapState::kTaken;
			} else if (wasSet && a_kind == Kind::kMine) {
				state = TrapState::kFired;
			}
		}
		if (!state || !Protocol::TrapSupersedes(*state, a_known)) {
			return std::nullopt;
		}
		return state;
	}

	enum class Action : std::uint8_t
	{
		kWait,       // not yet: its script can't be read, it is on its way, or it has only just loaded
		kNothing,    // it is spent here already
		kFireLive,   // set it off here, as it went off there: a mine's blast, a tripwire's trip and what it sets off
		kFireQuiet,  // spent without a show: it went off there a while ago, or out of our sight
		kDisarm,     // disarmed, without a show
		kTake,       // gone (disabled): a friend took it
	};

	// What to do with the session's state of a loaded trap (a_state), given its phase here. a_live: it went off in a
	// friend's game just now, here where it is in ours (Live). a_settled: it has been loaded, and our player in the world,
	// long enough for its script's OnLoad to have run (SETTLE).
	constexpr Action Decide(std::uint8_t a_state, Phase a_here, bool a_live, bool a_settled)
	{
		using Protocol::TrapState;
		if (!a_settled || a_here == Phase::kUnknown || a_here == Phase::kGoing) {
			return Action::kWait;
		}
		if (a_here == Phase::kGone) {
			return Action::kNothing;
		}
		if (a_state == TrapState::kTaken) {
			return Action::kTake;
		}
		if (a_here != Phase::kArmed) {
			return Action::kNothing;
		}
		if (a_state == TrapState::kDisarmed) {
			return Action::kDisarm;
		}
		return a_live ? Action::kFireLive : Action::kFireQuiet;
	}

	// A trap that has only just loaded is left alone (Decide), and what it does is not reported: its script's OnLoad may
	// still be setting it up (a bear trap in DLC03NorthwoodRidgeQuarry01, 020385EB, read open when found and closed a
	// moment later with nobody near). What it is once settled is taken as how it was.
	inline constexpr auto SETTLE = std::chrono::seconds(2);

	// An activator found with none of the trap scripts bound may only not have them bound yet (its cell has just loaded):
	// it is looked at again, once a second, until it has been loaded this long. Anything else is known at the first look.
	inline constexpr auto BIND_WAIT = std::chrono::seconds(10);

	constexpr bool ClassFinal(Kind a_kind, bool a_activator, Clock::time_point a_firstLooked, Clock::time_point a_now)
	{
		return a_kind != Kind::kNone || !a_activator || a_now - a_firstLooked >= BIND_WAIT;
	}

	constexpr bool Settled(Clock::time_point a_seenSince, Clock::time_point a_inWorldSince, Clock::time_point a_now)
	{
		const auto since = a_seenSince > a_inWorldSince ? a_seenSince : a_inWorldSince;
		return a_now - since >= SETTLE;
	}

	// A friend's trap going off is set off here as it went (a mine's blast, a tripwire's trip) only just after it went,
	// and only if it is where they said: a report from a while ago (it waited for the trap to load here, or for our
	// player to be in the world) spends ours quietly, as one that went off out of our sight.
	inline constexpr auto LIVE_WINDOW = std::chrono::seconds(3);
	inline constexpr float LIVE_SLACK = 512.0f;  // how far our trap may be from where theirs was

	constexpr bool Live(bool a_liveReport, Clock::time_point a_arrived, Clock::time_point a_now, float a_distance)
	{
		return a_liveReport && a_now - a_arrived <= LIVE_WINDOW && a_distance <= LIVE_SLACK;
	}

	// How long, after a trap went off here (by itself or set off for a friend's), the explosions near it are its own: not
	// reported as our player's (WeaponFire's explosion watch would, within a moment of our throwing something). Every
	// game sets its own off, so one reported too would go off twice. A grenade bouquet's grenades go off a few seconds
	// after it is tripped, a gun trap fires for a few.
	inline constexpr auto CLAIM_WINDOW = std::chrono::seconds(8);
	inline constexpr float CLAIM_RADIUS = 1500.0f;  // a bouquet throws its grenades about
	inline constexpr float MINE_RADIUS = 256.0f;    // a mine's blast is where it lies

	struct Spot
	{
		float x = 0.0f, y = 0.0f, z = 0.0f;
	};

	constexpr float DistanceSquared(const Spot& a_a, const Spot& a_b)
	{
		const float dx = a_a.x - a_b.x;
		const float dy = a_a.y - a_b.y;
		const float dz = a_a.z - a_b.z;
		return dx * dx + dy * dy + dz * dz;
	}

	struct Fired
	{
		Spot              at;
		Clock::time_point when;
	};

	// Whether an explosion at a_at is a trap's: near one that went off here lately, or at a mine we watch (one going
	// off: the watcher may not have looked since).
	inline bool Claims(const std::vector<Fired>& a_fired, const std::vector<Spot>& a_mines, const Spot& a_at, Clock::time_point a_now)
	{
		for (const auto& fired : a_fired) {
			if (a_now - fired.when <= CLAIM_WINDOW && DistanceSquared(fired.at, a_at) <= CLAIM_RADIUS * CLAIM_RADIUS) {
				return true;
			}
		}
		for (const auto& mine : a_mines) {
			if (DistanceSquared(mine, a_at) <= MINE_RADIUS * MINE_RADIUS) {
				return true;
			}
		}
		return false;
	}

	// The session's state of the traps, as this game knows it: first come first kept, a take replacing a spring or a
	// disarm (Protocol::TrapSupersedes), as the server keeps it.
	struct Known
	{
		Protocol::TrapState state;
		bool                live = false;  // it came as news (a friend's trap going off just now), not a world state
		Clock::time_point   arrived{};
		bool                applied = false;  // done here, or nothing to do: not looked at again until it changes
	};

	// Takes a state into a_book; true if it is news.
	template <class Book>
	bool Take(Book& a_book, const Protocol::TrapState& a_state, bool a_live, Clock::time_point a_now)
	{
		const auto it = a_book.find(a_state.refId);
		const auto old = it != a_book.end() ? std::optional{ it->second.state.state } : std::nullopt;
		if (!Protocol::TrapSupersedes(a_state.state, old)) {
			return false;
		}
		a_book[a_state.refId] = Known{ a_state, a_live, a_now, false };
		return true;
	}

	// Takes a list from the server into a_book; how many of its states were news.
	template <class Book>
	std::size_t TakeList(Book& a_book, const Protocol::TrapStates& a_list, Clock::time_point a_now)
	{
		std::size_t news = 0;
		for (const auto& trap : a_list.traps) {
			news += Take(a_book, trap, a_list.live, a_now) ? 1 : 0;
		}
		return news;
	}
}
