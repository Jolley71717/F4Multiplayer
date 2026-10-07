#pragma once

// The story: whether the main story and faction quests are shared, and whether the player is still
// in the game's opening (before leaving Vault 111), where nothing from the session reaches their
// game. Main thread only.
namespace Story
{
	// From the host's Welcome: true = main story and faction quests are shared like side quests.
	void SetShared(bool a_shared);
	[[nodiscard]] bool Shared();

	// Main story, faction and DLC story quests (QUEST_DATA::questType 1-5, 8+). Side quests are 7.
	[[nodiscard]] bool IsStoryType(std::int8_t a_type);

	// Before "Exit Vault 111" (Out of Time) is done, in the pre-war world or Vault 111. Its scripted
	// scenes need their NPCs, doors and time of day as the game set them, so the session leaves
	// this player's world alone and doesn't report it: no shared NPCs, world changes, quests, time
	// or weather, damage or teleports. What arrived meanwhile is applied once they're out.
	[[nodiscard]] bool InOpening();

	// Updates InOpening. Every frame, before the rest of the session: right after loading a save
	// it must already be right.
	void Frame();

	std::string Describe();
}
