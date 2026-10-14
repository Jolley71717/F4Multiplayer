#include "ServerFixture.h"
#include "Test.h"

#include "Protocol.h"
#include "game/AimPoint.h"

#include <cmath>
#include <limits>

// Where a friend's stand-in points its gun (src/game/AimPoint.h): the owner's look pitch goes out in their
// PlayerState, through the real server to a friend, and comes back as the point the friend's game gives the
// stand-in's weapon aim, the way Session and Puppets do it.
namespace
{
	using namespace TestServer;

	constexpr float NaN = std::numeric_limits<float>::quiet_NaN();

	float AngleDifference(float a_a, float a_b)
	{
		const float d = std::fmod(std::abs(a_a - a_b), 2.0f * AimPoint::PI);
		return (std::min)(d, 2.0f * AimPoint::PI - d);
	}

	float Distance(const AimPoint::Point& a_a, const AimPoint::Point& a_b)
	{
		return std::sqrt((a_a.x - a_b.x) * (a_a.x - a_b.x) + (a_a.y - a_b.y) * (a_a.y - a_b.y) + (a_a.z - a_b.z) * (a_a.z - a_b.z));
	}
}

TEST("aim point: a friend looking up or down has their stand-in aimed that way, through the server")
{
	Fixture f;
	const auto alice = f.Join(1, Fixture::MakeHello("Alice"));
	REQUIRE(alice);
	REQUIRE(f.Join(2, Fixture::MakeHello("Bob")));

	// Alice's angle x (radians, looking down is positive) and her heading; the bot's --pitch 60 and -60 too.
	struct Look
	{
		float angleX;
		float heading;
	};
	std::uint32_t sequence = 0;
	for (const auto look : { Look{ -0.6f, 1.2f }, Look{ 0.45f, 4.0f }, Look{ 0.0f, 0.0f }, Look{ -1.0472f, 2.5f }, Look{ 1.0472f, 6.0f } }) {
		Protocol::PlayerState state;
		state.sequence = ++sequence;
		state.x = 1000.0f;
		state.y = -2000.0f;
		state.z = 300.0f;
		state.heading = look.heading;
		state.pitch = Protocol::PackPitch(AimPoint::PitchFromLooking(look.angleX));
		f.fake->Take(2);
		f.fake->Deliver(1, Protocol::Encode(state));
		const auto packet = f.fake->WaitFor(2, Type(MT::kPlayerStates));
		REQUIRE(packet);
		const auto batch = Protocol::DecodePlayerStates(*packet);
		REQUIRE(batch);
		REQUIRE(batch->players.size() == 1);
		CHECK(batch->players[0].playerId == alice->playerId);
		const auto& got = batch->players[0].state;
		CHECK(got.sequence == sequence);

		// Bob's game: the stand-in stands a little off the sent place (it is interpolated) and is given the point.
		const AimPoint::Point standIn{ got.x + 12.0f, got.y - 7.0f, got.z + 1.0f };
		const auto            point = AimPoint::ForPuppet(true, standIn, got.heading, static_cast<float>(got.pitch));
		REQUIRE(point);
		const float wanted = -look.angleX * AimPoint::TO_DEGREES;
		CHECK(std::abs(AimPoint::PitchTo(standIn, *point) - wanted) < 0.6f);
		CHECK(AngleDifference(AimPoint::HeadingTo(standIn, *point), look.heading) < 0.001f);
		CHECK(std::abs(Distance(standIn, *point) - AimPoint::REACH) < 1.0f);
		// Looking up puts the point above the stand-in, down below it.
		CHECK((wanted > 1.0f) == (point->z > standIn.z + 1.0f));
		CHECK((wanted < -1.0f) == (point->z < standIn.z - 1.0f));
	}
}

TEST("aim point: the direction is the engine's: heading 0 along +y, a quarter turn along +x, up along +z")
{
	const AimPoint::Point at{ 10.0f, 20.0f, 30.0f };
	const auto            ahead = AimPoint::Along(at, 0.0f, 0.0f);
	CHECK(std::abs(ahead.x - 10.0f) < 0.01f);
	CHECK(std::abs(ahead.y - (20.0f + AimPoint::REACH)) < 0.01f);
	CHECK(std::abs(ahead.z - 30.0f) < 0.01f);
	const auto right = AimPoint::Along(at, AimPoint::PI / 2.0f, 0.0f);
	CHECK(std::abs(right.x - (10.0f + AimPoint::REACH)) < 0.1f);
	CHECK(std::abs(right.y - 20.0f) < 0.1f);
	const auto up = AimPoint::Along(at, 0.0f, 45.0f);
	CHECK(std::abs((up.z - 30.0f) - (up.y - 20.0f)) < 0.1f);
	CHECK(up.z > 30.0f);

	// Looking down is a positive angle x and a negative pitch, as `turnto` and `player.setangle x` have it.
	CHECK(AimPoint::PitchFromLooking(0.5f) < 0.0f);
	CHECK(std::abs(AimPoint::PitchFromLooking(-AimPoint::PI / 6.0f) - 30.0f) < 0.01f);

	// Straight up still says which way the stand-in faces; nonsense counts as level and ahead.
	const auto straightUp = AimPoint::Along(at, AimPoint::PI / 2.0f, 90.0f);
	CHECK(straightUp.x > at.x);
	CHECK(std::abs(AimPoint::HeadingTo(at, straightUp) - AimPoint::PI / 2.0f) < 0.001f);
	const auto nonsense = AimPoint::Along(at, NaN, NaN);
	CHECK(std::abs(nonsense.y - ahead.y) < 0.01f && std::abs(nonsense.z - ahead.z) < 0.01f);
	CHECK(Protocol::PackPitch(AimPoint::PitchFromLooking(NaN)) == 0);
	CHECK(Protocol::PackPitch(AimPoint::PitchFromLooking(-3.0f)) == 90);
}

TEST("aim point: only a friend's stand-in in a known place is given one")
{
	const AimPoint::Point at{ 1.0f, 2.0f, 3.0f };
	CHECK(AimPoint::ForPuppet(true, at, 0.3f, -20.0f).has_value());
	// A mirrored NPC's aim belongs to its own game: no point from us.
	CHECK(!AimPoint::ForPuppet(false, at, 0.3f, -20.0f).has_value());
	CHECK(!AimPoint::ForPuppet(true, { NaN, 2.0f, 3.0f }, 0.3f, -20.0f).has_value());
	CHECK(!AimPoint::ForPuppet(true, { 1.0f, 2.0f, std::numeric_limits<float>::infinity() }, 0.3f, -20.0f).has_value());
}
