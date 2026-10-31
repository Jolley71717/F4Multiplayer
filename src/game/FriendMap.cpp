#include "game/FriendMap.h"

#include "game/FriendMapPlace.h"

#include "Config.h"
#include "game/Compass.h"
#include "game/Hud.h"
#include "game/OpenMenus.h"
#include "game/PlayerMarks.h"
#include "game/RemotePlayers.h"
#include "MainThread.h"
#include "Protocol.h"

// Compass.cpp's: a player's shape (PlayerMarks), about 13 px across and centered on (0, 0), into a Flash Graphics with its line and
// fill already begun. Declared here, not in Compass.h, which would then write to the UI by the UI-hold test's count
// (a header names the Scaleform value; the guard looks for the files that use it).
namespace Compass
{
	void DrawShape(Scaleform::GFx::Value& a_graphics, PlayerMarks::Shape a_shape);
}

namespace FriendMap
{
	namespace
	{
		using GValue = Scaleform::GFx::Value;
		using FriendMapPlace::Map;
		using FriendMapPlace::Point;

		// Pipboy_MapPage.swf, under PipboyMenu.swf's root.Menu_mc (the PipboyMenu class): its CurrentPage is the map
		// page while the map is up, with a holder per map; each holder's MarkerHolder_mc holds the page's markers,
		// its markerArray their data (world x and y, the clip the page placed, whether it is clamped to the edge).
		constexpr const char* MENU_PATH = "root.Menu_mc";
		constexpr std::uint32_t MAX_SAMPLES = 24;
		// Our marks are a little larger than the compass's, beside the page's own markers.
		constexpr double MARK_SCALE = 1.6;
		constexpr std::size_t NAME_LENGTH = 24;

		struct MapSpot
		{
			bool  shown = false;
			Point at;
		};

		struct Target
		{
			std::uint32_t id = 0;
			std::string   name;
			MapSpot       world;   // on the world map
			MapSpot       local;   // on the local map
			bool          inside = false;  // in an interior: the world map shows its way out
		};

		std::mutex          targetsLock;
		std::vector<Target> latestTargets;  // from the main thread's frame, for the menu's next update
		std::atomic<bool>   updateQueued{ false };

		// Bookkeeping of the menu's own update (that thread only).
		struct Placed
		{
			double x = 0.0;
			double y = 0.0;
			double scale = 0.0;
			double alpha = 0.0;
			bool   visible = false;
		};
		struct MapBook
		{
			std::set<std::uint32_t>            made;    // friends we have made a mark for on this map
			std::map<std::uint32_t, Placed>    placed;  // what each mark was last given: unchanged values aren't written again
			std::uint32_t                      children = 0;  // the layer's children at our last write
		};
		std::array<MapBook, 2> books;

		// What the last update of each map found, for Probe and Describe.
		struct Seen
		{
			std::uint32_t markers = 0;
			std::uint32_t samples = 0;
			std::uint32_t used = 0;
			double        worst = 0.0;
			bool          fallback = false;
			bool          fitted = false;
			double        markerScale = 0.0;
			Point         scale;
			Point         offset;
			std::uint32_t shown = 0;
		};
		std::mutex             seenLock;
		std::array<Seen, 2>    seen;

		std::atomic<std::uint32_t> advances{ 0 };
		std::atomic<std::uint32_t> updates{ 0 };
		std::atomic<std::uint32_t> made{ 0 };
		std::atomic<std::uint32_t> fitFailures{ 0 };
		std::atomic<std::uint32_t> noPage{ 0 };
		std::atomic<std::uint32_t> shownNow{ 0 };
		std::atomic<std::uintptr_t> hookedVtable{ 0 };

		// Interiors' ways out (main thread), from every door in the game's data, read once when first wanted.
		FriendMapPlace::Doors doors;
		bool                  doorsRead = false;
		std::uint32_t         lastOutside = 0;  // the worldspace we were last outside in (an interior with no way out)

