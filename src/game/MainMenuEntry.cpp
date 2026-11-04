#include "game/MainMenuEntry.h"

#include "MainThread.h"
#include "Text.h"
#include "game/Hud.h"
#include "game/MainMenuPlace.h"
#include "game/MPMenu.h"

#include <array>
#include <format>
#include <atomic>
#include <mutex>
#include <optional>
#include <utility>
#include <string>
#include <vector>

namespace MainMenuEntry
{
	namespace
	{
		using GValue = Scaleform::GFx::Value;
		using AdvanceFn = void (*)(RE::IMenu*, float, std::uint64_t);

		constexpr std::size_t ADVANCE_MOVIE_INDEX = 0x04;  // IMenu::AdvanceMovie, as the compass hooks the HUD's

		// The menu's main list, a BSScrollingList (MainMenu.swf: MainPanel_mc of the document class MainMenu, its List_mc).
		// The second is a guess for a replaced movie that wraps the menu once more.
		constexpr std::array<const char*, 2> LIST_PATHS{ "root.Menu_mc.MainPanel_mc.List_mc", "root.MainPanel_mc.List_mc" };
		constexpr const char*                ITEM_PRESS = "BSScrollingList::itemPress";

		struct Hooked
		{
			const char* menu;
			AdvanceFn   original = nullptr;
			bool        loggedNoList = false;  // the menu's update thread only
			bool        dimmed = false;        // its list shows that the multiplayer menu has the focus; the same thread
		};
		Hooked mainMenu{ "main menu" };
		Hooked pauseMenu{ "pause menu" };

		std::string label;  // read once at Install, after Text::Load: the menus' update threads only read it

		// A list rebuilt by the menu gets our entry again and another listener with it; the older listeners stay on a
		// list the menu kept. They all hear one press in the same dispatch, inside one update of the menu, so a press
		// is taken once per update.
		std::atomic<std::uint64_t> advances{ 0 };
		std::atomic<std::uint64_t> pressedAt{ ~0ull };

		// The mouse from the multiplayer menu's movie, for the main list's next update.
		struct PassedMouse
		{
			float x = 0.0f;
			float y = 0.0f;
			bool  click = false;
		};
		std::mutex                 mouseLock;
		std::optional<PassedMouse> passedMouse;  // under mouseLock
		std::optional<int>         pressWaiting;  // the row to press once the multiplayer menu has gone; the menu's update thread

		// The main menu's panel box on the stage, read in the menu's update, for the multiplayer menu to line up with (both
		// movies are 1280x720, shown whole). Until a menu has been seen: about where the main menu's is.
		std::mutex boxLock;

		double Number(const GValue& a_value)
		{
			return a_value.IsNumber() ? a_value.GetNumber() : a_value.IsInt() ? a_value.GetInt() : a_value.IsUInt() ? a_value.GetUInt() : -1.0;
		}

		struct Bounds
		{
			double x = 0.0;
			double y = 0.0;
			double width = 0.0;
			double height = 0.0;
		};
		Bounds mainBox{ 230.0, 230.0, 160.0, 330.0 };  // under boxLock

		// An object's box on the stage (getBounds against the movie's root): the menu's panels are drawn at offsets of their
		// own inside the menu object, which is itself placed (and scaled) to the screen's safe area.
		Bounds StageBoundsOf(Scaleform::GFx::Movie* a_movie, GValue& a_object)
		{
			GValue root;
			GValue rect;
			if (!a_object.IsDisplayObject() || !a_movie->GetVariable(&root, "root") || !a_object.Invoke("getBounds", &rect, &root, 1) || !rect.IsObject()) {
				return {};
			}
			GValue x, y, width, height;
			rect.GetMember("x", &x);
			rect.GetMember("y", &y);
			rect.GetMember("width", &width);
			rect.GetMember("height", &height);
			return { Number(x), Number(y), Number(width), Number(height) };
		}

		bool FindList(Scaleform::GFx::Movie* a_movie, GValue& a_list)
		{
			for (const auto path : LIST_PATHS) {
				if (a_movie->GetVariable(&a_list, path) && a_list.IsDisplayObject()) {
					return true;
				}
			}
			return false;
		}

		// Called by the list's itemPress event (in the menu's own input handling or update): only hands the press to
		// the main thread, where the multiplayer menu is opened.
		class PressHandler final : public Scaleform::GFx::FunctionHandler
		{
		public:
			void Call(const Params& a_params) override
			{
				GValue list;
				GValue selected;
				GValue index;
				if (!a_params.movie || !FindList(a_params.movie, list) || !list.GetMember("selectedEntry", &selected) || !selected.IsObject() ||
					!selected.GetMember("index", &index) || static_cast<int>(Number(index)) != MainMenuPlace::ENTRY_INDEX) {
					return;
				}
				const auto now = advances.load();
				if (pressedAt.exchange(now) == now) {
					return;
				}
				REX::INFO("MainMenuEntry: Multiplayer pressed");
				MainThread::Post([] { OpenPanel(); });
			}
		};
		PressHandler* handler = nullptr;  // made on the first update (Scaleform's heap is up by then), never let go

