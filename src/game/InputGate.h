#pragma once

#include <cstdint>
#include <format>
#include <optional>
#include <string>

// No input reaches the game while its window is not in front, though the game runs on (bRunInBackground).
//
// The game itself stops reading its devices when its window loses focus: Main::SetActive(false) turns the input
// manager's polling off (BSInputDeviceManager::pollingEnabled) and clears every device's state. But with
// bAlwaysActive on, which bRunInBackground sets in a session (Background.h), SetActive takes every focus change as
// "active": polling stays on while the player is in another window, and the mouse, keyboard and pad go on reaching
// the player's controls (2026-10-19: alt-tabbed, the character fired at clicks meant for another window). So once a
// frame this does what the game would: polling off while the window is not in front, the devices' state cleared as
// it closes and again as it opens, so nothing pressed meanwhile (or latched as the window went) is read as pressed.
// The click that brings the window back is the other window's, too: with a mouse button down as the game comes to
// the front, it stays closed until the button is up. The game runs on all the while; only its input waits.
//
// Pure, so it is unit tested; main.cpp reads the window and the buttons and sets what it answers.
namespace InputGate
{
	struct Step
	{
		std::optional<bool> polling;  // set the input manager's polling to this
		bool                clear = false;   // clear every device's state (the game's own ClearInputState)
		bool                closed = false;  // the window went out of front: input stopped (for the log)
		bool                opened = false;  // back in front: input goes again (for the log)
	};

	class Gate
	{
	public:
		enum class State : std::uint8_t
		{
			kOpen,
			kClosed,       // the window is not in front
			kWaitRelease,  // in front again, with a mouse button still down from the click that brought it there
		};

		// a_enabled: the game runs on unfocused (its bAlwaysActive on): without it the game stops its input itself.
		// a_inFront: the game's window is in front. a_buttonDown: a mouse button is down now.
		Step Frame(bool a_enabled, bool a_inFront, bool a_buttonDown)
		{
			Step step;
			if (!a_enabled) {
				// Off while closed (the session ended alt-tabbed): the game took the last focus change as "active", as
				// bAlwaysActive was on then, and would read input in the background until the next one. Kept closed until the
				// window is in front; from then on the game handles its focus itself.
				if (state != State::kOpen) {
					if (a_inFront) {
						Open(step);
					} else {
						state = State::kClosed;
						step.polling = false;
					}
				}
				return step;
			}
			switch (state) {
			case State::kOpen:
				if (!a_inFront) {
					state = State::kClosed;
					step.polling = false;
					step.clear = true;
					step.closed = true;
					++closes;
				}
				break;
			case State::kClosed:
			case State::kWaitRelease:
				if (!a_inFront) {
					state = State::kClosed;
					step.polling = false;  // again each frame: the game's SetActive turns it back on at a focus change
				} else if (a_buttonDown) {
					state = State::kWaitRelease;
					step.polling = false;
					if (!waitedForRelease) {
						waitedForRelease = true;
						++swallowedClicks;
					}
				} else {
					Open(step);
				}
				break;
			}
			return step;
		}

		[[nodiscard]] State Now() const { return state; }

		[[nodiscard]] std::string Describe() const
		{
			const char* name = state == State::kOpen ? "open" : state == State::kClosed ? "closed" : "waiting for the click's release";
			return std::format("input gate: {} (closed {} time(s), {} click(s) bringing the game back kept from it)", name, closes, swallowedClicks);
		}

		std::uint32_t closes = 0;
		std::uint32_t swallowedClicks = 0;

	private:
		void Open(Step& a_step)
		{
			state = State::kOpen;
			waitedForRelease = false;
			a_step.polling = true;
			a_step.clear = true;
			a_step.opened = true;
		}

		State state = State::kOpen;
		bool  waitedForRelease = false;
	};
}
