// Where a friend goes on the Pip-Boy's maps (game/FriendMapPlace.h): the page's own sums (Pipboy_MapPage.swf's
// ConvertWorldToLocalMarkerPosition and normalizedToMapSpace) played here with a map's corners, texture and gutters, the
// fit made from the markers the page placed, and the way out of an interior through its doors.

#include "Test.h"

#include "game/FriendMapPlace.h"

#include <cmath>
#include <numbers>

using FriendMapPlace::Corners;
using FriendMapPlace::Door;
using FriendMapPlace::Doors;
using FriendMapPlace::FitSamples;
using FriendMapPlace::Map;
using FriendMapPlace::Normalized;
using FriendMapPlace::Point;
using FriendMapPlace::Sample;

namespace
{
	constexpr std::uint32_t COMMONWEALTH = 0x0000003C;
	constexpr std::uint32_t HOUSE = 0x0001F2A4;    // an interior with a door outside
	constexpr std::uint32_t CELLAR = 0x0001F2A5;   // under it: its only door leads up into the house
	constexpr std::uint32_t VAULT = 0x0001F2A6;    // no door out at all
	constexpr std::uint32_t TUNNEL = 0x0001F2A7;   // two ways out, one through the house

	// The page's map: its corners, its texture's size and its gutters (WorldMapHolder's for the Commonwealth).
	struct Page
	{
		Map     map = Map::kWorld;
		Corners corners{ { -100000.0, 100000.0 }, { 100000.0, 100000.0 }, { -100000.0, -100000.0 } };
		double  width = 2048.0, height = 2048.0;
		double  left = 0.02685546875, right = 0.0283203125, top = 0.02685546875, bottom = 0.0283203125;

		// What the page does (MapHolder::normalizedToMapSpace after the holder's ConvertWorldToLocalMarkerPosition).
		[[nodiscard]] Point Place(Point a_world) const
		{
			Point n;
			if (map == Map::kWorld) {
				const double w = std::hypot(corners.ne.x - corners.nw.x, corners.ne.y - corners.nw.y);
				const double h = std::hypot(corners.sw.x - corners.nw.x, corners.sw.y - corners.nw.y);
				n = { (a_world.x - corners.nw.x) / w, (corners.nw.y - a_world.y) / h };
			} else {
				// LocalMapHolder: the spot's projection on each axis, its length over the axis's, negative behind it.
				const Point  v{ a_world.x - corners.nw.x, a_world.y - corners.nw.y };
				const Point  r{ corners.ne.x - corners.nw.x, corners.ne.y - corners.nw.y };
				const Point  d{ corners.sw.x - corners.nw.x, corners.sw.y - corners.nw.y };
				const auto   project = [&](Point a_axis) {
                    const double t = (v.x * a_axis.x + v.y * a_axis.y) / (a_axis.x * a_axis.x + a_axis.y * a_axis.y);
                    const double length = std::hypot(a_axis.x * t, a_axis.y * t) / std::hypot(a_axis.x, a_axis.y);
                    return t < 0 ? -length : length;
				};
				n = { project(r), project(d) };
			}
			const double x0 = width * left, y0 = height * top, x1 = width * (1 - right), y1 = height * (1 - bottom);
			return { x0 + n.x * (x1 - x0), y0 + n.y * (y1 - y0) };
		}

		[[nodiscard]] Sample SampleAt(Point a_world) const
		{
			return { *Normalized(map, corners, a_world), Place(a_world) };
		}
	};

	bool Near(Point a_a, Point a_b, double a_within = 0.01)
	{
		return std::hypot(a_a.x - a_b.x, a_a.y - a_b.y) <= a_within;
	}

	Door Out(std::uint32_t a_cell, Point a_to, std::uint32_t a_world = COMMONWEALTH)
	{
		return { a_cell, 0, a_world, a_to };
	}

	Door Into(std::uint32_t a_cell, std::uint32_t a_to)
	{
		return { a_cell, a_to, 0, {} };
	}
}

TEST("friend map: a spot on the world map is where the page itself would put a marker there")
{
	const Page                page;
	const std::vector<Sample> samples{ page.SampleAt({ -80000, 70000 }), page.SampleAt({ 20000, -30000 }), page.SampleAt({ 65000, 90000 }),
		page.SampleAt({ -10000, -95000 }), page.SampleAt({ 0, 0 }) };
	const auto fit = FitSamples(samples, { page.width, page.height });
	REQUIRE(fit);
	CHECK(!fit->fallback);
	CHECK(fit->used == samples.size());
	CHECK(fit->worst < 0.01);
	for (const Point friendAt : { Point{ 12345, -6789 }, Point{ -99000, 99000 }, Point{ 99999, -99999 }, Point{ 150000, 0 } }) {
		CHECK(Near(fit->Place(*Normalized(Map::kWorld, page.corners, friendAt)), page.Place(friendAt)));
	}
}

