#include "game/MenuIcons.h"

#include "Config.h"
#include "game/Equipment.h"
#include "game/GameStop.h"
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
		// Dev (menuicons layer <path> <class>): where the layer goes and what it is made as, to try others on the rig.
		std::mutex        layerLock;
		std::string       layerParent = ROOT_PATH;
		std::string       layerClass = "flash.display.Sprite";
		std::string       oldParent;
		std::atomic<bool> relayer{ false };
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
			float               drawnScale = 0.0f;  // the size the glyph was drawn at
		};
		// Dev (menuicons opt <name> 0|1), to find how the HUD takes our writes: scale the shape instead of drawing it at its
		// size; add an icon to its parent again after moving it; make it anew each time it moves.
		std::atomic<bool> optScale{ false };
		std::atomic<bool> optReadd{ false };
		std::atomic<bool> optRemake{ false };
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
			return GameStop::Now() || OpenMenus::AnyOpen({ kDialogue, kPipboy, kWorkshop, kLooks, kPowerArmorMod });
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

		// The glyph, MenuIconPlace::SIZE across at a_scale 1, its bottom edge at (a_x, a_y) so it stands on the spot over the
		// head. Drawn at its size and place rather than scaled or moved (a_clear first clears what the graphics hold).
		void DrawGlyph(GValue& a_graphics, Glyph a_glyph, std::uint32_t a_color, double a_scale = 1.0, double a_x = 0.0, double a_y = 0.0, bool a_clear = true)
		{
			const auto x = [&](double a_value) { return GValue(a_x + a_value * a_scale); };
			const auto y = [&](double a_value) { return GValue(a_y + a_value * a_scale); };
			const auto l = [&](double a_value) { return GValue(a_value * a_scale); };
			const auto color = GValue(a_color);
			const auto dark = GValue(DARK);
			if (a_clear) {
				Draw(a_graphics, "clear", {});
			}
			switch (a_glyph) {
			case Glyph::kPipboy:
				// The wrist unit: a dark body, a lit screen and the dial beside it.
				Draw(a_graphics, "lineStyle", { l(2.0), color, GValue(1.0) });
				Draw(a_graphics, "beginFill", { dark, GValue(0.8) });
				Draw(a_graphics, "drawRoundRect", { x(-13.0), y(-22.0), l(26.0), l(19.0), l(6.0), l(6.0) });
				Draw(a_graphics, "endFill", {});
				Draw(a_graphics, "lineStyle", { GValue(0.0), color, GValue(0.0) });
				Draw(a_graphics, "beginFill", { color, GValue(0.9) });
				Draw(a_graphics, "drawRect", { x(-9.0), y(-18.5), l(12.0), l(12.0) });
				Draw(a_graphics, "drawCircle", { x(8.0), y(-12.5), l(2.5) });
				Draw(a_graphics, "endFill", {});
				break;
			case Glyph::kPause:
				Draw(a_graphics, "lineStyle", { l(1.5), dark, GValue(0.9) });
				Draw(a_graphics, "beginFill", { color, GValue(1.0) });
				Draw(a_graphics, "drawRect", { x(-8.0), y(-22.0), l(5.5), l(18.0) });
				Draw(a_graphics, "drawRect", { x(2.5), y(-22.0), l(5.5), l(18.0) });
				Draw(a_graphics, "endFill", {});
				break;
			case Glyph::kBusy:
				// A speech bubble with three dots, its tail pointing down at the head.
				Draw(a_graphics, "lineStyle", { l(2.0), color, GValue(1.0) });
				Draw(a_graphics, "beginFill", { dark, GValue(0.8) });
				Draw(a_graphics, "drawRoundRect", { x(-14.0), y(-24.0), l(28.0), l(16.0), l(10.0), l(10.0) });
				Draw(a_graphics, "endFill", {});
				Draw(a_graphics, "moveTo", { x(-4.0), y(-8.0) });
				Draw(a_graphics, "lineTo", { x(0.0), y(-2.0) });
				Draw(a_graphics, "lineTo", { x(4.0), y(-8.0) });
				Draw(a_graphics, "lineStyle", { GValue(0.0), color, GValue(0.0) });
				Draw(a_graphics, "beginFill", { color, GValue(1.0) });
				Draw(a_graphics, "drawCircle", { x(-7.0), y(-16.0), l(2.2) });
				Draw(a_graphics, "drawCircle", { x(0.0), y(-16.0), l(2.2) });
				Draw(a_graphics, "drawCircle", { x(7.0), y(-16.0), l(2.2) });
				Draw(a_graphics, "endFill", {});
				break;
			case Glyph::kNone:
				break;
			}
		}

		// Dev (menuicons opt canvas 1): every icon drawn into the parent's own graphics, at its place on the stage.
		std::atomic<bool> optCanvas{ false };
		std::string       canvasDrawn;  // HUD thread only: what the canvas holds, so the same is not drawn again

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

		std::optional<GValue> Make(Scaleform::GFx::Movie* a_movie, GValue& a_parent, const char* a_class, const std::string& a_name, Glyph a_glyph = Glyph::kNone, std::uint32_t a_color = 0, double a_scale = 1.0)
		{
			GValue object;
			a_movie->CreateObject(&object, a_class);
			if (!object.IsDisplayObject()) {
				return std::nullopt;
			}
			object.SetMember("name", GValue(a_name.c_str()));
			if (a_glyph != Glyph::kNone) {
				// Drawn before it goes on the stage, as the compass markers are.
				GValue graphics;
				if (!object.GetMember("graphics", &graphics)) {
					return std::nullopt;
				}
				DrawGlyph(graphics, a_glyph, a_color, a_scale);
			}
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
		if (!movie) {
			return;
		}
		std::string parentPath;
		std::string className;
		std::string previous;
		{
			std::scoped_lock l{ layerLock };
			parentPath = layerParent;
			className = layerClass;
			previous = oldParent;
		}
		if (relayer.exchange(false)) {
			canvasDrawn.clear();
			GValue old;
			if (movie->GetVariable(&old, previous.c_str()) && old.IsDisplayObject()) {
				if (auto stale = ChildByName(old, LAYER_NAME)) {
					old.Invoke("removeChild", nullptr, &*stale, 1);
				}
			}
			drawn.clear();
		}
		GValue root;
		if (!movie->GetVariable(&root, parentPath.c_str()) || !root.IsDisplayObject()) {
			return;
		}
		const bool direct = className == "none";  // dev: the icons straight into the parent, no layer of ours
		auto layer = direct ? std::optional<GValue>(root) : ChildByName(root, LAYER_NAME);
		if (!layer) {
			if (hide) {
				shown = 0;
				return;  // a HUD rebuilt since: nothing of ours is on it
			}
			layer = Make(movie, root, className.c_str(), LAYER_NAME);
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
		if (optCanvas.load()) {
			const auto     frame = movie->GetVisibleFrameRect();
			const MenuIconPlace::Stage stage{ frame.x1, frame.y1, frame.x2, frame.y2 };
			std::string    signature;
			std::vector<std::pair<Glyph, MenuIconPlace::Spot>> spots;
			if (!hide) {
				for (const auto& target : targets) {
					const auto spot = MenuIconPlace::Place(target.screen, target.distance, stage);
					if (spot.shown) {
						spots.emplace_back(target.glyph, spot);
						signature += std::format("{}:{:.0f},{:.0f},{:.2f};", std::to_underlying(target.glyph), spot.x * 2.0f, spot.y * 2.0f, spot.scale);
					}
				}
				signature += std::format("{:06X}", color);
			}
			if (signature != canvasDrawn) {
				GValue graphics;
				GValue ox;
				GValue oy;
				const double originX = root.GetMember("x", &ox) && ox.IsNumber() ? ox.GetNumber() : 0.0;  // the parent's place on the stage
				const double originY = root.GetMember("y", &oy) && oy.IsNumber() ? oy.GetNumber() : 0.0;
				if (root.GetMember("graphics", &graphics)) {
					Draw(graphics, "clear", {});
					for (const auto& [glyph, spot] : spots) {
						DrawGlyph(graphics, glyph, color, spot.scale, spot.x - originX, spot.y - originY, false);
					}
					canvasDrawn = signature;
				}
			}
			shown = static_cast<std::uint32_t>(spots.size());
			return;
		}
		if (hide || targets.empty()) {
			if (layerVisible) {
				if (!direct) SetVisible(*layer, false);
				layerVisible = false;
				++hides;
			}
			shown = 0;
			return;
		}
		if (!layerVisible) {
			if (!direct) SetVisible(*layer, true);
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
			const bool scaled = optScale.load();
			const auto size = scaled ? 1.0f : spot.scale;  // the size it is drawn at
			if (icon && spot.shown && optRemake.load() && was.visible && (MenuIconPlace::Moved(was.spot, spot) || std::abs(was.drawnScale - size) >= MenuIconPlace::SCALE_STEP)) {
				layer->Invoke("removeChild", nullptr, &*icon, 1);
				icon.reset();
			}
			if (!icon) {
				if (!spot.shown) {
					continue;
				}
				icon = Make(movie, *layer, "flash.display.Shape", name, target.glyph, color, size);
				if (!icon) {
					continue;
				}
				was = { .glyph = target.glyph, .color = color, .drawnScale = size };
			}
			if (!spot.shown) {
				if (was.visible) {
					SetVisible(*icon, false);
					was.visible = false;
				}
				continue;
			}
			bool changed = false;
			if (was.glyph != target.glyph || was.color != color || std::abs(was.drawnScale - size) >= MenuIconPlace::SCALE_STEP) {
				GValue graphics;
				if (!icon->GetMember("graphics", &graphics)) {
					continue;
				}
				DrawGlyph(graphics, target.glyph, color, size);
				was.glyph = target.glyph;
				was.color = color;
				was.drawnScale = size;
				changed = true;
			}
			if (!was.visible || MenuIconPlace::Moved(was.spot, spot)) {
				icon->SetMember("x", GValue(static_cast<double>(spot.x)));
				icon->SetMember("y", GValue(static_cast<double>(spot.y)));
				if (scaled) {
					icon->SetMember("scaleX", GValue(static_cast<double>(spot.scale)));
					icon->SetMember("scaleY", GValue(static_cast<double>(spot.scale)));
				}
				was.spot = spot;
				changed = true;
			}
			if (!was.visible) {
				SetVisible(*icon, true);
				was.visible = true;
			}
			if (changed && optReadd.load()) {
				layer->Invoke("addChild", nullptr, &*icon, 1);
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

	std::string Opt(const std::string& a_name, bool a_on)
	{
		auto* opt = a_name == "scale" ? &optScale : a_name == "readd" ? &optReadd : a_name == "remake" ? &optRemake : a_name == "canvas" ? &optCanvas : nullptr;
		if (!opt) {
			return "error: menuicons opt scale|readd|remake 0|1";
		}
		*opt = a_on;
		queued = true;
		return std::format("{} = {}", a_name, a_on);
	}

	std::string Layer(const std::string& a_parent, const std::string& a_class)
	{
		{
			std::scoped_lock l{ layerLock };
			oldParent = layerParent;
			layerParent = a_parent;
			layerClass = a_class;
		}
		relayer = true;
		queued = true;
		return std::format("layer goes in {} as {}", a_parent, a_class);
	}

	std::string Describe()
	{
		std::uint32_t color = 0;
		{
			std::scoped_lock l{ targetsLock };
			color = latestColor;
		}
		return std::format("menuIcons: shown={} made={} writes={} hides={} skippedMenus={} color={:06X} opt=scale:{},readd:{},remake:{}", shown.load(), made.load(), writes.load(), hides.load(), skippedMenus.load(), color, optScale.load(), optReadd.load(), optRemake.load());
	}
}
