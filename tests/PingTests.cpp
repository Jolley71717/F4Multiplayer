#include "Test.h"

#include "Protocol.h"
#include "game/PingRules.h"

#include <chrono>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

// Pings: the message (what was pinged, and that a ping names it the way its kind does), and the rules the plugin pings
// by (src/game/PingRules.h): what the crosshair is on, how long a ping shows and how often a player may ping.
namespace
{
	using namespace Protocol;
	using namespace std::chrono_literals;
	using MT = MessageType;
	using PingRules::Candidate;
	using PingRules::Vec;

	constexpr float NaN = std::numeric_limits<float>::quiet_NaN();
	constexpr std::uint32_t RAIDER = 0x0001F3A2;       // a plugin's reference
	constexpr std::uint32_t RAIDER_BASE = 0x0001B56D;  // its base form
	constexpr std::uint32_t SPAWN = 0xFF000123;        // a spawn id (an ambush's raider, shared)

	Ping Outside(PingKind a_kind, std::uint32_t a_target, std::uint32_t a_base, std::string a_name)
	{
		Ping ping{ 4, 0, 0x3C, 100.0f, -200.0f, 30.5f };
		ping.kind = a_kind;
		ping.target = a_target;
		ping.base = a_base;
		ping.name = std::move(a_name);
		return ping;
	}

	std::optional<Ping> RoundTrip(const Ping& a_ping, MT a_type = MT::kPing)
	{
		return DecodePing(Encode(a_ping, a_type));
	}

	// The ray of a player at the origin, eyes 120 up, looking along +y (heading 0), level.
	PingRules::Ray Ahead()
	{
		return PingRules::LookRay({ 0.0f, 0.0f, 120.0f }, 0.0f, 0.0f);
	}

	Candidate Actor(std::uint32_t a_id, float a_x, float a_y, float a_z = 120.0f)
	{
		return { PingKind::kActor, a_id, { a_x, a_y, a_z }, PingRules::ACTOR_RADIUS };
	}

	Candidate Item(std::uint32_t a_id, float a_x, float a_y, float a_z)
	{
		return { PingKind::kItem, a_id, { a_x, a_y, a_z }, PingRules::ITEM_RADIUS };
	}
}

TEST("ping: every kind round-trips with what it names")
{
	for (const auto type : { MT::kPing, MT::kPinged }) {
		const auto spot = RoundTrip(Outside(PingKind::kSpot, 0, 0, ""), type);
		REQUIRE(spot);
		CHECK(spot->playerId == 4);
		CHECK(spot->worldspace == 0x3C);
		CHECK(spot->cell == 0);
		CHECK(spot->x == 100.0f);
		CHECK(spot->y == -200.0f);
		CHECK(spot->z == 30.5f);
		CHECK(spot->kind == PingKind::kSpot);

		const auto npc = RoundTrip(Outside(PingKind::kActor, RAIDER, RAIDER_BASE, "Raider"), type);
		REQUIRE(npc);
		CHECK(npc->kind == PingKind::kActor);
		CHECK(npc->target == RAIDER);
		CHECK(npc->base == RAIDER_BASE);
		CHECK(npc->name == "Raider");

		const auto spawned = RoundTrip(Outside(PingKind::kActor, SPAWN, RAIDER_BASE, "Raider"), type);
		REQUIRE(spawned);
		CHECK(spawned->target == SPAWN);  // the spawn id is the same NPC in every game (NpcSync::LocalRef finds ours)

		const auto stimpak = RoundTrip(Outside(PingKind::kItem, 0x0002A1B2, 0x00023736, "Stimpak"), type);
		REQUIRE(stimpak);
		CHECK(stimpak->kind == PingKind::kItem);
		CHECK(stimpak->target == 0x0002A1B2);
		CHECK(stimpak->base == 0x00023736);
		CHECK(stimpak->name == "Stimpak");

		const auto dropped = RoundTrip(Outside(PingKind::kItem, 0, 0x00023736, "Stimpak"), type);
		REQUIRE(dropped);
		CHECK(dropped->target == 0);  // one made at runtime goes by its base and place only

		const auto friendly = RoundTrip(Outside(PingKind::kPlayer, 2, 0, ""), type);
		REQUIRE(friendly);
		CHECK(friendly->kind == PingKind::kPlayer);
		CHECK(friendly->target == 2);
	}
	// Inside: a cell and no worldspace.
	Ping inside{ 1, 0x0001A2B3, 0, -1.5f, 2.5f, -3.5f };
	const auto in = RoundTrip(inside);
	REQUIRE(in);
	CHECK(in->cell == 0x0001A2B3);
	CHECK(in->worldspace == 0);
}

