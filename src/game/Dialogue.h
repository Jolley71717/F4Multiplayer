#pragma once

// The player's conversation as the engine holds it (MenuTopicManager). While one is open the player
// can't draw a weapon, open the Pip-Boy or build. Main thread only.
namespace Dialogue
{
	// The engine's MenuTopicManager, or null if it isn't there (not RE::MenuTopicManager::GetSingleton(),
	// which reads another object: see Dialogue.cpp).
	[[nodiscard]] RE::MenuTopicManager* TopicManager();

	// Whether a_ref is in the player's conversation: the one speaking to them, or in the scene they are
	// in. Deleting it now would leave the conversation without its speaker.
	[[nodiscard]] bool Holds(RE::TESObjectREFR* a_ref);

	// The plugin is taking a reference (form ID a_id; a_ref when its handle still resolves) out of the
	// world: a conversation it leaves open as its speaker is the plugin's to end (see StrandedDialogue).
	// Call before disabling it, so the scene it is in is still known.
	void Removed(std::uint32_t a_id, RE::TESObjectREFR* a_ref);

	// Ends a conversation left open without its speaker (gone, disabled or deleted) and without its
	// dialogue menu, when the plugin itself took that speaker out of the world (Removed): a friend's
	// stand-in. One the game left open, and a quest's scene, are left to the game. Call every frame;
	// out of dialogue it only checks that.
	void Frame();

	// Test: runs the reset every conversation ends with (lets go of its input layer, clears menuOpen), and with
	// a_overScene also clears overSceneActor, which a conversation ended out of turn can leave set. Returns
	// the fields before and after.
	std::string ResetForTest(bool a_overScene);

	[[nodiscard]] std::string Describe();
}