TEST("friend map: the gutters of another map (Far Harbor's) come out of its markers, not out of a table")
{
	Page page;
	page.corners = { { -60000.0, 45000.0 }, { 30000.0, 45000.0 }, { -60000.0, -50000.0 } };
	page.width = 1536.0;
	page.height = 1600.0;
	page.left = 0.00920067;
	page.right = 0.03374173;
	page.top = 0.02515624;
	page.bottom = 0.00958931;
	const std::vector<Sample> samples{ page.SampleAt({ -50000, 40000 }), page.SampleAt({ 10000, -20000 }), page.SampleAt({ 25000, 30000 }) };
	const auto                fit = FitSamples(samples, { page.width, page.height });
	REQUIRE(fit);
	CHECK(Near(fit->Place(*Normalized(Map::kWorld, page.corners, { -1234, 5678 })), page.Place({ -1234, 5678 })));
}

TEST("friend map: the local map is turned and fits as well, a spot behind its corner included")
{
	Page page;
	page.map = Map::kLocal;
	page.left = page.right = page.top = page.bottom = 0.0;
	page.width = 1024.0;
	page.height = 768.0;
	// A local map of 4000 by 3000 units turned 30 degrees, its NW corner at (500, 800).
	const double turn = std::numbers::pi / 6.0;
	const Point  nw{ 500.0, 800.0 };
	const Point  across{ std::cos(turn) * 4000.0, std::sin(turn) * 4000.0 };
	const Point  downward{ std::sin(turn) * 3000.0, -std::cos(turn) * 3000.0 };
	page.corners = { nw, { nw.x + across.x, nw.y + across.y }, { nw.x + downward.x, nw.y + downward.y } };
	const std::vector<Sample> samples{ page.SampleAt({ 1500, 900 }), page.SampleAt({ 2500, -400 }), page.SampleAt({ 3000, 1500 }) };
	const auto                fit = FitSamples(samples, { page.width, page.height });
	REQUIRE(fit);
	for (const Point friendAt : { Point{ 2000, 0 }, Point{ 0, 2000 }, Point{ -300, 900 } }) {
		CHECK(Near(fit->Place(*Normalized(Map::kLocal, page.corners, friendAt)), page.Place(friendAt)));
	}
}

TEST("friend map: a local map with too few markers to tell is taken at its texture's size, if they agree")
{
	Page page;
	page.map = Map::kLocal;
	page.left = page.right = page.top = page.bottom = 0.0;  // the local map has no gutters
	page.corners = { { 0.0, 3000.0 }, { 4000.0, 3000.0 }, { 0.0, 0.0 } };
	page.width = 1024.0;
	page.height = 768.0;
	const Point size{ page.width, page.height };

	// None at all, then the player's arrow alone.
	auto fit = FitSamples({}, size);
	REQUIRE(fit);
	CHECK(fit->fallback);
	CHECK(Near(fit->Place(*Normalized(Map::kLocal, page.corners, { 1000, 1000 })), page.Place({ 1000, 1000 })));
	const std::vector<Sample> one{ page.SampleAt({ 2000, 1500 }) };
	fit = FitSamples(one, size);
	REQUIRE(fit);
	CHECK(fit->fallback);

	// The player and a door in a line across the map: the down axis can't be told from them.
	const std::vector<Sample> line{ page.SampleAt({ 2000, 1500 }), page.SampleAt({ 2000, 500 }) };
	fit = FitSamples(line, size);
	REQUIRE(fit);
	CHECK(fit->fallback);

	// A map that isn't laid out that way (gutters after all): the one marker gives it away, and nothing is drawn.
	page.left = 0.1;
	const std::vector<Sample> off{ page.SampleAt({ 2000, 1500 }) };
	CHECK(!FitSamples(off, size));
	CHECK(!FitSamples(off, { 0, 0 }));  // nor without a texture
}