TEST("ping: a long name is cut to MAX_PING_NAME_LENGTH, and control characters are dropped")
{
	const auto longName = RoundTrip(Outside(PingKind::kActor, RAIDER, RAIDER_BASE, std::string(200, 'a')));
	REQUIRE(longName);
	CHECK(longName->name.size() == MAX_PING_NAME_LENGTH);
	const auto control = RoundTrip(Outside(PingKind::kActor, RAIDER, RAIDER_BASE, "Rai\x01" "der\n"));
	REQUIRE(control);
	CHECK(control->name == "Raider");
}

TEST("ping: a ping whose target does not fit its kind is refused")
{
	// A spot names nothing.
	CHECK(!RoundTrip(Outside(PingKind::kSpot, RAIDER, 0, "")));
	CHECK(!RoundTrip(Outside(PingKind::kSpot, 0, RAIDER_BASE, "")));
	CHECK(!RoundTrip(Outside(PingKind::kSpot, 0, 0, "Raider")));
	// An NPC: a plugin's reference or a spawn id, never our player's own reference or another game's runtime one.
	CHECK(!RoundTrip(Outside(PingKind::kActor, PLAYER_REF_ID, RAIDER_BASE, "Raider")));
	CHECK(!RoundTrip(Outside(PingKind::kActor, 0xFF000000, RAIDER_BASE, "Raider")));
	CHECK(!RoundTrip(Outside(PingKind::kActor, RAIDER, 0xFF000010, "Raider")));  // a base made at runtime goes as 0
	CHECK(RoundTrip(Outside(PingKind::kActor, 0, 0, "Raider")));                  // one the session does not share
	// An item: its own reference (a runtime one goes as 0, its id is another item in every other game).
	CHECK(!RoundTrip(Outside(PingKind::kItem, 0xFF000123, 0x00023736, "Stimpak")));
	CHECK(!RoundTrip(Outside(PingKind::kItem, PLAYER_REF_ID, 0x00023736, "Stimpak")));
	// A friend is a player id, with no base.
	CHECK(!RoundTrip(Outside(PingKind::kPlayer, 0, 0, "")));
	CHECK(!RoundTrip(Outside(PingKind::kPlayer, 2, RAIDER_BASE, "")));
}

TEST("ping: an unknown kind, a bad place or a short or long packet is refused")
{
	auto packet = Encode(Outside(PingKind::kActor, RAIDER, RAIDER_BASE, "Raider"), MT::kPing);
	REQUIRE(DecodePing(packet));
	auto unknown = packet;
	unknown[1 + 4 * 6] = 4;  // the kind, after the player, cell, worldspace and x, y, z
	CHECK(!DecodePing(unknown));
	auto shorter = packet;
	shorter.pop_back();
	CHECK(!DecodePing(shorter));
	auto longer = packet;
	longer.push_back(0);
	CHECK(!DecodePing(longer));

	for (const float bad : { NaN, std::numeric_limits<float>::infinity(), 2.0e6f }) {
		auto ping = Outside(PingKind::kSpot, 0, 0, "");
		ping.x = bad;
		CHECK(!RoundTrip(ping));
		ping.x = 0.0f;
		ping.z = bad;
		CHECK(!RoundTrip(ping));
	}
	// Inside and outside at once, or neither.
	auto both = Outside(PingKind::kSpot, 0, 0, "");
	both.cell = 0x0001A2B3;
	CHECK(!RoundTrip(both));
	auto nowhere = Outside(PingKind::kSpot, 0, 0, "");
	nowhere.worldspace = 0;
	CHECK(!RoundTrip(nowhere));
}

TEST("ping rules: a ping shows for LIFETIME and goes then")
{
	PingRules::Board board;
	const auto       start = PingRules::Clock::now();
	board.Add(Outside(PingKind::kSpot, 0, 0, ""), start);
	board.Expire(start + PingRules::LIFETIME - 1ms);
	CHECK(board.Marks().size() == 1);
	CHECK(PingRules::LIFETIME >= 10s);
	CHECK(PingRules::LIFETIME <= 20s);
	board.Expire(start + PingRules::LIFETIME);
	CHECK(board.Marks().empty());
}

