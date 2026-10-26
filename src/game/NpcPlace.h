#pragma once

#include "game/RemoteEdits.h"
#include "game/RetryBudget.h"

#include <chrono>

// Where our copy of an NPC a friend's game runs stands, against where their game has it: whether ours is moved to
// theirs (NpcSync's BringIn), and whether ours is mirrored from there (UpdateMirrors). Each game's own story can
// leave the same NPC in different places: the host finished Concord, so its Preston stands in Sanctuary, while
// ours still waits in the museum with its 3D loaded. Kept free of the game so it can be tested.
namespace NpcPlace
{
	using RemoteEdits::Spot;

	// Of our player: the owner's copy is right here, ours is in some unloaded cell or in another space.
	constexpr float BRING_IN_RANGE = 3000.0f;
	// The owner's copy is this far from ours (it fast traveled or was moved by a script): ours isn't dragged across
	// the map (where nothing may be loaded); it runs its own AI meanwhile.
	constexpr float MAX_FOLLOW_DISTANCE = 4096.0f;  // one exterior cell
	// No states for this long (the owner's game is paused, loading or gone): ours runs its own AI until they come back.
	constexpr std::chrono::steady_clock::duration LOST_AFTER = std::chrono::seconds{ 3 };

	// Our copy of the NPC, as our game has it.
	struct Ours
	{
		bool exists = false;       // the NPC is in our game (a shareable ref)
		bool off = false;          // switched off here: switching it on comes first (SwitchOn)
		bool unusable = false;     // deleted, dead or the player
		bool loaded = false;       // its 3D is loaded
		Spot at;                   // its cell or world, and where in it
		bool companion = false;    // our own companion: it follows us here
		bool inScene = false;      // in a scene here (our own story is using it)
		bool fightingUs = false;   // its combat target is our player
		bool talkingToUs = false;  // the NPC in our player's conversation
		bool holdsUs = false;      // the engine's speaker in our player's conversation, or in the scene our player is in
	};

	enum class Why
	{
		kHere,          // loaded where the owner's copy is: mirrored from there, nothing to move
		kOwnerIndoors,  // the owner's copy is in an interior (or nowhere): only one outdoors near us, or in the interior we are in, is brought in
		kNotNearUs,     // our player is indoors, in another world, or farther than BRING_IN_RANGE from the owner's copy
		kCannot,        // ours is not in our game, is off, dead or deleted, or is the player
		kBusy,          // ours is our companion, in a scene here, fighting us, talking to us or holding us: it stays with us
		kOwnerAway,     // the owner's stand-in is not beside where their game has it: only their word for it
		kMove,          // moved to where the owner's copy stands, and mirrored from there
	};

	// Whether ours is moved to where the owner's copy stands (a_owners, from the owner's state). a_player: where our
	// player is. a_ownerBeside: the owner's stand-in stands beside that spot (RemoteEdits::OwnerBeside).
	//
	// Our player stands outdoors in the owner's world, or in the interior the owner's copy is in, within BRING_IN_RANGE
	// of the owner's copy, so ours is never taken from a cell our player is in with it: talking to Preston inside the
	// museum, ours stays there. Nor is one moved that our game is using with us. Anything else outside the owner's
	// space, loaded or not, is moved: a companion that walked here with its player from a cell we have not loaded, or
	// a copy its own quest left in an interior.
	constexpr Why Decide(const Spot& a_owners, const Ours& a_ours, const Spot& a_player, bool a_ownerBeside)
	{
		if (a_ours.exists && a_ours.loaded && RemoteEdits::SameSpace(a_ours.at, a_owners)) {
			return Why::kHere;
		}
		// Our player is in the interior the owner's copy is in: a companion that followed its player through the door.
		const bool inItsInterior = a_owners.cell != 0 && a_player.cell == a_owners.cell;
		if (!inItsInterior) {
			if (a_owners.cell != 0 || a_owners.worldspace == 0) {
				return Why::kOwnerIndoors;
			}
			if (a_player.cell != 0 || a_player.worldspace != a_owners.worldspace) {
				return Why::kNotNearUs;
			}
		}
		const float dx = a_owners.x - a_player.x, dy = a_owners.y - a_player.y, dz = a_owners.z - a_player.z;
		if (!(dx * dx + dy * dy + dz * dz <= BRING_IN_RANGE * BRING_IN_RANGE)) {
			return Why::kNotNearUs;
		}
		if (!a_ours.exists || a_ours.off || a_ours.unusable) {
			return Why::kCannot;
		}
		if (a_ours.companion || a_ours.inScene || a_ours.fightingUs || a_ours.talkingToUs || a_ours.holdsUs) {
			return Why::kBusy;
		}
		return a_ownerBeside ? Why::kMove : Why::kOwnerAway;
	}

	// Our copy as RetryBudget::Plan takes it, with a_why from Decide. off and unusable are the copy's own (unusable
	// also covers a character of our own story that is off here: NpcSync's CopyFor).
	constexpr RetryBudget::Copy CopyOf(const Ours& a_ours, Why a_why)
	{
		return {
			.exists = a_ours.exists,
			.off = a_ours.exists && a_ours.off,
			.unusable = a_ours.exists && a_ours.unusable,
			.here = a_why == Why::kHere,
			.movable = a_why == Why::kMove,
		};
	}

	// Whether ours runs its own AI here whoever runs it elsewhere: our companion follows us, and an NPC our player is
	// talking to, or one in the scene our player is in, has its lines and the scene to finish here. Mirrored, its AI is
	// off: the scene never ends, and the engine holds our player in it (no weapons, Pip-Boy or workshop) for as long as
	// another game runs it. A friend joining, or talking to the same NPC in their own game, takes it over (a story mod's
	// opening scene, both players at it).
	constexpr bool StaysWithUs(const Ours& a_ours)
	{
		return a_ours.companion || a_ours.talkingToUs || a_ours.holdsUs;
	}

	// Whether ours, loaded at a_ours, is mirrored from the owner's latest state (at a_owners, a_age old): in the same
	// cell or world, heard from lately, and near. Otherwise it runs its own AI here.
	constexpr bool Follows(const Spot& a_ours, const Spot& a_owners, std::chrono::steady_clock::duration a_age)
	{
		if (!RemoteEdits::SameSpace(a_ours, a_owners) || a_age > LOST_AFTER) {
			return false;
		}
		const float dx = a_ours.x - a_owners.x, dy = a_ours.y - a_owners.y, dz = a_ours.z - a_owners.z;
		return dx * dx + dy * dy + dz * dz <= MAX_FOLLOW_DISTANCE * MAX_FOLLOW_DISTANCE;
	}
}
