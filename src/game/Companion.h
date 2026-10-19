#pragma once

// Companion mode, for testing only (dev channel): our player is driven by the game's own AI, like a companion,
// following another reference (a friend's stand-in). The engine walks it there with its real locomotion and
// pathfinding, around obstacles, at a companion's distance.
//
// How: the player is put under AI control (Papyrus Game.SetPlayerAIDriven, as the game's scenes do), linked to the
// target and handed a vanilla follow package that follows that linked ref. The default is Fallout4.esm's
// REFollowBrahmin (00143A5E, a FollowNPC package: 350 to 500 units from the ref linked under RELinkFollow 00084EB7).
// The plain FollowNPC template (00023D36, an unkeyworded link) and RunOnce packages did not move the player
// (2026-10-19). Nothing is added to the load order, so the content hash is untouched. Main thread only.
namespace Companion
{
	inline constexpr std::uint32_t FOLLOW_PACKAGE = 0x00143A5E;       // REFollowBrahmin
	inline constexpr std::uint32_t FOLLOW_LINK_KEYWORD = 0x00084EB7;  // RELinkFollow

	// Starts (or hands over again) following a_target with a_package, its target the player's ref linked under
	// a_keyword (0: unkeyworded). A short line saying what was done, or "error: ...".
	std::string On(RE::TESObjectREFR* a_target, RE::TESPackage* a_package, std::uint32_t a_keyword);

	// Gives the player back to the keyboard: AI control off, linked ref cleared, package re-evaluated.
	std::string Off();

	// A stand-in is being taken out of the world (form ID a_id): if it is what we follow, off first, and a conversation
	// of the player's with it is forgotten. An AI-driven player left following a stand-in that went away crashed the
	// game in its conversation with it (Actor::UpdateInDialogue on the gone stand-in, 2026-10-19, as its friend took a
	// load door). Call before disabling it.
	void Released(std::uint32_t a_id);

	// Each frame, after the session: off at once if what we follow is gone, lost its full process or is in another cell
	// (see Released).
	void Frame();

	// AI control on or off alone (a_native: PlayerCharacter::SetAIControlledPackage instead of the Papyrus call).
	std::string Drive(bool a_on, bool a_native);

	// Papyrus Actor.PathToReference(a_target, a_speed) on the player: one trip there (for standing close to a downed
	// friend; the follow package keeps its distance). An AI-driven player walks it, whatever a_speed asks.
	std::string PathTo(RE::TESObjectREFR* a_target, float a_speed);

	// Whether we are on, the target and the distance to it, the linked ref, AI control and the player's combat state.
	[[nodiscard]] std::string Describe();
}
