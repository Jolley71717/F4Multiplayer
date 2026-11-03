#pragma once

#include <string>

// A queued hit reaction never reaches an actor with no AI process (CombatReaction.h says why: a stand-in replaced in a
// fight crashed the game in AIProcess::EnterCombat). This patches the engine (1.11.240, by Address Library ID) and calls
// CombatReaction::Judge.
namespace CombatReactions
{
	// At plugin load: the task queue's call of the victim's reaction goes through a hook of ours, once the call is
	// checked to be 1.11.240's. Where it isn't (another version, another plugin's patch), nothing is hooked, logged.
	// Whether the hook is in.
	bool Install();

	// For `net status`: whether the hook is in, and how many reactions it skipped.
	[[nodiscard]] std::string Describe();
}
