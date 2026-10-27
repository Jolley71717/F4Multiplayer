#pragma once

#include "Protocol.h"
#include "game/FrameUse.h"
#include "game/OwnGear.h"

// Equipped armor and weapons: read from the local player, copied onto puppets. Main thread only.
namespace Equipment
{
	// Whether a change the session makes to an inventory or to what an actor wears, for a friend's game, may go to this
	// reference (OwnGear::Judge): never to the local player, whose gear is theirs alone. Every dressing below asks first,
	// and so do the shared loot and pickups (WorldSync). A refusal is counted and logged.
	[[nodiscard]] bool MayChange(RE::TESObjectREFR* a_ref, OwnGear::Change a_change);

	// MayChange in two steps, for a caller that may wait on a refusal rather than take it (a friend's pickup of an item
	// in no cell, OwnGear::TryAgain): the verdict, read from the engine and neither counted nor logged; then Count, which
	// counts and logs a refusal and says whether the change goes ahead.
	[[nodiscard]] OwnGear::Verdict Judge(RE::TESObjectREFR* a_ref, OwnGear::Change a_change);
	bool                           Count(RE::TESObjectREFR* a_ref, OwnGear::Change a_change, OwnGear::Verdict a_verdict);

	// A session starts (its welcome): the refusals' counts and log budgets start again.
	void OnSessionStart();

	// The refusals so far this session, for the status line.
	std::string DescribeOwnGear();

	// What the local player carries and wears, for the read-only `inventory` dev command: the counts, a checksum of every
	// stack (form and count), the Pip-Boy and the caps, and what is equipped.
	std::string DescribeInventory(RE::Actor* a_actor);

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
	// A gun in its hand is only taken away (StandInGear::TakeAwayFirst): true then, and its new copy is given by a later
	// call (StandInGear::TookAway).
	bool WearOnStandIn(RE::Actor* a_actor, const std::vector<std::uint32_t>& a_items, const std::vector<Protocol::ItemMods>& a_mods);

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

	// The frame item the engine would put on the actor were it switched into a frame now (PowerArmor's frame pick, 2219424
	// in 1.11.240, picking afresh): the first frame item one of whose own keywords is on an item the actor has equipped,
	// else the default frame item. 0 when there is none. See ArmorFrames::ChoosePick.
	[[nodiscard]] std::uint32_t FramePick(RE::Actor* a_actor);

	// An item that makes the engine pick this frame item for an actor that has it on (FramePick): armor that is neither a
	// frame item nor a piece, with a keyword of the frame item's own (not the frame keyword or ArmorTypePower, which every
	// frame item has); one of a_prefer if one will do, else the first in the game's list (a vault suit for the plain
	// frame). 0 when none is (a frame item with no keyword of its own, as the default one; or no frame item here).
	[[nodiscard]] std::uint32_t FrameKey(std::uint32_t a_frame, const std::vector<std::uint32_t>& a_prefer);

	// Gives the actor the item if it has none and equips it, for the frame pick (FrameKey).
	void PutOnForPick(RE::Actor* a_actor, std::uint32_t a_item);

	// In power armor: gives the actor these pieces and weapons (if it doesn't have them) and equips those it doesn't wear
	// yet, and takes off the pieces and weapons it wears that are not among them. Nothing else is touched, and nothing
	// leaves its inventory: Apply empties it first, which would take the frame the engine put on it, and clothes put on
	// would go under the armor at best or into the frame's slots.
	// Their pieces and weapons are built with their mods as in WearOnStandIn, and true as there.
	bool WearInArmor(RE::Actor* a_actor, const std::vector<std::uint32_t>& a_items, const std::vector<Protocol::ItemMods>& a_mods);

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

	// The rotation of the actor's left thigh (LEG_BONE, which both skeletons have) relative to its parent, row by row:
	// what its animation graph writes each frame it plays something (see ArmorFrames::Stride). False when its 3D or the
	// bone is not there.
	bool LegRotation(RE::Actor* a_actor, float (&a_out)[9]);

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

	// What the engine lists as equipped on the actor (form@equipIndex, a weapon's preloads), and the state
	// of a throwable's 3D in its biped: for the log and the equipped dev command.
	std::string DescribeHeld(RE::Actor* a_actor);

	// The grenade or mine the engine lists as equipped on the actor, or null.
	RE::TESObjectWEAP* HeldThrowable(RE::Actor* a_actor);

	// The node of that name anywhere under a_root, or null (also when the object by that name is no node).
	RE::NiNode* ChildNode(RE::NiAVObject* a_root, std::string_view a_name);

	// The child of a_node (not deeper) with that name, or null.
	RE::NiAVObject* DirectChild(RE::NiNode* a_node, std::string_view a_name);

	// What hangs under the actor's hand and weapon nodes (the children's names; the engine's WeaponLeft with
	// where it sits and what it holds): where a throwable's model is during a throw, for the log.
	std::string DescribeHands(RE::Actor* a_actor);

	// What the engine looks at as our player tries to get into a frame (see FrameUse.h): its tilt, its markers' users,
	// and the engine's own two answers. Main thread.
	[[nodiscard]] FrameUse::Facts FrameUseOf(RE::TESObjectREFR* a_frame);

	// That, with what else holds our player off it (FrameUse::Holds: the gate asked to look, FrameAsk::kLook, nothing told;
	// its activation block; its state; our player's input), for the log and the paframes dev command.
	std::string DescribeFrameUse(RE::TESObjectREFR* a_frame);

	// Our player as the engine has them for furniture: their sit state, the furniture they occupy, the frame they wear or
	// left last, the frame they last asked to get into, and whether an input layer has the Activate user event off (by
	// the layers' names) or their input is blocked. For the log and paframes. Main thread.
	std::string DescribePlayerUse();

	// Patches TESFurniture::Activate's call into the player's furniture use (FrameUse::USE_CALL): as our player gets into
	// a frame, what would have the engine say "You cannot use this at this time." is mended first (FrameUse::ChooseMend),
	// and a refusal is logged with what the engine found (DescribeFrameUse). The player's furniture enter's message, the
	// other place the game says it, is logged too (FrameUse::ENTER_MESSAGE_CALL). Each call is patched only where
	// 1.11.240 has it. Once, at load.
	void InstallFrameUse();

	// What is asked of a frame when the gate is asked.
	enum class FrameAsk
	{
		kEnter,     // our player gets in (before anything is mended or the engine starts getting them in)
		kTransfer,  // our player opened its inventory (Transfer): the menu is closed again
		kOther,     // someone else here got in (a companion ordered into it): they are sent out again (RemotePlayers)
		kLook,      // only asked whether our player would be kept out (DescribeFrameUse): nobody is told, nothing is counted
	};

	// Asked as our player gets into a frame or opens its inventory, and as anyone else here is found in one: true keeps
	// them out (RemotePlayers::KeepOutOfFrame, which says why on the HUD). a_by: who (null for our player). Main thread.
	using FrameGate = bool (*)(RE::TESObjectREFR* a_frame, FrameAsk a_ask, const RE::Actor* a_by);
	void SetFrameGate(FrameGate a_gate);

	// The copies our player is kept out of now, by their handles, each frame from the main
	// thread: the container menu's hook closes Transfer on one as the menu is shown (on the UI's thread).
	void SetTransfersKeptOut(std::vector<RE::ObjectRefHandle> a_handles);

	// Hooks the container menu's messages in its vtable, for a frame's Transfer (kTransfer). Once, at load.
	void InstallFrameGate();

	// What that hook mended and logged, for the paframes dev command.
	std::string DescribeFrameMends();
}
