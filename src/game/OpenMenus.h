#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string_view>

// Which of the game's menus are open, kept from the menus' own open and close events. Asking the UI
// instead (UI::GetMenu, UI::GetMenuOpen) takes a reference to the menu and lets it go again, and the
// thread that lets go of a closing menu's last reference is the one that destroys it. A host froze
// scrapping in the workshop: the main thread, drawing the menus with the renderer's list locked, let go
// of the workshop menu's last reference, and its destructor waited for that same lock for ever. So the
// plugin asks here, which holds no menu and can be asked from any thread, and never the UI (a test
// checks the source). The bookkeeping is free of the game, so it is unit tested.
namespace OpenMenus
{
	enum class Menu : std::uint32_t
	{
		kConsole,
		kDialogue,
		kLooks,
		kPause,
		kPipboy,
		kPowerArmorMod,
		kVats,
		kWorkshop,
		kCount
	};

	// The game's names for them (MenuOpenCloseEvent::menuName), in Menu's order.
	constexpr std::array<std::string_view, static_cast<std::size_t>(Menu::kCount)> NAMES{ "Console", "DialogueMenu", "LooksMenu", "PauseMenu", "PipboyMenu",
		"PowerArmorModMenu", "VATSMenu", "WorkshopMenu" };

	// The open menus, one bit each. Events come on the UI's thread while any thread asks.
	class Book
	{
	public:
		// A menu opened or closed (its event). A menu not in the list is no business of the book's.
		void Seen(std::string_view a_name, bool a_opening)
		{
			for (std::size_t i = 0; i < NAMES.size(); ++i) {
				if (SameName(a_name, NAMES[i])) {
					const auto bit = 1u << i;
					if (a_opening) {
						bits.fetch_or(bit);
					} else {
						bits.fetch_and(~bit);
					}
					return;
				}
			}
		}

		[[nodiscard]] bool Open(Menu a_menu) const
		{
			return (bits.load() & Bit(a_menu)) != 0;
		}

		[[nodiscard]] bool AnyOpen(std::initializer_list<Menu> a_menus) const
		{
			std::uint32_t mask = 0;
			for (const auto menu : a_menus) {
				mask |= Bit(menu);
			}
			return (bits.load() & mask) != 0;
		}

	private:
		// The game's strings are case-blind (BSFixedString, and the UI's own lookup): its string pool keeps a name in the
		// case it was first made in, which need not be the menu's own.
		static constexpr bool SameName(std::string_view a_lhs, std::string_view a_rhs)
		{
			constexpr auto lower = [](char a_c) { return a_c >= 'A' && a_c <= 'Z' ? static_cast<char>(a_c - 'A' + 'a') : a_c; };
			if (a_lhs.size() != a_rhs.size()) {
				return false;
			}
			for (std::size_t i = 0; i < a_lhs.size(); ++i) {
				if (lower(a_lhs[i]) != lower(a_rhs[i])) {
					return false;
				}
			}
			return true;
		}

		static constexpr std::uint32_t Bit(Menu a_menu) { return 1u << static_cast<std::uint32_t>(a_menu); }

		std::atomic<std::uint32_t> bits{ 0 };
	};

	static_assert(static_cast<std::size_t>(Menu::kCount) <= 32);

	// The game's side (OpenMenus.cpp).

	// Watches the menus open and close. At the game's data being ready, before any of them can be up (a console
	// opened before then is taken for closed until it next opens: it only keeps hotkeys off).
	void Install();

	// Whether a_menu is open. Any thread.
	[[nodiscard]] bool Open(Menu a_menu);

	// Whether any of a_menus is open. Any thread.
	[[nodiscard]] bool AnyOpen(std::initializer_list<Menu> a_menus);
}