		// The row's clip at a point on the stage, among those the list shows.
		bool ClipAt(GValue& a_list, float a_x, float a_y, GValue& a_clip, int& a_row)
		{
			GValue count;
			if (!a_list.GetMember("numListItems", &count)) {
				return false;
			}
			const auto shown = static_cast<std::uint32_t>(Number(count));
			for (std::uint32_t i = 0; i < shown; ++i) {
				GValue       clip;
				GValue       index{ static_cast<double>(i) };
				if (!a_list.Invoke("GetClipByIndex", &clip, &index, 1) || !clip.IsDisplayObject()) {
					continue;
				}
				GValue visible;
				if (clip.GetMember("visible", &visible) && visible.IsBoolean() && !visible.GetBoolean()) {
					continue;
				}
				const std::array<GValue, 3> at{ GValue(static_cast<double>(a_x)), GValue(static_cast<double>(a_y)), GValue(false) };
				GValue                      hit;
				GValue                      row;
				if (clip.Invoke("hitTestPoint", &hit, at.data(), at.size()) && hit.IsBoolean() && hit.GetBoolean() && clip.GetMember("itemIndex", &row) && Number(row) >= 0.0) {
					a_clip = clip;
					a_row = static_cast<int>(Number(row));
					return true;
				}
			}
			return false;
		}

		// The mouse passed on from the multiplayer menu: a hover chooses the row, a click on another row closes that menu, and
		// once it has gone the row is pressed by a click on its clip, as the game's own list hears one.
		void PassMouse(Scaleform::GFx::Movie* a_movie, GValue& a_list, const std::vector<MainMenuPlace::Entry>& a_entries)
		{
			if (pressWaiting && !MPMenu::Showing()) {
				const int row = *std::exchange(pressWaiting, std::nullopt);
				a_list.SetMember("selectedIndex", GValue(static_cast<double>(row)));
				GValue selected;
				GValue clipIndex;
				GValue clip;
				if (a_list.GetMember("selectedEntry", &selected) && selected.IsObject() && selected.GetMember("clipIndex", &clipIndex)) {
					GValue index{ Number(clipIndex) };
					GValue event;
					const std::array<GValue, 2> args{ GValue("click"), GValue(true) };
					if (a_list.Invoke("GetClipByIndex", &clip, &index, 1) && clip.IsDisplayObject()) {
						a_movie->CreateObject(&event, "flash.events.MouseEvent", args.data(), static_cast<std::uint32_t>(args.size()));
						clip.Invoke("dispatchEvent", nullptr, &event, 1);
					}
				}
				return;
			}
			std::optional<PassedMouse> mouse;
			{
				std::scoped_lock l{ mouseLock };
				mouse.swap(passedMouse);
			}
			if (!mouse || !MPMenu::Showing()) {
				return;
			}
			GValue clip;
			int    row = -1;
			if (!ClipAt(a_list, mouse->x, mouse->y, clip, row)) {
				return;
			}
			GValue selectedIndex;
			a_list.GetMember("selectedIndex", &selectedIndex);
			if (static_cast<int>(Number(selectedIndex)) != row) {
				a_list.SetMember("selectedIndex", GValue(static_cast<double>(row)));
				RE::UIUtils::PlayMenuSound("UIMenuFocus");
			}
			const bool ours = row >= 0 && row < static_cast<int>(a_entries.size()) && a_entries[row].index == MainMenuPlace::ENTRY_INDEX;
			if (mouse->click && !ours) {
				pressWaiting = row;
				MainThread::Post([] { MPMenu::Close(); });
			}
		}

