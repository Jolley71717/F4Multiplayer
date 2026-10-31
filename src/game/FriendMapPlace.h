#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Where a friend goes on the Pip-Boy's maps (FriendMap), free of the game so it is unit tested.
//
// The map page (Interface/Pipboy_MapPage.swf) puts each of its markers, a child of the map's MarkerHolder_mc, at
//   normalizedToMapSpace(n), n = the marker's world spot on the map's own axes, 0..1 from the NW corner,
// with the axes from the corners the game hands the page (DataObj.WorldMapNWCorner/NECorner/SWCorner, and the
// local map's), and normalizedToMapSpace a scale and offset on each axis (the texture's size less its gutters, which
// the page keeps to itself and which differ by map: the Commonwealth's, Far Harbor's, Nuka-World's, the local map's).
// The marker layer is not moved by panning or zooming (the whole holder is): only the markers' scale changes.
// So we work the axes out from the corners as the page does, and the scale and offset from the page's own markers
// (their world spot, and where the page put them), checked against all of them: a map laid out in some other way
// (another mod's map page) fits badly, and then nothing is drawn.
//
// A friend in an interior is shown on the world map where its way out leads (the doors, followed out through any
// interiors between), as the game shows the player.
namespace FriendMapPlace
{
	struct Point
	{
		double x = 0.0;
		double y = 0.0;
	};

	enum class Map
	{
		kWorld,
		kLocal,
	};

	// The map's corners in the world, as the game hands them to the page.
	struct Corners
	{
		Point nw;
		Point ne;
		Point sw;
	};

	// A world spot on the map's axes (0..1 across from the NW corner, 0..1 down), as the page's
	// ConvertWorldToLocalMarkerPosition works it: the world map's straight along x and y, the local map's projected
	// on its own axes. Nothing for corners that make no map.
	inline std::optional<Point> Normalized(Map a_map, const Corners& a_corners, Point a_world)
	{
		const Point  right{ a_corners.ne.x - a_corners.nw.x, a_corners.ne.y - a_corners.nw.y };
		const Point  down{ a_corners.sw.x - a_corners.nw.x, a_corners.sw.y - a_corners.nw.y };
		const double rr = right.x * right.x + right.y * right.y;
		const double dd = down.x * down.x + down.y * down.y;
		if (!std::isfinite(rr) || !std::isfinite(dd) || rr < 1.0 || dd < 1.0 || !std::isfinite(a_world.x) || !std::isfinite(a_world.y)) {
			return std::nullopt;
		}
		const Point v{ a_world.x - a_corners.nw.x, a_world.y - a_corners.nw.y };
		if (a_map == Map::kWorld) {
			return Point{ v.x / std::sqrt(rr), -v.y / std::sqrt(dd) };
		}
		return Point{ (v.x * right.x + v.y * right.y) / rr, (v.x * down.x + v.y * down.y) / dd };
	}

	// Whether a spot on the map's axes is on the map at all (the local map covers only the ground round the player).
	constexpr bool OnMap(Point a_normalized)
	{
		return a_normalized.x >= 0.0 && a_normalized.x <= 1.0 && a_normalized.y >= 0.0 && a_normalized.y <= 1.0;
	}

	// One of the page's markers: its spot on the map's axes, and where the page put it in the marker layer.
	struct Sample
	{
		Point normalized;
		Point placed;
	};

	struct Axis
	{
		double scale = 0.0;
		double offset = 0.0;
	};

	// The page's normalizedToMapSpace, as far as its markers tell it.
	struct Fit
	{
		Axis        x;
		Axis        y;
		double      worst = 0.0;  // the farthest any marker used is from where the fit puts it, in the layer's pixels
		std::size_t used = 0;     // the markers it agrees with
		bool        fallback = false;  // too few markers to tell: the texture's size, no gutters (the local map's own)

		[[nodiscard]] Point Place(Point a_normalized) const
		{
			return { x.offset + x.scale * a_normalized.x, y.offset + y.scale * a_normalized.y };
		}
	};

	// How far a marker may be from the fit (a pixel or two of rounding, in a layer some hundreds of pixels across).
	constexpr double TOLERANCE = 2.0;

	namespace detail
	{
		// A least-squares line through (n, p) pairs on one axis; nothing when the n hardly differ.
		template <class N, class P>
		std::optional<Axis> Line(std::span<const Sample> a_samples, const std::vector<bool>& a_use, N a_n, P a_p)
		{
			double      sn = 0, sp = 0, snn = 0, snp = 0;
			std::size_t count = 0;
			for (std::size_t i = 0; i < a_samples.size(); ++i) {
				if (!a_use[i]) {
					continue;
				}
				const double n = a_n(a_samples[i]);
				const double p = a_p(a_samples[i]);
				sn += n;
				sp += p;
				snn += n * n;
				snp += n * p;
				++count;
			}
			if (count < 2) {
				return std::nullopt;
			}
			const double c = static_cast<double>(count);
			const double spread = snn - sn * sn / c;
			if (!(spread > 1e-6 * c)) {
				return std::nullopt;  // all in a line across this axis: they tell its offset, not its scale
			}
			const double scale = (snp - sn * sp / c) / spread;
			return Axis{ scale, (sp - scale * sn) / c };
		}

		inline double Miss(const Fit& a_fit, const Sample& a_sample)
		{
			const auto at = a_fit.Place(a_sample.normalized);
			return std::hypot(at.x - a_sample.placed.x, at.y - a_sample.placed.y);
		}
	}

