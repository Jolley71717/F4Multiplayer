#pragma once

#include "Protocol.h"

// Equipped armor and weapons: read from the local player, copied onto puppets. Main thread only.
namespace Equipment
{
	// Base forms of the armor and weapons the actor has equipped, sorted, without duplicates.
	std::vector<std::uint32_t> Read(RE::Actor* a_actor);

	// The mods on the actor's copies of those items (their BGSObjectInstanceExtra), for the items whose copy carries
	// one: what a friend's stand-in of the actor is given its copies built with.
	std::vector<Protocol::ItemMods> ReadMods(RE::Actor* a_actor);

	// Replaces everything the puppet carries with these items and equips them, each built with its mods in a_mods (or the
	// item's default combination, none on record; see Give).
	void Apply(RE::Actor* a_puppet, const std::vector<std::uint32_t>& a_items, const std::vector<Protocol::ItemMods>& a_mods);

	// Dresses a mirrored NPC like its owner's copy as far as its own inventory allows (GearPlan): equips those of
	// these items it carries, unless one would take off something it wears that they are not. Nothing is given
	// and nothing taken off (the NPC and its inventory are this game's, and go into its saves), and an actor in a
	// power armor frame is left alone. Where its own roll lacks the owner's item, it wears its own. The one item
	// it may be given is a grenade or mine to hold for a replayed throw (HoldThrowable), which the throw spends.
	// Returns how many equip commands it issued.
	std::uint32_t Wear(RE::Actor* a_actor, const std::vector<std::uint32_t>& a_items);

	// Dresses a friend's stand-in (a puppet of this game, not an NPC of its world) over what it wears: gives it these
	// items if it doesn't have them and equips those it doesn't wear yet, and takes off any other armor or weapon it
	// wears; the rest of its inventory is left alone. What it has on already is left alone too: two console scripts
	// fewer for each, and the gun in its hand is not equipped over again; unless its copy carries other mods than theirs
	// in a_mods (StandInGear::ChooseCopy): then the copy is taken away and one built with their mods is given and equipped.
	void WearOnStandIn(RE::Actor* a_actor, const std::vector<std::uint32_t>& a_items, const std::vector<Protocol::ItemMods>& a_mods);

	// Whether the engine has the actor in power armor: the ExtraPowerArmor it puts on an actor as the actor gets
	// into a frame and takes off as it gets out. (The engine's own PlayerInPowerArmor said false on a player
	// visibly in a frame, 1.11.240.)
	[[nodiscard]] bool InPowerArmor(const RE::Actor* a_actor);

	// Whether this base form is a power armor frame: furniture with the keyword the engine itself looks for before it
	// treats furniture as a frame (PowerArmor::GetFurnitureKeyword, as TESFurniture::Activate and
	// Actor::StopInteractingQuick do). A terminal or a chair is furniture too.
	[[nodiscard]] bool IsPowerArmorFrame(const RE::TESForm* a_base);

	// Whether this item is the frame itself as armor, which the engine puts on an actor as it gets into a frame and takes
	// off as it gets out: armor with PowerArmor::GetFrameKeyword, or PowerArmor::GetDefaultFrame (what the engine itself
	// looks for, 1.11.240).
	[[nodiscard]] bool IsFrameItem(const RE::TESForm* a_item);

	// Whether this item is a power armor piece (armor with the ArmorTypePower keyword), the frame item aside.
	[[nodiscard]] bool IsPowerArmorPiece(const RE::TESForm* a_item);

	// In power armor: gives the actor these pieces and weapons (if it doesn't have them) and equips those it doesn't wear
	// yet, and takes off the pieces and weapons it wears that are not among them. A frame item among them goes on in place
	// of the one the engine picked, which is taken away (see ArmorFrames::Wears); with none, the engine's stays. Nothing
	// else is touched, and nothing else leaves its inventory: Apply empties it first, which would take the frame item the
	// engine put on it, and clothes put on would go under the armor at best or into the frame's slots.
	// Their pieces and weapons are built with their mods as in WearOnStandIn.
	void WearInArmor(RE::Actor* a_actor, const std::vector<std::uint32_t>& a_items, const std::vector<Protocol::ItemMods>& a_mods);

	// The frame the engine has the actor in (ExtraPowerArmor's frame), or none.
	[[nodiscard]] RE::NiPointer<RE::TESObjectREFR> PowerArmorFrameOf(RE::Actor* a_actor);

	// Switches the actor into the frame at once, as the game's SwitchToPowerArmor does (PowerArmor's switch-in,
	// 2219442): no AI, no package, no climb. The frame's own items are taken out of it first (only the pieces it is told to
	// wear go on), and it is given to the actor's base, so getting in is never a theft where the cell has an owner. Main
	// thread; the actor must have its 3D, a high or middle-high process (the switch-in queues the rebuild of its 3D only
	// then: AIProcess::Update3dModel with true, which hands it to TaskQueueInterface::QueueActorUpdateModel) and no power
	// armor yet. Whether the engine has it in that frame now. Its 3D on return is still the one it had: the power armor
	// skeleton is there only once that task has run (HasPowerArmorSkeleton), so nothing of the new skeleton, biped or
	// collision may be read right after this.
	bool SwitchIntoPowerArmor(RE::Actor* a_actor, RE::TESObjectREFR* a_frame);

	// Whether the actor's 3D is built on the power armor skeleton: the one the switch-in's queued rebuild gives an actor
	// in a frame, which has bones its own skeleton has not. Power armor pieces are modelled for those bones.
	[[nodiscard]] bool HasPowerArmorSkeleton(RE::Actor* a_actor);

	// The engine keeps a frame persistent while an actor is in it (BGSDynamicPersistenceManager::PromoteReference, owner
	// the power armor race) and lets it go as the actor's exit finishes; an actor deleted in its frame never gets out, so
	// the frame is let go here, as that exit does, before both are deleted. Nothing is done to a frame no actor was
	// switched into: DemoteReference finds no promotion by that owner on it and returns (ExtraDataList::
	// RemovePromotedRefOwner false; read from disk, 1.11.240), so every frame of ours may be let go before it is deleted.
	void ReleasePowerArmorFrame(RE::TESObjectREFR* a_frame);

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

	// What the engine lists as equipped on the actor (form@equipIndex), and the state of a throwable's
	// 3D in its biped: for the log and the equipped dev command.
	std::string DescribeHeld(RE::Actor* a_actor);
}
