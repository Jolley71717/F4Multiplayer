#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

// The game's menus as their open and close events tell them, for the stress loop's checks (the menus dev
// command; game/MenuWatch.cpp feeds it). Kept from the events rather than asked of the UI: asking
// (GetMenuOpen) takes a reference to the menu, and a reference held beside the renderer is how the
// workshop menu once deadlocked the game when it closed. Pure, so it is unit tested; not thread-safe.
namespace MenuLog
{
	using Clock = std::chrono::steady_clock;

	// Menus up nearly all the time in play: never the one a test is stuck in.
	constexpr std::initializer_list<std::string_view> ALWAYS_UP{ "HUDMenu", "CursorMenu", "FaderMenu" };

	// More menu names than this are not tracked (the game has a few dozen; a flood of names is a bug).
	constexpr std::size_t MAX_MENUS = 64;

	struct Menu
	{
		std::string       name;
		bool              open = false;
		Clock::time_point since{};  // when it opened (open) or closed (not)
		std::uint32_t     opens = 0;
		std::uint32_t     closes = 0;
	};

	class Log
	{
	public:
		// A menu opened or closed. A second open of an open menu keeps the first one's time: the menu has
		// been up since then. A close of a menu never seen open (the watch began while it was up) counts.
		void Event(std::string_view a_name, bool a_opening, Clock::time_point a_now)
		{
			++events;
			auto menu = Find(a_name);
			if (!menu) {
				if (menus.size() >= MAX_MENUS) {
					++untracked;
					return;
				}
				menu = &menus.emplace_back(Menu{ .name = std::string{ a_name } });
			}
			if (a_opening) {
				++menu->opens;
				if (!menu->open) {
					menu->open = true;
					menu->since = a_now;
				}
			} else {
				++menu->closes;
				menu->open = false;
				menu->since = a_now;
			}
		}

		[[nodiscard]] bool IsOpen(std::string_view a_name) const
		{
			const auto menu = Find(a_name);
			return menu && menu->open;
		}

		// How long the menu has been open; zero when it isn't.
		[[nodiscard]] Clock::duration OpenFor(std::string_view a_name, Clock::time_point a_now) const
		{
			const auto menu = Find(a_name);
			return menu && menu->open ? a_now - menu->since : Clock::duration{};
		}

		[[nodiscard]] std::uint32_t Opens(std::string_view a_name) const
		{
			const auto menu = Find(a_name);
			return menu ? menu->opens : 0;
		}

		[[nodiscard]] std::uint32_t Closes(std::string_view a_name) const
		{
			const auto menu = Find(a_name);
			return menu ? menu->closes : 0;
		}

		// The menu open the longest, leaving out the ones always up in play; null when none is.
		[[nodiscard]] const Menu* Longest() const
		{
			const Menu* longest = nullptr;
			for (const auto& menu : menus) {
				if (menu.open && !AlwaysUp(menu.name) && (!longest || menu.since < longest->since)) {
					longest = &menu;
				}
			}
			return longest;
		}

		// One line for scripts, fields separated by spaces:
		//   open=WorkshopMenu:12.3,PipboyMenu:0.5 longest=WorkshopMenu:12.3 events=40 counts=WorkshopMenu:20/19,...
		// open lists every open menu with its seconds open (or none); longest is Longest() (or none);
		// counts is opens/closes per menu, in the order they were first seen.
		[[nodiscard]] std::string Describe(Clock::time_point a_now) const
		{
			std::string open;
			std::string counts;
			for (const auto& menu : menus) {
				if (menu.open) {
					open += std::format("{}{}:{:.1f}", open.empty() ? "" : ",", menu.name, Seconds(a_now - menu.since));
				}
				counts += std::format("{}{}:{}/{}", counts.empty() ? "" : ",", menu.name, menu.opens, menu.closes);
			}
			const auto longest = Longest();
			auto out = std::format("open={} longest={} events={}", open.empty() ? "none" : open,
				longest ? std::format("{}:{:.1f}", longest->name, Seconds(a_now - longest->since)) : std::string{ "none" }, events);
			if (untracked) {
				out += std::format(" untracked={}", untracked);
			}
			out += std::format(" counts={}", counts.empty() ? "none" : counts);
			return out;
		}

		[[nodiscard]] static bool AlwaysUp(std::string_view a_name) { return std::ranges::find(ALWAYS_UP, a_name) != ALWAYS_UP.end(); }

	private:
		static double Seconds(Clock::duration a_duration) { return std::chrono::duration<double>(a_duration).count(); }

		Menu* Find(std::string_view a_name)
		{
			const auto it = std::ranges::find(menus, a_name, &Menu::name);
			return it != menus.end() ? &*it : nullptr;
		}

		const Menu* Find(std::string_view a_name) const
		{
			const auto it = std::ranges::find(menus, a_name, &Menu::name);
			return it != menus.end() ? &*it : nullptr;
		}

		std::vector<Menu> menus;  // in the order first seen (a vector: Longest hands out pointers only until the next event)
		std::uint32_t     events = 0;
		std::uint32_t     untracked = 0;
	};
}
