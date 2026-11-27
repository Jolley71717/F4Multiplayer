#pragma once

#include <chrono>
#include <optional>

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

	// Whether a stage of a_quest set in this game is not reported now: a scene of the quest was stopped here lately
	// (ForeignScene::Guard), so a stage its end set is not progress. a_remote: when a stage of it last came from another game.
	[[nodiscard]] bool StageHeldBack(std::uint32_t a_quest, std::optional<std::chrono::steady_clock::time_point> a_remote);

	// The plugin is taking a reference (form ID a_id; a_ref when its handle still resolves) out of the
	// world: a conversation it leaves open as its speaker is the plugin's to end (see StrandedDialogue).
	// Call before disabling it, so the scene it is in is still known.
	void Removed(std::uint32_t a_id, RE::TESObjectREFR* a_ref);

	// Ends a conversation left open without its speaker (gone, disabled or deleted) and without its
	// dialogue menu, when the plugin itself took that speaker out of the world (Removed): a friend's
	// stand-in. One the game left open, and a quest's scene, are left to the game. Call every frame;
	// out of dialogue it only checks that.
	void Frame();

	// Ends the conversation the engine has open with a speaker, as a player ends one that lets them go: a_forced false asks
	// the menu to cancel (Tab, its Goodbye: MenuInput), which the engine ends the conversation itself on; true ends it
	// ourselves with the reset every conversation ends with, for one that took no cancel (a greeting that can't be skipped).
	// For a conversation our game must not have (NpcSync, TalkOwner). The forced ending hides the menu only after the reset:
	// a hide while the conversation still held the speaker crashed the game (MenuMessages). Empty when it went, else why not.
	// Main thread.
	std::string End(bool a_forced);

	// Test (dev `dialogue reset`): ends the conversation as End(true) does (the reset every conversation ends with, which lets go of
	// its input layer and clears menuOpen, then the menu hidden: it stays up after the reset alone), and with
	// a_overScene also clears overSceneActor, which a conversation ended out of turn can leave set. Returns
	// the fields before and after.
	std::string ResetForTest(bool a_overScene);

	[[nodiscard]] std::string Describe();
}
