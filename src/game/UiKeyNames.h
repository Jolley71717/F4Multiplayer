#pragma once

#include "Text.h"

#include <cstdint>
#include <format>
#include <string>
#include <string_view>

// The keys a player can bind in the multiplayer menu: what each is called, and which can't be had. Windows virtual keys, as
// the ini holds them and Hotkeys reads them. Kept free of the game so it can be tested.
namespace UiKeyNames
{
	inline constexpr std::uint32_t BACKSPACE = 0x08;
	inline constexpr std::uint32_t TAB = 0x09;
	inline constexpr std::uint32_t ENTER = 0x0D;
	inline constexpr std::uint32_t ESCAPE = 0x1B;
	inline constexpr std::uint32_t DELETE_KEY = 0x2E;

	// What a key is called: the text itself for a letter, digit, F-key or symbol, or the id of a worded name in the language
	// files ("key.space"), which the page looks up (UiPages.cpp) so each language has its own. IsId tells them apart; the
	// number pad's id takes its digit as {n} (Number).
	[[nodiscard]] inline bool IsId(std::string_view a_name)
	{
		return a_name.starts_with("key.");
	}

	// The digit of a number pad key, for the {n} of its text; -1 for any other key.
	[[nodiscard]] constexpr int Number(std::uint32_t a_vk)
	{
		return a_vk >= 0x60 && a_vk <= 0x69 ? static_cast<int>(a_vk - 0x60) : -1;
	}

	[[nodiscard]] inline std::string Name(std::uint32_t a_vk)
	{
		if ((a_vk >= 'A' && a_vk <= 'Z') || (a_vk >= '0' && a_vk <= '9')) {
			return std::string(1, static_cast<char>(a_vk));
		}
		if (a_vk >= 0x70 && a_vk <= 0x87) {
			return std::format("F{}", a_vk - 0x6F);
		}
		if (a_vk >= 0x60 && a_vk <= 0x69) {
			return "key.num";
		}
		switch (a_vk) {
		case 0x04: return "key.mouse3";
		case 0x05: return "key.mouse4";
		case 0x06: return "key.mouse5";
		case 0x08: return "key.backspace";
		case 0x09: return "key.tab";
		case 0x0D: return "key.enter";
		case 0x1B: return "key.esc";
		case 0x10: return "key.shift";
		case 0x14: return "key.capslock";
		case 0x20: return "key.space";
		case 0x21: return "key.pageup";
		case 0x22: return "key.pagedown";
		case 0x23: return "key.end";
		case 0x24: return "key.home";
		case 0x25: return "key.left";
		case 0x26: return "key.up";
		case 0x27: return "key.right";
		case 0x28: return "key.down";
		case 0x2D: return "key.insert";
		case 0x2E: return "key.delete";
		case 0xA0: return "key.lshift";
		case 0xA1: return "key.rshift";
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

	// Name as the player reads it: the worded names in the current language (the English where a language lacks one).
	[[nodiscard]] inline std::string Display(std::uint32_t a_vk)
	{
		auto name = Name(a_vk);
		return IsId(name) ? Text::Format(name, { { "n", Number(a_vk) } }) : name;
	}

	// Keys a bind may not take: leaving menus (Esc, Tab), choosing in them (Enter), the game's quicksave and quickload (F5, F9:
	// a hotkey on F9 once reloaded a game mid-session), the clicks, the keys that make a chord (Hotkeys leaves a press with
	// Alt, Ctrl or Win alone, so one of those could never fire) and Print Screen.
	[[nodiscard]] constexpr bool Reserved(std::uint32_t a_vk)
	{
		switch (a_vk) {
		case 0x01:
		case 0x02:
		case TAB:
		case 0x0D:
		case 0x11:
		case 0x12:
		case ESCAPE:
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
