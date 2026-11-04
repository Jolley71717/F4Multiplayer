#include "game/MenuInput.h"

#include <array>
#include <format>
#include <vector>

namespace MenuInput
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		struct Release
		{
			std::string       menu;
			std::string       userEvent;
			std::uint32_t     key = 0;
			Clock::time_point due{};
		};

		std::vector<Release> releases;  // taps' releases still to send (main thread)
	}

	GameButtonEvent& NewEvent(std::int32_t a_device, std::string_view a_userEvent, std::int32_t a_id, bool a_down, float a_heldSecs)
	{
		static auto        slots = new std::array<GameButtonEvent, 8>{};
		static std::size_t next = 0;
		auto&              event = (*slots)[next++ % slots->size()];
		event.vtable = RE::VTABLE::ButtonEvent[0].address();
		event.device = a_device;
		event.deviceID = 0;
		event.eventType = static_cast<std::int32_t>(RE::INPUT_EVENT_TYPE::kButton);
		event.next = nullptr;
		event.timeCode = 0xFFFFFFFF;
		event.handled = 0;
		event.strUserEvent = std::string{ a_userEvent }.c_str();
		event.idCode = a_id;
		event.disabled = false;
		event.value = a_down ? 1.0f : 0.0f;
		event.heldDownSecs = a_down ? 0.0f : a_heldSecs;
		return event;
	}

	std::string Send(std::string_view a_menu, std::string_view a_userEvent, std::uint32_t a_key, bool a_down, float a_heldSecs)
	{
		const auto ui = RE::UI::GetSingleton();
		if (!ui) {
			return "no UI";
		}
		const auto menu = ui->GetMenu(RE::BSFixedString{ std::string{ a_menu }.c_str() });
		if (!menu) {
			return "the menu is gone";
		}
		if (!menu->OnStack()) {
			return "the menu is not on the menu stack";
		}
		// The menu's input side, as the game's input code reaches it (IMenu is a BSInputEventUser).
		const auto user = static_cast<RE::BSInputEventUser*>(menu.get());
		if (!user->inputEventHandlingEnabled) {
			return "the menu takes no input now";
		}
		auto&      event = NewEvent(0, a_userEvent, static_cast<std::int32_t>(a_key), a_down, a_heldSecs);
		const auto button = reinterpret_cast<const RE::ButtonEvent*>(&event);
		if (!user->ShouldHandleEvent(button)) {
			return std::format("the menu doesn't take {} now", a_userEvent);
		}
		user->OnButtonEvent(button);
		return {};
	}

	std::string Tap(std::string_view a_menu, std::string_view a_userEvent, std::uint32_t a_key)
	{
		auto why = Send(a_menu, a_userEvent, a_key, true);
		if (why.empty()) {
			releases.push_back({ std::string{ a_menu }, std::string{ a_userEvent }, a_key, Clock::now() + TAP_HOLD });
		}
		return why;
	}

	void Frame()
	{
		if (releases.empty()) {
			return;
		}
		const auto now = Clock::now();
		std::erase_if(releases, [&](const Release& a_release) {
			if (now < a_release.due) {
				return false;
			}
			if (const auto why = Send(a_release.menu, a_release.userEvent, a_release.key, false, std::chrono::duration<float>(TAP_HOLD).count()); !why.empty()) {
				REX::DEBUG("MenuInput: {}'s {} release not sent: {}", a_release.menu, a_release.userEvent, why);
			}
			return true;
		});
	}
}
