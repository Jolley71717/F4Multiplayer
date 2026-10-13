#pragma once

#include "Protocol.h"

// Shared settlement building: what a player places, moves or scraps in workshop mode is copied
// into the other players' games (a plain copy of the object: it isn't part of their workshop, so
// they can't scrap or power it; the builder keeps that). Copies are removed when the session
// ends. The host also reports the settlement junk its save had scrapped before the session.
// Main thread only.
namespace WorkshopSync
{
	// Hooks the workshop's placed/moved/scrapped events. Call once the game is up; no-op after.
	void Install();

	// Places copies (and scraps pieces of the world) that were waiting for their cell to load and, on
	// the host (a_host), walks the loaded cells for junk scrapped before the session. Call every frame
	// while in a session.
	void Frame(bool a_host);

	// The reports to send. a_sending: false while they can't be (not in a session): they are dropped,
	// and a piece of the world they reported scrapped is reported again when the walk finds it.
	[[nodiscard]] std::vector<std::vector<std::uint8_t>> TakeOutgoing(bool a_sending);

	// Another player built, moved or scrapped something.
	void Apply(const Protocol::WorkshopItem& a_item);

	// Whether the reference is a settlement's workbench (whatever its state): what a friend's build or scrap
	// must name as its workbench, and a container no friend's loot changes.
	[[nodiscard]] bool IsWorkbenchAnchor(const RE::TESObjectREFR* a_ref);

	// Dev: reports an existing reference as if the player had just placed (or scrapped) it.
	void Report(RE::TESObjectREFR* a_ref, RE::TESObjectREFR* a_workshop, std::uint8_t a_op);

	// Removes every copy (leaving the session).
	void Reset();

	// A save was loaded: its cells are walked again for scrapped junk.
	void OnGameLoaded();

	[[nodiscard]] std::string Describe();

	// Dev: the workbenches the walk found (or this one), with whether the player stands in each one's
	// build area; for any other reference, what the walk makes of it.
	[[nodiscard]] std::string DescribeBenches(RE::TESObjectREFR* a_ref = nullptr);
}