		double NumberOf(const GValue& a_value, double a_default = 0.0)
		{
			switch (a_value.GetType()) {
			case Scaleform::GFx::Value::ValueType::kNumber:
				return a_value.GetNumber();
			case Scaleform::GFx::Value::ValueType::kInt:
				return a_value.GetInt();
			case Scaleform::GFx::Value::ValueType::kUInt:
				return a_value.GetUInt();
			default:
				return a_default;
			}
		}

		std::optional<double> Member(const GValue& a_object, const char* a_name)
		{
			GValue value;
			if (!a_object.GetMember(a_name, &value)) {
				return std::nullopt;
			}
			const double number = NumberOf(value, std::numeric_limits<double>::quiet_NaN());
			return std::isfinite(number) ? std::optional{ number } : std::nullopt;
		}

		std::optional<Point> PointOf(const GValue& a_object, const char* a_name)
		{
			GValue point;
			if (!a_object.GetMember(a_name, &point) || !point.IsObject()) {
				return std::nullopt;
			}
			const auto x = Member(point, "x");
			const auto y = Member(point, "y");
			return x && y ? std::optional{ Point{ *x, *y } } : std::nullopt;
		}

		// ---- the main thread's side ----

		// A worldspace's map: its parent's, for a worldspace inside another (they share its coordinates).
		std::uint32_t MapWorld(const RE::TESWorldSpace* a_world)
		{
			for (int i = 0; a_world && a_world->parentWorld && i < 8; ++i) {
				a_world = a_world->parentWorld;
			}
			return a_world ? a_world->GetFormID() : 0;
		}

		std::uint32_t MapWorld(std::uint32_t a_worldId)
		{
			return a_worldId ? MapWorld(RE::TESForm::GetFormByID<RE::TESWorldSpace>(a_worldId)) : 0;
		}

		// Every interior door with a way through it: persistent references, loaded with the game data.
		void ReadDoors()
		{
			doorsRead = true;
			const auto start = std::chrono::steady_clock::now();
			// Each door with the door it leads to, whose own cell says where that is (looked up after the forms' lock).
			std::vector<std::pair<FriendMapPlace::Door, RE::ObjectRefHandle>> found;
			{
				const auto& [map, lock] = RE::TESForm::GetAllForms();
				RE::BSAutoReadLock l{ lock };
				if (!map) {
					return;
				}
				for (const auto& [id, form] : *map) {
					const auto ref = form ? form->As<RE::TESObjectREFR>() : nullptr;
					const auto cell = ref ? ref->GetParentCell() : nullptr;
					if (!cell || !cell->IsInterior() || !ref->extraList || !Protocol::IsShareableRef(id)) {
						continue;
					}
					const auto teleport = ref->extraList->GetByType<RE::ExtraTeleport>();
					const auto data = teleport ? teleport->teleportData : nullptr;
					if (!data || !data->linkedDoor) {
						continue;
					}
					found.emplace_back(FriendMapPlace::Door{ cell->GetFormID(), 0, 0, { data->position.x, data->position.y } }, data->linkedDoor);
				}
			}
			for (auto& [door, handle] : found) {
				const auto linked = handle.get();
				const auto there = linked ? linked->GetParentCell() : nullptr;
				if (!there) {
					continue;
				}
				if (there->IsInterior()) {
					door.toCell = there->GetFormID();
				} else {
					door.toWorld = MapWorld(there->worldSpace);
				}
				doors.Add(door);
			}
			const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
			REX::INFO("FriendMap: {} interior doors in {} interiors, read in {} ms", doors.Count(), doors.Interiors(), took.count());
		}

		std::optional<FriendMapPlace::Spot> WayOut(std::uint32_t a_cell)
		{
			if (!doorsRead) {
				ReadDoors();
			}
			return doors.ExteriorOf(a_cell);
		}

