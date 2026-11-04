#include "game/MPMenu.h"
#include "game/MainMenuEntry.h"

#include "Config.h"
#include "MainThread.h"
#include "Text.h"
#include "game/Hud.h"
#include "game/Equipment.h"
#include "game/KeyNames.h"
#include "game/Menu.h"

#include <algorithm>
#include <array>
#include <format>
#include <atomic>
#include <utility>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace MPMenu
{
	namespace
	{
		using GValue = Scaleform::GFx::Value;

		constexpr const char* NAME = "F4MPMenu";
		constexpr const char* MOVIE = "Interface/F4Multiplayer/MPMenu.swf";

		// The movie's calls into us (BGSCodeObj in MPMenu.as), by the ID each is mapped with.
		enum class Code : std::uintptr_t
		{
			kPressed,
			kClose,
			kPlaySound,
			kLog,
			kMouseOutside,
		};

		// The page to draw: set on the main thread, taken in the menu's own update.
		struct Page
		{
			std::uint32_t            serial = 0;
			std::string              body;
			std::vector<std::string> rows;
			std::vector<std::string> hints;
			bool                     keepRow = false;
			bool                     heading = false;
			std::vector<std::string> players;
			std::string              header;
		};
		std::mutex          pageLock;
		std::optional<Page> nextPage;  // under pageLock
		std::optional<std::string> nextStatus;  // under pageLock

		// Opened and not closed since. A menu going away counts only if it is the latest opened: one being hidden can
		// go after the next has been asked for.
		std::atomic<bool>          wanted{ false };
		std::atomic<std::uint32_t> opened{ 0 };
		std::atomic<bool>          capturing{ false };  // the next key is a keybind's
		std::atomic<bool>          hideWaiting{ false };  // closed while the UI was held: hidden once it has settled

		// The main menu's panel box on the stage: set on the main thread before the show message, read as the menu is made.
		std::atomic<float> boxLeft{ 0.0f };
		std::atomic<float> boxTop{ 0.0f };
		std::atomic<float> boxRight{ 0.0f };
		std::atomic<float> boxBottom{ 0.0f };
		// Opened in the world: the box gives only the size, the panel goes in the middle of the stage, and the game goes on under it
		// (the pause menu is there to pause).
		std::atomic<bool>  centered{ false };

		// The height of the pause menu's panel box on the 1280x720 stage (read in a game, 327.13), so a panel with none to line
		// up beside has the same scale as one that has.
		constexpr float CENTERED_BOX_HEIGHT = 327.0f;

		// The player's HUD color (Settings > Display), which the main menu is drawn in: the game's own menus are white in
		// their movies and tinted by the renderer, which a menu of ours drawn flat is not, so the movie is told the color.
		std::uint32_t ReadHudColor()
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

		constexpr std::int64_t MENU_KEY_AFTER_MS = 250;

		std::int64_t NowMs()
		{
			return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
		}

		// With the game going on under the panel, its keys and mouse don't reach the player. Not with an input layer: turning off
		// fighting and the like puts the weapon away and takes the HUD with it, and the HUD is to stay. Main thread.
		void SetPlayerControls(bool a_on)
		{
			static bool off = false;
			static bool before = false;
			const auto  controls = RE::PlayerControls::GetSingleton();
			if (!controls || off != a_on) {
				return;  // as asked already
			}
			if (!a_on) {
				before = controls->blockPlayerInput;
			}
			controls->blockPlayerInput = a_on ? before : true;
			// It doesn't stop activating what's under the crosshair, nor does the activate handler's own switch: a layer does, of
			// that alone, which leaves the weapon and the HUD be. The key then comes to the menu as DISABLED (OnButtonEvent).
			static RE::BSTSmartPointer<RE::BSInputEnableLayer> layer;
			const auto manager = RE::BSInputEnableManager::GetSingleton();
			if (manager && (layer || manager->AllocateNewLayer(layer, "F4Multiplayer menu")) && layer) {
				manager->EnableUserEvent(layer->layerID, RE::UserEvents::USER_EVENT_FLAG::kActivate, a_on, RE::UserEvents::SENDER_ID::kScript);
			} else {
				REX::WARN("MPMenu: no input layer: the activate key still works in the world under the menu");
			}
			off = !a_on;
		}

		// In power armor its HUD has a color of its own, and a panel opened in the world goes with that HUD.
		constexpr std::uint32_t POWER_ARMOR_COLOR = 0xF7D169;
		std::atomic<std::uint32_t> lookColor{ 0 };  // set on the main thread before the show message, read as the menu is made

		std::uint32_t NumberOf(const GValue& a_value)
		{
			return a_value.IsUInt() ? a_value.GetUInt() :
			       a_value.IsInt()  ? static_cast<std::uint32_t>(a_value.GetInt()) :
			       a_value.IsNumber() ? static_cast<std::uint32_t>(a_value.GetNumber()) :
			                            0;
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
						if (a_params.argCount > 1) {
							Menu::PanelPress(NumberOf(a_params.args[0]), NumberOf(a_params.args[1]));
						}
					}
					break;
				case Code::kClose:
					MainThread::Post([] { Menu::PanelBack(); });
					break;
				case Code::kPlaySound:
					if (a_params.argCount > 0 && a_params.args[0].IsString()) {
						RE::UIUtils::PlayMenuSound(a_params.args[0].GetString());
					}
					break;
				case Code::kMouseOutside:
					if (a_params.argCount > 2 && a_params.args[0].IsNumber() && a_params.args[1].IsNumber() && a_params.args[2].IsBoolean()) {
						MainMenuEntry::PanelMouse(static_cast<float>(a_params.args[0].GetNumber()), static_cast<float>(a_params.args[1].GetNumber()), a_params.args[2].GetBoolean());
					}
					break;
				case Code::kLog:
					if (a_params.argCount > 0 && a_params.args[0].IsString()) {
						REX::INFO("MPMenu: movie says {}", a_params.args[0].GetString());
					}
					break;
				}
			}
		};
		CodeHandler* codeHandler = nullptr;  // made with the first menu (Scaleform's heap is up by then), never let go

		class Panel final : public RE::GameMenuBase
		{
		public:
			Panel() :
				number(opened.load())
			{
				menuFlags.set(RE::UI_MENU_FLAGS::kUsesCursor, RE::UI_MENU_FLAGS::kUsesMenuContext,
					RE::UI_MENU_FLAGS::kDisablePauseMenu, RE::UI_MENU_FLAGS::kUpdateUsesCursor);
				// The player's movement keys or stick, whatever they are bound to, come to the movie as Up and Down, as in the main menu.
				menuFlags.set(RE::UI_MENU_FLAGS::kUsesMovementToDirection);
				// Over the main or pause menu it holds what's under it. In the world the game and the HUD go on under it, so a friend's
				// toasts can be read: a modal menu would stop the HUD.
				if (!centered) {
					menuFlags.set(RE::UI_MENU_FLAGS::kPausesGame, RE::UI_MENU_FLAGS::kModal);
				}
				// Through GameMenuBase's renderer, FlatScreenModel, as the game's menus, cursor and message boxes are drawn. A menu
				// that isn't draws in a later pass, over all of them.
				menuFlags.set(RE::UI_MENU_FLAGS::kCustomRendering);
				// Above the main and pause menus (kPauseMenu), as a message box is.
				depthPriority = RE::UI_DEPTH_PRIORITY::kMessage;
				inputContext = RE::UserEvents::INPUT_CONTEXT_ID::kBasicMenuNav;
				const auto scaleform = RE::BSScaleformManager::GetSingleton();
				if (!scaleform || !scaleform->LoadMovieEx(*this, MOVIE, "root")) {
					REX::ERROR("MPMenu: {} could not be loaded; is it in Data/Interface? Using message boxes", MOVIE);
					MainThread::Post([] { Menu::Open(); });
					return;
				}
				// Our objects with a size, as shader targets for the renderer (each one's bounds becomes a quad; the movie's root
				// gave an empty one). What they draw keeps the movie's colors under a white filter (no-color draws them black). The
				// panel's ground is the renderer's, as the main menu's is (its backgrounds are no-color, shaded with the gameplay HUD
				// color): the movie's Background_mc only gives it its size.
				{
					for (const auto path : { "root.Panel_mc.Content_mc", "root.Hint_mc" }) {
						auto target = std::make_unique<RE::BSGFxShaderFXTarget>(*uiMovie, path);
						target->CreateAndSetFiltersToColor(std::uint8_t{ 255 }, std::uint8_t{ 255 }, std::uint8_t{ 255 }, 1.0f);
						shaderFXObjects.push_back(target.get());
						targets.push_back(std::move(target));
					}
					for (const auto path : { "root.Panel_mc.Background_mc", "root.HintBackground_mc" }) {
						auto ground = std::make_unique<RE::BSGFxShaderFXTarget>(*uiMovie, path);
						ground->CreateAndSetFiltersToHUD(RE::HUDColorTypes::kNoColorMultiplier);
						ground->EnableShadedBackground(RE::HUDColorTypes::kGameplayHUDColor);
						shaderFXObjects.push_back(ground.get());
						targets.push_back(std::move(ground));
					}
				}
				SetCode();
				RefreshPlatform();  // again, now that the movie can answer: keyboard or gamepad, for the back hint
				SetLook();
			}

			~Panel() override
			{
				if (number == opened.load()) {
					wanted = false;
					MainThread::Post([] { SetPlayerControls(true); });  // closed by us or by the game
				}
			}

			// Flash objects are written only here, in the menu's own update.
			void AdvanceMovie(float a_timeDelta, std::uint64_t a_time) override
			{
				if (menuKeyFrom.load() == 0) {
					menuKeyFrom = NowMs() + MENU_KEY_AFTER_MS;
				}
				// Nothing written while the UI is held (UiHold.h): the engine may be releasing the movie. The page waits.
				if (uiMovie && Hud::IsSettled()) {
					if (hideWaiting.exchange(false)) {
						Hide();
					}
					DrawPage();
					if (acceptWaiting.exchange(false)) {
						const std::array<GValue, 2> args{ GValue("Accept"), GValue(true) };
						menuObj.Invoke("ProcessUserEvent", nullptr, args.data(), args.size());
					}
				}
				GameMenuBase::AdvanceMovie(a_timeDelta, a_time);
			}

			// The game's user events go on to the movie (ProcessUserEvent), but for the way back out, read here.
			void OnButtonEvent(const RE::ButtonEvent* a_event) override
			{
				// The player's activate key, turned off in the world (SetPlayerControls), is still the menu's Accept.
				if (a_event && centered && a_event->QJustPressed() && std::string_view{ a_event->QUserEvent().c_str() } == "DISABLED") {
					const auto map = RE::ControlMap::GetSingleton();
					if (map && a_event->QIDCode() == map->GetMappedKey("Activate", a_event->device.get())) {
						acceptWaiting = true;
						return;
					}
				}
				if (a_event && a_event->QJustPressed()) {
					REX::DEBUG("MPMenu: input '{}' device {} code {:X}", a_event->QUserEvent().c_str(), static_cast<int>(a_event->device.get()), a_event->QIDCode());
					// The way back out of a menu: Esc or Tab, or the gamepad's B. None comes as a user event to a menu of
					// ours (the game's own menus get theirs from their own input handling), so they are read by code.
					const bool keyboard = a_event->device.get() == RE::INPUT_DEVICE::kKeyboard;
					const bool gamepad = a_event->device.get() == RE::INPUT_DEVICE::kGamepad;
					const bool mouse = a_event->device.get() == RE::INPUT_DEVICE::kMouse;
					if (keyboard && centered && a_event->QIDCode() == 0x1B) {
						return;  // EscPressed has it: never both, where the game does let it through
					}
					if (capturing && (keyboard || (mouse && KeyNames::FromMouse(a_event->QIDCode()) != 0))) {
						capturing = false;
						MainThread::Post([keyboard, code = a_event->QIDCode()] { Menu::PanelKey(keyboard, code); });
						return;
					}
					// The key that opened it closes it, from any page. Menu::Frame sees the panel gone and tidies up.
					if (keyboard && centered && a_event->QIDCode() == Config::Get().keyMenu) {
						// The press that opened it reaches it too: not a second press.
						const auto at = menuKeyFrom.load();
						if (at == 0 || NowMs() < at) {
							REX::INFO("MPMenu: the menu key while opening: ignored");
							return;
						}
						REX::INFO("MPMenu: closed by the menu key");
						RE::UIUtils::PlayMenuSound("UIMenuCancel");
						MainThread::Post([] { Close(); });
						return;
					}
					constexpr std::uint32_t ESCAPE = 0x1B, TAB = 0x09, PAD_B = 0x2000;  // Windows virtual keys, XInput's B
					if ((keyboard && (a_event->QIDCode() == ESCAPE || a_event->QIDCode() == TAB)) || (gamepad && a_event->QIDCode() == PAD_B)) {
						RE::UIUtils::PlayMenuSound("UIMenuCancel");
						MainThread::Post([] { Menu::PanelBack(); });
						return;
					}
				}
				GameMenuBase::OnButtonEvent(a_event);
			}

		private:
			std::vector<std::unique_ptr<RE::BSGFxShaderFXTarget>> targets;  // before the base's list of them goes
			std::atomic<bool>         acceptWaiting{ false };  // for the menu's own update
			std::atomic<std::int64_t> menuKeyFrom{ 0 };  // the menu key closes it from then on (set in its first update, read in input)

			void SetCode()
			{
				GValue engines;
				const bool had = menuObj.GetMember("BGSCodeObj", &engines);
				REX::INFO("MPMenu: the game's code object: {} (type {}, Pressed {})", had, static_cast<int>(engines.GetType()),
					engines.IsObject() && engines.HasMember("Pressed"));
				if (!codeHandler) {
					codeHandler = new CodeHandler();
				}
				GValue code;
				uiMovie->CreateObject(&code);
				for (const auto [name, id] : { std::pair{ "Pressed", Code::kPressed }, std::pair{ "Close", Code::kClose }, std::pair{ "PlaySound", Code::kPlaySound },
						 std::pair{ "Log", Code::kLog }, std::pair{ "MouseOutside", Code::kMouseOutside } }) {
					GValue function;
					uiMovie->CreateFunction(&function, codeHandler, reinterpret_cast<void*>(static_cast<std::uintptr_t>(id)));
					code.SetMember(name, function);
				}
				if (!menuObj.SetMember("BGSCodeObj", code)) {
					REX::WARN("MPMenu: the movie took no BGSCodeObj");
				}
			}

			void DrawPage()
			{
				std::optional<Page>        page;
				std::optional<std::string> status;
				{
					std::scoped_lock l{ pageLock };
					page.swap(nextPage);
					status.swap(nextStatus);
				}
				if (status) {
					const std::array<GValue, 1> args{ GValue(status->c_str()) };
					menuObj.Invoke("SetStatus", nullptr, args.data(), args.size());
				}
				if (!page) {
					return;
				}
				GValue rows;
				uiMovie->CreateArray(&rows);
				for (const auto& row : page->rows) {
					rows.PushBack(GValue(row.c_str()));  // the movie copies it into a string of its own
				}
				GValue hints;
				uiMovie->CreateArray(&hints);
				for (const auto& hint : page->hints) {
					hints.PushBack(GValue(hint.c_str()));
				}
				GValue players;
				uiMovie->CreateArray(&players);
				for (const auto& player : page->players) {
					players.PushBack(GValue(player.c_str()));
				}
				const std::array<GValue, 8> args{ GValue(static_cast<double>(page->serial)), GValue(page->body.c_str()), rows, hints, GValue(page->keepRow), GValue(page->heading), players, GValue(page->header.c_str()) };
				if (!menuObj.Invoke("SetPage", nullptr, args.data(), args.size())) {
					REX::WARN("MPMenu: the movie has no SetPage");
				}
			}

			void SetLook()
			{
				auto version = std::format("{}", F4SE::GetPluginVersion());
				while (version.ends_with(".0") && std::ranges::count(version, '.') > 2) {
					version.resize(version.size() - 2);  // 0.10.1.0: 0.10.1
				}
				const auto back = Text::Get("mpmenu.back");
				const std::array<GValue, 9> args{ GValue(static_cast<double>(boxLeft.load())), GValue(static_cast<double>(boxTop.load())),
					GValue(static_cast<double>(boxRight.load())), GValue(static_cast<double>(boxBottom.load())),
					GValue(static_cast<double>(lookColor.load())), GValue("F4Multiplayer"), GValue(version.c_str()), GValue(back.c_str()), GValue(centered.load()) };
				if (!menuObj.Invoke("SetLook", nullptr, args.data(), args.size())) {
					REX::WARN("MPMenu: the movie has no SetLook");
				}
			}

			std::uint32_t number;  // which opening this menu is
		};

		RE::IMenu* Create(const RE::UIMessage&)
		{
			return new Panel();
		}

		void Show(float a_left, float a_top, float a_right, float a_bottom, bool a_centered)
		{
			boxLeft = a_left;
			boxTop = a_top;
			boxRight = a_right;
			boxBottom = a_bottom;
			centered = a_centered;
			{
				std::scoped_lock l{ pageLock };
				nextPage.reset();
				nextStatus.reset();
			}
			++opened;
			wanted = true;
			hideWaiting = false;
			if (const auto queue = RE::UIMessageQueue::GetSingleton()) {
				queue->AddMessage(NAME, RE::UI_MESSAGE_TYPE::kShow);
			}
			Menu::OpenInPanel();
		}
	}

	void Register()
	{
		if (const auto ui = RE::UI::GetSingleton()) {
			ui->RegisterMenu(NAME, &Create);
			REX::INFO("MPMenu: registered {}", NAME);
		}
	}

	void Open(float a_left, float a_top, float a_right, float a_bottom)
	{
		MainThread::Assert("MPMenu::Open");
		if (!Hud::IsSettled()) {
			return;  // the entry's press came as the UI went down: pressed again once it is back
		}
		lookColor = ReadHudColor();
		Show(a_left, a_top, a_right, a_bottom, false);
	}

	void OpenCentered()
	{
		MainThread::Assert("MPMenu::OpenCentered");
		SetPlayerControls(false);
		lookColor = Equipment::InPowerArmor(RE::PlayerCharacter::GetSingleton()) ? POWER_ARMOR_COLOR : ReadHudColor();
		Show(0.0f, 0.0f, 0.0f, CENTERED_BOX_HEIGHT, true);
	}

	void SetPage(std::uint32_t a_serial, std::string a_body, std::vector<std::string> a_rows, std::vector<std::string> a_hints, bool a_keepRow, bool a_heading, std::vector<std::string> a_players, std::string a_header)
	{
		std::scoped_lock l{ pageLock };
		nextPage = Page{ a_serial, std::move(a_body), std::move(a_rows), std::move(a_hints), a_keepRow, a_heading, std::move(a_players), std::move(a_header) };
		nextStatus.reset();
	}

	void SetStatus(std::string a_text)
	{
		std::scoped_lock l{ pageLock };
		nextStatus = std::move(a_text);
	}

	void CaptureKey(bool a_on)
	{
		capturing = a_on;
	}

	void Close()
	{
		SetPlayerControls(true);
		capturing = false;
		wanted = false;
		if (Hud::IsSettled()) {
			Hide();
		} else {
			hideWaiting = true;
		}
	}

	void EscPressed()
	{
		MainThread::Assert("MPMenu::EscPressed");
		constexpr std::uint32_t ESCAPE = 0x1B;
		if (capturing.exchange(false)) {
			Menu::PanelKey(true, ESCAPE);
			return;
		}
		RE::UIUtils::PlayMenuSound("UIMenuCancel");
		Menu::PanelBack();
	}

	void OnHudAdvance(RE::IMenu* a_hud)
	{
		// What sits in the middle of the HUD, which draws over the panel. The rest of the HUD stays, for its toasts.
		static constexpr std::array PATHS{ "root.CenterGroup_mc.HUDCrosshair_mc", "root.CenterGroup_mc.RolloverWidget_mc", "root.CenterGroup_mc.QuickContainerWidget_mc" };
		static std::array<bool, PATHS.size()> before{};
		static bool                           hidden = false;
		const bool                            hide = centered && wanted;
		if (!a_hud || !a_hud->uiMovie || hide == hidden) {
			if (!hide || !a_hud || !a_hud->uiMovie) {
				return;
			}
		}
		for (std::size_t i = 0; i < PATHS.size(); ++i) {
			GValue object;
			if (!a_hud->uiMovie->GetVariable(&object, PATHS[i]) || !object.IsDisplayObject()) {
				continue;
			}
			if (hide && !hidden) {
				GValue visible;
				before[i] = !object.GetMember("visible", &visible) || !visible.IsBoolean() || visible.GetBoolean();
			}
			// Each update while it's up: the HUD sets them itself as what's under the crosshair changes.
			object.SetMember("visible", GValue(hide ? false : before[i]));
		}
		hidden = hide;
	}

	bool Centered()
	{
		return centered;
	}

	std::uint32_t HudColor()
	{
		return ReadHudColor();
	}

	bool Showing()
	{
		return wanted;
	}
}
