#pragma once

// Companion mode, for testing only (dev channel): our player is driven by the game's own AI, like a companion,
// following another reference (a friend's stand-in). The engine walks it there with its real locomotion and
// pathfinding: around obstacles, through doors, at a companion's distance.
//
// How: the player is linked to the target (an unkeyworded linked ref), put under AI control (Papyrus
// Game.SetPlayerAIDriven, as the game's scenes do) and handed a vanilla follow package that follows the linked
// ref. The default is Fallout4.esm's FollowNPC template package (00023D36: follow the linked ref, 200 to 500
// units). Nothing is added to the load order, so the content hash is untouched. Main thread only.
namespace Companion
{
	inline constexpr std::uint32_t FOLLOW_NPC_PACKAGE = 0x00023D36;

	enum class Way
	{
		kPut,      // Actor::PutCreatedPackage (a temporary package, as Papyrus PathToReference uses)
		kRunOnce,  // AIProcess::SetRunOncePackage
	};

	// Starts (or restarts, if the package was lost) following a_target with a_package. a_drive: put the player
	// under AI control first. A short line saying what was done, or "error: ...".
	std::string On(RE::TESObjectREFR* a_target, RE::TESPackage* a_package, Way a_way, bool a_drive);

	// Gives the player back to the keyboard: AI control off, linked ref cleared, package re-evaluated.
	std::string Off();

	// AI control on or off alone (a_native: PlayerCharacter::SetAIControlledPackage instead of the Papyrus call).
	std::string Drive(bool a_on, bool a_native);

	// Papyrus Actor.PathToReference(a_target, a_speed) on the player: one trip there (the script's fallback, and
	// for standing close to a downed friend; the follow package keeps its distance).
	std::string PathTo(RE::TESObjectREFR* a_target, float a_speed);

	// Whether we are on, what the player's AI is running (package, its procedure type, its target), the linked ref,
	// the distance to the target and the player's combat state.
	[[nodiscard]] std::string Describe();
}
