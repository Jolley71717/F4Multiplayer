#include "game/UiPanel.h"
#include "game/UiMainMenuEntry.h"

#include "Config.h"
#include "MainThread.h"
#include "Platform.h"
#include "Text.h"
#include "game/Hud.h"
#include "game/Equipment.h"
#include "game/UiKeyNames.h"
#include "game/UiPages.h"

#include <algorithm>
#include <array>
#include <format>
#include <atomic>
#include <utility>
#include <chrono>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

extern "C"
{
	__declspec(dllimport) short __stdcall GetAsyncKeyState(int a_key);
	__declspec(dllimport) short __stdcall GetKeyState(int a_key);
	__declspec(dllimport) unsigned int __stdcall MapVirtualKeyW(unsigned int a_code, unsigned int a_type);
	__declspec(dllimport) int __stdcall ToUnicode(unsigned int a_key, unsigned int a_scan, const unsigned char* a_state, wchar_t* a_out, int a_size, unsigned int a_flags);
}

namespace UiPanel
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
		std::mutex          pageLock;
		std::deque<Page>    nextPages;  // under pageLock; in order: a dialog can come with the page it goes over

		// Opened and not closed since. A menu going away counts only if it is the latest opened: one being hidden can
		// go after the next has been asked for.
		std::atomic<bool>          wanted{ false };
		std::atomic<std::uint32_t> opened{ 0 };
		std::atomic<bool>          capturing{ false };  // the next key is a keybind's
		std::atomic<bool> typing{ false };  // every key is UiPages::PanelTyped's (CaptureText)
		// The key last typed, held: the game gives a menu only its press and release, so the repeat is ours.
		// The game's text mode holds back the menu's own events too: what moves in a dialog with a place to type is read as keys, and
		// goes to the movie from its update. -1 Up, 1 Down.
		std::atomic<int>  typingStep{ 0 };
		std::atomic<bool> typingStepUp{ false };
		std::atomic<std::uint32_t> repeatKey{ 0 };
		std::atomic<std::uint32_t> repeatCharacter{ 0 };
		std::atomic<std::int64_t>  repeatAt{ 0 };
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

		// What a key types on the player's own layout, with the modifiers down now; 0 for nothing, or more than a character.
		std::uint32_t CharacterOf(std::uint32_t a_key)
		{
			std::array<unsigned char, 256> state{};
			for (const int modifier : { 0x10, 0x11, 0x12 }) {  // Shift, Ctrl, Alt
				state[modifier] = (GetAsyncKeyState(modifier) & 0x8000) != 0 ? 0x80 : 0;
			}
			constexpr int CAPS_LOCK = 0x14;
			state[CAPS_LOCK] = (GetKeyState(CAPS_LOCK) & 1) != 0 ? 1 : 0;
			wchar_t out[4]{};
			return ToUnicode(a_key, MapVirtualKeyW(a_key, 0), state.data(), out, 4, 0) == 1 ? static_cast<std::uint32_t>(out[0]) : 0;
		}

		constexpr std::int64_t MENU_KEY_AFTER_MS = 250;
		constexpr std::int64_t TYPING_REPEAT_DELAY_MS = 350;
		constexpr std::int64_t TYPING_REPEAT_EVERY_MS = 50;

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
				REX::WARN("UiPanel: no input layer: the activate key still works in the world under the menu");
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
							UiPages::PanelPress(NumberOf(a_params.args[0]), NumberOf(a_params.args[1]));
						}
					}
					break;
				case Code::kClose:
					MainThread::Post([] { UiPages::PanelBack(); });
					break;
				case Code::kPlaySound:
					if (a_params.argCount > 0 && a_params.args[0].IsString()) {
						RE::UIUtils::PlayMenuSound(a_params.args[0].GetString());
					}
					break;
				case Code::kMouseOutside:
					if (a_params.argCount > 2 && a_params.args[0].IsNumber() && a_params.args[1].IsNumber() && a_params.args[2].IsBoolean()) {
						UiMainMenuEntry::PanelMouse(static_cast<float>(a_params.args[0].GetNumber()), static_cast<float>(a_params.args[1].GetNumber()), a_params.args[2].GetBoolean());
					}
					break;
				case Code::kLog:
					if (a_params.argCount > 0 && a_params.args[0].IsString()) {
						REX::INFO("UiPanel: movie says {}", a_params.args[0].GetString());
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
					REX::ERROR("UiPanel: {} could not be loaded; is it in Data/Interface? Using message boxes", MOVIE);
					MainThread::Post([] { UiPages::Open(); });
					return;
				}
				// Our objects with a size, as shader targets for the renderer (each one's bounds becomes a quad; the movie's root
				// gave an empty one). What they draw keeps the movie's colors under a white filter (no-color draws them black). The
				// panel's ground is the renderer's, as the main menu's is (its backgrounds are no-color, shaded with the gameplay HUD
				// color): the movie's Background_mc only gives it its size.
				{
					for (const auto path : { "root.Panel_mc.Content_mc", "root.CommandBar_mc" }) {
						auto target = std::make_unique<RE::BSGFxShaderFXTarget>(*uiMovie, path);
						target->CreateAndSetFiltersToColor(std::uint8_t{ 255 }, std::uint8_t{ 255 }, std::uint8_t{ 255 }, 1.0f);
						shaderFXObjects.push_back(target.get());
						targets.push_back(std::move(target));
					}
					for (const auto path : { "root.Panel_mc.Background_mc", "root.CommandBarBackground_mc" }) {
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
					MainThread::Post([] {
						SetPlayerControls(true);  // closed by us or by the game
						CaptureText(false);
					});
				}
			}

			// Flash objects are written only here, in the menu's own update.
			void AdvanceMovie(float a_timeDelta, std::uint64_t a_time) override
			{
				if (menuKeyFrom.load() == 0) {
					menuKeyFrom = NowMs() + MENU_KEY_AFTER_MS;
				}
				if (const auto key = repeatKey.load(); key != 0) {
					if (!typing || (GetAsyncKeyState(static_cast<int>(key)) & 0x8000) == 0) {
						repeatKey = 0;
					} else if (NowMs() >= repeatAt.load()) {
						repeatAt = NowMs() + TYPING_REPEAT_EVERY_MS;
						MainThread::Post([key, character = repeatCharacter.load()] { UiPages::PanelTyped(key, character); });
					}
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
					if (const auto step = typingStep.exchange(0); step != 0) {
						const std::array<GValue, 2> args{ GValue(step < 0 ? "Up" : "Down"), GValue(true) };
						menuObj.Invoke("ProcessUserEvent", nullptr, args.data(), args.size());
					}
					if (typingStepUp.exchange(false)) {
						const std::array<GValue, 2> args{ GValue("Up"), GValue(false) };  // either way: the repeat stops
						menuObj.Invoke("ProcessUserEvent", nullptr, args.data(), args.size());
					}
				}
				GameMenuBase::AdvanceMovie(a_timeDelta, a_time);
			}

			// The game's user events go on to the movie (ProcessUserEvent), but for the way back out, read here.
			void OnButtonEvent(const RE::ButtonEvent* a_event) override
			{
				// Before anything else, and never logged: it may be a password. What types goes to the text; the arrows or the D-pad and Enter
				// or A work the dialog, and Esc, Tab, B and the rest go on as ever.
				if (a_event && typing && (a_event->device.get() == RE::INPUT_DEVICE::kKeyboard || a_event->device.get() == RE::INPUT_DEVICE::kGamepad)) {
					const bool keys = a_event->device.get() == RE::INPUT_DEVICE::kKeyboard;
					const std::uint32_t code = a_event->QIDCode();
					constexpr std::uint32_t KEY_UP = 0x26, KEY_DOWN = 0x28, PAD_UP = 0x0001, PAD_DOWN = 0x0002, PAD_A = 0x1000;  // Windows virtual keys, XInput's buttons
					const int step = code == (keys ? KEY_UP : PAD_UP) ? -1 : code == (keys ? KEY_DOWN : PAD_DOWN) ? 1 : 0;
					if (step != 0) {
						if (a_event->QJustPressed()) {
							typingStep = step;
						} else if (a_event->QReleased()) {
							typingStepUp = true;
						}
						return;
					}
					if (code == (keys ? UiKeyNames::ENTER : PAD_A)) {
						if (a_event->QJustPressed()) {
							acceptWaiting = true;
						}
						return;
					}
				}
				if (a_event && typing && a_event->QJustPressed() && a_event->device.get() == RE::INPUT_DEVICE::kKeyboard) {
					const auto key = a_event->QIDCode();
					const auto character = key == UiKeyNames::ENTER || key == UiKeyNames::ESCAPE || key == UiKeyNames::TAB ? 0 : CharacterOf(key);
					if (character != 0 || key == UiKeyNames::BACKSPACE) {
						repeatKey = key;
						repeatCharacter = character;
						repeatAt = NowMs() + TYPING_REPEAT_DELAY_MS;
						MainThread::Post([key, character] { UiPages::PanelTyped(key, character); });
						return;
					}
				}
				// The player's activate key, turned off in the world (SetPlayerControls), is still the menu's Accept; unless it's the key to bind.
				if (a_event && centered && !capturing && a_event->QJustPressed() && std::string_view{ a_event->QUserEvent().c_str() } == "DISABLED") {
					const auto map = RE::ControlMap::GetSingleton();
					if (map && a_event->QIDCode() == map->GetMappedKey("Activate", a_event->device.get())) {
						acceptWaiting = true;
						return;
					}
				}
				if (a_event && a_event->QJustPressed()) {
					REX::DEBUG("UiPanel: input '{}' device {} code {:X}", a_event->QUserEvent().c_str(), static_cast<int>(a_event->device.get()), a_event->QIDCode());
					// The way back out of a menu: Esc or Tab, or the gamepad's B. None comes as a user event to a menu of
					// ours (the game's own menus get theirs from their own input handling), so they are read by code.
					const bool keyboard = a_event->device.get() == RE::INPUT_DEVICE::kKeyboard;
					const bool gamepad = a_event->device.get() == RE::INPUT_DEVICE::kGamepad;
					const bool mouse = a_event->device.get() == RE::INPUT_DEVICE::kMouse;
					if (keyboard && centered && a_event->QIDCode() == UiKeyNames::ESCAPE) {
						return;  // EscPressed has it: never both, where the game does let it through
					}
					if (capturing && (keyboard || (mouse && UiKeyNames::FromMouse(a_event->QIDCode()) != 0))) {
						capturing = false;
						MainThread::Post([keyboard, code = a_event->QIDCode()] { UiPages::PanelKey(keyboard, code); });
						return;
					}
					// The key that opened it closes it, from any page. UiPages::Frame sees the panel gone and tidies up.
					const auto boundKey = keyboard ? a_event->QIDCode() : mouse ? UiKeyNames::FromMouse(a_event->QIDCode()) : 0;
					if (centered && boundKey != 0 && boundKey == Config::Get().keyMenu) {
						// The press that opened it reaches it too: not a second press.
						const auto at = menuKeyFrom.load();
						if (at == 0 || NowMs() < at) {
							REX::INFO("UiPanel: the menu key while opening: ignored");
							return;
						}
						REX::INFO("UiPanel: closed by the menu key");
						RE::UIUtils::PlayMenuSound("UIMenuCancel");
						MainThread::Post([] { Close(); });
						return;
					}
					constexpr std::uint32_t PAD_B = 0x2000;  // XInput's B
					if ((keyboard && (a_event->QIDCode() == UiKeyNames::ESCAPE || a_event->QIDCode() == UiKeyNames::TAB)) || (gamepad && a_event->QIDCode() == PAD_B)) {
						RE::UIUtils::PlayMenuSound("UIMenuCancel");
						MainThread::Post([] { UiPages::PanelBack(); });
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
				REX::INFO("UiPanel: the game's code object: {} (type {}, Pressed {})", had, static_cast<int>(engines.GetType()),
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
					REX::WARN("UiPanel: the movie took no BGSCodeObj");
				}
			}

			void DrawPage()
			{
				std::deque<Page> pages;
				{
					std::scoped_lock l{ pageLock };
					pages.swap(nextPages);
				}
				for (const auto& page : pages) {
					page.dialog ? DrawDialog(page) : DrawFullPage(page);
				}
			}

			void DrawDialog(const Page& a_page)
			{
				GValue rows;
				uiMovie->CreateArray(&rows);
				for (const auto& row : a_page.rows) {
					rows.PushBack(GValue(row.c_str()));
				}
				const std::array<GValue, 7> args{ GValue(static_cast<double>(a_page.serial)), GValue(a_page.title.c_str()), GValue(a_page.body.c_str()), rows, GValue(a_page.keepRow), GValue(a_page.input), GValue(a_page.inputText.c_str()) };
				if (!menuObj.Invoke("SetDialog", nullptr, args.data(), args.size())) {
					REX::WARN("UiPanel: the movie has no SetDialog");
				}
			}

			void DrawFullPage(const Page& a_page)
			{
				const auto* page = &a_page;
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
				const std::array<GValue, 9> args{ GValue(static_cast<double>(page->serial)), GValue(page->body.c_str()), rows, hints, GValue(page->keepRow), GValue(page->framed), players, GValue(page->header.c_str()), GValue(page->markdown) };
				if (!menuObj.Invoke("SetPage", nullptr, args.data(), args.size())) {
					REX::WARN("UiPanel: the movie has no SetPage");
				}
			}

			void SetLook()
			{
				auto version = std::format("{}", F4SE::GetPluginVersion());
				while (version.ends_with(".0") && std::ranges::count(version, '.') > 2) {
					version.resize(version.size() - 2);  // 0.10.1.0: 0.10.1
				}
				const auto back = Text::Get("mpmenu.back");
				const std::array<GValue, 8> args{ GValue(static_cast<double>(boxLeft.load())), GValue(static_cast<double>(boxTop.load())),
					GValue(static_cast<double>(boxRight.load())), GValue(static_cast<double>(boxBottom.load())),
					GValue(static_cast<double>(lookColor.load())), GValue(version.c_str()), GValue(back.c_str()), GValue(centered.load()) };
				if (!menuObj.Invoke("SetLook", nullptr, args.data(), args.size())) {
					REX::WARN("UiPanel: the movie has no SetLook");
				}
			}

			std::uint32_t number;  // which opening this menu is
		};

		// No movie (they're built from ui/, and not in the repo): the message boxes, without opening a menu that has nothing to show.
		bool MovieThere()
		{
			std::error_code error;
			if (std::filesystem::exists(Platform::GamePath(std::format("Data/{}", MOVIE)), error)) {
				return true;
			}
			REX::ERROR("UiPanel: {} is not in Data; using message boxes", MOVIE);
			MainThread::Post([] { UiPages::Open(); });
			return false;
		}

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
				nextPages.clear();
			}
			++opened;
			wanted = true;
			hideWaiting = false;
			if (const auto queue = RE::UIMessageQueue::GetSingleton()) {
				queue->AddMessage(NAME, RE::UI_MESSAGE_TYPE::kShow);
			}
			UiPages::OpenInPanel();
		}
	}

	void Register()
	{
		if (const auto ui = RE::UI::GetSingleton()) {
			ui->RegisterMenu(NAME, &Create);
			REX::INFO("UiPanel: registered {}", NAME);
		}
	}

	void Open(float a_left, float a_top, float a_right, float a_bottom)
	{
		MainThread::Assert("UiPanel::Open");
		if (!Hud::IsSettled()) {
			return;  // the entry's press came as the UI went down: pressed again once it is back
		}
		if (!MovieThere()) {
			return;
		}
		lookColor = ReadHudColor();
		Show(a_left, a_top, a_right, a_bottom, false);
	}

	void OpenCentered()
	{
		MainThread::Assert("UiPanel::OpenCentered");
		if (!MovieThere()) {
			return;
		}
		SetPlayerControls(false);
		lookColor = Equipment::InPowerArmor(RE::PlayerCharacter::GetSingleton()) ? POWER_ARMOR_COLOR : ReadHudColor();
		Show(0.0f, 0.0f, 0.0f, CENTERED_BOX_HEIGHT, true);
	}

	void SetPage(Page a_page)
	{
		std::scoped_lock l{ pageLock };
		nextPages.push_back(std::move(a_page));
	}

	void CaptureKey(bool a_on)
	{
		capturing = a_on;
	}

	void CaptureText(bool a_on)
	{
		// The game's own text mode, or the keys typed work the game too: the Pip-Boy, VATS. It counts, so only as this changes.
		if (typing.exchange(a_on) != a_on) {
			if (const auto map = RE::ControlMap::GetSingleton()) {
				map->SetTextEntryMode(a_on);
			}
		}
	}

	void Close()
	{
		SetPlayerControls(true);
		capturing = false;
		CaptureText(false);
		wanted = false;
		if (Hud::IsSettled()) {
			Hide();
		} else {
			hideWaiting = true;
		}
	}

	void EscPressed()
	{
		MainThread::Assert("UiPanel::EscPressed");
		if (capturing.exchange(false)) {
			UiPages::PanelKey(true, UiKeyNames::ESCAPE);
			return;
		}
		RE::UIUtils::PlayMenuSound("UIMenuCancel");
		UiPages::PanelBack();
	}

	void OnHudAdvance(RE::IMenu* a_hud)
	{
		// What sits in the middle of the HUD, which draws over the panel. The rest of the HUD stays, for its toasts.
		static constexpr std::array PATHS{ "root.CenterGroup_mc.HUDCrosshair_mc", "root.CenterGroup_mc.RolloverWidget_mc", "root.CenterGroup_mc.QuickContainerWidget_mc" };
		static bool hidden = false;
		const bool  hide = centered && wanted;
		if (!a_hud || !a_hud->uiMovie || (!hide && !hidden)) {
			return;
		}
		for (const auto path : PATHS) {
			GValue object;
			if (!a_hud->uiMovie->GetVariable(&object, path) || !object.IsDisplayObject()) {
				continue;
			}
			// By alpha: the HUD sets their visible itself, as what's under the crosshair changes and as the player gets in or out of power
			// armor, so what it says is still right when the panel goes. A visible saved at opening and put back left the crosshair off.
			object.SetMember("alpha", GValue(hide ? 0.0 : 1.0));
		}
		hidden = hide;
	}

	bool Centered()
	{
		return centered;
	}

	bool Showing()
	{
		return wanted;
	}
}
