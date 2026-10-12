#include "game/Compass.h"

#include "game/RemotePlayers.h"
#include "Config.h"
#include "game/Hud.h"

namespace Compass
{
	namespace
	{
		using GValue = Scaleform::GFx::Value;

		constexpr const char* COMPASS_PATH = "root.BottomCenterGroup_mc.CompassWidget_mc";
		// The compass bar is 304 px across and shows about 100 degrees (N and E are 272 px apart).
		constexpr float PIXELS_PER_DEGREE = 3.02f;
		constexpr float HALF_WIDTH = 148.0f;  // past this they're pinned to the edge
		enum class Shape
		{
			kCircle,
			kSquare,
			kDiamond,
			kTriangle,
		};

		struct Marker
		{
			std::uint32_t    color;
			Shape            shape;
			std::string_view name;
		};

		constexpr std::array<Marker, 4> MARKERS{ {
			{ 0x4FC3F7, Shape::kCircle, "blue circle" },
			{ 0xFFB74D, Shape::kSquare, "orange square" },
			{ 0xF06292, Shape::kDiamond, "pink diamond" },
			{ 0xB39DDB, Shape::kTriangle, "purple triangle" },
		} };

		const Marker& MarkerOf(std::uint32_t a_playerId)
		{
			return MARKERS[a_playerId % MARKERS.size()];
		}

		struct Target
		{
			std::uint32_t id;
			float         x;        // on the compass
			bool          pinned;   // off to the side or behind: shown at the edge, dimmer
		};

		// Players we have made a marker for (UI thread only). The markers themselves are looked up
		// by name every time: loading a save rebuilds the HUD, maybe at the same address, and a
		// kept reference would point at a destroyed object.
		std::set<std::uint32_t> markerIds;

		// What each marker was last given (UI thread only): a value that has not changed is not written
		// again, so a HUD with nobody near shows no writes at all.
		struct Placed
		{
			float x = 0.0f;
			bool  pinned = false;
			bool  visible = false;
		};
		std::map<std::uint32_t, Placed> placed;
		std::atomic<bool>          updateQueued{ false };
		// While a loading screen is up the HUD is being torn down or rebuilt; no marker is touched until
		// it has closed. Crashes in the HUD's render tree on loading screens pointed here (TreeNode::
		// SetVisible on the render thread, no plugin frame).
		std::atomic<bool>          loading{ false };
		std::atomic<std::uint32_t> skippedLoading{ 0 };
		// Nor while another menu is up (a conversation, the Pip-Boy, the workshop): the HUD hides and
		// rearranges itself then, and a host crashed in the HUD's render tree as a conversation opened,
		// with the markers being moved every frame by the dialogue camera turning the player.
		std::atomic<std::uint32_t> skippedMenus{ 0 };
		// Which threads the frame and the UI task ran on (distinct ids seen), for the crash reports.
		std::mutex                 threadsLock;
		std::set<std::thread::id>  frameThreads;
		std::set<std::thread::id>  uiThreads;

		void Seen(std::set<std::thread::id>& a_threads)
		{
			std::scoped_lock l{ threadsLock };
			a_threads.insert(std::this_thread::get_id());
		}

		// Menus during which the HUD is left alone, one bit each, kept from the menus' open and close
		// events. Asking the UI instead (GetMenuOpen) takes a reference to the menu; from the HUD's update,
		// which runs beside the main thread's drawing, such a reference can decide which thread destroys a
		// closing menu, and the workshop menu destroyed mid-draw deadlocks the game.
		constexpr std::array OTHER_MENUS{ "DialogueMenu"sv, "PipboyMenu"sv, "WorkshopMenu"sv, "LooksMenu"sv, "PowerArmorModMenu"sv };
		std::atomic<std::uint32_t> otherMenus{ 0 };

		bool OtherMenuOpen()
		{
			const auto ui = RE::UI::GetSingleton();
			return !ui || ui->menuMode != 0 || otherMenus.load() != 0;
		}

