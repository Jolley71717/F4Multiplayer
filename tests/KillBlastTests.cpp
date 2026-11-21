// Which torsos the console's kill may blast (KillBlast::Judge, as KillBlasts' hook on the command's test asks it): the
// command reads the torso part's limb value unchecked, so a body whose torso always explodes but names no value (Nuka-World's
// flying ant swarm) crashed every game that landed its death with that command.

#include "Test.h"

#include "game/KillBlast.h"
#include "game/TextureFlush.h"

#include <array>
#include <cstdint>
#include <initializer_list>

namespace
{
	using KillBlast::Torso;
	using KillBlast::Verdict;

	// The torso parts of the game's own body part data (their BPND flags and whether they name an actor value), as read
	// from the esm files.
	constexpr Torso SWARM{ .flags = 0x5C, .value = false };    // DLC04_SwarmBodyPartData, Swrm_BN_C_Torso
	constexpr Torso BLOATFLY{ .flags = 0x4C, .value = true };  // BloatflyBodyPartData, Pelvis
	constexpr Torso TURRET{ .flags = 0x4A, .value = true };    // TurretTripodBodyPartData, BODY (the bubble's and the defender's too)
	constexpr Torso ALIEN{ .flags = 0x48, .value = true };     // DLC04_AnimatronicAlienBodyPartData, COM

	// The part's fields and flags, where the command reads them.
	static_assert(KillBlast::PART_VALUE == 0x100);  // mov rsi,[rsi+100h] at +5C053D
	static_assert(KillBlast::PART_FLAGS == 0x114);  // movzx ecx,byte ptr [rax+114h] at +5C0514
	static_assert(KillBlast::EXPLODABLE == 0x08 && KillBlast::ABSOLUTE_CHANCE == 0x40);  // test cl,8 and test cl,40h
}

TEST("kill blast: the flying ant swarm's torso (always explodes, no limb value) is not blasted: reading its value crashed the game")
{
	CHECK(KillBlast::Blasts(SWARM));
	CHECK(KillBlast::Crashes(SWARM));
	CHECK(KillBlast::Judge(SWARM) == Verdict::kSkip);
}

TEST("kill blast: a torso that always explodes and names its limb value is the engine's to blast, as before")
{
	for (const auto& torso : { BLOATFLY, TURRET, ALIEN }) {
		CHECK(KillBlast::Blasts(torso));
		CHECK(!KillBlast::Crashes(torso));
		CHECK(KillBlast::Judge(torso) == Verdict::kEngine);
	}
}

TEST("kill blast: only both flags together make the command blast; a torso with one of them never reaches the value")
{
	// With no limb value either: the command stops at its flag test, so nothing crashes and nothing is skipped.
	for (const std::uint8_t flags : { std::uint8_t{ 0x00 }, KillBlast::EXPLODABLE, KillBlast::ABSOLUTE_CHANCE, std::uint8_t{ 0x14 }, std::uint8_t{ 0xB7 } }) {
		const Torso torso{ .flags = flags, .value = false };
		CHECK(!KillBlast::Blasts(torso));
		CHECK(!KillBlast::Crashes(torso));
		CHECK(KillBlast::Judge(torso) == Verdict::kEngine);
	}
	// Every other flag set beside the two changes nothing.
	CHECK(KillBlast::Judge({ .flags = 0xFF, .value = false }) == Verdict::kSkip);
	CHECK(KillBlast::Judge({ .flags = 0xFF, .value = true }) == Verdict::kEngine);
}

TEST("kill blast: a body with no torso part is left to the engine (the command stops there itself)")
{
	CHECK(!KillBlast::Blasts({ .present = false, .flags = 0x5C, .value = false }));
	CHECK(KillBlast::Judge({ .present = false, .flags = 0x5C, .value = false }) == Verdict::kEngine);
	CHECK(KillBlast::Judge({ .present = false }) == Verdict::kEngine);
}

TEST("kill blast: without the hook, the session's kill is left out only for a body the command would crash on")
{
	CHECK(KillBlast::Refused(false, SWARM));
	CHECK(!KillBlast::Refused(true, SWARM));  // the hook mends it: killed as anything else is
	for (const auto& torso : { BLOATFLY, TURRET, ALIEN, Torso{}, Torso{ .present = false }, Torso{ .flags = KillBlast::EXPLODABLE, .value = false } }) {
		CHECK(!KillBlast::Refused(false, torso));
		CHECK(!KillBlast::Refused(true, torso));
	}
}

TEST("kill blast: the command's call before the blast, as 1.11.240 has it, is read as a call of the AI-process test")
{
	// +5C03D0 + 0x160 = +5C0530: E8 FB 54 71 00, a call of +CD5A30 (the test of the actor's AI process).
	constexpr std::uintptr_t BASE = 0x140000000;
	constexpr std::uintptr_t SITE = BASE + 0x5C03D0 + KillBlast::TEST_CALL;
	static_assert(SITE == BASE + 0x5C0530);
	constexpr std::array<std::uint8_t, 5> CALL{ 0xE8, 0xFB, 0x54, 0x71, 0x00 };
	CHECK(TextureFlush::CallTarget(CALL, SITE) == BASE + 0xCD5A30);
	// A jump there (another plugin's hook) is not a call: the hook stays out, and Refused takes over.
	constexpr std::array<std::uint8_t, 5> JUMP{ 0xE9, 0xFB, 0x54, 0x71, 0x00 };
	CHECK(!TextureFlush::CallTarget(JUMP, SITE));
}
