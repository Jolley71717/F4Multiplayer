#pragma once

#include "Protocol.h"
#include "game/OpenMenus.h"

#include <array>
#include <cstdint>
#include <string_view>

// Which of the game's menus run in real time in a session (bRealTimePipboy, bRealTimeMenus; the host's rules). Alone,
// each of them stops the game while it is open (the menu's "pauses the game" flag): in a session that stopped only
// our game, and friends saw us stand still while the world around them went on, the enemies we were fighting
// included. In real time the world goes on while we look, as in Fallout 76. Free of the game, so it is tested;
// game/RealTime.cpp makes them so.
//
// Never in real time: the pause menu (Esc stops our game as it always did; the session goes on for friends), the
// console, waiting and sleeping (they move the clock on), the character's looks (the looks menu, and the
// character's creation), and the loading and main menus.
namespace RealTimeMenus
{
	enum class Group : std::uint8_t
	{
		kNone,    // stops the game as it always did
		kPipboy,  // bRealTimePipboy: the Pip-Boy, its holotapes, and the perk chart
		kMenus,   // bRealTimeMenus: containers, trading, workbenches, terminals, lockpicking, books, message boxes
	};

	struct Entry
	{
		std::string_view menu;  // the game's name for it
		Group            group;
		OpenMenus::Menu  open;  // how OpenMenus knows it
	};

	inline constexpr std::array MENUS{
		Entry{ "PipboyMenu", Group::kPipboy, OpenMenus::Menu::kPipboy },
		Entry{ "PipboyHolotapeMenu", Group::kPipboy, OpenMenus::Menu::kPipboyHolotape },
		Entry{ "LevelUpMenu", Group::kPipboy, OpenMenus::Menu::kLevelUp },  // the perk chart, from the Pip-Boy or the level-up notice
		Entry{ "ContainerMenu", Group::kMenus, OpenMenus::Menu::kContainer },
		Entry{ "BarterMenu", Group::kMenus, OpenMenus::Menu::kBarter },
		Entry{ "ExamineMenu", Group::kMenus, OpenMenus::Menu::kExamine },  // weapon, armor and power armor workbenches; inspecting an item
		Entry{ "CookingMenu", Group::kMenus, OpenMenus::Menu::kCooking },  // cooking, chemistry
		Entry{ "TerminalMenu", Group::kMenus, OpenMenus::Menu::kTerminal },
		Entry{ "TerminalHolotapeMenu", Group::kMenus, OpenMenus::Menu::kTerminalHolotape },
		Entry{ "LockpickingMenu", Group::kMenus, OpenMenus::Menu::kLockpicking },
		Entry{ "BookMenu", Group::kMenus, OpenMenus::Menu::kBook },
		Entry{ "MessageBoxMenu", Group::kMenus, OpenMenus::Menu::kMessageBox },  // the game's message boxes: its tutorials, questions, the F1 menu
		Entry{ "PowerArmorModMenu", Group::kMenus, OpenMenus::Menu::kPowerArmorMod },
		Entry{ "FavoritesMenu", Group::kMenus, OpenMenus::Menu::kFavorites },
	};

	constexpr Group GroupOf(std::string_view a_name)
	{
		for (const auto& entry : MENUS) {
			if (OpenMenus::SameName(entry.menu, a_name)) {
				return entry.group;
			}
		}
		return Group::kNone;
	}

	// Any of these menus is open (a_open: OpenMenus's, in a test a stand-in). In real time they no longer stop the game,
	// so what kept still while one was up (the hotkeys) asks this too.
	template <class OpenFn>
	bool AnyOpen(OpenFn&& a_open)
	{
		for (const auto& entry : MENUS) {
			if (a_open(entry.open)) {
				return true;
			}
		}
		return false;
	}

	// Whether a menu of this group opened now runs in real time. a_inSession: welcomed into a session.
	constexpr bool RealTime(Group a_group, bool a_inSession, const Protocol::SessionRules& a_rules)
	{
		if (!a_inSession) {
			return false;
		}
		switch (a_group) {
		case Group::kPipboy:
			return a_rules.realTimePipboy;
		case Group::kMenus:
			return a_rules.realTimeMenus;
		default:
			return false;
		}
	}

	// The menu flags that stop the game: "pauses the game", and the frozen picture of the world some menus show
	// behind them (kFreezeFramePause), which stops the world being drawn. A real-time menu loses both.
	inline constexpr std::uint32_t PAUSES_GAME = 1u << 0;
	inline constexpr std::uint32_t FREEZE_FRAME_PAUSE = 1u << 24;

	constexpr std::uint32_t RealTimeFlags(std::uint32_t a_flags)
	{
		return a_flags & ~(PAUSES_GAME | FREEZE_FRAME_PAUSE);
	}

	// VATS: alone, aiming slows the world almost to a stop (and the shots play out slowed). In a session (bRealTimeVats)
	// it runs at full speed, as in Fallout 76: the game's slow motion is set back to normal while VATS is up. Not the
	// slow motion of Jet and the like (the game's "magic" time slowdown), which the player asked for.
	// a_multiplier: the game's global time multiplier now; a_magicSlowdown: VATS's own slowdown for Jet (0 = none).
	// The multiplier to set, or a negative value to leave it.
	constexpr float VatsTimeMultiplier(bool a_inSession, bool a_realTimeVats, bool a_vatsUp, float a_multiplier, float a_magicSlowdown)
	{
		if (!a_inSession || !a_realTimeVats || !a_vatsUp || a_magicSlowdown > 0.0f) {
			return -1.0f;
		}
		return a_multiplier < 0.999f ? 1.0f : -1.0f;
	}
}
