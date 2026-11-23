#pragma once

#include <string>

namespace RE
{
	class TESObjectREFR;
}

// A magic effect's queued hit-effect refresh never reaches a freed effect (EffectTask.h says why: a stand-in replaced in
// a fight crashed the game on a pure virtual call). This patches the engine (1.11.240, by Address Library ID) and calls
// EffectTask::Judge.
namespace EffectTasks
{
	// At plugin load: the task queue's call of the hit-effect refresh goes through a hook of ours, once the call is checked
	// to be 1.11.240's. Where it isn't (another version, another plugin's patch), nothing is hooked, logged.
	// Whether the hook is in.
	bool Install();

	// Before we disable an actor (any reference: only an actor has effects), on the main thread: its effects, which the
	// disable frees with its AI process, are kept as gone so a refresh queued for one of them is skipped.
	void Retire(RE::TESObjectREFR* a_ref);

	// For `net status`: whether the hook is in, how many effects were retired and how many refreshes it skipped.
	[[nodiscard]] std::string Describe();
}
