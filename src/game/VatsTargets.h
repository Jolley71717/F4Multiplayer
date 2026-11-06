#pragma once

#include <string>

// VATS never picks a friend's stand-in (bVatsSkipsFriends; VatsTargetRules.h says how VATS picks its targets and why
// this is a hook). This patches the engine (1.11.240, by Address Library ID) and calls VatsTargetRules::Judge.
namespace VatsTargets
{
	// At plugin load: the four calls of VATS's target check go through a hook of ours, once each is checked to be
	// 1.11.240's. Where one isn't (another version, another plugin's patch), none is hooked, logged. Whether the hook is in.
	bool Install();

	// Dev (`vatstargets skip on|off`): bVatsSkipsFriends for this run, read by the hook from then on (any thread).
	// Install takes the ini's.
	void SetSkipFriends(bool a_skip);

	// For `net status`: whether the hook is in, whether it skips friends, and how often it left a stand-in out.
	[[nodiscard]] std::string Describe();

	// Dev (`vatstargets`): VATS's potential target list as the engine last gathered it (each reference's form ID, its
	// distance from the camera and whether it is a friend's stand-in), after what Describe says. Main thread.
	[[nodiscard]] std::string DescribeTargets();
}
