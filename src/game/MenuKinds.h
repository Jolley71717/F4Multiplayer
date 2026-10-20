#pragma once

#include "Protocol.h"
#include "game/OpenMenus.h"

#include <concepts>
#include <cstdint>
#include <initializer_list>

// The kind of menu our player is in, for friends (PlayerStatus::menu, Protocol::MenuKind): read from the open-menu book
// (OpenMenus), never from the UI. Only the kind goes out, never what the menu shows. Kept free of the game so it can be
// tested.
namespace MenuKinds
{
	// The kind for the menus open now; a_open(Menu) says whether one is. With several open, the one that says most about
	// what the player is doing: the pause menu (or the console, sleeping, a level up) over anything under it, then the
	// Pip-Boy, then a terminal, a container or trade, a workbench or workshop mode, a conversation, and the menus that
	// only hold the player a moment (a lock, a message box, a book, the looks menu). VATS is no menu here: friends see
	// that as a fight.
	template <class Open>
		requires std::invocable<Open&, OpenMenus::Menu>
	[[nodiscard]] constexpr std::uint8_t Of(Open&& a_open)
	{
		using enum OpenMenus::Menu;
		const auto any = [&](std::initializer_list<OpenMenus::Menu> a_menus) {
			for (const auto menu : a_menus) {
				if (a_open(menu)) {
					return true;
				}
			}
			return false;
		};
		if (any({ kPause, kConsole, kSleepWait, kSitWait, kLevelUp })) {
			return Protocol::MenuKind::kPaused;
		}
		if (any({ kPipboy })) {
			return Protocol::MenuKind::kPipboy;
		}
		if (any({ kTerminal, kTerminalHolotape, kHolotape })) {
			return Protocol::MenuKind::kTerminal;
		}
		if (any({ kContainer, kBarter })) {
			return Protocol::MenuKind::kLooting;
		}
		if (any({ kExamine, kCooking, kPowerArmorMod, kWorkshop })) {
			return Protocol::MenuKind::kCrafting;
		}
		if (any({ kDialogue })) {
			return Protocol::MenuKind::kTalking;
		}
		if (any({ kLockpicking, kMessageBox, kBook, kLooks })) {
			return Protocol::MenuKind::kOther;
		}
		return Protocol::MenuKind::kNone;
	}

	// The same, from the book.
	[[nodiscard]] inline std::uint8_t Of(const OpenMenus::Book& a_book)
	{
		return Of([&](OpenMenus::Menu a_menu) { return a_book.Open(a_menu); });
	}
}
