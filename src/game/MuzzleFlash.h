#pragma once

// The muzzle flash of a friend's shot, or a mirrored NPC's (ROADMAP #17). A shot is replayed on the stand-in as
// "attackStart" on its graph (WeaponFire::PlayShot). The engine draws the gun's own flash and its light when the
// firing animation's "WeaponFire" fires the gun: it keeps a flash of its own on the equipped gun (seen on the rig for
// pistols, shotguns, laser and automatic guns). That fire also launched a real bullet from the stand-in, which hit
// our player, so a stand-in's plain one (a bullet or a beam, AttackKind::HoldsShotOf) is kept from the engine
// (AttackKind::HoldsOwnFire), and the engine draws no flash for it either (seen on the rig), and so is one that may go
// off (a missile, a shell, an explosive round: its blast is the shooter's report); a flamer's stream still leaves the
// stand-in's gun as before, with the engine's flash. A stand-in's held shot, and any
// replay the graph refuses (a new attack while the
// last one still plays), gets this flash instead: the gun's flash model at the gun's ProjectileNode, for the
// projectile's muzzle flash time, as a temporary art effect (MuzzleFlashBook.h keeps the times and the limits).
// Main thread only.
namespace MuzzleFlash
{
	// When this shows a flash (dev `muzzle mode`, for trying it on the rig).
	enum class Mode : std::uint8_t
	{
		kOff,
		kRefused,  // only for a replay the graph refused, which the engine draws no flash for (the default)
		kAlways,   // for every replayed shot, on top of the engine's own (a test aid)
	};

	// A replayed shot of this actor's (its gun out); a_animated: the graph took the shot's "attackStart", so the engine
	// draws the flash itself.
	void OnShot(RE::Actor* a_actor, bool a_animated);

	// What the actor's equipped gun fires (its mods' and its ammunition's say included), or null.
	[[nodiscard]] RE::BGSProjectile* GunProjectile(RE::Actor* a_actor);
	// Whether the gun the actor holds carries an enchantment (a legendary's effect: an explosive round may go off by one).
	[[nodiscard]] bool GunEnchanted(RE::Actor* a_actor);

	// Keeps showing flashes drawn and takes down those that are due. Call every frame.
	void Frame();

	// The actor is about to be deleted: its flash comes off now.
	void Drop(std::uint32_t a_actorFormId);

	// Every flash comes off: before a save or a load, and when the session ends.
	void DropAll();

	void               SetMode(Mode a_mode);
	[[nodiscard]] Mode GetMode();

	// A test aid (dev `muzzle hold <seconds>`, 0 to end it): every flash shows this long, to be caught in a screenshot.
	void SetHold(float a_seconds);

	// What the actor's gun flashes and where, and the engine's own flash object for it (dev `muzzle <actor>`); a_tree
	// adds the muzzle's nodes.
	[[nodiscard]] std::string DescribeActor(RE::Actor* a_actor, bool a_tree);

	// Dev `muzzle watch`: samples the engine's flash object of this actor's gun every frame for a_frames frames; returns
	// what the last watch saw.
	std::string Watch(std::uint32_t a_actorFormId, int a_frames);

	[[nodiscard]] std::string Describe();
}
