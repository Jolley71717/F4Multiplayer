#include "game/MenuIcons.h"

#include "Config.h"
#include "game/Equipment.h"
#include "game/Hud.h"
#include "game/MenuIconPlace.h"
#include "game/OpenMenus.h"
#include "game/RemotePlayers.h"

namespace MenuIcons
{
	namespace
	{
		using GValue = Scaleform::GFx::Value;
		using MenuIconPlace::Glyph;

		// The HUD movie's own timeline: the icons go in a layer of their own on it.
		constexpr const char* ROOT_PATH = "root";
		constexpr const char* LAYER_NAME = "f4mpMenuIcons";
		// The bone the icon hangs over (Fallout 4's human skeleton).
		constexpr const char* HEAD_BONE = "Head";
		// What the icons are drawn in besides the HUD's own color (the player's choice in the game's settings).
		constexpr std::uint32_t DARK = 0x101010;

		// One friend's icon this frame, from the main thread.
		struct Target
		{
			std::uint32_t         id = 0;
			Glyph                 glyph = Glyph::kNone;
			MenuIconPlace::Screen screen;
			float                 distance = 0.0f;
		};

		std::mutex             targetsLock;
		std::vector<Target>    latestTargets;
		std::uint32_t          latestColor = 0x12FF15;  // the HUD's default green, until the game says
		std::atomic<bool>      queued{ false };
		std::atomic<bool>      hideQueued{ false };      // a menu of ours is up: hide what shows, once

		// What each icon was last given (HUD thread only), so an unchanged value is not written again.
		struct Drawn
		{
			Glyph               glyph = Glyph::kNone;
			std::uint32_t       color = 0;
			MenuIconPlace::Spot spot;
			bool                visible = false;
		};
		std::map<std::uint32_t, Drawn> drawn;
		bool                           layerVisible = false;  // HUD thread only

		std::atomic<std::uint32_t> shown{ 0 };     // icons showing after the last write
		std::atomic<std::uint32_t> made{ 0 };      // shapes made (again after each load: the HUD is rebuilt)
		std::atomic<std::uint32_t> writes{ 0 };    // HUD updates that wrote something
		std::atomic<std::uint32_t> hides{ 0 };     // the layer hidden for a menu of ours, or nobody left in one
		std::atomic<std::uint32_t> skippedMenus{ 0 };

		// The same menus the compass leaves the HUD alone in (Compass's OtherMenuOpen), asked of OpenMenus, never the UI.
		bool OurMenuOpen()
		{
			using enum OpenMenus::Menu;
			const auto ui = RE::UI::GetSingleton();
			return !ui || ui->menuMode != 0 || OpenMenus::AnyOpen({ kDialogue, kPipboy, kWorkshop, kLooks, kPowerArmorMod });
		}

		std::uint32_t HudColor()
		{
			const auto color = RE::HUDMenuUtils::GetGameplayHUDColor();
			const auto channel = [](float a_value) { return static_cast<std::uint32_t>(std::lround(std::clamp(a_value, 0.0f, 1.0f) * 255.0f)); };
			return channel(color.r) << 16 | channel(color.g) << 8 | channel(color.b);
		}

		// The spot the icon hangs over: above the stand-in's head bone, or above its feet when its skeleton has none.
		RE::NiPoint3 OverHead(RE::Actor* a_actor)
		{
			if (const auto head = Equipment::ChildNode(a_actor->Get3D(), HEAD_BONE)) {
				auto at = head->world.translate;
				at.z += MenuIconPlace::OVER_HEAD;
				return at;
			}
			auto at = a_actor->data.location;
			at.z += MenuIconPlace::OVER_FEET;
			return at;
		}

		std::vector<Target> FindTargets()
		{
			std::vector<Target> targets;
			const auto player = RE::PlayerCharacter::GetSingleton();
			if (!player || !player->GetParentCell()) {
				return targets;
			}
			for (const auto& info : RemotePlayers::List()) {
				const auto glyph = MenuIconPlace::GlyphOf(info.menu);
				if (glyph == Glyph::kNone || !info.actor || !info.actor->Get3D() || info.actor->IsDisabled()) {
					continue;
				}
				const auto at = OverHead(info.actor);
				const auto screen = RE::HUDMenuUtils::WorldPtToScreenPt3(at);
				targets.push_back({ info.id, glyph, { screen.x, screen.y, screen.z }, at.GetDistance(player->data.location) });
			}
			return targets;
		}

		bool Draw(GValue& a_graphics, const char* a_method, std::initializer_list<GValue> a_args)
		{
			return a_graphics.Invoke(a_method, nullptr, a_args.begin(), a_args.size());
		}

