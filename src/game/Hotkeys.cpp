#include "game/Hotkeys.h"

#include "Config.h"
#include "Platform.h"
#include "game/KeyWatch.h"
#include "game/OpenMenus.h"
#include "net/ModDiff.h"

// user32/kernel32 (windows.h clashes with CommonLibF4's names).
extern "C" {
__declspec(dllimport) void* __stdcall GetForegroundWindow();
__declspec(dllimport) void* __stdcall GetActiveWindow();
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
			int               logged = 0;        // presses written to the log (the first few: LOGGED_PRESSES)
		};

		// Each hotkey's first presses go to the log, taken or not and why: "F1 does nothing" is then one look at
		// the log (the key never seen at all, or seen and blocked by focus or a menu). A key, not what was typed.
		constexpr int LOGGED_PRESSES = 3;

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

		KeyWatch::Watch keyWatch;

		enum class Focus
		{
			kOurs,
			kNone,         // no window is in front at all
			kOther,        // another program's window is
			kWineActive,   // Wine with no foreground window, but the game's thread has its window active (see GameFocus)
		};

		Focus GameFocus()
		{
			const auto window = GetForegroundWindow();
			if (!window) {
				// Windows has no foreground window only for a moment (a switch between windows). Wine can have none while
				// the game is played (a compositor that never tells it which window is in front): there the game's own
				// thread is asked whether its window is active. Poll runs on that thread; elsewhere it answers no.
				return Platform::UnderWine() && GetActiveWindow() ? Focus::kWineActive : Focus::kNone;
			}
			unsigned long pid = 0;
			GetWindowThreadProcessId(window, &pid);
			return pid == GetCurrentProcessId() ? Focus::kOurs : Focus::kOther;
		}

		bool GameHasFocus()
		{
			const auto focus = GameFocus();
			if (focus == Focus::kWineActive) {
				static bool told = false;
				if (!std::exchange(told, true)) {
					REX::INFO("Hotkeys: Wine has no window in front; the game's window counts as in front while its thread has it active");
				}
			}
			return focus == Focus::kOurs || focus == Focus::kWineActive;
		}

		std::string_view FocusName(Focus a_focus)
		{
			switch (a_focus) {
			case Focus::kNone:
				return "no window is in front";
			case Focus::kOther:
				return "another program's window is in front";
			default:
				return "the game is in front";
			}
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

		// Why a hotkey that went down was not taken, for the log.
		std::string Blocked(bool a_chord)
		{
			const auto focus = GameFocus();
			if (focus != Focus::kOurs && focus != Focus::kWineActive) {
				return std::string{ FocusName(focus) };
			}
			if (a_chord) {
				return "Alt, Ctrl or Win is held with it";
			}
			const auto ui = RE::UI::GetSingleton();
			if (!ui) {
				return "the game's UI isn't there yet";
			}
			if (OpenMenus::Open(OpenMenus::Menu::kConsole)) {
				return "the console is open";
			}
			return std::format("a menu is open (menu mode {})", static_cast<int>(ui->menuMode));
		}

		// How the keys are read, once, with the first Poll.
		void LogMethod()
		{
			const auto& settings = Config::Get();
			REX::INFO("Hotkeys: read with GetAsyncKeyState while the game's window is in front and no menu is open{}; menu {}, player list {}, teleport {}, ping {}",
				Platform::UnderWine() ? " (Wine)" : "", ModDiff::KeyName(settings.keyMenu), ModDiff::KeyName(settings.keyPlayerList), ModDiff::KeyName(settings.keyTeleport),
				ModDiff::KeyName(settings.keyPing));
		}

		// Keys a player in play presses sooner or later (see KeyWatch): W, A, S, D, Space, E, the mouse buttons.
		bool AnyPlayKeyDown()
		{
			constexpr std::uint32_t KEYS[] = { 'W', 'A', 'S', 'D', 0x20, 'E', 0x01, 0x02 };
			return std::ranges::any_of(KEYS, IsDown);
		}

		void WatchKeys(bool a_inPlay, bool a_hotkeyDown)
		{
			switch (keyWatch.Tick(Clock::now(), a_inPlay, a_hotkeyDown || AnyPlayKeyDown())) {
			case KeyWatch::Event::kNothingSeen:
				REX::WARN("Hotkeys: in {} s of play, not one key was seen down (W A S D, Space, E, the mouse buttons, the hotkeys): the key reads "
						  "don't reach the mod here, so F1 and the other hotkeys do nothing{}. Unless you played with a controller all along",
					std::chrono::duration_cast<std::chrono::seconds>(KeyWatch::Watch::LIMIT).count(),
					Platform::UnderWine() ? " (Wine: try a newer Proton, 9 or later)" : "");
				break;
			case KeyWatch::Event::kSeenAfterAll:
				REX::INFO("Hotkeys: a key was seen down after all; the key reads work");
				break;
			default:
				break;
			}
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
		if (polls++ == 0) {
			LogMethod();
		}
		bool anyHotkey = false;

		// Pressed: fires when the key goes down.
		const auto press = [&](Key& a_key, std::uint32_t a_vk, Action a_action, std::string_view a_name) {
			const bool physical = IsDown(a_vk);
			const bool down = active && !chord && physical;
			anyHotkey |= physical;
			if (physical && !a_key.physical && a_key.logged < LOGGED_PRESSES) {
				++a_key.logged;
				if (down) {
					REX::INFO("Hotkeys: {} ({}) pressed: taken", a_name, ModDiff::KeyName(a_vk));
				} else {
					REX::INFO("Hotkeys: {} ({}) pressed, not taken: {}", a_name, ModDiff::KeyName(a_vk), Blocked(chord));
				}
			}
			if (down && !a_key.down) {
				actions.push_back(a_action);
			} else if (physical && !a_key.physical && !down) {
				++blockedPresses;  // Describe says why
			}
			a_key.down = down;
			a_key.physical = physical;
		};
		press(menu, settings.keyMenu, Action::kMenu, "menu");
		press(list, settings.keyPlayerList, Action::kPlayerList, "player list");
		press(ping, settings.keyPing, Action::kPing, "ping");
		press(emotePoint, settings.keyEmotePoint, Action::kEmotePoint, "emote point");
		press(emoteCheer, settings.keyEmoteCheer, Action::kEmoteCheer, "emote cheer");
		press(emoteClap, settings.keyEmoteClap, Action::kEmoteClap, "emote clap");

		// Teleport: a tap picks, a hold goes.
		const bool teleportPhysical = IsDown(settings.keyTeleport);
		anyHotkey |= teleportPhysical;
		const bool down = active && teleportPhysical;
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
		WatchKeys(active, anyHotkey);
		return actions;
	}

	bool Held(std::uint32_t a_vk)
	{
		return IsDown(a_vk) && GameHasFocus();
	}

	bool GameInFront()
	{
		return GameHasFocus();
	}

	std::string Describe()
	{
		const auto&                    settings = Config::Get();
		const auto                     ui = RE::UI::GetSingleton();
		const auto                     key = [](const char* a_name, std::uint32_t a_vk) {
			return std::format(" {}={:02X}{}", a_name, a_vk, IsDown(a_vk) ? "(down)" : "");
		};
		return std::format("hotkeys: focus={} ({}) menuMode={} console={} chord: alt={} ctrl={} win={} polls={} fired={} blocked={} keysSeen={} keys:",
				   GameHasFocus(), FocusName(GameFocus()), ui ? static_cast<int>(ui->menuMode) : -1, OpenMenus::Open(OpenMenus::Menu::kConsole), IsDown(0x12),
				   IsDown(0x11), IsDown(0x5B) || IsDown(0x5C), polls, actionsFired, blockedPresses, keyWatch.Seen()) +
		       key("menu", settings.keyMenu) + key("list", settings.keyPlayerList) + key("teleport", settings.keyTeleport) + key("ping", settings.keyPing) + key("voice", settings.keyVoice);
	}
}