		class MenuWatcher :
			public RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent& a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				if (a_event.menuName == "LoadingMenu"sv) {
					loading = a_event.opening;
				}
				for (std::size_t i = 0; i < OTHER_MENUS.size(); ++i) {
					if (a_event.menuName == OTHER_MENUS[i]) {
						const auto bit = 1u << i;
						if (a_event.opening) {
							otherMenus.fetch_or(bit);
						} else {
							otherMenus.fetch_and(~bit);
						}
					}
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		MenuWatcher watcher;
		std::atomic<std::uint32_t> shown{ 0 };
		std::atomic<std::uint32_t> made{ 0 };

		float WrapDegrees(float a_degrees)
		{
			while (a_degrees > 180.0f) {
				a_degrees -= 360.0f;
			}
			while (a_degrees < -180.0f) {
				a_degrees += 360.0f;
			}
			return a_degrees;
		}

		bool Draw(GValue& a_graphics, const char* a_method, std::initializer_list<GValue> a_args)
		{
			return a_graphics.Invoke(a_method, nullptr, a_args.begin(), a_args.size());
		}

		// Each player slot gets its own shape as well as its own color.
		void DrawShape(GValue& a_graphics, Shape a_shape)
		{
			switch (a_shape) {
			case Shape::kCircle:
				Draw(a_graphics, "drawCircle", { GValue(0.0), GValue(0.0), GValue(6.0) });
				break;
			case Shape::kSquare:
				Draw(a_graphics, "drawRect", { GValue(-5.5), GValue(-5.5), GValue(11.0), GValue(11.0) });
				break;
			case Shape::kDiamond:
				Draw(a_graphics, "moveTo", { GValue(0.0), GValue(-7.5) });
				Draw(a_graphics, "lineTo", { GValue(6.5), GValue(0.0) });
				Draw(a_graphics, "lineTo", { GValue(0.0), GValue(7.5) });
				Draw(a_graphics, "lineTo", { GValue(-6.5), GValue(0.0) });
				Draw(a_graphics, "lineTo", { GValue(0.0), GValue(-7.5) });
				break;
			case Shape::kTriangle:
				Draw(a_graphics, "moveTo", { GValue(0.0), GValue(6.5) });
				Draw(a_graphics, "lineTo", { GValue(7.0), GValue(-6.0) });
				Draw(a_graphics, "lineTo", { GValue(-7.0), GValue(-6.0) });
				Draw(a_graphics, "lineTo", { GValue(0.0), GValue(6.5) });
				break;
			}
		}

		std::string ChildName(std::uint32_t a_playerId)
		{
			return std::format("f4mpMarker{}", a_playerId);
		}

		// The player's marker on the compass, if it's there.
		std::optional<GValue> FindShape(GValue& a_compass, std::uint32_t a_playerId)
		{
			const auto name = ChildName(a_playerId);
			GValue arg{ name.c_str() };
			GValue shape;
			if (!a_compass.Invoke("getChildByName", &shape, &arg, 1) || !shape.IsDisplayObject()) {
				return std::nullopt;
			}
			return shape;
		}

		// The player's marker, outlined in black so it stands out from the HUD, centered on (0, 0).
		std::optional<GValue> MakeShape(Scaleform::GFx::Movie* a_movie, GValue& a_compass, std::uint32_t a_playerId)
		{
			const auto& marker = MarkerOf(a_playerId);
			GValue shape;
			a_movie->CreateObject(&shape, "flash.display.Shape");
			GValue graphics;
			if (!shape.IsDisplayObject() || !shape.GetMember("graphics", &graphics)) {
				return std::nullopt;
			}
			const auto name = ChildName(a_playerId);
			shape.SetMember("name", GValue(name.c_str()));
			Draw(graphics, "lineStyle", { GValue(1.5), GValue(0x101010u), GValue(0.9) });
			Draw(graphics, "beginFill", { GValue(marker.color), GValue(1.0) });
			DrawShape(graphics, marker.shape);
			Draw(graphics, "endFill", {});
			if (!a_compass.Invoke("addChild", nullptr, &shape, 1)) {
				return std::nullopt;
			}
			++made;
			return shape;
		}

		// Inside the HUD's own update (HudAdvance).
		void Apply(RE::IMenu* a_hud, const std::vector<Target>& a_targets)
		{
			Seen(uiThreads);
			if (loading) {
				++skippedLoading;
				return;
			}
			if (OtherMenuOpen()) {
				++skippedMenus;
				return;
			}
			const auto movie = a_hud->uiMovie.get();
			GValue     compass;
			if (!movie || !movie->GetVariable(&compass, COMPASS_PATH) || !compass.IsDisplayObject()) {
				return;
			}
			std::uint32_t visible = 0;
			for (const auto& target : a_targets) {
				auto shape = FindShape(compass, target.id);
				if (!shape) {
					shape = MakeShape(movie, compass, target.id);
					if (!shape) {
						continue;
					}
					markerIds.insert(target.id);
					placed.erase(target.id);  // a fresh shape: everything below is written once
					shape->SetMember("y", GValue(0.0));
				}
				auto& was = placed[target.id];
				if (was.x != target.x) {
					shape->SetMember("x", GValue(static_cast<double>(target.x)));
					was.x = target.x;
				}
				if (!was.visible || was.pinned != target.pinned) {
					shape->SetMember("alpha", GValue(target.pinned ? 0.5 : 1.0));
					was.pinned = target.pinned;
				}
				if (!was.visible) {
					shape->SetMember("visible", GValue(true));
					was.visible = true;
				}
				++visible;
			}
			for (const auto id : markerIds) {
				if (std::ranges::none_of(a_targets, [&](const Target& a_target) { return a_target.id == id; })) {
					auto& was = placed[id];
					if (!was.visible) {
						continue;
					}
					if (auto shape = FindShape(compass, id)) {
						shape->SetMember("visible", GValue(false));
					}
					was.visible = false;
				}
			}
			shown = visible;
		}

		// The markers are written from inside the HUD menu's own AdvanceMovie, after the engine's update of
		// it: our frame task and F4SE's UI tasks run on the game's job threads while the engine advances
		// menus in a job of its own and the main thread draws them, and marker writes from there were
		// behind the crashes in Scaleform's render tree (loads, a conversation opening). In the HUD's own
		// update nothing else touches the HUD movie.
		constexpr std::size_t ADVANCE_MOVIE_INDEX = 0x04;  // IMenu::AdvanceMovie
		using AdvanceFn = void (*)(RE::IMenu*, float, std::uint64_t);
		std::atomic<std::uintptr_t> hookedHudVtable{ 0 };
		AdvanceFn                   originalAdvance = nullptr;
		std::mutex                  targetsLock;
		std::vector<Target>         latestTargets;  // from the frame task, for the next HUD update
		std::atomic<std::uint32_t>  advances{ 0 };

		void HudAdvance(RE::IMenu* a_this, float a_timeDelta, std::uint64_t a_time)
		{
			originalAdvance(a_this, a_timeDelta, a_time);
			++advances;
			if (loading) {
				return;
			}
			Hud::OnHudAdvance();
			if (!updateQueued.exchange(false)) {
				return;
			}
			std::vector<Target> targets;
			{
				std::scoped_lock l{ targetsLock };
				targets = latestTargets;
			}
			Apply(a_this, targets);
		}

		void HookHud()
		{
			if (hookedHudVtable != 0) {
				return;  // one HUD class; never patched twice, and the HUD isn't looked up again
			}
			const auto ui = RE::UI::GetSingleton();
			const auto menu = ui ? ui->GetMenu("HUDMenu") : nullptr;
			if (!menu) {
				return;
			}
			const auto vtable = *reinterpret_cast<std::uintptr_t*>(menu.get());
			const auto slot = vtable + sizeof(void*) * ADVANCE_MOVIE_INDEX;
			originalAdvance = *reinterpret_cast<AdvanceFn*>(slot);
			REL::WriteSafeData(slot, reinterpret_cast<std::uintptr_t>(&HudAdvance));
			hookedHudVtable = vtable;
			REX::INFO("Compass: hooked the HUD's update (vtable {:X})", vtable);
		}

		std::vector<Target> FindTargets()
		{
			std::vector<Target> targets;
			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto cell = player ? player->GetParentCell() : nullptr;
			if (!cell) {
				return targets;
			}
			const auto here = cell->IsInterior() ? std::pair{ cell->GetFormID(), 0u } :
			                                       std::pair{ 0u, cell->worldSpace ? cell->worldSpace->GetFormID() : 0u };
			const auto& position = player->data.location;
			const float heading = player->data.angle.z * 180.0f / std::numbers::pi_v<float>;
			for (const auto& info : RemotePlayers::List()) {
				if (!info.state || std::pair{ info.state->cell, info.state->worldspace } != here) {
					continue;
				}
				const float dx = info.state->x - position.x;
				const float dy = info.state->y - position.y;
				if (dx * dx + dy * dy < 1.0f) {
					continue;
				}
				const float bearing = std::atan2(dx, dy) * 180.0f / std::numbers::pi_v<float>;
				const float x = WrapDegrees(bearing - heading) * PIXELS_PER_DEGREE;
				targets.push_back({ info.id, std::clamp(x, -HALF_WIDTH, HALF_WIDTH), std::abs(x) > HALF_WIDTH });
			}
			return targets;
		}
	}

