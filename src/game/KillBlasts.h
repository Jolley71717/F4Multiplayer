#pragma once

#include <string>

// The console's kill never blasts a torso that has no limb value (KillBlast.h says why: the command read the value
// through null, and every flying ant swarm that died in one game crashed the other games, which land the death with that
// command). This patches the engine (1.11.240, by Address Library ID) and calls KillBlast::Judge.
namespace KillBlasts
{
	// At plugin load: the kill command's call of the AI-process test before its blast goes through a hook of ours, once
	// the call is checked to be 1.11.240's. Where it isn't (another version, another plugin's patch), nothing is hooked,
	// logged. Whether the hook is in.
	bool Install();

	// Whether the session's console kill of this actor is to be left out: the hook is not in and the command would crash
	// the game on it (KillBlast::Refused). Logged, the first few. Main thread.
	[[nodiscard]] bool Refused(RE::Actor* a_actor);

	// For `net status`: whether the hook is in, how many blasts it skipped and how many kills were left out without it.
	[[nodiscard]] std::string Describe();
}
