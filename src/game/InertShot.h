#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>

// A friend's or a mirrored NPC's shot that would go off (a missile, a mini nuke, a grenade launcher's shell, a
// cannonball, an Explosive round, a beam with a blast) flies in our game for everyone to see, and goes off nowhere: it
// is launched from a copy of its projectile record with no explosion, no force and nothing to pin or pick up, so the
// engine never has a blast to set off. The blast comes once, as the shooter's game's report (docs/DESIGN.md, "Who sets
// off an explosion"). The decisions, free of the game, are here (unit tested); InertShots.cpp puts them to the engine.
//
// Where: the engine makes every projectile in Projectile::Launch (Address Library ID 2236958, +E421E0 in 1.11.240),
// which takes a ProjectileLaunchData (the projectile record, the shooter, the gun, the ammunition, the aim) and builds
// the projectile class the record's motion type says, each class's constructor reading the record (its explosion, its
// speed, its model). The whole function is ours to see first: a jump at its entry to InertShots' hook, which swaps the
// record for the inert copy when the shooter is a stand-in or a copy and the record goes off, then goes on to the
// engine's own code through a few bytes of ours (Continuation) that do what the jump overwrote. Nothing of the
// projectile exists before the swap, so no frame is left for it to land in (a point-blank missile, a beam) before it
// was made harmless: that was the double blast of defusing it a frame after launch.
namespace InertShot
{
	// Projectile::Launch, and the bytes it begins with: mov [rsp+8],rcx (its first argument spilled), one instruction of
	// the jump's five bytes, moved whole into the continuation. The hook goes in only where the game has these bytes.
	inline constexpr std::uint32_t LAUNCH_ID = 2236958;
	inline constexpr std::array<std::uint8_t, 5> ENTRY_BYTES{ 0x48, 0x89, 0x4C, 0x24, 0x08 };
	inline constexpr std::size_t JUMP_SIZE = 5;

	// What of the projectile record (BGSProjectile::BGSProjectileFlags) the copy loses: its blast (and the alternate
	// trigger of one), pinning limbs (a harpoon's, a railway spike's), being picked up again, and a mine's switch.
	inline constexpr std::uint32_t FLAG_EXPLOSION = 0x2;
	inline constexpr std::uint32_t FLAG_EXPLOSION_ALT_TRIGGER = 0x4;
	inline constexpr std::uint32_t FLAG_CAN_TURN_OFF = 0x20;
	inline constexpr std::uint32_t FLAG_CAN_PICKUP = 0x40;
	inline constexpr std::uint32_t FLAG_PINS_LIMBS = 0x100;
	inline constexpr std::uint32_t FLAGS_TAKEN = FLAG_EXPLOSION | FLAG_EXPLOSION_ALT_TRIGGER | FLAG_CAN_TURN_OFF | FLAG_CAN_PICKUP | FLAG_PINS_LIMBS;

	constexpr std::uint32_t InertFlags(std::uint32_t a_flags)
	{
		return a_flags & ~FLAGS_TAKEN;
	}

	// Whether a projectile record's shot goes off: it names an explosion, or has the explosion flag (the same test
	// WeaponFire::ShotGoesOff makes of the gun's record, the shooter's game watching for the blast by it).
	constexpr bool GoesOff(std::uint32_t a_flags, bool a_namesExplosion)
	{
		return a_namesExplosion || (a_flags & FLAG_EXPLOSION) != 0;
	}

	// Whether a launch is swapped: a stand-in's or a copy's (a_puppet), of a record that goes off. Anything else is the
	// engine's as before: our player's and our NPCs' shots, a flamer's stream from a stand-in (hurts nobody, Puppets'
	// zero damage multiplier), a trap's.
	constexpr bool Swaps(bool a_puppet, std::uint32_t a_flags, bool a_namesExplosion)
	{
		return a_puppet && GoesOff(a_flags, a_namesExplosion);
	}

	// A rel32 from the end of an instruction at a_next to a_to, when it reaches (2 GB either way).
	constexpr std::optional<std::int32_t> Rel32(std::uintptr_t a_next, std::uintptr_t a_to)
	{
		const auto delta = static_cast<std::int64_t>(a_to) - static_cast<std::int64_t>(a_next);
		if (delta < INT32_MIN || delta > INT32_MAX) {
			return std::nullopt;
		}
		return static_cast<std::int32_t>(delta);
	}

	// Whether a_bytes, read at the function's entry, are Projectile::Launch's as 1.11.240 has it. Anything else (another
	// version, another plugin's hook) is left as it is, and the shots stay held.
	constexpr bool EntryMatches(std::span<const std::uint8_t, ENTRY_BYTES.size()> a_bytes)
	{
		for (std::size_t i = 0; i < ENTRY_BYTES.size(); ++i) {
			if (a_bytes[i] != ENTRY_BYTES[i]) {
				return false;
			}
		}
		return true;
	}

	// The continuation at a_stub: the entry's own instruction, then a jump to the engine's code after the five bytes
	// the hook's jump took (a_function + JUMP_SIZE). None when the jump doesn't reach.
	inline constexpr std::size_t CONTINUATION_SIZE = ENTRY_BYTES.size() + 5;

	constexpr std::optional<std::array<std::uint8_t, CONTINUATION_SIZE>> Continuation(std::uintptr_t a_stub, std::uintptr_t a_function)
	{
		const auto rel = Rel32(a_stub + CONTINUATION_SIZE, a_function + JUMP_SIZE);
		if (!rel) {
			return std::nullopt;
		}
		std::array<std::uint8_t, CONTINUATION_SIZE> bytes{};
		for (std::size_t i = 0; i < ENTRY_BYTES.size(); ++i) {
			bytes[i] = ENTRY_BYTES[i];
		}
		const auto raw = static_cast<std::uint32_t>(*rel);
		bytes[ENTRY_BYTES.size()] = 0xE9;
		bytes[ENTRY_BYTES.size() + 1] = static_cast<std::uint8_t>(raw);
		bytes[ENTRY_BYTES.size() + 2] = static_cast<std::uint8_t>(raw >> 8);
		bytes[ENTRY_BYTES.size() + 3] = static_cast<std::uint8_t>(raw >> 16);
		bytes[ENTRY_BYTES.size() + 4] = static_cast<std::uint8_t>(raw >> 24);
		return bytes;
	}

	// What the trampoline is asked for: the continuation, and the 14-byte branch the jump at the entry goes through.
	inline constexpr std::size_t BRANCH_STUB = 14;
	inline constexpr std::size_t TRAMPOLINE_NEEDED = CONTINUATION_SIZE + BRANCH_STUB;
}