		std::vector<Target> FindTargets()
		{
			std::vector<Target> targets;
			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto cell = player ? player->GetParentCell() : nullptr;
			if (!cell) {
				return targets;
			}
			// The world map shows the worldspace we are in, or for an interior the one its way out leads to.
			const bool          inside = cell->IsInterior();
			const std::uint32_t ourCell = inside ? cell->GetFormID() : 0;
			if (!inside) {
				lastOutside = MapWorld(cell->worldSpace);
			}
			// Inside with no way out known (a sealed vault), the world we were last outside in.
			std::optional<std::uint32_t> ourWorld;
			const auto                   world = [&]() {
                if (!ourWorld) {
                    const auto out = inside ? WayOut(ourCell) : std::nullopt;
                    ourWorld = out ? out->worldspace : lastOutside;
                }
                return *ourWorld;
			};
			for (const auto& info : RemotePlayers::List()) {
				if (!info.state) {
					continue;
				}
				const auto& state = *info.state;
				Target      target{ info.id, info.name.substr(0, NAME_LENGTH) };
				if (state.cell != 0) {
					target.inside = true;
					target.local = { inside && state.cell == ourCell, { state.x, state.y } };
					if (const auto out = WayOut(state.cell); out && out->worldspace != 0 && out->worldspace == world()) {
						target.world = { true, out->at };
					}
				} else if (state.worldspace != 0) {
					const auto theirs = MapWorld(state.worldspace);
					target.world = { theirs != 0 && theirs == world(), { state.x, state.y } };
					target.local = { !inside && theirs == world(), { state.x, state.y } };
				}
				if (target.world.shown || target.local.shown) {
					targets.push_back(std::move(target));
				}
			}
			return targets;
		}

		// ---- the menu's own update ----

		bool Draw(GValue& a_graphics, const char* a_method, std::initializer_list<GValue> a_args)
		{
			return a_graphics.Invoke(a_method, nullptr, a_args.begin(), a_args.size());
		}

		std::string ChildName(std::uint32_t a_id)
		{
			return std::format("f4mpFriend{}", a_id);
		}

		std::optional<GValue> FindMark(GValue& a_layer, std::uint32_t a_id)
		{
			const auto name = ChildName(a_id);
			GValue     arg{ name.c_str() };
			GValue     mark;
			if (!a_layer.Invoke("getChildByName", &mark, &arg, 1) || !mark.IsDisplayObject()) {
				return std::nullopt;
			}
			return mark;
		}

		// The friend's name beside their mark, in the Pip-Boy's own font, in their color with a dark edge.
		void AddName(Scaleform::GFx::Movie* a_movie, GValue& a_mark, const std::string& a_name, std::uint32_t a_color)
		{
			if (a_name.empty()) {
				return;
			}
			GValue field;
			a_movie->CreateObject(&field, "flash.text.TextField");
			GValue format;
			a_movie->CreateObject(&format, "flash.text.TextFormat");
			if (!field.IsDisplayObject() || !format.IsObject()) {
				return;
			}
			format.SetMember("font", GValue("$MAIN_Font_Bold"));
			format.SetMember("size", GValue(15.0));
			format.SetMember("color", GValue(a_color));
			field.SetMember("defaultTextFormat", format);
			field.SetMember("embedFonts", GValue(true));
			field.SetMember("autoSize", GValue("left"));
			field.SetMember("selectable", GValue(false));
			field.SetMember("mouseEnabled", GValue(false));
			field.SetMember("text", GValue(a_name.c_str()));
			field.SetMember("x", GValue(8.0));
			field.SetMember("y", GValue(-11.0));
			const GValue glowArgs[]{ GValue(0x000000u), GValue(1.0), GValue(3.0), GValue(3.0), GValue(4.0) };
			GValue       glow;
			a_movie->CreateObject(&glow, "flash.filters.GlowFilter", glowArgs, static_cast<std::uint32_t>(std::size(glowArgs)));
			if (glow.IsObject()) {
				GValue filters;
				a_movie->CreateArray(&filters);
				filters.PushBack(glow);
				field.SetMember("filters", filters);
			}
			a_mark.Invoke("addChild", nullptr, &field, 1);
		}