TEST("ping rules: a player's next ping replaces their last one, early, and starts its own time")
{
	PingRules::Board board;
	const auto       start = PingRules::Clock::now();
	board.Add(Outside(PingKind::kSpot, 0, 0, ""), start);
	auto other = Outside(PingKind::kSpot, 0, 0, "");
	other.playerId = 7;
	board.Add(other, start);
	REQUIRE(board.Marks().size() == 2);

	auto again = Outside(PingKind::kActor, RAIDER, RAIDER_BASE, "Raider");  // player 4 again, 10 s later
	board.Add(again, start + 10s);
	REQUIRE(board.Marks().size() == 2);
	CHECK(board.Marks().at(4).ping.kind == PingKind::kActor);
	CHECK(board.Marks().at(4).ping.target == RAIDER);

	board.Expire(start + PingRules::LIFETIME + 1ms);  // player 7's first is gone; player 4's second is not
	REQUIRE(board.Marks().size() == 1);
	CHECK(board.Marks().contains(4));
	board.Expire(start + 10s + PingRules::LIFETIME);
	CHECK(board.Marks().empty());
}

TEST("ping rules: a player who leaves takes their ping with them")
{
	PingRules::Board board;
	const auto       now = PingRules::Clock::now();
	board.Add(Outside(PingKind::kSpot, 0, 0, ""), now);
	board.Forget(9);
	CHECK(board.Marks().size() == 1);
	board.Forget(4);
	CHECK(board.Marks().empty());
}

TEST("ping rules: one ping a second, as the server passes them on")
{
	PingRules::Limiter limit;
	const auto         start = PingRules::Clock::now();
	CHECK(limit.Try(start));
	CHECK(!limit.Try(start + 10ms));
	CHECK(!limit.Try(start + PingRules::MIN_GAP - 1ms));
	CHECK(limit.Refused() == 2);
	CHECK(limit.Try(start + PingRules::MIN_GAP));
	CHECK(!limit.Try(start + PingRules::MIN_GAP + 500ms));  // refused presses don't move the window
	CHECK(limit.Try(start + 2 * PingRules::MIN_GAP));
	limit.Reset();
	CHECK(limit.Try(start + 2 * PingRules::MIN_GAP + 1ms));  // a new session starts afresh
	CHECK(PingRules::MIN_GAP == 1s);                          // Server.cpp's handlePing holds pings to this too
}

TEST("ping rules: the look ray follows the heading and the look angle")
{
	const auto north = PingRules::LookRay({ 0, 0, 0 }, 0.0f, 0.0f);
	CHECK(std::abs(north.direction.y - 1.0f) < 1e-5f);
	CHECK(std::abs(north.direction.z) < 1e-5f);
	const auto east = PingRules::LookRay({ 0, 0, 0 }, 3.14159265f / 2.0f, 0.0f);
	CHECK(std::abs(east.direction.x - 1.0f) < 1e-5f);
	const auto down = PingRules::LookRay({ 0, 0, 0 }, 0.0f, 0.5f);  // angle x positive: looking down
	CHECK(down.direction.z < -0.4f);
	const auto length = std::sqrt(down.direction.x * down.direction.x + down.direction.y * down.direction.y + down.direction.z * down.direction.z);
	CHECK(std::abs(length - 1.0f) < 1e-5f);
	const auto nonsense = PingRules::LookRay({ 0, 0, 0 }, NaN, NaN);
	CHECK(PingRules::IsFinite(nonsense.direction));
}

TEST("ping rules: the crosshair is on the NPC it passes through, not one beside it")
{
	const auto                   ray = Ahead();
	const std::vector<Candidate> near{ Actor(1, 0.0f, 1000.0f), Actor(2, 300.0f, 1000.0f) };
	const auto                   chosen = PingRules::ChooseTarget(ray, std::nullopt, near);
	REQUIRE(chosen);
	CHECK(near[*chosen].id == 1);
	// Just off its middle, still within its body.
	const std::vector<Candidate> off{ Actor(3, 30.0f, 1000.0f, 150.0f) };
	CHECK(PingRules::ChooseTarget(ray, std::nullopt, off));
	// Nothing on the crosshair: none.
	const std::vector<Candidate> beside{ Actor(4, 400.0f, 1000.0f), Actor(5, -400.0f, 1000.0f) };
	CHECK(!PingRules::ChooseTarget(ray, std::nullopt, beside));
}