		// The glyph, MenuIconPlace::SIZE across, its bottom edge at (0, 0) so it stands on the spot over the head.
		void DrawGlyph(GValue& a_graphics, Glyph a_glyph, std::uint32_t a_color)
		{
			Draw(a_graphics, "clear", {});
			switch (a_glyph) {
			case Glyph::kPipboy:
				// The wrist unit: a dark body, a lit screen and the dial beside it.
				Draw(a_graphics, "lineStyle", { GValue(2.0), GValue(a_color), GValue(1.0) });
				Draw(a_graphics, "beginFill", { GValue(DARK), GValue(0.8) });
				Draw(a_graphics, "drawRoundRect", { GValue(-13.0), GValue(-22.0), GValue(26.0), GValue(19.0), GValue(6.0), GValue(6.0) });
				Draw(a_graphics, "endFill", {});
				Draw(a_graphics, "lineStyle", { GValue(0.0), GValue(a_color), GValue(0.0) });
				Draw(a_graphics, "beginFill", { GValue(a_color), GValue(0.9) });
				Draw(a_graphics, "drawRect", { GValue(-9.0), GValue(-18.5), GValue(12.0), GValue(12.0) });
				Draw(a_graphics, "drawCircle", { GValue(8.0), GValue(-12.5), GValue(2.5) });
				Draw(a_graphics, "endFill", {});
				break;
			case Glyph::kPause:
				Draw(a_graphics, "lineStyle", { GValue(1.5), GValue(DARK), GValue(0.9) });
				Draw(a_graphics, "beginFill", { GValue(a_color), GValue(1.0) });
				Draw(a_graphics, "drawRect", { GValue(-8.0), GValue(-22.0), GValue(5.5), GValue(18.0) });
				Draw(a_graphics, "drawRect", { GValue(2.5), GValue(-22.0), GValue(5.5), GValue(18.0) });
				Draw(a_graphics, "endFill", {});
				break;
			case Glyph::kBusy:
				// A speech bubble with three dots, its tail pointing down at the head.
				Draw(a_graphics, "lineStyle", { GValue(2.0), GValue(a_color), GValue(1.0) });
				Draw(a_graphics, "beginFill", { GValue(DARK), GValue(0.8) });
				Draw(a_graphics, "drawRoundRect", { GValue(-14.0), GValue(-24.0), GValue(28.0), GValue(16.0), GValue(10.0), GValue(10.0) });
				Draw(a_graphics, "endFill", {});
				Draw(a_graphics, "moveTo", { GValue(-4.0), GValue(-8.0) });
				Draw(a_graphics, "lineTo", { GValue(0.0), GValue(-2.0) });
				Draw(a_graphics, "lineTo", { GValue(4.0), GValue(-8.0) });
				Draw(a_graphics, "lineStyle", { GValue(0.0), GValue(a_color), GValue(0.0) });
				Draw(a_graphics, "beginFill", { GValue(a_color), GValue(1.0) });
				Draw(a_graphics, "drawCircle", { GValue(-7.0), GValue(-16.0), GValue(2.2) });
				Draw(a_graphics, "drawCircle", { GValue(0.0), GValue(-16.0), GValue(2.2) });
				Draw(a_graphics, "drawCircle", { GValue(7.0), GValue(-16.0), GValue(2.2) });
				Draw(a_graphics, "endFill", {});
				break;
			case Glyph::kNone:
				break;
			}
		}

		std::string IconName(std::uint32_t a_id)
		{
			return std::format("f4mpMenuIcon{}", a_id);
		}

		std::optional<GValue> ChildByName(GValue& a_parent, const char* a_name)
		{
			GValue arg{ a_name };
			GValue child;
			if (!a_parent.Invoke("getChildByName", &child, &arg, 1) || !child.IsDisplayObject()) {
				return std::nullopt;
			}
			return child;
		}

		std::optional<GValue> Make(Scaleform::GFx::Movie* a_movie, GValue& a_parent, const char* a_class, const std::string& a_name)
		{
			GValue object;
			a_movie->CreateObject(&object, a_class);
			if (!object.IsDisplayObject()) {
				return std::nullopt;
			}
			object.SetMember("name", GValue(a_name.c_str()));
			if (!a_parent.Invoke("addChild", nullptr, &object, 1)) {
				return std::nullopt;
			}
			++made;
			return object;
		}

		void SetVisible(GValue& a_object, bool a_visible)
		{
			a_object.SetMember("visible", GValue(a_visible));
		}
	}

	void Frame()
	{
		if (Hud::LoadingScreenUp() || !Hud::IsSettled()) {
			return;  // the HUD is being torn down or rebuilt; the layer goes with it
		}
		if (OurMenuOpen()) {
			++skippedMenus;
			if (shown.load() != 0) {
				hideQueued = true;
			}
			return;
		}
		auto targets = Config::Get().menuIcons ? FindTargets() : std::vector<Target>{};
		if (targets.empty() && shown.load() == 0) {
			return;  // nobody in a menu and nothing showing: the HUD is not touched
		}
		const auto color = HudColor();
		{
			std::scoped_lock l{ targetsLock };
			latestTargets = std::move(targets);
			latestColor = color;
		}
		queued = true;
	}