		// A friend's mark: their shape filled in their color, outlined in black, centered on their spot, and their name.
		// A MovieClip, as the page's own markers are: the page walks the layer's children as MovieClips (ClearMarkers
		// reads each one's markerId), and anything else there would stop it with an error.
		std::optional<GValue> MakeMark(Scaleform::GFx::Movie* a_movie, GValue& a_layer, const Target& a_target)
		{
			const auto& mark = PlayerMarks::Of(a_target.id);
			GValue      clip;
			a_movie->CreateObject(&clip, "flash.display.MovieClip");
			GValue graphics;
			if (!clip.IsDisplayObject() || !clip.GetMember("graphics", &graphics)) {
				return std::nullopt;
			}
			clip.SetMember("name", GValue(ChildName(a_target.id).c_str()));
			clip.SetMember("mouseEnabled", GValue(false));
			clip.SetMember("mouseChildren", GValue(false));
			Draw(graphics, "lineStyle", { GValue(1.5), GValue(0x101010u), GValue(0.9) });
			Draw(graphics, "beginFill", { GValue(mark.color), GValue(1.0) });
			Compass::DrawShape(graphics, mark.shape);
			Draw(graphics, "endFill", {});
			if (Config::Get().friendsOnMapNames) {
				AddName(a_movie, clip, a_target.name, mark.color);
			}
			if (!a_layer.Invoke("addChild", nullptr, &clip, 1)) {
				return std::nullopt;
			}
			++made;
			return clip;
		}

		// The page's markers, sampled: their world spot on the map's axes and where the page put them.
		std::vector<FriendMapPlace::Sample> ReadSamples(const GValue& a_holder, Map a_map, const FriendMapPlace::Corners& a_corners, Seen& a_seen, double& a_markerScale)
		{
			std::vector<FriendMapPlace::Sample> samples;
			GValue                              markers;
			if (!a_holder.GetMember("markerArray", &markers) || !markers.IsArray()) {
				return samples;
			}
			const auto count = markers.GetArraySize();
			a_seen.markers = count;
			a_markerScale = 0.0;
			for (const auto index : FriendMapPlace::SampleIndices(count, MAX_SAMPLES)) {
				GValue info;
				if (!markers.GetElement(index, &info) || !info.IsObject()) {
					continue;
				}
				GValue clamp;
				if (info.GetMember("shouldClamp", &clamp) && clamp.IsBoolean() && clamp.GetBoolean()) {
					continue;  // pinned to the map's edge while off it
				}
				GValue clip;
				if (!info.GetMember("clip", &clip) || !clip.IsDisplayObject()) {
					continue;
				}
				const auto wx = Member(info, "x");
				const auto wy = Member(info, "y");
				const auto px = Member(clip, "x");
				const auto py = Member(clip, "y");
				if (!wx || !wy || !px || !py) {
					continue;
				}
				const auto normalized = FriendMapPlace::Normalized(a_map, a_corners, { *wx, *wy });
				if (!normalized) {
					continue;
				}
				samples.push_back({ *normalized, { *px, *py } });
				// The smallest marker's scale: the page's own for the zoom (the selected one is scaled up).
				if (const auto scale = Member(clip, "scaleX"); scale && *scale > 0.0 && (a_markerScale == 0.0 || *scale < a_markerScale)) {
					a_markerScale = *scale;
				}
			}
			a_seen.samples = static_cast<std::uint32_t>(samples.size());
			return samples;
		}

