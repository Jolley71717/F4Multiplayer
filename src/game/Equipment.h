#pragma once

// Equipped armor and weapons: read from the local player, copied onto puppets. Main thread only.
namespace Equipment
{
	// Base forms of the armor and weapons the actor has equipped, sorted, without duplicates.
	std::vector<std::uint32_t> Read(RE::Actor* a_actor);

	// Replaces everything the puppet carries with these items and equips them.
	void Apply(RE::Actor* a_puppet, const std::vector<std::uint32_t>& a_items);
}
