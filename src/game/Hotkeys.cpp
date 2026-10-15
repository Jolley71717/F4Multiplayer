#include "game/Hotkeys.h"

#include "Config.h"
#include "game/OpenMenus.h"

// user32/kernel32 (windows.h clashes with CommonLibF4's names).
extern "C" {
__declspec(dllimport) void* __stdcall GetForegroundWindow();
__declspec(dllimport) unsigned long __stdcall GetWindowThreadProcessId(void* a_window, unsigned long* a_processId);
__declspec(dllimport) unsigned long __stdcall GetCurrentProcessId();
__declspec(dllimport) short __stdcall GetAsyncKeyState(int a_key);
}

namespace Hotkeys
{
	namespace
	{
		using Clock = std::chrono::steady_clock;
		constexpr auto HOLD_TIME = 1s;

		struct Key
		{
			bool              down = false;      // down and counted (focus, no menu, no chord)
			bool              physical = false;  // down at all
			bool              held = false;      // the hold action fired for this press
			Clock::time_point since{};
		};

		Key menu;
		Key list;
		Key teleport;
		Key ping;
		Key emotePoint;
		Key emoteCheer;
		Key emoteClap;

		// For Describe: how often Poll ran, and the last actions it produced.
		std::uint64_t polls = 0;
		std::uint64_t actionsFired = 0;
		std::uint64_t blockedPresses = 0;  // a hotkey went down while focus, a menu or a chord blocked it

		bool GameHasFocus()
		{
			unsigned long pid = 0;
			GetWindowThreadProcessId(GetForegroundWindow(), &pid);
			return pid == GetCurrentProcessId();
		}

		bool MenuOpen()
		{
			const auto ui = RE::UI::GetSingleton();
			return !ui || ui->menuMode != 0 || OpenMenus::Open(OpenMenus::Menu::kConsole);
		}

		bool IsDown(std::uint32_t a_vk)
		{
			return a_vk != 0 && a_vk < 256 && (GetAsyncKeyState(static_cast<int>(a_vk)) & 0x8000) != 0;
		}
	}

	std::vector<Action> Poll()
	{
		std::vector<Action> actions;
		const auto&         settings = Config::Get();
		const bool          active = GameHasFocus() && !MenuOpen();
		// A chord with Alt, Ctrl or Win belongs to something else (Alt+F9 is ShadowPlay's record key): not ours.
		const bool          chord = IsDown(0x12) || IsDown(0x11) || IsDown(0x5B) || IsDown(0x5C);
		const auto          now = Clock::now();
		++polls;

		// Pressed: fires when the key goes down.
		const auto press = [&](Key& a_key, std::uint32_t a_vk, Action a_action) {
			const bool physical = IsDown(a_vk);
			const bool down = active && !chord && physical;
			if (down && !a_key.down) {
				actions.push_back(a_action);
			} else if (physical && !a_key.physical && !down) {
				++blockedPresses;  // Describe says why
			}
			a_key.down = down;
			a_key.physical = physical;
		};
		press(menu, settings.keyMenu, Action::kMenu);
		press(list, settings.keyPlayerList, Action::kPlayerList);
		press(ping, settings.keyPing, Action::kPing);
		press(emotePoint, settings.keyEmotePoint, Action::kEmotePoint);
		press(emoteCheer, settings.keyEmoteCheer, Action::kEmoteCheer);
		press(emoteClap, settings.keyEmoteClap, Action::kEmoteClap);

		// Teleport: a tap picks, a hold goes.
		const bool down = active && IsDown(settings.keyTeleport);
		if (down && !teleport.down) {
			teleport.since = now;
			teleport.held = false;
		} else if (down && !teleport.held && now - teleport.since >= HOLD_TIME) {
			teleport.held = true;
			actions.push_back(Action::kTeleportGo);
		} else if (!down && teleport.down && !teleport.held && active) {
			actions.push_back(Action::kTeleportPick);
		}
		teleport.down = down;
		actionsFired += actions.size();
		return actions;
	}

	bool Held(std::uint32_t a_vk)
	{
		return IsDown(a_vk) && GameHasFocus();
	}

	std::string Describe()
	{
		const auto&                    settings = Config::Get();
		const auto                     ui = RE::UI::GetSingleton();
		const auto                     key = [](const char* a_name, std::uint32_t a_vk) {
			return std::format(" {}={:02X}{}", a_name, a_vk, IsDown(a_vk) ? "(down)" : "");
		};
		return std::format("hotkeys: focus={} menuMode={} console={} chord: alt={} ctrl={} win={} polls={} fired={} blocked={} keys:",
				   GameHasFocus(), ui ? static_cast<int>(ui->menuMode) : -1, OpenMenus::Open(OpenMenus::Menu::kConsole), IsDown(0x12), IsDown(0x11), IsDown(0x5B) || IsDown(0x5C),
				   polls, actionsFired, blockedPresses) +
		       key("menu", settings.keyMenu) + key("list", settings.keyPlayerList) + key("teleport", settings.keyTeleport) + key("ping", settings.keyPing) + key("voice", settings.keyVoice);
	}
}