		// Inside the menu's update: our entry after Load, once per list the menu builds.
		void Apply(Hooked& a_hooked, RE::IMenu* a_menu)
		{
			if (Hud::LoadingScreenUp() || !Hud::IsSettled()) {
				return;
			}
			const auto movie = a_menu->uiMovie.get();
			GValue     list;
			if (!movie || !FindList(movie, list)) {
				if (movie && !std::exchange(a_hooked.loggedNoList, true)) {
					REX::WARN("MainMenuEntry: the {} has no list where MainMenu.swf has it; no Multiplayer entry there", a_hooked.menu);
				}
				return;
			}
			// The main panel's box on the stage: ours goes beside it, as the game's own second column does.
			GValue box;
			if (movie->GetVariable(&box, "root.Menu_mc.MainPanel_mc.MainPanelBackground_mc") || movie->GetVariable(&box, "root.MainPanel_mc.MainPanelBackground_mc")) {
				if (const auto at = StageBoundsOf(movie, box); at.width > 0.0 && at.height > 0.0) {
					std::scoped_lock l{ boxLock };
					mainBox = at;
				}
			}
			GValue entries;
			if (!list.GetMember("entryList", &entries) || !entries.IsArray()) {
				return;
			}
			// The texts are copied out: a GValue's string lives only as long as the value.
			const auto                         count = entries.GetArraySize();
			std::vector<std::string>           texts(count);
			std::vector<MainMenuPlace::Entry>  read(count);
			for (std::uint32_t i = 0; i < count; ++i) {
				GValue entry;
				GValue text;
				GValue index;
				if (entries.GetElement(i, &entry) && entry.IsObject()) {
					if (entry.GetMember("text", &text) && text.IsString()) {
						texts[i] = text.GetString();
					}
					entry.GetMember("index", &index);
					read[i].index = static_cast<int>(Number(index));
				}
			}
			for (std::uint32_t i = 0; i < count; ++i) {
				read[i].text = texts[i];
			}
			// While the multiplayer menu is open, our row looks as the game's own does with its submenu open: a list whose
			// entries find the focus elsewhere draws the chosen one's square at half strength (BSScrollingListFadeEntry).
			if (const bool dim = MPMenu::Showing(); dim != a_hooked.dimmed) {
				GValue stage;
				if (list.GetMember("stage", &stage) && stage.IsObject()) {
					a_hooked.dimmed = dim;
					stage.SetMember("focus", dim ? GValue(nullptr) : list);
					list.Invoke("UpdateList");
				}
			}
			PassMouse(movie, list, read);
			const auto at = MainMenuPlace::InsertAt(read);
			if (!at) {
				return;
			}
			if (!handler) {
				handler = new PressHandler();
			}
			GValue entry;
			movie->CreateObject(&entry);
			entry.SetMember("text", GValue(label.c_str()));
			entry.SetMember("index", GValue(static_cast<double>(MainMenuPlace::ENTRY_INDEX)));
			const std::array<GValue, 3> splice{ GValue(static_cast<double>(*at)), GValue(0.0), entry };
			if (!entries.Invoke("splice", nullptr, splice.data(), splice.size())) {
				return;
			}
			list.Invoke("InvalidateData");
			GValue onPress;
			movie->CreateFunction(&onPress, handler);
			const std::array<GValue, 2> listen{ GValue(ITEM_PRESS), onPress };
			const bool                  listening = list.Invoke("addEventListener", nullptr, listen.data(), listen.size());
			REX::INFO("MainMenuEntry: Multiplayer entry added to the {} at {} of {} (listening: {})", a_hooked.menu, *at, count + 1, listening);
		}

		void MainMenuAdvance(RE::IMenu* a_this, float a_timeDelta, std::uint64_t a_time)
		{
			mainMenu.original(a_this, a_timeDelta, a_time);
			++advances;
			Apply(mainMenu, a_this);
		}

		void PauseMenuAdvance(RE::IMenu* a_this, float a_timeDelta, std::uint64_t a_time)
		{
			pauseMenu.original(a_this, a_timeDelta, a_time);
			++advances;
			Apply(pauseMenu, a_this);
		}

		void Hook(Hooked& a_hooked, std::uintptr_t a_vtable, AdvanceFn a_advance)
		{
			const auto slot = a_vtable + sizeof(void*) * ADVANCE_MOVIE_INDEX;
			a_hooked.original = *reinterpret_cast<AdvanceFn*>(slot);
			REL::WriteSafeData(slot, reinterpret_cast<std::uintptr_t>(a_advance));
			REX::INFO("MainMenuEntry: hooked the {}'s update (vtable {:X})", a_hooked.menu, a_vtable);
		}
	}

	void PanelMouse(float a_x, float a_y, bool a_click)
	{
		std::scoped_lock l{ mouseLock };
		// A click isn't lost to the move after it in the same update.
		if (!passedMouse || !passedMouse->click) {
			passedMouse = PassedMouse{ a_x, a_y, a_click };
		}
	}

	void OpenPanel()
	{
		Bounds box;
		{
			std::scoped_lock l{ boxLock };
			box = mainBox;
		}
		MPMenu::Open(static_cast<float>(box.x), static_cast<float>(box.y), static_cast<float>(box.x + box.width), static_cast<float>(box.y + box.height));
	}

	void Install()
	{
		label = Text::Get("mainmenu.entry");
		Hook(mainMenu, RE::VTABLE::MainMenu[0].address(), &MainMenuAdvance);
		Hook(pauseMenu, RE::VTABLE::PauseMenu[0].address(), &PauseMenuAdvance);
	}
}