		void UpdateMap(Scaleform::GFx::Movie* a_movie, const GValue& a_data, GValue& a_holder, Map a_map, const std::vector<Target>& a_targets)
		{
			const auto index = static_cast<std::size_t>(a_map);
			auto&      book = books[index];
			Seen       now;
			GValue     layer;
			GValue     texture;
			if (!a_holder.GetMember("MarkerHolder_mc", &layer) || !layer.IsDisplayObject() || !a_holder.GetMember("TextureHolder_mc", &texture) || !texture.IsDisplayObject()) {
				return;
			}
			const char* prefix = a_map == Map::kWorld ? "WorldMap" : "LocalMap";
			const auto  nw = PointOf(a_data, std::format("{}NWCorner", prefix).c_str());
			const auto  ne = PointOf(a_data, std::format("{}NECorner", prefix).c_str());
			const auto  sw = PointOf(a_data, std::format("{}SWCorner", prefix).c_str());
			if (!nw || !ne || !sw) {
				return;
			}
			const FriendMapPlace::Corners corners{ *nw, *ne, *sw };
			double                        markerScale = 0.0;
			const auto                    samples = ReadSamples(a_holder, a_map, corners, now, markerScale);
			const Point                   size{ Member(texture, "width").value_or(0.0), Member(texture, "height").value_or(0.0) };
			const auto                    fit = FriendMapPlace::FitSamples(samples, size);
			if (fit) {
				now.fitted = true;
				now.used = static_cast<std::uint32_t>(fit->used);
				now.worst = fit->worst;
				now.fallback = fit->fallback;
				now.scale = { fit->x.scale, fit->y.scale };
				now.offset = { fit->x.offset, fit->y.offset };
			} else {
				++fitFailures;
			}
			now.markerScale = markerScale;
			const double scale = (markerScale > 0.0 ? markerScale : 1.0) * MARK_SCALE;

			const auto children = static_cast<std::uint32_t>(Member(layer, "numChildren").value_or(0.0));
			const bool raise = children != book.children;  // the page added markers of its own: ours go back on top
			std::set<std::uint32_t> drawn;
			for (const auto& target : a_targets) {
				const auto& spot = a_map == Map::kWorld ? target.world : target.local;
				if (!spot.shown || !fit) {
					continue;
				}
				const auto normalized = FriendMapPlace::Normalized(a_map, corners, spot.at);
				if (!normalized || (a_map == Map::kLocal && !FriendMapPlace::OnMap(*normalized))) {
					continue;  // off the local map's piece of the world
				}
				const auto at = fit->Place(*normalized);
				auto       mark = FindMark(layer, target.id);
				bool       fresh = false;
				if (!mark) {
					mark = MakeMark(a_movie, layer, target);
					if (!mark) {
						continue;
					}
					book.made.insert(target.id);
					book.placed.erase(target.id);
					fresh = true;
				}
				auto& was = book.placed[target.id];
				if (was.x != at.x) {
					mark->SetMember("x", GValue(at.x));
					was.x = at.x;
				}
				if (was.y != at.y) {
					mark->SetMember("y", GValue(at.y));
					was.y = at.y;
				}
				if (was.scale != scale) {
					mark->SetMember("scaleX", GValue(scale));
					mark->SetMember("scaleY", GValue(scale));
					was.scale = scale;
				}
				// At an interior's door out on the world map: a little faded, as they're not out there themselves.
				const double alpha = a_map == Map::kWorld && target.inside ? 0.7 : 1.0;
				if (was.alpha != alpha) {
					mark->SetMember("alpha", GValue(alpha));
					was.alpha = alpha;
				}
				if (!was.visible) {
					mark->SetMember("visible", GValue(true));
					was.visible = true;
				}
				if (raise && !fresh && children > 0) {
					const GValue args[]{ *mark, GValue(static_cast<double>(children - 1)) };
					layer.Invoke("setChildIndex", nullptr, args, 2);
				}
				drawn.insert(target.id);
			}
			for (const auto id : book.made) {
				if (drawn.contains(id)) {
					continue;
				}
				auto& was = book.placed[id];
				if (!was.visible) {
					continue;
				}
				if (auto mark = FindMark(layer, id)) {
					mark->SetMember("visible", GValue(false));
				}
				was.visible = false;
			}
			book.children = static_cast<std::uint32_t>(Member(layer, "numChildren").value_or(0.0));
			now.shown = static_cast<std::uint32_t>(drawn.size());
			std::scoped_lock l{ seenLock };
			seen[index] = now;
		}

		void Apply(RE::IMenu* a_menu, const std::vector<Target>& a_targets)
		{
			const auto movie = a_menu->uiMovie.get();
			GValue     menu;
			if (!movie || !movie->GetVariable(&menu, MENU_PATH) || !menu.IsDisplayObject()) {
				++noPage;
				return;
			}
			GValue page;
			GValue data;
			if (!menu.GetMember("CurrentPage", &page) || !page.IsDisplayObject() || !menu.GetMember("DataObj", &data) || !data.IsObject()) {
				++noPage;
				return;
			}
			std::uint32_t shown = 0;
			for (const auto [name, map] : { std::pair{ "WorldMapHolder_mc", Map::kWorld }, std::pair{ "LocalMapHolder_mc", Map::kLocal } }) {
				GValue holder;
				if (!page.GetMember(name, &holder) || !holder.IsDisplayObject()) {
					continue;  // not the map page
				}
				GValue visible;
				if (holder.GetMember("visible", &visible) && visible.IsBoolean() && !visible.GetBoolean()) {
					continue;  // the other map is up; this one is brought up to date when it is
				}
				UpdateMap(movie, data, holder, map, a_targets);
				std::scoped_lock l{ seenLock };
				shown += seen[static_cast<std::size_t>(map)].shown;
			}
			shownNow = shown;
			++updates;
		}

