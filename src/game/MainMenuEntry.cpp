#include "game/MainMenuEntry.h"

#include "MainThread.h"
#include "Text.h"
#include "game/Hud.h"
#include "game/MainMenuPlace.h"
#include "game/Menu.h"

#include <array>
#include <atomic>
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
		constexpr std::array<const char*, 2> LIST_PATHS{ "root.MainPanel_mc.List_mc", "root.Menu_mc.MainPanel_mc.List_mc" };
		constexpr const char*                ITEM_PRESS = "BSScrollingList::itemPress";

		struct Hooked
		{
			const char* menu;
			AdvanceFn   original = nullptr;
			bool        loggedNoList = false;  // the menu's update thread only
		};
		Hooked mainMenu{ "main menu" };
		Hooked pauseMenu{ "pause menu" };

		std::string label;  // read once at Install, after Text::Load: the menus' update threads only read it

		// A list rebuilt by the menu gets our entry again and another listener with it; the older listeners stay on a
		// list the menu kept. They all hear one press in the same dispatch, inside one update of the menu, so a press
		// is taken once per update.
		std::atomic<std::uint64_t> advances{ 0 };
		std::atomic<std::uint64_t> pressedAt{ ~0ull };

		double Number(const GValue& a_value)
		{
			return a_value.IsNumber() ? a_value.GetNumber() : a_value.IsInt() ? a_value.GetInt() : a_value.IsUInt() ? a_value.GetUInt() : -1.0;
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
				MainThread::Post([] { Menu::Open(); });
			}
		};
		PressHandler* handler = nullptr;  // made on the first update (Scaleform's heap is up by then), never let go

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

	void Install()
	{
		label = Text::Get("mainmenu.entry");
		Hook(mainMenu, RE::VTABLE::MainMenu[0].address(), &MainMenuAdvance);
		Hook(pauseMenu, RE::VTABLE::PauseMenu[0].address(), &PauseMenuAdvance);
	}
}
