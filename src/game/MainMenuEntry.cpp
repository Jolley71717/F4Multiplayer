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
			bool        loggedLayout = false;
		};
		Hooked mainMenu{ "main menu" };
		Hooked pauseMenu{ "pause menu" };

		std::string label;  // read once at Install, after Text::Load: the menus' update threads only read it

		// A list rebuilt by the menu gets our entry again and another listener with it; the older listeners stay on a
		// list the menu kept. They all hear one press in the same dispatch, inside one update of the menu, so a press
		// is taken once per update.
		std::atomic<std::uint64_t> advances{ 0 };
		std::atomic<std::uint64_t> pressedAt{ ~0ull };

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

		Bounds BoundsOf(GValue& a_object)
		{
			Bounds bounds;
			if (!a_object.IsDisplayObject()) {
				return bounds;
			}
			GValue x, y, width, height;
			a_object.GetMember("x", &x);
			a_object.GetMember("y", &y);
			a_object.GetMember("width", &width);
			a_object.GetMember("height", &height);
			return { Number(x), Number(y), Number(width), Number(height) };
		}

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

		// While matching our menu's look to the game's: the main menu's panels, lists and texts once, to the log (place,
		// size, and a text's font and size), three levels down.
		void LogObjects(GValue& a_object, const std::string& a_path, int a_depth)
		{
			GValue count;
			if (a_depth <= 0 || !a_object.IsDisplayObject() || !a_object.GetMember("numChildren", &count)) {
				return;
			}
			for (int i = 0; i < static_cast<int>(Number(count)) && i < 24; ++i) {
				GValue child;
				GValue index{ static_cast<double>(i) };
				if (!a_object.Invoke("getChildAt", &child, &index, 1) || !child.IsDisplayObject()) {
					continue;
				}
				GValue name, visible, text, alpha, transform, colors;
				child.GetMember("name", &name);
				child.GetMember("visible", &visible);
				child.GetMember("alpha", &alpha);
				std::string tint;
				if (child.GetMember("transform", &transform) && transform.IsObject() && transform.GetMember("colorTransform", &colors) && colors.IsObject()) {
					GValue r, g, b, a, ro, go, bo;
					colors.GetMember("redMultiplier", &r);
					colors.GetMember("greenMultiplier", &g);
					colors.GetMember("blueMultiplier", &b);
					colors.GetMember("alphaMultiplier", &a);
					colors.GetMember("redOffset", &ro);
					colors.GetMember("greenOffset", &go);
					colors.GetMember("blueOffset", &bo);
					tint = std::format(" alpha={:.2f} mult=({:.2f},{:.2f},{:.2f},{:.2f}) offset=({},{},{})", Number(alpha), Number(r), Number(g), Number(b), Number(a), Number(ro),
						Number(go), Number(bo));
				}
				const auto path = a_path + "." + (name.IsString() ? name.GetString() : std::format("#{}", i));
				const auto at = BoundsOf(child);
				std::string extra;
				if (child.GetMember("text", &text) && text.IsString()) {
					GValue format, font, size, color;
					if (child.Invoke("getTextFormat", &format) && format.IsObject()) {
						format.GetMember("font", &font);
						format.GetMember("size", &size);
					}
					child.GetMember("textColor", &color);
					extra = std::format(" text='{}' font='{}' size={} color={:06X}", text.GetString(), font.IsString() ? font.GetString() : "?", Number(size),
						static_cast<std::uint32_t>(Number(color)));
				}
				REX::INFO("MainMenuEntry: layout {} x={:.1f} y={:.1f} w={:.1f} h={:.1f} visible={}{}{}", path, at.x, at.y, at.width, at.height,
					visible.IsBoolean() && visible.GetBoolean(), tint, extra);
				LogObjects(child, path, a_depth - 1);
			}
		}

		// While matching our menu's look: how the game draws this menu (its renderer and flags) and the shader targets it
		// tints and shades, by the name of each one's object.
		void LogRendering(RE::IMenu* a_menu, const Hooked& a_hooked)
		{
			const auto game = static_cast<RE::GameMenuBase*>(a_menu);
			REX::INFO("MainMenuEntry: the {} renders with '{}', flags {:X}, depth {}, {} shader targets", a_hooked.menu, a_menu->customRendererName.c_str(),
				a_menu->menuFlags.underlying(), static_cast<int>(a_menu->depthPriority), game->shaderFXObjects.size());
			for (const auto target : game->shaderFXObjects) {
				if (!target) {
					continue;
				}
				GValue name;
				GValue shaded;
				const bool display = target->IsDisplayObject();
				if (display) {
					target->GetMember("name", &name);
					target->GetMember("bUseShadedBackground", &shaded);
				}
				REX::INFO("MainMenuEntry: shader target '{}' color {} background {} shaded member {}", name.IsString() ? name.GetString() : "?",
					static_cast<int>(target->HUDColorType.get()), static_cast<int>(target->backgroundColorType.get()),
					shaded.IsBoolean() ? (shaded.GetBoolean() ? "true" : "false") : "none");
			}
		}

		void LogLayout(Scaleform::GFx::Movie* a_movie, const Hooked& a_hooked)
		{
			REX::INFO("MainMenuEntry: the {}'s layout, once", a_hooked.menu);
			for (const auto path : { "root.MainPanel_mc", "root.SettingsPanel_mc", "root.Menu_mc.MainPanel_mc", "root.Menu_mc.SettingsPanel_mc" }) {
				GValue object;
				if (a_movie->GetVariable(&object, path)) {
					const auto at = BoundsOf(object);
					REX::INFO("MainMenuEntry: layout {} x={:.1f} y={:.1f} w={:.1f} h={:.1f}", path, at.x, at.y, at.width, at.height);
					LogObjects(object, path, 3);
				}
			}
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
				MainThread::Post([] { Bounds box;
					{
						std::scoped_lock l{ boxLock };
						box = mainBox;
					}
					MPMenu::Open(static_cast<float>(box.x), static_cast<float>(box.y), static_cast<float>(box.x + box.width), static_cast<float>(box.y + box.height)); });
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
			if (!std::exchange(a_hooked.loggedLayout, true)) {
				LogLayout(movie, a_hooked);
				LogRendering(a_menu, a_hooked);
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
