#include "Test.h"
#include "Protocol.h"
#include "game/InertShot.h"
#include "game/ShotAim.h"

#include <chrono>
#include <cmath>
#include <limits>
#include <vector>

namespace
{
	constexpr float PI = 3.14159265358979f;

	// Projectile records' flags: a bullet (the missile motion), a laser's beam, a lobbed shell, a flamer's stream.
	constexpr std::uint32_t BULLET = 0x10000;
	constexpr std::uint32_t BEAM = 0x40000;
	constexpr std::uint32_t LOBBED = 0x20000;
	constexpr std::uint32_t STREAM = 0x80000;
	constexpr std::uint32_t HITSCAN = 0x1;
}

TEST("ShotAim: the aim of a shot leaving now is the heading within one turn and the look in whole degrees, up positive")
{
	using namespace ShotAim;
	const auto level = Snapshot(1.5f, 0.0f);
	CHECK(Known(level));
	CHECK(level.heading == 1.5f);
	CHECK(level.pitch == 0);

	// The game's look angle is positive downwards.
	CHECK(Snapshot(0.0f, 30.0f * PI / 180.0f).pitch == -30);
	CHECK(Snapshot(0.0f, -45.0f * PI / 180.0f).pitch == 45);
	CHECK(Snapshot(0.0f, -3.0f).pitch == 90);  // held to straight up
	CHECK(Snapshot(0.0f, 3.0f).pitch == -90);

	// A heading the game let run past a turn, or below zero, is brought within one: the decoder refuses more.
	const auto negative = Snapshot(-0.5f, 0.0f);
	CHECK(std::abs(negative.heading - (2.0f * PI - 0.5f)) < 1e-4f);
	const auto wound = Snapshot(4.0f * PI + 1.0f, 0.0f);
	CHECK(std::abs(wound.heading - 1.0f) < 1e-4f);
	for (const float z : { 0.0f, -0.0f, 6.2831f, -6.2831f, -1e-9f, 100.0f, -100.0f, 1e6f }) {
		const auto aim = Snapshot(z, 0.2f);
		CHECK(Known(aim));
		CHECK(aim.heading >= 0.0f);
		CHECK(aim.heading <= Protocol::MAX_SHOT_HEADING);
		// ... and so it goes over the wire as it is.
		const auto back = Protocol::DecodeShot(Protocol::Encode(Protocol::Shot{ 1, 0, Protocol::ShotAction::kShot, 0, aim.heading, aim.pitch, 0 }, Protocol::MessageType::kReportShot));
		REQUIRE(back);
		CHECK(back->heading == aim.heading);
		CHECK(back->pitch == aim.pitch);
	}

	// Angles that are no numbers: no aim, and the receiver goes by the stand-in's own.
	const auto nan = std::numeric_limits<float>::quiet_NaN();
	const auto inf = std::numeric_limits<float>::infinity();
	CHECK((Snapshot(nan, 0.0f) == Sent{}));
	CHECK((Snapshot(0.0f, nan) == Sent{}));
	CHECK((Snapshot(inf, 0.0f) == Sent{}));
	CHECK((Snapshot(0.0f, -inf) == Sent{}));
	CHECK(!Known(Sent{}));
	CHECK(Sent{}.pitch == Protocol::NO_AIM);
}

TEST("ShotAim: each shot sent gets its own aim, the newest of those caught, and a swing none")
{
	using namespace ShotAim;
	const Sent a{ .heading = 1.0f, .pitch = 10 };
	const Sent b{ .heading = 2.0f, .pitch = -20 };
	const Sent c{ .heading = 3.0f, .pitch = 30 };
	const Sent none{};

	const std::vector<Sent> two{ a, b };
	CHECK((Pair(2, 0, two) == std::vector<Sent>{ a, b }));
	// The queue is one short (its newest not yet caught): that shot goes with no aim.
	const std::vector<Sent> one{ a };
	CHECK((Pair(2, 0, one) == std::vector<Sent>{ a, none }));
	// The queue is one long (a leftover at its front): the newest are the ones.
	const std::vector<Sent> three{ a, b, c };
	CHECK((Pair(2, 0, three) == std::vector<Sent>{ b, c }));
	CHECK((Pair(1, 0, three) == std::vector<Sent>{ c }));
	// Swings and punches sent as shots come after the gunshots, with no aim; a leftover aim is never theirs.
	CHECK((Pair(1, 2, one) == std::vector<Sent>{ a, none, none }));
	CHECK((Pair(0, 1, one) == std::vector<Sent>{ none }));
	CHECK((Pair(0, 2, three) == std::vector<Sent>{ none, none }));
	// Nothing caught at all (the hook missed): every shot still goes, with no aim.
	const std::vector<Sent> empty;
	CHECK((Pair(3, 0, empty) == std::vector<Sent>{ none, none, none }));
	CHECK(Pair(0, 0, empty).empty());
	CHECK(Pair(0, 0, three).empty());
}

