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

	// Puts the grenade or mine among these items in the actor's hand the way the combat AI does before
	// a throw: through the equip manager, in the weapon's own slot. The console's equipitem leaves a
	// throwable in an NPC's inventory but never in the engine's equipped list (the one the throw
	// animation's weaponAttach event shows and WeaponFire launches), so a replayed throw played
	// empty-handed. One is given when the actor has none left (a replayed throw spends it). Returns
	// whether the engine lists it as equipped afterwards; false when the items hold no throwable.
	bool HoldThrowable(RE::Actor* a_actor, const std::vector<std::uint32_t>& a_items);

	// How often a throwable's 3D was asked of the engine's AttachWeapon (its equip path loads none on
	// an NPC); for the status line.
	std::uint32_t AttachesAsked();

	// The grenade or mine the engine lists as equipped on the actor, or null.
	RE::TESObjectWEAP* HeldThrowable(RE::Actor* a_actor);

	// What the engine lists as equipped on the actor (form@equipIndex, a weapon's preloads), and the
	// state of a throwable's 3D in its biped: for the log and the equipped dev command.
	std::string DescribeHeld(RE::Actor* a_actor);

	// What hangs under the actor's hand, weapon and anim-object nodes (the children's names): where a
	// throwable's model sits during a throw, for the log.
	std::string DescribeHands(RE::Actor* a_actor);
}