		// The Pip-Boy menu's own update: the page's markers are written there by the engine and the page, and nothing
		// else touches its movie meanwhile (the compass's markers are written in the HUD's for the same reason).
		constexpr std::size_t ADVANCE_MOVIE_INDEX = 0x04;  // IMenu::AdvanceMovie
		using AdvanceFn = void (*)(RE::IMenu*, float, std::uint64_t);
		AdvanceFn originalAdvance = nullptr;

		void PipboyAdvance(RE::IMenu* a_this, float a_timeDelta, std::uint64_t a_time)
		{
			originalAdvance(a_this, a_timeDelta, a_time);
			++advances;
			if (Hud::LoadingScreenUp() || !Hud::IsSettled()) {
				updateQueued = false;
				return;
			}
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

		// By the class's vtable, so the menu is never looked up (OpenMenus); the slot is PipboyMenu's own.
		void HookPipboy()
		{
			if (hookedVtable != 0) {
				return;
			}
			MainThread::Assert("FriendMap::HookPipboy");
			const auto vtable = RE::VTABLE::PipboyMenu[0].address();
			const auto slot = vtable + sizeof(void*) * ADVANCE_MOVIE_INDEX;
			originalAdvance = *reinterpret_cast<AdvanceFn*>(slot);
			REL::WriteSafeData(slot, reinterpret_cast<std::uintptr_t>(&PipboyAdvance));
			hookedVtable = vtable;
			REX::INFO("FriendMap: hooked the Pip-Boy's update (vtable {:X})", vtable);
		}
	}

	void Frame()
	{
		if (Hud::LoadingScreenUp() || !Hud::IsSettled()) {
			return;
		}
		HookPipboy();
		if (!OpenMenus::Open(OpenMenus::Menu::kPipboy)) {
			return;
		}
		auto targets = Config::Get().friendsOnMap ? FindTargets() : std::vector<Target>{};
		if (targets.empty() && shownNow.load() == 0) {
			return;  // nobody to show and nothing showing: the Pip-Boy is not touched
		}
		{
			std::scoped_lock l{ targetsLock };
			latestTargets = std::move(targets);
		}
		updateQueued = true;
	}

	std::string Probe()
	{
		std::string out = Describe();
		std::scoped_lock l{ seenLock };
		for (const auto map : { Map::kWorld, Map::kLocal }) {
			const auto& s = seen[static_cast<std::size_t>(map)];
			out += std::format("\n{}: markers={} samples={} fitted={} used={} worst={:.2f} fallback={} scale=({:.1f},{:.1f}) offset=({:.1f},{:.1f}) markerScale={:.3f} shown={}",
				map == Map::kWorld ? "world" : "local", s.markers, s.samples, s.fitted, s.used, s.worst, s.fallback, s.scale.x, s.scale.y, s.offset.x, s.offset.y, s.markerScale, s.shown);
		}
		std::scoped_lock t{ targetsLock };
		for (const auto& target : latestTargets) {
			out += std::format("\nfriend {}: world={} ({:.0f},{:.0f}) local={} ({:.0f},{:.0f}) inside={}", target.id, target.world.shown, target.world.at.x, target.world.at.y,
				target.local.shown, target.local.at.x, target.local.at.y, target.inside);
		}
		return out;
	}

	std::string Describe()
	{
		return std::format("friendmap: shown={} made={} updates={} advances={} fitFailures={} noPage={} doors={}", shownNow.load(), made.load(), updates.load(), advances.load(),
			fitFailures.load(), noPage.load(), doors.Count());
	}
}