TEST("friend map: a marker out of place is left out of the fit, but a map the markers disagree on is not drawn on")
{
	const Page          page;
	std::vector<Sample> samples;
	for (int i = 0; i < 12; ++i) {
		samples.push_back(page.SampleAt({ -90000.0 + i * 15000.0, 80000.0 - i * 13000.0 + (i % 3) * 20000.0 }));
	}
	samples[4].placed.x += 40.0;  // a marker the page has not placed yet, or one being moved
	auto fit = FitSamples(samples, { page.width, page.height });
	REQUIRE(fit);
	CHECK(fit->used == samples.size() - 1);
	CHECK(Near(fit->Place(*Normalized(Map::kWorld, page.corners, { 5000, 5000 })), page.Place({ 5000, 5000 })));

	// A third of them elsewhere: some other layout (another mod's map page), not a stray marker.
	for (int i = 0; i < 4; ++i) {
		samples[i * 3].placed.y -= 100.0 + i * 37.0;
	}
	CHECK(!FitSamples(samples, { page.width, page.height }));
}

TEST("friend map: corners that make no map, and spots that are not numbers, are nobody's place")
{
	const Corners flat{ { 0, 0 }, { 0, 0 }, { 0, -100 } };
	CHECK(!Normalized(Map::kWorld, flat, { 1, 1 }));
	CHECK(!Normalized(Map::kLocal, flat, { 1, 1 }));
	const Corners good{ { 0, 100 }, { 100, 100 }, { 0, 0 } };
	CHECK(!Normalized(Map::kWorld, good, { std::nan(""), 1 }));
	CHECK(!Normalized(Map::kWorld, good, { 1, INFINITY }));
	const auto mid = Normalized(Map::kWorld, good, { 50, 50 });
	REQUIRE(mid);
	CHECK(Near(*mid, { 0.5, 0.5 }, 1e-9));
	CHECK(FriendMapPlace::OnMap(*mid));
	CHECK(!FriendMapPlace::OnMap(*Normalized(Map::kLocal, good, { 150, 50 })));  // east of the map
	CHECK(!FriendMapPlace::OnMap(*Normalized(Map::kLocal, good, { 50, 101 })));  // north of it
}

TEST("friend map: the markers to read are all of a short list and an even spread of a long one")
{
	CHECK(FriendMapPlace::SampleIndices(0, 24).empty());
	CHECK((FriendMapPlace::SampleIndices(3, 24) == std::vector<std::uint32_t>{ 0, 1, 2 }));
	const auto spread = FriendMapPlace::SampleIndices(400, 4);
	CHECK((spread == std::vector<std::uint32_t>{ 0, 100, 200, 300 }));
}

TEST("friend map: a friend in an interior is at its door out, through the interiors between, the nearest way out first")
{
	Doors doors;
	doors.Add(Out(HOUSE, { 1000, 2000 }));
	doors.Add(Into(HOUSE, CELLAR));
	doors.Add(Into(CELLAR, HOUSE));
	doors.Add(Into(TUNNEL, HOUSE));                  // the long way, added first
	doors.Add(Out(TUNNEL, { -5000, 300 }));          // the short way
	doors.Add(Into(VAULT, VAULT));                   // a door to itself is no way anywhere

	auto out = doors.ExteriorOf(HOUSE);
	REQUIRE(out);
	CHECK(out->worldspace == COMMONWEALTH);
	CHECK(Near(out->at, { 1000, 2000 }));

	out = doors.ExteriorOf(CELLAR);  // up through the house
	REQUIRE(out);
	CHECK(Near(out->at, { 1000, 2000 }));

	out = doors.ExteriorOf(TUNNEL);
	REQUIRE(out);
	CHECK(Near(out->at, { -5000, 300 }));

	CHECK(!doors.ExteriorOf(VAULT));
	CHECK(!doors.ExteriorOf(0x00ABCDEF));  // an interior we know no doors of
	CHECK(doors.Count() == 5);
	CHECK(doors.Interiors() == 3);
}

TEST("friend map: interiors that lead only into each other have no way out, and a door added later is found")
{
	Doors doors;
	doors.Add(Into(HOUSE, CELLAR));
	doors.Add(Into(CELLAR, HOUSE));
	CHECK(!doors.ExteriorOf(HOUSE));  // round and round, never out
	doors.Add(Out(CELLAR, { 7, 8 }));
	const auto out = doors.ExteriorOf(HOUSE);  // what was worked out before is forgotten
	REQUIRE(out);
	CHECK(Near(out->at, { 7, 8 }));

	// A chain longer than any the game has is given up on.
	Doors chain;
	for (std::uint32_t i = 1; i <= 20; ++i) {
		chain.Add(Into(i, i + 1));
	}
	chain.Add(Out(21, { 1, 1 }));
	CHECK(chain.ExteriorOf(18));
	CHECK(!chain.ExteriorOf(1));
}
