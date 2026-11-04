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
		kFader,  // FaderMenu: the screen fading to or from black (Game.FadeOutGame, a loading screen), or held black
		kLooks,
		kPause,
		kPipboy,
		kPowerArmorMod,
		kVats,
		kWorkshop,
		// The rest only say what kind of menu friends see us in (MenuKinds).
		kBarter,
		kBook,
		kContainer,
		kCooking,
		kExamine,
		kHolotape,
		kLevelUp,
		kLockpicking,
		kMessageBox,
		kSitWait,
		kSleepWait,
		kTerminal,
		kTerminalHolotape,
		// Menus a friend's stage can open here by itself (CatchUpMenus): the companion's new home (Workshop_CaravanMenu) and the
		// vignette over a featured item's inspection (VignetteMenu; the inspection itself is ExamineMenu).
		kCaravan,
		kVignette,
		kRobotMod,  // RobotModMenu: Automatron's robot workbench (an ExamineMenu: a crafting menu, Storage.cpp)
		// The multiplayer menu of our own (MPMenu): friends see us in a menu, as they did in its message box.
		kMultiplayer,
		kCount
	};

	// The game's names for them (MenuOpenCloseEvent::menuName), in Menu's order.
	constexpr std::array<std::string_view, static_cast<std::size_t>(Menu::kCount)> NAMES{ "Console", "DialogueMenu", "FaderMenu", "LooksMenu", "PauseMenu",
		"PipboyMenu", "PowerArmorModMenu", "VATSMenu", "WorkshopMenu", "BarterMenu", "BookMenu", "ContainerMenu", "CookingMenu", "ExamineMenu", "HolotapeMenu",
		"LevelUpMenu", "LockpickingMenu", "MessageBoxMenu", "SitWaitMenu", "SleepWaitMenu", "TerminalMenu", "TerminalHolotapeMenu", "Workshop_CaravanMenu",
		"VignetteMenu", "RobotModMenu", "F4MPMenu" };

	// The main menu: none of the menus above (it says nothing of what friends see us in), but kept on its own. Once it
	// opens, the world played until then is gone, whatever starts the next one: a load, a new game, or a coc from the menu,
	// which sends no message at all (WorkshopSync forgets our pieces of the old world by it).
	inline constexpr std::string_view MAIN_MENU = "MainMenu";

	// Whether two menu names are the same. The game's strings are case-blind (BSFixedString, and the UI's own lookup):
	// its string pool keeps a name in the case it was first made in, which need not be the menu's own.
	constexpr bool SameName(std::string_view a_lhs, std::string_view a_rhs)
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

	// The open menus, one bit each. Events come on the UI's thread while any thread asks.
	class Book
	{
	public:
		// A menu opened or closed (its event). A menu not in the list is no business of the book's.
		void Seen(std::string_view a_name, bool a_opening)
		{
			if (SameName(a_name, MAIN_MENU)) {
				if (a_opening) {
					mainMenus.fetch_add(1);
				}
				mainMenu = a_opening;
				return;
			}
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

		// Whether the main menu is open, and how often it has opened (the game's start is the first).
		[[nodiscard]] bool          MainMenuOpen() const { return mainMenu.load(); }
		[[nodiscard]] std::uint32_t MainMenus() const { return mainMenus.load(); }

	private:
		static constexpr std::uint32_t Bit(Menu a_menu) { return 1u << static_cast<std::uint32_t>(a_menu); }

		std::atomic<std::uint32_t> bits{ 0 };
		std::atomic<bool>          mainMenu{ false };
		std::atomic<std::uint32_t> mainMenus{ 0 };
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

	// Book::MainMenuOpen and Book::MainMenus. Any thread.
	[[nodiscard]] bool          MainMenuOpen();
	[[nodiscard]] std::uint32_t MainMenus();
}