TEST("ShotAim: a received aim becomes the launch's angles, and none stays none")
{
	using namespace ShotAim;
	const auto up = AnglesOf(Sent{ .heading = 1.0f, .pitch = 30 });
	REQUIRE(up);
	CHECK((*up == InertShot::LaunchAngles(1.0f, 30.0f)));
	CHECK(up->z == 1.0f);
	CHECK(std::abs(up->x + 30.0f * PI / 180.0f) < 1e-5f);  // up is a negative look angle
	const auto down = AnglesOf(Sent{ .heading = 0.0f, .pitch = -90 });
	REQUIRE(down);
	CHECK(std::abs(down->x - PI / 2.0f) < 1e-5f);

	// A snapshot comes back as the angles it was made from, to the pitch's whole degree.
	const auto round = AnglesOf(Snapshot(2.5f, 0.3f));
	REQUIRE(round);
	CHECK(round->z == 2.5f);
	CHECK(std::abs(round->x - 0.3f) < 0.5f * PI / 180.0f + 1e-5f);

	CHECK(!AnglesOf(Sent{}));
	CHECK(!AnglesOf(Sent{ .heading = 1.0f, .pitch = 100 }));
	CHECK(!AnglesOf(Sent{ .heading = 1.0f, .pitch = -100 }));
	CHECK(!AnglesOf(Sent{ .heading = std::numeric_limits<float>::quiet_NaN(), .pitch = 0 }));
}

TEST("ShotAim: a replayed shot's aim and record stand for half a second of its stand-in's launches")
{
	using namespace ShotAim;
	using namespace std::chrono_literals;
	CHECK(Fresh(0ms));
	CHECK(Fresh(16ms));
	CHECK(Fresh(EXPECT_FOR));
	CHECK(!Fresh(EXPECT_FOR + 1ms));  // a flamer's stream running on, a gun that winds up: the stand-in's own aim again
	CHECK(!Fresh(5s));
	CHECK(!Fresh(-1ms));
}

TEST("ShotAim: a swapped launch is made from the shooter's record only when that is another one that flies as a bullet, a missile or a beam")
{
	using namespace ShotAim;
	using InertShot::FLAG_EXPLOSION;
	// The stand-in's copy of the gun fires a plain bullet; its friend's, with their mods, something else that flies.
	CHECK(UsesSentRecord(true, false, BULLET, false, BEAM));
	CHECK(UsesSentRecord(true, false, BEAM, false, BULLET));
	CHECK(UsesSentRecord(true, false, 0, false, 0x40A0C));  // the game's own rounds
	// An Explosive legendary's round: it flies as itself (its inert copy has no blast), from the hook's launch ...
	CHECK(UsesSentRecord(true, false, BULLET, false, BULLET | FLAG_EXPLOSION));
	// ... and when the stand-in's own goes off too (a missile launcher with another missile).
	CHECK(UsesSentRecord(true, false, BULLET | FLAG_EXPLOSION, true, BULLET));
	CHECK(UsesSentRecord(true, false, BULLET, true, BULLET | FLAG_EXPLOSION));

	// Not known here (a mod's record this game lacks, or none sent), or the very record the stand-in's gun fires.
	CHECK(!UsesSentRecord(false, false, BULLET, false, BEAM));
	CHECK(!UsesSentRecord(true, true, BULLET, false, BULLET));
	// The stand-in's own launch is not one the hook swaps (a flamer's stream, a lobbed shell with no blast): the engine's.
	CHECK(!UsesSentRecord(true, false, STREAM, false, BULLET));
	CHECK(!UsesSentRecord(true, false, LOBBED, false, BULLET));
	// The shooter's is hitscan (its hit may be settled inside the launch), a stream or lobbed: the stand-in's own.
	CHECK(!UsesSentRecord(true, false, BULLET, false, BULLET | HITSCAN));
	CHECK(!UsesSentRecord(true, false, BULLET, false, BEAM | HITSCAN));
	CHECK(!UsesSentRecord(true, false, BULLET, false, STREAM));
	CHECK(!UsesSentRecord(true, false, BULLET, false, LOBBED));
	CHECK(!UsesSentRecord(true, false, BULLET | FLAG_EXPLOSION, true, LOBBED | FLAG_EXPLOSION));
}

TEST("ShotAim: our own launch of a trail uses the shooter's record only when it goes off nowhere")
{
	using namespace ShotAim;
	using InertShot::FLAG_EXPLOSION;
	CHECK(DirectUsesSentRecord(true, BEAM, false));
	CHECK(DirectUsesSentRecord(true, BULLET, false));
	CHECK(!DirectUsesSentRecord(true, BULLET | FLAG_EXPLOSION, false));  // an Explosive round: a direct launch flies a trail or nothing
	CHECK(!DirectUsesSentRecord(true, BULLET, true));                   // names an explosion without the flag
	CHECK(!DirectUsesSentRecord(false, BEAM, false));                   // not used by the hook's rule either
}