TEST("ping rules: a far target is easier to hit than its size alone allows, a small near one is not")
{
	const auto ray = Ahead();
	// 100 m out a raider is a few pixels: within MIN_ANGLE_DEGREES of the crosshair counts.
	const float far = 7000.0f;
	const float slack = far * std::tan(PingRules::MIN_ANGLE_DEGREES * 3.14159265f / 180.0f);
	CHECK(PingRules::ChooseTarget(ray, std::nullopt, std::vector{ Actor(1, slack * 0.9f, far) }));
	CHECK(!PingRules::ChooseTarget(ray, std::nullopt, std::vector{ Actor(1, slack * 1.2f, far) }));
	// Past the reach, nothing.
	CHECK(!PingRules::ChooseTarget(ray, std::nullopt, std::vector{ Actor(1, 0.0f, PingRules::REACH + 100.0f) }));
	// A Stimpak at our feet in front: only when the ray passes over it.
	CHECK(PingRules::ChooseTarget(PingRules::LookRay({ 0, 0, 120 }, 0.0f, 0.785f), 170.0f, std::vector{ Item(1, 0.0f, 120.0f, 0.0f) }));
	CHECK(!PingRules::ChooseTarget(ray, std::nullopt, std::vector{ Item(1, 0.0f, 120.0f, 0.0f) }));
}

TEST("ping rules: nothing behind the camera, inside our own body, or behind a wall is picked")
{
	const auto ray = Ahead();
	CHECK(!PingRules::ChooseTarget(ray, std::nullopt, std::vector{ Actor(1, 0.0f, -500.0f) }));
	CHECK(!PingRules::ChooseTarget(ray, std::nullopt, std::vector{ Actor(1, 0.0f, 10.0f) }));
	// A wall 500 units ahead: the raider at 1000 is behind it, the one at 480 in front of it.
	CHECK(!PingRules::ChooseTarget(ray, 500.0f, std::vector{ Actor(1, 0.0f, 1000.0f) }));
	CHECK(PingRules::ChooseTarget(ray, 500.0f, std::vector{ Actor(1, 0.0f, 480.0f) }));
	// The ray hit the raider's own collision, short of its middle: still the raider.
	CHECK(PingRules::ChooseTarget(ray, 1000.0f - PingRules::ACTOR_RADIUS, std::vector{ Actor(1, 0.0f, 1000.0f) }));
}

TEST("ping rules: of two on the crosshair the one it is nearer the middle of wins, and at a tie the nearer")
{
	const auto ray = Ahead();
	// An item lying just past a raider's feet, both on the crosshair: the raider's middle is on it.
	const std::vector<Candidate> both{ Item(10, 12.0f, 1500.0f, 120.0f), Actor(20, 0.0f, 1500.0f) };
	const auto                   chosen = PingRules::ChooseTarget(ray, std::nullopt, both);
	REQUIRE(chosen);
	CHECK(both[*chosen].id == 20);
	// Two raiders in a line: the front one.
	const std::vector<Candidate> line{ Actor(1, 0.0f, 2000.0f), Actor(2, 0.0f, 900.0f) };
	const auto                   front = PingRules::ChooseTarget(ray, std::nullopt, line);
	REQUIRE(front);
	CHECK(line[*front].id == 2);
	// Nonsense candidates are passed over.
	const std::vector<Candidate> bad{ Actor(1, NaN, 1000.0f), { PingKind::kActor, 2, { 0.0f, 1000.0f, 120.0f }, 0.0f }, Actor(3, 0.0f, 1200.0f) };
	const auto                   good = PingRules::ChooseTarget(ray, std::nullopt, bad);
	REQUIRE(good);
	CHECK(bad[*good].id == 3);
}

TEST("ping rules: with no target the ping goes on the surface, else where the player stands")
{
	const auto ray = Ahead();
	const Vec  standing{ 5.0f, 6.0f, 0.0f };
	const auto wall = PingRules::SpotFor(ray, 800.0f, standing);
	CHECK(std::abs(wall.y - 800.0f) < 0.01f);
	CHECK(std::abs(wall.z - 120.0f) < 0.01f);
	const auto sky = PingRules::SpotFor(ray, std::nullopt, standing);
	CHECK(sky.x == 5.0f);
	CHECK(sky.y == 6.0f);
	const auto beyond = PingRules::SpotFor(ray, PingRules::REACH * 2.0f, standing);
	CHECK(beyond.x == 5.0f);
	CHECK(PingRules::SpotFor(ray, NaN, standing).y == 6.0f);
}

TEST("ping rules: distances in whole meters")
{
	CHECK(PingRules::Meters(0.0f) == 0);
	CHECK(PingRules::Meters(70.0f) == 1);     // 70 units is a meter
	CHECK(PingRules::Meters(1610.0f) == 23);
	CHECK(PingRules::Meters(NaN) == 0);
}