	void Install()
	{
		if (const auto ui = RE::UI::GetSingleton()) {
			ui->RegisterSink<RE::MenuOpenCloseEvent>(&watcher);
		}
	}

	void Frame()
	{
		if (!loading) {
			HookHud();  // the HUD messages go out through it too, markers or not
		}
		if (!Config::Get().compassMarkers || loading || !Hud::IsSettled()) {
			return;  // the HUD is being torn down or rebuilt (loading screen, or a game load just finished)
		}
		Seen(frameThreads);
		if (OtherMenuOpen()) {
			++skippedMenus;
			return;  // the markers stay where they were until the menu has closed
		}
		auto targets = FindTargets();
		if (targets.empty() && shown.load() == 0) {
			return;  // nobody to show and nothing showing: the HUD is not touched at all (main menu, loads, playing alone)
		}
		{
			std::scoped_lock l{ targetsLock };
			latestTargets = std::move(targets);
		}
		updateQueued = true;
	}

	std::string_view MarkerName(std::uint32_t a_playerId)
	{
		return MarkerOf(a_playerId).name;
	}

	std::string Describe()
	{
		std::scoped_lock l{ threadsLock };
		return std::format("compass: shown={} made={} skippedLoading={} skippedMenus={} frameThreads={} hudThreads={} hudAdvances={}", shown.load(), made.load(),
			skippedLoading.load(), skippedMenus.load(), frameThreads.size(), uiThreads.size(), advances.load());
	}
}
