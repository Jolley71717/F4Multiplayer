#pragma once

#include "Protocol.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Loose objects a player moves by hand, shared: a broom, a bottle, a tire grabbed with the use key (held), carried,
// dropped or thrown, and objects knocked about by a player walking into them or shooting them. The rules are
// ObjectShare.h (game-free, tested); the server's book of who moves what is common/ObjectMovers.h.
//
// - Our player holds an object: where it is in their hands goes to the others 15 times a second while it moves (in our
//   player's frame: the friend's game draws it in front of its stand-in of us, however that is drawn), and when they let
//   go, where it left their hands and how fast (kLetGo), then where it came to rest (kRest).
// - A friend holds one: our copy of it is held still by the engine (keyframed, so gravity and our own physics leave it
//   alone) and drawn in their stand-in's hands every frame, eased towards their newest hold. At their let-go it is
//   physics again, put where it left their hands with their throw's velocity, so it flies here as it did there; at their
//   rest it is put where it lies in their world, if ours settled elsewhere.
// - Knocked about: a scan of the loose objects near our player, four times a second, finds those that moved with no hand
//   on them; one our player moved (walked into it, or shot it) is reported where it came to rest, a few a second at most,
//   friend near or not, and again if it rolls on soon after (KnockWatch's follow-on).
// - Later: the server keeps where each object came to rest last (ObjectMovers::Places, saved with the session) and tells
//   a game that joins or loads a save (a catch-up: playerId 0). A place for an object our game doesn't have loaded yet
//   waits (as a friend's rest that came meanwhile does) and is put there when it loads (WorldSync's retry, as references
//   load), so a friend arriving later finds it where it was left, and so does the next load of every save.
//
// Never: actors and corpses (their own sync), quest items (in a quest alias), references made at runtime (dropped items,
// what Drops, Spawns and WorkshopSync place, all 0xFF......), what SaveGuard keeps out of saves, power armor frames, and
// nothing at all in workshop mode (WorkshopSync's). Every pose written is checked first (finite, in the world:
// HavokGuard's rule), and physics is only ever touched on the main thread. Main thread only, but for OnHit.
namespace MovedObjects
{
	struct Packet
	{
		std::vector<std::uint8_t> data;
		bool                      reliable = true;
	};

	// Every frame (welcomed: in a session). Undoes what it did to our world while not welcomed or loading.
	void Frame(bool a_welcomed);

	std::vector<Packet> TakeOutgoing();

	// From the server.
	void OnHeld(const Protocol::ObjectHold& a_hold);
	void OnRested(const Protocol::ObjectRest& a_rest);

	// Our player hit a reference (TESHitEvent; any thread): a loose object that moves after it is ours to report.
	void OnHit(std::uint32_t a_ref);

	void OnPlayerLeft(std::uint32_t a_playerId);

	// The places waiting for objects not in our world yet (WorldSync's retry, when references load). ApplyWaiting is true
	// when it put the object there (or found it there already).
	[[nodiscard]] bool AnyWaiting();
	[[nodiscard]] std::vector<std::uint32_t> WaitingRefs();
	bool ApplyWaiting(std::uint32_t a_ref);

	// Before a save or a load: every object we hold still for a friend is physics again, as the game made it.
	void BeforeSaveOrLoad();

	// The session ended, or a save loaded: everything forgotten (what we changed is undone first, while it is still there).
	void Reset();

	// For `net status`.
	[[nodiscard]] std::string Status();

	// The dev channel's `objects` (docs/DEV-CHANNEL.md): status; `probe <ref>` (its place, angles, 3D and physics);
	// `grab` (what our player holds).
	[[nodiscard]] std::string Command(std::string_view a_args);
}
