#include "Test.h"
#include "Protocol.h"
#include "game/AimPoint.h"
#include "game/InertShot.h"

#include <array>
#include <cmath>
#include <limits>

TEST("InertShot: the copy of a projectile record keeps its motion and loses its blast, its pinning, its pickup and its switch")
{
	using namespace InertShot;
	constexpr std::uint32_t missile = 0x10000 | FLAG_EXPLOSION | 0x8;  // the missile motion, a blast, a muzzle flash
	CHECK(InertFlags(missile) == (0x10000 | 0x8));
	constexpr std::uint32_t shell = 0x20000 | FLAG_EXPLOSION | FLAG_EXPLOSION_ALT_TRIGGER | 0x10;  // lobbed, bounces
	CHECK(InertFlags(shell) == (0x20000 | 0x10));
	CHECK(InertFlags(0x40000 | FLAG_EXPLOSION) == 0x40000);  // a beam with a blast
	CHECK(InertFlags(FLAG_PINS_LIMBS | FLAG_CAN_PICKUP | FLAG_CAN_TURN_OFF | 0x80) == 0x80);
	CHECK(InertFlags(0) == 0);
	// The copy's record never goes off, whatever the original did.
	CHECK(!GoesOff(InertFlags(missile), false));
}

TEST("InertShot: a stand-in's or a copy's launch of a record that goes off is swapped, nothing else is")
{
	using namespace InertShot;
	CHECK(Swaps(true, 0x10000 | FLAG_EXPLOSION, true));   // a missile
	CHECK(Swaps(true, 0x10000, true));                    // a record naming an explosion without the flag
	CHECK(Swaps(true, 0x40000 | FLAG_EXPLOSION, false));  // a beam with the flag alone
	CHECK(!Swaps(true, 0x10000, false));                  // a bullet (held at the animation for a stand-in, fired for a copy)
	CHECK(!Swaps(true, 0x80000, false));                  // a flamer's stream
	CHECK(!Swaps(false, 0x10000 | FLAG_EXPLOSION, true));  // our player's or our NPC's missile: the engine's, watched and reported
	CHECK(!Swaps(false, 0, false));
}

TEST("InertShot: the hook goes in only where Projectile::Launch begins as 1.11.240 has it")
{
	using namespace InertShot;
	CHECK(EntryMatches(ENTRY_BYTES));
	std::array<std::uint8_t, 5> other{ 0x48, 0x89, 0x5C, 0x24, 0x08 };  // mov [rsp+8],rbx: another function's entry
	CHECK(!EntryMatches(other));
	std::array<std::uint8_t, 5> jumped{ 0xE9, 0x00, 0x00, 0x00, 0x00 };  // another plugin's jump already there
	CHECK(!EntryMatches(jumped));
	static_assert(LAUNCH_ID == 2236958);
}

TEST("InertShot: the continuation does what the jump overwrote, then jumps to the engine's code after it")
{
	using namespace InertShot;
	constexpr std::uintptr_t function = 0x140E421E0;
	constexpr std::uintptr_t stub = 0x140000100;
	const auto bytes = Continuation(stub, function);
	REQUIRE(bytes.has_value());
	CHECK(bytes->size() == CONTINUATION_SIZE);
	for (std::size_t i = 0; i < ENTRY_BYTES.size(); ++i) {
		CHECK((*bytes)[i] == ENTRY_BYTES[i]);
	}
	CHECK((*bytes)[5] == 0xE9);
	const std::uint32_t raw = (*bytes)[6] | (*bytes)[7] << 8 | (*bytes)[8] << 16 | (*bytes)[9] << 24;
	const auto rel = static_cast<std::int32_t>(raw);
	CHECK(stub + CONTINUATION_SIZE + rel == function + JUMP_SIZE);  // to the push rbp after the five bytes
	// A trampoline out of a jump's reach: none, so the hook is not installed.
	CHECK(!Continuation(0x7FF000000000, function).has_value());
	static_assert(TRAMPOLINE_NEEDED == 24);
}

TEST("InertShot: an inert shot is ended when it is stale, or settled in a cell that is no longer attached")
{
	using namespace InertShot;
	using namespace std::chrono_literals;
	CHECK(!Stale(0ms, true));
	CHECK(!Stale(3000ms, true));                 // a missile's flight
	CHECK(!Stale(MAX_LIFE, true));
	CHECK(Stale(MAX_LIFE + 1ms, true));          // a shell at rest for good
	CHECK(!Stale(500ms, false));                 // just launched: its cell may not have it yet
	CHECK(Stale(SETTLE + 1ms, false));           // its cell unloaded under it
}

