#pragma once

// Equipped armor and weapons: read from the local player, copied onto puppets. Main thread only.
namespace Equipment
{
	// Base forms of the armor and weapons the actor has equipped, sorted, without duplicates.
	std::vector<std::uint32_t> Read(RE::Actor* a_actor);

	// Replaces everything the puppet carries with these items and equips them.
	void Apply(RE::Actor* a_puppet, const std::vector<std::uint32_t>& a_items);

	// Gives the actor these items (if it doesn't have them) and equips them, and takes off any other
	// armor or weapon it wears; the rest of its inventory is left alone (a mirrored NPC's loot stays
	// its own).
	void Wear(RE::Actor* a_actor, const std::vector<std::uint32_t>& a_items);
}
