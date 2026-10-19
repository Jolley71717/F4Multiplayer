#pragma once

// The muzzle flash of a friend's shot, or a mirrored NPC's (ROADMAP #17). A replayed shot fires nothing, so the engine
// draws no flash; this shows the one the stand-in's gun would: its projectile's muzzle flash model, at the gun's
// ProjectileNode, for the projectile's muzzle flash time (MuzzleFlashBook.h keeps the times and the limits). Nothing
// is fired, so nothing can be hit. Main thread only.
namespace MuzzleFlash
{
	// How a flash is shown (dev `muzzle mode`, for trying them on the rig).
	enum class Mode : std::uint8_t
	{
		kOff,
		kArt,       // the engine's temporary effect: an art object made at runtime for the model, applied at the muzzle
		kDatabase,  // the model database's own node for the model, hung under the muzzle for the flash's time
	};

	// A replayed shot of this actor's (its gun out): its flash shows, or the one showing stays up.
	void OnShot(RE::Actor* a_actor);

	// Takes down flashes that are due. Call every frame.
	void Frame();

	// The actor is about to be deleted: its flash comes off now.
	void Drop(std::uint32_t a_actorFormId);

	// Every flash comes off: before a save or a load, and when the session ends.
	void DropAll();

	void               SetMode(Mode a_mode);
	[[nodiscard]] Mode GetMode();

	// A test aid (dev `muzzle hold <seconds>`, 0 to end it): every flash shows this long, to be caught in a screenshot.
	void SetHold(float a_seconds);

	// What the actor's gun flashes and where (dev `muzzle <actor>`); a_tree adds the muzzle's nodes.
	[[nodiscard]] std::string DescribeActor(RE::Actor* a_actor, bool a_tree);

	[[nodiscard]] std::string Describe();
}