TEST("InertShot: an unowned launch of a relayed explosion's spawn record is swapped for a moment after the explosion is placed")
{
	using namespace InertShot;
	using namespace std::chrono_literals;
	CHECK(SwapsSpawned(false, true, 0ms));
	CHECK(SwapsSpawned(false, true, SPAWN_WINDOW));
	CHECK(!SwapsSpawned(false, true, SPAWN_WINDOW + 1ms));  // long after: the game's own, whatever it is
	CHECK(!SwapsSpawned(false, true, -1ms));
	CHECK(!SwapsSpawned(true, true, 0ms));   // our own MIRV's children name our player: the engine's, reported
	CHECK(!SwapsSpawned(false, false, 0ms));  // an unowned launch of some other record: a trap's, a script's
}

TEST("InertShot: a swapped launch is aimed by the puppet's heading and its owner's pitch, the pitch turned over for the game's angle x")
{
	using namespace InertShot;
	const auto level = LaunchAngles(1.5f, 0.0f);
	CHECK(level.z == 1.5f);
	CHECK(level.x == 0.0f);
	const auto up = LaunchAngles(0.0f, 30.0f);
	CHECK(up.x < -0.52f && up.x > -0.53f);  // 30 degrees up is a negative angle x (looking down is positive)
	const auto down = LaunchAngles(3.0f, -45.0f);
	CHECK(down.z == 3.0f);
	CHECK(down.x > 0.78f && down.x < 0.79f);
}

TEST("InertShot: every launch of a stand-in's is given its friend's aim, a copy's only when it is swapped")
{
	using namespace InertShot;
	CHECK(Aims(true, false));   // a flamer's stream, a harpoon: the engine's own record, sent where the friend aimed
	CHECK(Aims(true, true));    // a stand-in's missile, launched inert
	CHECK(Aims(false, true));   // a copy's missile, launched inert
	CHECK(!Aims(false, false));  // a copy's own plain shot, our player's, our NPCs': the engine's as before
}

TEST("InertShot: a friend's look comes back as the same launch angle x, through the pitch sent and the stand-in's aim")
{
	using namespace InertShot;
	// The sender packs its angle x as whole degrees up (Session, AimPoint::PitchFromLooking); the launch hook turns that
	// pitch back into the angle x the shot leaves with. Within half a degree of the look, the sign kept: a friend looking
	// down shoots down, not up at a player standing above them.
	for (const float look : { -1.2f, -0.5f, -0.01f, 0.0f, 0.2f, 0.9f, 1.4f }) {
		const auto pitch = static_cast<float>(Protocol::PackPitch(AimPoint::PitchFromLooking(look)));
		const auto angles = LaunchAngles(2.0f, pitch);
		CHECK(std::abs(angles.x - look) <= 0.5f / AimPoint::TO_DEGREES + 1e-5f);
		CHECK(angles.z == 2.0f);
	}
}

TEST("InertShot: how far the engine's aim of a launch was from the friend's, in degrees")
{
	using namespace InertShot;
	constexpr float PI = 3.14159265358979f;
	CHECK(DegreesBetween({ .z = 1.0f, .x = 0.2f }, { .z = 1.0f, .x = 0.2f }) < 0.1f);
	CHECK(std::abs(DegreesBetween({ .z = 0.0f, .x = 0.0f }, { .z = PI / 2.0f, .x = 0.0f }) - 90.0f) < 0.1f);  // a quarter turn
	CHECK(std::abs(DegreesBetween({ .z = 0.0f, .x = 0.0f }, { .z = PI, .x = 0.0f }) - 180.0f) < 0.1f);        // straight behind
	CHECK(std::abs(DegreesBetween({ .z = 0.0f, .x = 0.0f }, { .z = 0.0f, .x = -PI / 6.0f }) - 30.0f) < 0.1f); // 30 degrees up
	// A heading a whole turn round is the same direction; straight down is the same whatever the heading.
	CHECK(DegreesBetween({ .z = 0.5f, .x = 0.0f }, { .z = 0.5f + 2.0f * PI, .x = 0.0f }) < 0.1f);
	CHECK(DegreesBetween({ .z = 0.0f, .x = PI / 2.0f }, { .z = 2.0f, .x = PI / 2.0f }) < 0.1f);
	const float NaN = std::numeric_limits<float>::quiet_NaN();
	CHECK(DegreesBetween({ .z = NaN, .x = 0.0f }, { .z = 0.0f, .x = 0.0f }) == 180.0f);
	CHECK(DegreesBetween({ .z = 0.0f, .x = 0.0f }, { .z = 0.0f, .x = std::numeric_limits<float>::infinity() }) == 180.0f);
}
