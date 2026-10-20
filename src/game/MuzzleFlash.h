#pragma once

// The muzzle flash of a friend's shot, or a mirrored NPC's (ROADMAP #17). A shot is replayed on the stand-in as
// "attackStart" on its graph (WeaponFire::PlayShot); nothing is fired. The engine draws the gun's own flash and its
// light for every replay the graph takes: it keeps a flash of its own on the stand-in's equipped gun and shows it when
// the firing animation plays (seen on the rig for pistols, shotguns, laser and automatic guns). A replay the graph
// refuses (a new attack while the last one still plays) gets no animation and so no flash from the engine: this
// shows the gun's flash model for it instead, at the gun's ProjectileNode, for the projectile's muzzle flash time, as
// a temporary art effect (MuzzleFlashBook.h keeps the times and the limits). Nothing is fired, so nothing can be hit.
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