	// The fit from the page's markers, or, with too few of them to tell an axis, a_size (the map texture's width and
	// height) with no gutters, as long as the markers there are agree with it. Every marker used must be within
	// TOLERANCE of it; a stray one (not placed yet, or being moved) is left out, but no more than one in four.
	inline std::optional<Fit> FitSamples(std::span<const Sample> a_samples, Point a_size)
	{
		std::vector<bool> use(a_samples.size(), true);
		const auto        make = [&]() {
			Fit  fit;
			auto x = detail::Line(a_samples, use, [](const Sample& a_s) { return a_s.normalized.x; }, [](const Sample& a_s) { return a_s.placed.x; });
			auto y = detail::Line(a_samples, use, [](const Sample& a_s) { return a_s.normalized.y; }, [](const Sample& a_s) { return a_s.placed.y; });
			fit.fallback = !x || !y;
			fit.x = x && y ? *x : Axis{ a_size.x, 0.0 };
			fit.y = x && y ? *y : Axis{ a_size.y, 0.0 };
			return fit;
		};
		const auto check = [&](Fit& a_fit) {
			a_fit.worst = 0.0;
			a_fit.used = 0;
			for (std::size_t i = 0; i < a_samples.size(); ++i) {
				if (use[i]) {
					a_fit.worst = (std::max)(a_fit.worst, detail::Miss(a_fit, a_samples[i]));
					++a_fit.used;
				}
			}
			return a_fit.worst <= TOLERANCE;
		};
		auto fit = make();
		if (!std::isfinite(fit.x.scale) || !std::isfinite(fit.y.scale) || std::abs(fit.x.scale) < 1.0 || std::abs(fit.y.scale) < 1.0) {
			return std::nullopt;
		}
		if (check(fit)) {
			return fit;
		}
		if (fit.fallback) {
			return std::nullopt;  // the markers there are disagree with the texture's own size
		}
		// The marker it misses most is left out and the fit made again from the rest, while three in four are left.
		for (std::size_t kept = a_samples.size(); (kept - 1) * 4 >= a_samples.size() * 3;) {
			std::size_t worst = a_samples.size();
			double      miss = -1.0;
			for (std::size_t i = 0; i < a_samples.size(); ++i) {
				if (use[i] && detail::Miss(fit, a_samples[i]) > miss) {
					miss = detail::Miss(fit, a_samples[i]);
					worst = i;
				}
			}
			use[worst] = false;
			--kept;
			fit = make();
			if (fit.fallback) {
				return std::nullopt;
			}
			if (check(fit)) {
				return fit;
			}
		}
		return std::nullopt;
	}

	// Which of a_count markers to read (all of them up to a_most, else a_most spread evenly through the list).
	inline std::vector<std::uint32_t> SampleIndices(std::uint32_t a_count, std::uint32_t a_most)
	{
		std::vector<std::uint32_t> out;
		if (a_count == 0 || a_most == 0) {
			return out;
		}
		const auto n = (std::min)(a_count, a_most);
		out.reserve(n);
		for (std::uint32_t i = 0; i < n; ++i) {
			out.push_back(static_cast<std::uint32_t>(static_cast<std::uint64_t>(i) * a_count / n));
		}
		return out;
	}

	// Outside: a worldspace and a spot in it.
	struct Spot
	{
		std::uint32_t worldspace = 0;
		Point         at;
	};

	// A door out of an interior: where it leads, another interior or outside.
	struct Door
	{
		std::uint32_t cell = 0;        // the interior it is in
		std::uint32_t toCell = 0;      // the interior it leads to (0 when it leads outside)
		std::uint32_t toWorld = 0;     // the worldspace it leads out to (0 when it leads to an interior)
		Point         to;              // where it puts you
	};

	// The interiors' doors, and the way out of each interior: the doors followed through the fewest interiors to one
	// that leads outside (the first such door found, in the order they were added, among those as near). Interiors
	// with no way out (a vault sealed but for a lift run by its script) have none.
	class Doors
	{
	public:
		static constexpr int MAX_DEPTH = 8;

		void Add(const Door& a_door)
		{
			if (a_door.cell == 0 || (a_door.toCell == 0 && a_door.toWorld == 0) || a_door.toCell == a_door.cell) {
				return;
			}
			from[a_door.cell].push_back(a_door);
			++count;
			exits.clear();
		}

		[[nodiscard]] std::optional<Spot> ExteriorOf(std::uint32_t a_cell) const
		{
			if (const auto it = exits.find(a_cell); it != exits.end()) {
				return it->second;
			}
			auto found = Search(a_cell);
			exits.emplace(a_cell, found);
			return found;
		}

		[[nodiscard]] std::size_t Count() const { return count; }
		[[nodiscard]] std::size_t Interiors() const { return from.size(); }

	private:
		[[nodiscard]] std::optional<Spot> Search(std::uint32_t a_cell) const
		{
			std::deque<std::pair<std::uint32_t, int>> queue{ { a_cell, 0 } };
			std::unordered_set<std::uint32_t>        seen{ a_cell };
			while (!queue.empty()) {
				const auto [cell, depth] = queue.front();
				queue.pop_front();
				const auto it = from.find(cell);
				if (it == from.end()) {
					continue;
				}
				for (const auto& door : it->second) {
					if (door.toWorld != 0) {
						return Spot{ door.toWorld, door.to };  // breadth first: no way out through fewer interiors
					}
				}
				if (depth + 1 >= MAX_DEPTH) {
					continue;
				}
				for (const auto& door : it->second) {
					if (door.toCell != 0 && seen.insert(door.toCell).second) {
						queue.emplace_back(door.toCell, depth + 1);
					}
				}
			}
			return std::nullopt;
		}

		std::unordered_map<std::uint32_t, std::vector<Door>>                  from;
		mutable std::unordered_map<std::uint32_t, std::optional<Spot>>        exits;
		std::size_t                                                           count = 0;
	};
}
