#include "game/MpMenu.h"

#include "MainThread.h"
#include "Text.h"
#include "game/Menu.h"

#include <algorithm>
#include <array>
#include <format>
#include <atomic>
#include <string>

namespace MpMenu
{
	namespace
	{
		using GValue = Scaleform::GFx::Value;

		constexpr const char* NAME = "F4MPMenu";
		constexpr const char* MOVIE = "Interface/F4MPMenu.swf";

		// The movie's calls into us (BGSCodeObj in F4MPMenu.as), by the ID each is mapped with.
		enum class Code : std::uintptr_t
		{
			kPressed,
			kClose,
			kPlaySound,
			kLog,
		};

		// The entries, in the movie's order. Each opens the F1 pages for now.
		constexpr std::array<const char*, 4> ENTRIES{ "mpmenu.host", "mpmenu.join", "mpmenu.direct", "mpmenu.options" };

		// The main menu's panel box on the stage: set on the main thread before the show message, read as the menu is made.
		std::atomic<float> boxLeft{ 0.0f };
		std::atomic<float> boxTop{ 0.0f };
		std::atomic<float> boxRight{ 0.0f };
		std::atomic<float> boxBottom{ 0.0f };

		// The player's HUD color (Settings > Display), which the main menu is drawn in: the game's own menus are white in
		// their movies and tinted by the renderer, which a menu of ours drawn flat is not, so the movie is told the color.
		std::uint32_t HudColor()
		{
			constexpr std::uint32_t DEFAULT = 0x12FF15;  // the game's own default
			const auto prefs = RE::INIPrefSettingCollection::GetSingleton();
			std::uint32_t color = 0;
			for (const auto [name, shift] : { std::pair{ "iHUDColorR:Interface", 16 }, std::pair{ "iHUDColorG:Interface", 8 }, std::pair{ "iHUDColorB:Interface", 0 } }) {
				const auto inPrefs = prefs ? prefs->GetSetting(name) : nullptr;
				const auto setting = inPrefs ? inPrefs : RE::GetINISetting(name);
				if (!setting || setting->GetType() != RE::Setting::SETTING_TYPE::kInt) {
					return DEFAULT;
				}
				color |= (static_cast<std::uint32_t>(setting->GetInt()) & 0xFF) << shift;
			}
			return color;
		}

		void Hide()
		{
			if (const auto queue = RE::UIMessageQueue::GetSingleton()) {
				queue->AddMessage(NAME, RE::UI_MESSAGE_TYPE::kHide);
			}
		}

		// The movie's calls into us. A handler of its own rather than the menu: a function the movie holds keeps its
		// handler alive, and the menu holding its movie would then never go. The code object the game sets up for a menu
		// (IMenu::SetMenuCodeObject) passed no call through to a menu of ours, so the movie's BGSCodeObj is ours.
		class CodeHandler final : public Scaleform::GFx::FunctionHandler
		{
		public:
			void Call(const Params& a_params) override
			{
				switch (static_cast<Code>(reinterpret_cast<std::uintptr_t>(a_params.userData))) {
				case Code::kPressed:
					{
						const auto index = a_params.argCount > 0 && a_params.args[0].IsNumber() ? static_cast<int>(a_params.args[0].GetNumber()) :
						                   a_params.argCount > 0 && a_params.args[0].IsInt()    ? a_params.args[0].GetInt() :
						                                                                          -1;
						REX::INFO("MpMenu: entry {} pressed", index);
						MainThread::Post([] {
							Hide();
							Menu::Open();
						});
					}
					break;
				case Code::kClose:
					MainThread::Post([] { Hide(); });
					break;
				case Code::kPlaySound:
					if (a_params.argCount > 0 && a_params.args[0].IsString()) {
						RE::UIUtils::PlayMenuSound(a_params.args[0].GetString());
					}
					break;
				case Code::kLog:
					if (a_params.argCount > 0 && a_params.args[0].IsString()) {
						REX::INFO("MpMenu: movie says {}", a_params.args[0].GetString());
					}
					break;
				}
			}
		};
		CodeHandler* codeHandler = nullptr;  // made with the first menu (Scaleform's heap is up by then), never let go

		class Panel final : public RE::GameMenuBase
		{
		public:
			Panel()
			{
				menuFlags.set(RE::UI_MENU_FLAGS::kPausesGame, RE::UI_MENU_FLAGS::kUsesCursor, RE::UI_MENU_FLAGS::kUsesMenuContext,
					RE::UI_MENU_FLAGS::kModal, RE::UI_MENU_FLAGS::kDisablePauseMenu, RE::UI_MENU_FLAGS::kUpdateUsesCursor);
				// Above the main and pause menus (kPauseMenu), as a message box is.
				depthPriority = RE::UI_DEPTH_PRIORITY::kMessage;
				inputContext = RE::UserEvents::INPUT_CONTEXT_ID::kBasicMenuNav;
				const auto scaleform = RE::BSScaleformManager::GetSingleton();
				if (!scaleform || !scaleform->LoadMovieEx(*this, MOVIE, "root")) {
					REX::ERROR("MpMenu: {} could not be loaded; is it in Data/Interface?", MOVIE);
					return;
				}
				SetCode();
				Fill();
			}

