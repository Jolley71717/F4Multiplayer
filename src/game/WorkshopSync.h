#pragma once

#include "Protocol.h"

// Shared settlement building: what a player places, moves or scraps in workshop mode is copied
// into the other players' games (a plain copy of the object: it isn't part of their workshop, so
// they can't scrap or power it; the builder keeps that). Copies are removed when the session
// ends. Main thread only.
namespace WorkshopSync
{
	// Hooks the workshop's placed/moved/scrapped events. Call once the game is up; no-op after.
	void Install();

	// Places copies that were waiting for their settlement to load. Call every frame.
	void Frame();

	[[nodiscard]] std::vector<std::vector<std::uint8_t>> TakeOutgoing();

	// Another player built, moved or scrapped something.
	void Apply(const Protocol::WorkshopItem& a_item);

	// Dev: reports an existing reference as if the player had just placed (or scrapped) it.
	void Report(RE::TESObjectREFR* a_ref, RE::TESObjectREFR* a_workshop, std::uint8_t a_op);

	// Removes every copy (leaving the session).
	void Reset();

	[[nodiscard]] std::string Describe();
}
