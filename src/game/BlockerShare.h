#pragma once

#include "Protocol.h"

#include <cstdint>

// Invisible walls a script takes down when it opens the way, shared with a friend's game. A door a script unlocks and
// opens is shared by its state (DoorShare), but the wall that stands in its doorway is a separate reference, a
// CollisionMarker (the engine's invisible collision box, base 00000021) that the same script disables, and nothing
// carried that to the friend's game. Grandchester Mystery Mansion (Nuka-World, a Nexus/GitHub report on 0.21.2):
// DLC04Grandchester's stage fragments unlock a door, set it open and disable DLC04_ParlorBalconyCollisionRef (stage 11),
// DLC04_FireplaceRoomCollisionRef (13) or DLC04_UpstairsHallCollisionRef (18), one wall to each doorway. The quest is
// not shared (it is a dungeon quest with no objectives, QuestSync's only with sQuests = safe), so the friend's stage ran
// in the friend's game alone: the door they opened was open in the host's game too, and its wall was still up. The
// friend walked through the doorway and the host was stopped in it by nothing to be seen (the host, and any player whose
// quest was behind, could close the door but not get through it). Vault 114's gear door (DN142: VDoorCollision) is the
// same shape, and ScriptDoors shares the stage that opens it; the Grandchester stages also move Lucy, wake robots and
// start scenes, so they stay each game's own and the wall is shared instead.
//
// What is shared is the way being opened, and only that: a CollisionMarker that a game's script disables is disabled in
// the friend's game too. A wall put back up (a trap's, an arena's, the quest's own reset) is never carried over, so
// the worst the sharing can do is let a player through a wall their own quest would have taken down later; it can never
// shut one in. It travels as a reference's state (Protocol::RefState, no new message, the same server table and the
// same replay to a player who joins later): open = kYes says the wall is down (the marker is disabled), kNo that it
// stands, and the lock is never said. A world state that repeats it is applied once and then only if the wall still
// stands. Kept free of the game so the rules can be tested.
namespace BlockerShare
{
	using RefState = Protocol::RefState;

	// The base of every invisible collision box a plugin places: Fallout4.esm's STAT CollisionMarker. (The game's other
	// markers, an XMarker or a trigger, are no walls.)
	inline constexpr std::uint32_t COLLISION_MARKER = 0x00000021;

	constexpr bool IsBlockerBase(std::uint32_t a_base)
	{
		return a_base == COLLISION_MARKER;
	}

	// What a game reads of a blocker: the way is open (kYes) when the marker is disabled. A disabled reference has no 3D,
	// so this needs none.
	constexpr RefState Read(std::uint32_t a_ref, bool a_disabled)
	{
		RefState state;
		state.refId = a_ref;
		state.open = a_disabled ? RefState::kYes : RefState::kNo;
		return state;
	}

	// Whether a look at a blocker is news to report: the way was shut or unknown when we last said (or took) it, and is
	// open now. A wall coming back is never news (the way is ours to keep open, however our own script goes on).
	constexpr bool Opened(const RefState& a_now, const RefState& a_lastSent)
	{
		return a_now.open == RefState::kYes && a_lastSent.open != RefState::kYes;
	}

	// What our game does with a friend's state of a blocker.
	enum class Apply
	{
		kNothing,  // their wall stands, or says nothing, or ours is down already
		kOpen,     // theirs is down and ours stands: disable it
	};

	constexpr Apply Decide(const RefState& a_theirs, bool a_oursDisabled)
	{
		return a_theirs.open == RefState::kYes && !a_oursDisabled ? Apply::kOpen : Apply::kNothing;
	}

	// Whether the walk of the loaded references puts a collision marker on the poll's list: one that stands (loaded, its
	// 3D up) and is a plugin's, which is the only kind that has anything to report. One already down is where its game
	// has it (the save's, or the session's), and is not read; if a script puts it back up it is found again by the
	// walk.
	constexpr bool Polled(bool a_isBlocker, bool a_shareable, bool a_disabled, bool a_has3D, bool a_deleted)
	{
		return a_isBlocker && a_shareable && !a_disabled && a_has3D && !a_deleted;
	}

	// A polled blocker is still worth reading while it stands loaded, or after it went down and before its news was said
	// (a disabled reference has no 3D). Once it is down and said (or taken from a friend) it is forgotten: nothing more
	// of it is to be reported until it stands again.
	constexpr bool WorthReading(bool a_disabled, bool a_has3D, bool a_deleted, bool a_said)
	{
		return !a_deleted && (a_has3D || (a_disabled && !a_said));
	}
}
