#pragma once

#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>

// The game (1.11) reads the mouse through Raw Input: it registers the mouse (usage page 1, usage 2) with
// RegisterRawInputDevices and reads WM_INPUT in its window procedure. A game whose window came up behind another (the
// rig's VM starts the game from a scheduled task; the host's game started from a shell) can be left with keys working and
// clicks and mouse moves never arriving, for the whole run (2026-10-31 and 2026-11-01: shots=0 after every click while key
// presses still moved the player).
//
// So the dev channel reads the process's raw input registrations and, with the window in front, registers the mouse for
// the game's window when it isn't (every 2 s, before each `focus`, `click` and `mouse`, and on `mousedev fix`), keeping
// the game's own flags if it had a registration for another window. `mousedev` also counts the WM_INPUT messages the game
// takes off its queue, mouse and keyboard, so a dead mouse shows whether raw input stopped arriving at all. Never for a
// player: the dev channel is off in their games. Pure, so the decision is unit tested; DevCommands calls user32.
namespace MouseCapture
{
	constexpr std::uint16_t kPageGeneric = 0x01;
	constexpr std::uint16_t kUsageMouse = 0x02;
	constexpr std::uint32_t kRemove = 0x00000001;  // RIDEV_REMOVE

	// One RAWINPUTDEVICE as GetRegisteredRawInputDevices lists it.
	struct Registration
	{
		std::uint16_t  page = 0;
		std::uint16_t  usage = 0;
		std::uint32_t  flags = 0;
		std::uintptr_t target = 0;  // the window raw input goes to; 0 = whichever window has the keyboard focus
	};

	// The mouse's registration among the process's, if there is one.
	inline std::optional<Registration> MouseOf(std::span<const Registration> a_list)
	{
		for (const auto& r : a_list) {
			if (r.page == kPageGeneric && r.usage == kUsageMouse && (r.flags & kRemove) == 0) {
				return r;
			}
		}
		return std::nullopt;
	}

	// What the mouse should be registered as for the game's window to get it, or nothing when it already does: no
	// registration at all, or one for another window (a stale or destroyed one), gets a registration for a_window with
	// the flags the game used. A registration that follows the keyboard focus (target 0) is left alone: the window in
	// front has the focus.
	inline std::optional<Registration> Fix(std::span<const Registration> a_list, std::uintptr_t a_window)
	{
		if (a_window == 0) {
			return std::nullopt;
		}
		const auto mouse = MouseOf(a_list);
		if (mouse && (mouse->target == 0 || mouse->target == a_window)) {
			return std::nullopt;
		}
		return Registration{ kPageGeneric, kUsageMouse, mouse ? mouse->flags : 0u, a_window };
	}

	// The registrations for a report: page:usage, flags, and whose window ("game", "focus" for 0, else "other").
	inline std::string Describe(std::span<const Registration> a_list, std::uintptr_t a_window)
	{
		if (a_list.empty()) {
			return "none";
		}
		std::string out;
		for (const auto& r : a_list) {
			const char* whose = r.target == 0 ? "focus" : (r.target == a_window ? "game" : "other");
			out += std::format("{}{}:{} flags=0x{:X} to={}", out.empty() ? "" : ",", r.page, r.usage, r.flags, whose);
		}
		return out;
	}
}