			// The game's user events go on to the movie (ProcessUserEvent), but for the way back out, read here.
			void OnButtonEvent(const RE::ButtonEvent* a_event) override
			{
				if (a_event && a_event->QJustPressed()) {
					REX::DEBUG("MpMenu: input '{}' device {} code {:X}", a_event->QUserEvent().c_str(), static_cast<int>(a_event->device.get()), a_event->QIDCode());
					// The way back out of a menu: Esc or Tab, or the gamepad's B. None comes as a user event to a menu of
					// ours (the game's own menus get theirs from their own input handling), so they are read by code.
					const bool keyboard = a_event->device.get() == RE::INPUT_DEVICE::kKeyboard;
					const bool gamepad = a_event->device.get() == RE::INPUT_DEVICE::kGamepad;
					constexpr std::uint32_t ESCAPE = 0x1B, TAB = 0x09, PAD_B = 0x2000;  // Windows virtual keys, XInput's B
					if ((keyboard && (a_event->QIDCode() == ESCAPE || a_event->QIDCode() == TAB)) || (gamepad && a_event->QIDCode() == PAD_B)) {
						RE::UIUtils::PlayMenuSound("UIMenuCancel");
						MainThread::Post([] { Hide(); });
						return;
					}
				}
				GameMenuBase::OnButtonEvent(a_event);
			}

		private:
			void SetCode()
			{
				GValue engines;
				const bool had = menuObj.GetMember("BGSCodeObj", &engines);
				REX::INFO("MpMenu: the game's code object: {} (type {}, Pressed {})", had, static_cast<int>(engines.GetType()),
					engines.IsObject() && engines.HasMember("Pressed"));
				if (!codeHandler) {
					codeHandler = new CodeHandler();
				}
				GValue code;
				uiMovie->CreateObject(&code);
				for (const auto [name, id] : { std::pair{ "Pressed", Code::kPressed }, std::pair{ "Close", Code::kClose }, std::pair{ "PlaySound", Code::kPlaySound },
						 std::pair{ "Log", Code::kLog } }) {
					GValue function;
					uiMovie->CreateFunction(&function, codeHandler, reinterpret_cast<void*>(static_cast<std::uintptr_t>(id)));
					code.SetMember(name, function);
				}
				if (!menuObj.SetMember("BGSCodeObj", code)) {
					REX::WARN("MpMenu: the movie took no BGSCodeObj");
				}
			}

			void Fill()
			{
				GValue labels;
				uiMovie->CreateArray(&labels);
				for (const auto key : ENTRIES) {
					const auto text = Text::Get(key);
					labels.PushBack(GValue(text.c_str()));  // the movie copies it into a string of its own
				}
				auto version = std::format("{}", F4SE::GetPluginVersion());
				while (version.ends_with(".0") && std::ranges::count(version, '.') > 2) {
					version.resize(version.size() - 2);  // 0.10.1.0: 0.10.1
				}
				const auto footer = std::format("F4Multiplayer {}", version);
				const auto back = Text::Get("mpmenu.back");
				const std::array<GValue, 8> args{ labels, GValue(static_cast<double>(boxLeft.load())), GValue(static_cast<double>(boxTop.load())),
					GValue(static_cast<double>(boxRight.load())), GValue(static_cast<double>(boxBottom.load())), GValue(static_cast<double>(HudColor())),
					GValue(footer.c_str()), GValue(back.c_str()) };
				if (!menuObj.Invoke("SetEntries", nullptr, args.data(), args.size())) {
					REX::WARN("MpMenu: the movie has no SetEntries");
				}
			}
		};

		RE::IMenu* Create(const RE::UIMessage&)
		{
			return new Panel();
		}
	}

	void Register()
	{
		if (const auto ui = RE::UI::GetSingleton()) {
			ui->RegisterMenu(NAME, &Create);
			REX::INFO("MpMenu: registered {}", NAME);
		}
	}

	void Open(float a_left, float a_top, float a_right, float a_bottom)
	{
		MainThread::Assert("MpMenu::Open");
		boxLeft = a_left;
		boxTop = a_top;
		boxRight = a_right;
		boxBottom = a_bottom;
		if (const auto queue = RE::UIMessageQueue::GetSingleton()) {
			queue->AddMessage(NAME, RE::UI_MESSAGE_TYPE::kShow);
		}
	}
}
