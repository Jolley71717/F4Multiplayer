#include "Test.h"
#include "game/InertShot.h"

#include <array>

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
	CHECK(LAUNCH_ID == 2236958);
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
	CHECK(TRAMPOLINE_NEEDED == 24);
}
