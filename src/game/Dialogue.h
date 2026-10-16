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

	// Ends a conversation left open without its speaker (gone, disabled or deleted) and without its
	// dialogue menu, when that speaker was one of ours: a friend's stand-in, or an NPC the session
	// shares. Call every frame; out of dialogue it only checks that.
	void Frame();

	// Test: runs the reset every conversation ends with (lets go of its input layer, clears menuOpen), and with
	// a_overScene also clears overSceneActor, which a conversation ended out of turn can leave set. Returns
	// the fields before and after.
	std::string ResetForTest(bool a_overScene);

	[[nodiscard]] std::string Describe();
}
