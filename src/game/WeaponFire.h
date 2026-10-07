#pragma once

// Weapon shots: the local player's and those of NPCs we run are reported, so other players see
// the shooter's firing animation. Only the animation is replayed (no projectile, no damage):
// damage already reaches everyone through health sync.
namespace WeaponFire
{
	// Hooks the player's animation events. Call every frame; does nothing once hooked.
	void Install();

	// Hooks NPCs' animation events (all NPCs share the hook), using any loaded NPC.
	void InstallForNpcs(RE::Actor* a_npc);

	// Shots by the local player since the last call.
	[[nodiscard]] std::uint32_t TakeShots();

	// NPCs (form IDs) that fired since the last call, one entry per shot. Includes NPCs we don't
	// run; the caller filters.
	[[nodiscard]] std::vector<std::uint32_t> TakeNpcShots();

	// Plays a firing animation on a stand-in or mirrored NPC (if its weapon is out).
	void PlayShot(RE::Actor* a_actor);

	// Replays a reload or aiming (a ShotAction other than a shot) on a stand-in or mirrored NPC.
	void PlayAction(RE::Actor* a_actor, std::uint8_t a_action);

	// Reloads and aiming by the local player, and by NPCs (form ID, action), since the last call.
	[[nodiscard]] std::vector<std::uint8_t> TakeActions();
	[[nodiscard]] std::vector<std::pair<std::uint32_t, std::uint8_t>> TakeNpcActions();

	// Ends the gunshot sounds that would otherwise loop (automatic weapons' fire sounds loop until
	// stopped). Call every frame.
	void Frame();

	[[nodiscard]] std::string Describe();
}

namespace WeaponFire
{
	// Test: starts (or stops) recording the player's animation events; returns what was recorded.
	std::string LogAnimationEvents(bool a_start);

	// Test: also records this NPC's events (hooks NPCs first).
	void LogActor(RE::Actor* a_actor);
}
