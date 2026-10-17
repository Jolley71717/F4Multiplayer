#include "Test.h"

#include "game/EnemySpawn.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>

// The `spawnenemies` dev command (src/game/EnemySpawn.h): what it accepts, where it sets the enemies and what it answers.

TEST("spawnenemies: reads a base, a count and a radius, and caps a horde")
{
	const auto order = EnemySpawn::Parse("0001A4D7 3 800");
	REQUIRE(order.has_value());
	CHECK(order->base == 0x0001A4D7);
	CHECK(order->asked == 3);
	CHECK(order->count == 3);
	CHECK(order->radius == 800.0f);

	const auto spaced = EnemySpawn::Parse("  0x0001a4d7   2  300.5 ");
	REQUIRE(spaced.has_value());
	CHECK(spaced->base == 0x0001A4D7);
	CHECK(spaced->count == 2);
	CHECK(spaced->radius == 300.5f);

	const auto horde = EnemySpawn::Parse("0001A4D7 50 1000");
	REQUIRE(horde.has_value());
	CHECK(horde->asked == 50);
	CHECK(horde->count == EnemySpawn::MAX_COUNT);

	CHECK(EnemySpawn::Parse("0001A4D7 1 256").has_value());   // both limits are allowed
	CHECK(EnemySpawn::Parse("0001A4D7 1 4000").has_value());
}

TEST("spawnenemies: refuses what it can't read or would place badly")
{
	CHECK(!EnemySpawn::Parse(""));
	CHECK(!EnemySpawn::Parse("0001A4D7"));
	CHECK(!EnemySpawn::Parse("0001A4D7 3"));
	CHECK(!EnemySpawn::Parse("0001A4D7 3 800 extra"));
	CHECK(!EnemySpawn::Parse("raider 3 800"));
	CHECK(!EnemySpawn::Parse("0 3 800"));            // no form has id 0
	CHECK(!EnemySpawn::Parse("1FFFFFFFF 3 800"));    // more than a form ID holds
	CHECK(!EnemySpawn::Parse("0001A4D7 0 800"));
	CHECK(!EnemySpawn::Parse("0001A4D7 -2 800"));
	CHECK(!EnemySpawn::Parse("0001A4D7 2.5 800"));
	CHECK(!EnemySpawn::Parse("0001A4D7 3 100"));     // inside each other and the player
	CHECK(!EnemySpawn::Parse("0001A4D7 3 9000"));    // past the loaded cells
	CHECK(!EnemySpawn::Parse("0001A4D7 3 nan"));
	CHECK(!EnemySpawn::Parse("0001A4D7 3 inf"));
	CHECK(!EnemySpawn::Parse("0001A4D7 3 800m"));
	CHECK(EnemySpawn::Usage().starts_with("error: usage: spawnenemies"));
}

TEST("spawnenemies: sets them evenly round the player at the radius, the first ahead, each facing the player")
{
	constexpr float X = 1000.0f, Y = -2000.0f, Z = 50.0f, RADIUS = 600.0f;
	constexpr float PI = std::numbers::pi_v<float>;
	const auto near = [](float a_a, float a_b) { return std::abs(a_a - a_b) < 0.01f; };

	const auto ahead = EnemySpawn::Ring(X, Y, Z, 0.0f, 4, RADIUS);
	REQUIRE(ahead.size() == 4);
	CHECK(near(ahead[0].x, X) && near(ahead[0].y, Y + RADIUS));   // heading 0 looks north, +y
	CHECK(near(ahead[1].x, X + RADIUS) && near(ahead[1].y, Y));   // turning clockwise: east next
	CHECK(near(ahead[2].x, X) && near(ahead[2].y, Y - RADIUS));
	CHECK(near(ahead[3].x, X - RADIUS) && near(ahead[3].y, Y));
	CHECK(near(ahead[0].heading, PI));                             // the one ahead looks south, back at the player

	for (const float heading : { 0.0f, 1.0f, 3.0f, -2.0f, 6.0f }) {
		for (const int count : { 1, 2, 5, EnemySpawn::MAX_COUNT }) {
			const auto ring = EnemySpawn::Ring(X, Y, Z, heading, count, RADIUS);
			REQUIRE(ring.size() == static_cast<std::size_t>(count));
			CHECK(near(ring[0].x, X + std::sin(heading) * RADIUS) && near(ring[0].y, Y + std::cos(heading) * RADIUS));
			for (const auto& spot : ring) {
				CHECK(near(std::hypot(spot.x - X, spot.y - Y), RADIUS));
				CHECK(spot.z == Z + EnemySpawn::LIFT);
				CHECK(spot.heading >= 0.0f && spot.heading < 2.0f * PI);
				// A step along its heading takes it toward the player.
				const float stepX = spot.x + std::sin(spot.heading) * RADIUS;
				const float stepY = spot.y + std::cos(spot.heading) * RADIUS;
				CHECK(near(stepX, X) && near(stepY, Y));
			}
		}
	}

	// At the smallest radius the most a command places still stand apart from each other.
	const auto crowd = EnemySpawn::Ring(0.0f, 0.0f, 0.0f, 0.0f, EnemySpawn::MAX_COUNT, EnemySpawn::MIN_RADIUS);
	CHECK(std::hypot(crowd[1].x - crowd[0].x, crowd[1].y - crowd[0].y) > 90.0f);
	CHECK(EnemySpawn::Ring(0.0f, 0.0f, 0.0f, 0.0f, 0, RADIUS).empty());
}

TEST("spawnenemies: answers with the new references, the base's name one quoted word")
{
	const auto order = EnemySpawn::Parse("0001A4D7 50 800");
	REQUIRE(order.has_value());
	const std::array<std::uint32_t, 2> placed{ 0xFF001234, 0xFF00ABCD };
	CHECK(EnemySpawn::Answer(*order, "Raider", placed) == "placed=2 asked=50 base=0001A4D7 'Raider' radius=800 ids=FF001234,FF00ABCD");
	CHECK(EnemySpawn::Answer(*order, "Gunner's Dog", {}) == "placed=0 asked=50 base=0001A4D7 'Gunner`s Dog' radius=800 ids=none");
}
