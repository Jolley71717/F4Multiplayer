#include "Test.h"

#include "game/VatsTargetRules.h"

#include <algorithm>

// VATS never picks a friend's stand-in (game/VatsTargetRules.h): who is left out, and the four call sites checked
// before any is patched.
namespace
{
	// The five bytes of a near call at a_at to a_target.
	std::array<std::uint8_t, 5> CallTo(std::uintptr_t a_at, std::uintptr_t a_target)
	{
		const auto rel = static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::intptr_t>(a_target) - static_cast<std::intptr_t>(a_at + 5)));
		return { 0xE8, static_cast<std::uint8_t>(rel), static_cast<std::uint8_t>(rel >> 8), static_cast<std::uint8_t>(rel >> 16), static_cast<std::uint8_t>(rel >> 24) };
	}

	constexpr std::uintptr_t BASE = 0x7FF700000000;
	constexpr std::uintptr_t TARGET = BASE + 0xB16220;
	// The four sites as 1.11.240 has them: each function's start plus VatsTargetRules::SITES's offset.
	constexpr std::array<std::uintptr_t, 4> FUNCTIONS{ BASE + 0xB164B0, BASE + 0xB16380, BASE + 0xB154F0, BASE + 0xB15DC0 };

	std::array<std::uintptr_t, 4> Sites()
	{
		std::array<std::uintptr_t, 4> at{};
		for (std::size_t i = 0; i < at.size(); ++i) {
			at[i] = FUNCTIONS[i] + VatsTargetRules::SITES[i].offset;
		}
		return at;
	}

	std::array<std::array<std::uint8_t, 5>, 4> GameBytes()
	{
		const auto                                  at = Sites();
		std::array<std::array<std::uint8_t, 5>, 4> bytes{};
		for (std::size_t i = 0; i < at.size(); ++i) {
			bytes[i] = CallTo(at[i], TARGET);
		}
		return bytes;
	}
}

TEST("vats targets: a friend's stand-in is left out only with bVatsSkipsFriends on")
{
	using VatsTargetRules::Judge;
	using VatsTargetRules::Verdict;
	CHECK(Judge(true, true) == Verdict::kSkip);
	// Anyone else (an NPC, a mirrored NPC, a companion) is the engine's call, as without the mod.
	CHECK(Judge(true, false) == Verdict::kEngine);
	// Off: VATS treats a stand-in as any NPC, as 0.13.0 and earlier did.
	CHECK(Judge(false, true) == Verdict::kEngine);
	CHECK(Judge(false, false) == Verdict::kEngine);
}

TEST("vats targets: the four calls of the target check are where 1.11.240 has them")
{
	// The call sites read from the game: the gather +B1663F, the reference's check +B163E9, the menu button +B155BB and
	// the list's refill +B15E18.
	const auto at = Sites();
	CHECK(at[0] == BASE + 0xB1663F);
	CHECK(at[1] == BASE + 0xB163E9);
	CHECK(at[2] == BASE + 0xB155BB);
	CHECK(at[3] == BASE + 0xB15E18);
	static_assert(VatsTargetRules::IS_VALID_TARGET == 2224729);
	// One ID per site, none twice (each is its own function).
	std::array<std::uint64_t, 4> ids{};
	for (std::size_t i = 0; i < ids.size(); ++i) {
		ids[i] = VatsTargetRules::SITES[i].id;
	}
	std::ranges::sort(ids);
	CHECK(std::ranges::adjacent_find(ids) == ids.end());
	CHECK(ids == (std::array<std::uint64_t, 4>{ 2224716, 2224725, 2224730, 2224731 }));
}

TEST("vats targets: all four sites call the check, or none is patched")
{
	const auto at = Sites();
	auto       bytes = GameBytes();
	CHECK(VatsTargetRules::Check(bytes, at, TARGET).All());
	CHECK(VatsTargetRules::Check(bytes, at, TARGET).calling == 4);

	// Another plugin's patch at one site (a call elsewhere): not all.
	bytes[1] = CallTo(at[1], BASE + 0x1000);
	CHECK(!VatsTargetRules::Check(bytes, at, TARGET).All());
	CHECK(VatsTargetRules::Check(bytes, at, TARGET).calling == 3);

	// A jump written over a call (a detour's 14-byte branch begins FF 25): not a call.
	bytes = GameBytes();
	bytes[3] = { 0xFF, 0x25, 0x00, 0x00, 0x00 };
	CHECK(!VatsTargetRules::Check(bytes, at, TARGET).All());

	// Our own patch read back (each call now goes to the trampoline): not 1.11.240's, so a second install would not
	// patch again.
	bytes = GameBytes();
	for (std::size_t i = 0; i < at.size(); ++i) {
		bytes[i] = CallTo(at[i], BASE + 0x3000000);
	}
	CHECK(VatsTargetRules::Check(bytes, at, TARGET).calling == 0);

	// A caller's mistake (fewer byte reads than sites): nothing at all.
	const std::array<std::array<std::uint8_t, 5>, 3> three{ bytes[0], bytes[1], bytes[2] };
	CHECK(VatsTargetRules::Check(three, at, TARGET).total == 0);
	CHECK(!VatsTargetRules::Check(three, at, TARGET).All());
	// And no sites at all is never "all".
	CHECK(!VatsTargetRules::Check(std::span<const std::array<std::uint8_t, 5>>{}, std::span<const std::uintptr_t>{}, TARGET).All());
}