	void OnHudAdvance(RE::IMenu* a_hud)
	{
		const bool hide = hideQueued.exchange(false);
		if (!queued.exchange(false) && !hide) {
			return;
		}
		const auto movie = a_hud ? a_hud->uiMovie.get() : nullptr;
		GValue     root;
		if (!movie || !movie->GetVariable(&root, ROOT_PATH) || !root.IsDisplayObject()) {
			return;
		}
		auto layer = ChildByName(root, LAYER_NAME);
		if (!layer) {
			if (hide) {
				shown = 0;
				return;  // a HUD rebuilt since: nothing of ours is on it
			}
			layer = Make(movie, root, "flash.display.Sprite", LAYER_NAME);
			if (!layer) {
				return;
			}
			drawn.clear();  // a fresh layer (a load rebuilt the HUD): every icon is made and written again
			layerVisible = true;
		}
		std::vector<Target> targets;
		std::uint32_t       color = 0;
		if (!hide) {
			std::scoped_lock l{ targetsLock };
			targets = latestTargets;
			color = latestColor;
		}
		++writes;
		if (hide || targets.empty()) {
			if (layerVisible) {
				SetVisible(*layer, false);
				layerVisible = false;
				++hides;
			}
			shown = 0;
			return;
		}
		if (!layerVisible) {
			SetVisible(*layer, true);
			layerVisible = true;
		}
		const auto     frame = movie->GetVisibleFrameRect();
		const MenuIconPlace::Stage stage{ frame.x1, frame.y1, frame.x2, frame.y2 };
		std::uint32_t  visible = 0;
		std::set<std::uint32_t> seen;
		for (const auto& target : targets) {
			seen.insert(target.id);
			const auto spot = MenuIconPlace::Place(target.screen, target.distance, stage);
			const auto name = IconName(target.id);
			auto       icon = ChildByName(*layer, name.c_str());
			auto&      was = drawn[target.id];
			if (!icon) {
				if (!spot.shown) {
					continue;
				}
				icon = Make(movie, *layer, "flash.display.Shape", name);
				if (!icon) {
					continue;
				}
				was = {};
			}
			if (!spot.shown) {
				if (was.visible) {
					SetVisible(*icon, false);
					was.visible = false;
				}
				continue;
			}
			if (was.glyph != target.glyph || was.color != color) {
				GValue graphics;
				if (!icon->GetMember("graphics", &graphics)) {
					continue;
				}
				DrawGlyph(graphics, target.glyph, color);
				was.glyph = target.glyph;
				was.color = color;
			}
			if (!was.visible || MenuIconPlace::Moved(was.spot, spot)) {
				icon->SetMember("x", GValue(static_cast<double>(spot.x)));
				icon->SetMember("y", GValue(static_cast<double>(spot.y)));
				icon->SetMember("scaleX", GValue(static_cast<double>(spot.scale)));
				icon->SetMember("scaleY", GValue(static_cast<double>(spot.scale)));
				was.spot = spot;
			}
			if (!was.visible) {
				SetVisible(*icon, true);
				was.visible = true;
			}
			++visible;
		}
		// Friends out of their menus (or gone): their icons are hidden, kept for the next time.
		for (auto& [id, was] : drawn) {
			if (!was.visible || seen.contains(id)) {
				continue;
			}
			if (auto icon = ChildByName(*layer, IconName(id).c_str())) {
				SetVisible(*icon, false);
			}
			was.visible = false;
		}
		shown = visible;
	}

	std::string Probe()
	{
		const auto player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			return "error: no player";
		}
		std::string out = std::format("menuIcons={} ourMenu={} settled={}", Config::Get().menuIcons, OurMenuOpen(), Hud::IsSettled());
		for (const auto& info : RemotePlayers::List()) {
			out += std::format("; player {} menu={} glyph={}", info.id, info.menu, std::to_underlying(MenuIconPlace::GlyphOf(info.menu)));
			if (!info.actor || !info.actor->Get3D()) {
				out += " standIn=none";
				continue;
			}
			const auto head = Equipment::ChildNode(info.actor->Get3D(), HEAD_BONE);
			const auto at = OverHead(info.actor);
			const auto screen = RE::HUDMenuUtils::WorldPtToScreenPt3(at);
			const auto distance = at.GetDistance(player->data.location);
			const auto spot = MenuIconPlace::Place({ screen.x, screen.y, screen.z }, distance, {});
			out += std::format(" standIn={:08X} head={} at=({:.0f},{:.0f},{:.0f}) screen=({:.3f},{:.3f},{:.3f}) distance={:.0f} shown={} stage=({:.0f},{:.0f}) scale={:.2f}",
				info.actor->GetFormID(), head != nullptr, at.x, at.y, at.z, screen.x, screen.y, screen.z, distance, spot.shown, spot.x, spot.y, spot.scale);
		}
		return out;
	}

	std::string Describe()
	{
		return std::format("menuIcons: shown={} made={} writes={} hides={} skippedMenus={}", shown.load(), made.load(), writes.load(), hides.load(), skippedMenus.load());
	}
}
