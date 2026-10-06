#include "game/Compass.h"

#include "game/RemotePlayers.h"

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

		// UI thread only.
		struct Markers
		{
			Scaleform::GFx::Movie*                                     movie = nullptr;
			std::unordered_map<std::uint32_t, std::unique_ptr<GValue>> shapes;
		};
		Markers markers;

		std::atomic<bool>          updateQueued{ false };
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

		// The player's marker, outlined in black so it stands out from the HUD, centered on (0, 0).
		std::unique_ptr<GValue> MakeShape(Scaleform::GFx::Movie* a_movie, GValue& a_compass, const Marker& a_marker)
		{
			auto shape = std::make_unique<GValue>();
			a_movie->CreateObject(shape.get(), "flash.display.Shape");
			GValue graphics;
			if (!shape->IsDisplayObject() || !shape->GetMember("graphics", &graphics)) {
				return nullptr;
			}
			Draw(graphics, "lineStyle", { GValue(1.5), GValue(0x101010u), GValue(0.9) });
			Draw(graphics, "beginFill", { GValue(a_marker.color), GValue(1.0) });
			DrawShape(graphics, a_marker.shape);
			Draw(graphics, "endFill", {});
			GValue child{ *shape };
			if (!a_compass.Invoke("addChild", nullptr, &child, 1)) {
				return nullptr;
			}
			++made;
			return shape;
		}

		// UI thread.
		void Apply(const std::vector<Target>& a_targets)
		{
			updateQueued = false;
			const auto ui = RE::UI::GetSingleton();
			const auto menu = ui ? ui->GetMenu("HUDMenu") : nullptr;
			const auto movie = menu ? menu->uiMovie.get() : nullptr;
			if (!movie) {
				return;
			}
			if (movie != markers.movie) {
				// A new HUD: the old shapes went with the old movie (don't touch them).
				for (auto& [id, shape] : markers.shapes) {
					static_cast<void>(shape.release());
				}
				markers.shapes.clear();
				markers.movie = movie;
			}
			GValue compass;
			if (!movie->GetVariable(&compass, COMPASS_PATH) || !compass.IsDisplayObject()) {
				return;
			}
			std::uint32_t visible = 0;
			for (const auto& target : a_targets) {
				auto& shape = markers.shapes[target.id];
				if (!shape) {
					shape = MakeShape(movie, compass, MarkerOf(target.id));
					if (!shape) {
						markers.shapes.erase(target.id);
						continue;
					}
				}
				shape->SetMember("x", GValue(static_cast<double>(target.x)));
				shape->SetMember("y", GValue(0.0));
				shape->SetMember("alpha", GValue(target.pinned ? 0.5 : 1.0));
				shape->SetMember("visible", GValue(true));
				++visible;
			}
			for (auto& [id, shape] : markers.shapes) {
				if (std::ranges::none_of(a_targets, [&](const Target& a_target) { return a_target.id == id; })) {
					shape->SetMember("visible", GValue(false));
				}
			}
			shown = visible;
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

	void Frame()
	{
		if (updateQueued.exchange(true)) {
			return;  // the last update hasn't reached the UI thread yet
		}
		F4SE::GetTaskInterface()->AddUITask([targets = FindTargets()]() { Apply(targets); });
	}

	std::string_view MarkerName(std::uint32_t a_playerId)
	{
		return MarkerOf(a_playerId).name;
	}

	std::string Describe()
	{
		return std::format("compass: shown={} made={}", shown.load(), made.load());
	}
}
