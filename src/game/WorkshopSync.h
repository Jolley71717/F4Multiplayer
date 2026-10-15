#pragma once

#include "Protocol.h"

// Shared settlement building: what a player places, moves or scraps in workshop mode is copied
// into the other players' games (a plain copy of the object: it isn't part of their workshop, so
// they can't scrap or power it; the builder keeps that). Copies are removed when the session
// ends; what a player kept in a copy of a container comes to them first. The host also reports
// the settlement junk its save had scrapped before the session.
// Main thread only.
namespace WorkshopSync
{
	// Hooks the workshop's placed/moved/scrapped events. Call once the game is up; no-op after.
	void Install();

	// Places copies (and scraps pieces of the world) that were waiting for their cell to load and, on
	// the host (a_host), walks the loaded cells for junk scrapped before the session. Call every frame
	// while in a session.
	void Frame(bool a_host);

	// The reports to send now: no more than SendPacer::WORKSHOP_PER_SECOND a second, the rest waiting their turn
	// in order. a_sending: false while they can't be (not in a session): all of them are dropped, and a piece of
	// the world they reported scrapped is reported again when the walk finds it.
	[[nodiscard]] std::vector<std::vector<std::uint8_t>> TakeOutgoing(bool a_sending);

	// Another player built, moved or scrapped something: in line behind what came before, and placed a
	// few a frame (Frame).
	void Apply(const Protocol::WorkshopItem& a_item);

	// Whether the reference is a settlement's workbench (whatever its state): what a friend's build or scrap
	// must name as its workbench, and a container no friend's loot changes.
	[[nodiscard]] bool IsWorkbenchAnchor(const RE::TESObjectREFR* a_ref);

	// Dev: whether workshop mode lets the player scrap the reference (the game's own test).
	[[nodiscard]] bool WorkshopModeScraps(RE::TESObjectREFR& a_ref);

	// Dev: reports an existing reference as if the player had just placed (or scrapped) it.
	void Report(RE::TESObjectREFR* a_ref, RE::TESObjectREFR* a_workshop, std::uint8_t a_op);

	// Removes every copy (leaving the session).
	void Reset();

	// A save was loaded: the copies made before it are let go (the session sends its building again),
	// and its cells are walked again for scrapped junk.
	void OnGameLoaded();

	// The loaded save is another character's world (the session is left): the copies were placed in the world before
	// the load, and their handles name nothing of ours now (or something else). Forgotten, not removed.
	void OnWorldGone();

	[[nodiscard]] std::string Describe();

	// Dev: the workbenches the walk found (or this one), with whether the player stands in each one's
	// build area; for any other reference, what the walk makes of it.
	[[nodiscard]] std::string DescribeBenches(RE::TESObjectREFR* a_ref = nullptr);
}
