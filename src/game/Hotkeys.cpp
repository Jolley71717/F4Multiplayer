#include "game/Hotkeys.h"

#include "Config.h"

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
			bool              down = false;
			bool              held = false;  // the hold action fired for this press
			Clock::time_point since{};
		};

		Key list;
		Key teleport;
		Key ping;
		Key emotePoint;
		Key emoteCheer;
		Key emoteClap;

		bool GameHasFocus()
		{
			unsigned long pid = 0;
			GetWindowThreadProcessId(GetForegroundWindow(), &pid);
			return pid == GetCurrentProcessId();
		}

		bool MenuOpen()
		{
			static const RE::BSFixedString console{ "Console" };
			const auto ui = RE::UI::GetSingleton();
			return !ui || ui->menuMode != 0 || ui->GetMenuOpen(console);
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

		// Pressed: fires when the key goes down.
		const auto press = [&](Key& a_key, std::uint32_t a_vk, Action a_action) {
			const bool down = active && !chord && IsDown(a_vk);
			if (down && !a_key.down) {
				actions.push_back(a_action);
			}
			a_key.down = down;
		};
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
		return actions;
	}

	bool Held(std::uint32_t a_vk)
	{
		return IsDown(a_vk) && GameHasFocus();
	}
}
