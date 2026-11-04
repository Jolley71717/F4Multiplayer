#pragma once

#include <cstdint>
#include <format>
#include <string>

// The keys a player can bind in the multiplayer menu: what each is called, and which can't be had. Windows virtual keys, as
// the ini holds them and Hotkeys reads them. Kept free of the game so it can be tested.
namespace KeyNames
{
	[[nodiscard]] inline std::string Name(std::uint32_t a_vk)
	{
		if (a_vk >= 'A' && a_vk <= 'Z' || a_vk >= '0' && a_vk <= '9') {
			return std::string(1, static_cast<char>(a_vk));
		}
		if (a_vk >= 0x70 && a_vk <= 0x87) {
			return std::format("F{}", a_vk - 0x6F);
		}
		if (a_vk >= 0x60 && a_vk <= 0x69) {
			return std::format("Num {}", a_vk - 0x60);
		}
		switch (a_vk) {
		case 0x04: return "Mouse 3";
		case 0x05: return "Mouse 4";
		case 0x06: return "Mouse 5";
		case 0x08: return "Backspace";
		case 0x10: return "Shift";
		case 0x14: return "Caps Lock";
		case 0x20: return "Space";
		case 0x21: return "Page Up";
		case 0x22: return "Page Down";
		case 0x23: return "End";
		case 0x24: return "Home";
		case 0x25: return "Left";
		case 0x26: return "Up";
		case 0x27: return "Right";
		case 0x28: return "Down";
		case 0x2D: return "Insert";
		case 0xA0: return "Left Shift";
		case 0xA1: return "Right Shift";
		case 0xBA: return ";";
		case 0xBB: return "=";
		case 0xBC: return ",";
		case 0xBD: return "-";
		case 0xBE: return ".";
		case 0xBF: return "/";
		case 0xC0: return "`";
		case 0xDB: return "[";
		case 0xDC: return "\\";
		case 0xDD: return "]";
		case 0xDE: return "'";
		default: return std::format("Key {:02X}", a_vk);
		}
	}

	// Keys a bind may not take: leaving menus (Esc, Tab), choosing in them (Enter), the game's quicksave and quickload (F5, F9:
	// a hotkey on F9 once reloaded a game mid-session), the clicks, the keys that make a chord (Hotkeys leaves a press with
	// Alt, Ctrl or Win alone, so one of those could never fire) and Print Screen.
	[[nodiscard]] constexpr bool Reserved(std::uint32_t a_vk)
	{
		switch (a_vk) {
		case 0x01:
		case 0x02:
		case 0x09:
		case 0x0D:
		case 0x11:
		case 0x12:
		case 0x1B:
		case 0x2C:
		case 0x5B:
		case 0x5C:
		case 0x74:
		case 0x78:
		case 0xA2:
		case 0xA3:
		case 0xA4:
		case 0xA5:
			return true;
		default:
			return false;
		}
	}

	// A mouse button's code in the game's input events to its virtual key: the side buttons and the middle one (0: not one to bind).
	[[nodiscard]] constexpr std::uint32_t FromMouse(std::uint32_t a_code)
	{
		return a_code == 2 ? 0x04 : a_code == 3 ? 0x05 : a_code == 4 ? 0x06 : 0;
	}
}
