#pragma once

#include "Protocol.h"

#include <cstdint>
#include <unordered_map>

// Which open and lock states a game shares, and which of the session's it takes. Only a door a player
// opens is shared as open or closed. What a script or the game moves is left to them: an elevator (its
// doors, its car, the call panel), a vault's gear door, a load elevator, a door that opens by itself.
// Forcing those open in a friend's game opened the elevator onto an empty shaft, as its car was
// elsewhere (Vault 81, 2026-10-14). Locks are shared on everything that has one. Kept free of the game
// so the rules can be tested.
namespace DoorShare
{
	using RefState = Protocol::RefState;

	// DOOR record flags (TESObjectDOOR::flags).
	inline constexpr std::uint8_t DOOR_AUTOMATIC = 0x02;  // opens and closes by itself as someone comes near

	// What the game knows of a reference that has an open state.
	struct Facts
	{
		bool         door = false;      // its base is a door, not an activator, a container or a light
		std::uint8_t doorFlags = 0;     // the door's record flags
		bool         scripted = false;  // a script is attached to it (from its base, or of its own)
	};

	// Whether a reference's open state is a player's to share. An activator's open state is always a
	// script's (an elevator, its car, a gear door, a two-state switch: the security gates among those, run by the
	// game's standard two-state script alone, are shared through that script instead, SharedGates); so is a door's
	// with a script on it, and an automatic door's is the game's.
	constexpr bool SharesOpen(const Facts& a_facts)
	{
		return a_facts.door && (a_facts.doorFlags & DOOR_AUTOMATIC) == 0 && !a_facts.scripted;
	}

	// The part of a reference's state that is shared: the lock always, the open state if it is a player's.
	constexpr RefState Shared(RefState a_state, bool a_sharesOpen)
	{
		if (!a_sharesOpen) {
			a_state.open = RefState::kUnknown;
		}
		return a_state;
	}

	// Whether a state says anything to share or apply.
	constexpr bool Says(const RefState& a_state)
	{
		return a_state.open != RefState::kUnknown || a_state.locked != RefState::kUnknown;
	}

	// What the session last said of each reference, as far as this game knows: what it reported itself
	// and what it took from the others. A world state repeats every door the session knows each time it
	// is sent, which is more than once: a game that misses a container change asks again every minute and
	// gets the whole world state each time. Taken again each time, a door an NPC or a script had since
	// closed was forced back open, an elevator's doors with it, once a minute (140 changes in an evening
	// of play). A live report from another player is always news; a world state only where it differs
	// from what we know. Cleared when a save loads: its doors are as the save had them, and the next
	// world state is applied in full.
	class Book
	{
	public:
		// We reported this state.
		void Sent(const RefState& a_state) { known[a_state.refId] = a_state; }

		// Whether a state from the server should be applied: always for a live report (one that names
		// its reporter), and for a world state when it isn't what we know already. Either way it is what
		// the session says now.
		bool Take(const RefState& a_state)
		{
			auto [it, added] = known.try_emplace(a_state.refId, a_state);
			const bool news = a_state.playerId != 0 || added || !it->second.SameState(a_state);
			it->second = a_state;
			return news;
		}

		void Clear() { known.clear(); }

		[[nodiscard]] std::size_t Size() const { return known.size(); }

	private:
		std::unordered_map<std::uint32_t, RefState> known;
	};
}
