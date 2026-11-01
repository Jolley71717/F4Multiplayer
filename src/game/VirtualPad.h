#pragma once

#include <string>
#include <string_view>

namespace RE
{
	class BSInputDeviceManager;
}

// The dev channel's virtual gamepad, for scripted rig tests (PadPlan.h holds what it holds). The game's XInput imports
// (XInputGetState, XInputGetCapabilities, XInputSetState from XINPUT1_3.dll) are hooked, and while the pad is plugged
// in, pad 0 is ours: the game's own gamepad code reads it as a real one and moves the player with it. Each game process
// has its own pad, so the host's and the VM's games are driven apart, and nothing goes through Windows' input: the
// game's window need not be in front, and the keyboard and mouse of whoever sits at the PC are left alone.
//
// With the game's window not in front, InputGate turns the game's input polling off (the game would read the desktop's
// mouse and keys meant for another window). While the pad is plugged in, GateFrame keeps polling on for the pad alone:
// the game's keyboard and mouse devices skip their polls until the window is in front again.
//
// Dev channel only (bDevChannel): without it nothing is hooked and no pad is ever plugged in.
namespace VirtualPad
{
	// At plugin load: hooks the game's XInput imports (dev channel only).
	void Install();

	// pad ... (PadPlan::USAGE), on the main thread.
	std::string Command(std::string_view a_args);

	// walkto ... (PadPlan::WALK_USAGE), on the main thread: steers the player with the sticks toward a reference or a
	// point, re-aimed each frame, until within the radius; progress goes to the log once a second.
	std::string WalkTo(std::string_view a_args);

	// Each frame on the main thread: steps a walkto.
	void Frame();

	// Each frame on the main thread, after InputGate: with the pad plugged in and the window not in front, the game's
	// polling is kept on for the pad, and its keyboard and mouse muted.
	void GateFrame(RE::BSInputDeviceManager& a_input, bool a_front);

	// Whether the pad is plugged in (main.cpp asks the window's state only when it matters).
	bool Plugged();
}
