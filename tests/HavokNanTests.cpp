#include "Test.h"

#include "game/HavokNan.h"

#include <limits>
#include <numbers>

using namespace std::chrono_literals;
using HavokNan::Action;
using HavokNan::Cleanup;
using HavokNan::Found;

namespace
{
	// The game's image base in the 2026-10-11 dump, and where the solver's function and its two loops were in it.
	constexpr std::uintptr_t BASE = 0x7FF7D4E20000;
	constexpr std::uintptr_t FUNCTION = BASE + 0x156F5C0;

	std::array<std::uint8_t, HavokNan::SITE_BYTES.size()> Site()
	{
		return HavokNan::SITE_BYTES;
	}

	// Where the rel32 at a_at of a_code (an instruction ending at a_next) goes, a_code sitting at a_base.
	std::uintptr_t JumpTarget(std::span<const std::uint8_t> a_code, std::uintptr_t a_base, std::size_t a_at, std::size_t a_next)
	{
		const std::uint32_t raw = static_cast<std::uint32_t>(a_code[a_at]) | static_cast<std::uint32_t>(a_code[a_at + 1]) << 8 | static_cast<std::uint32_t>(a_code[a_at + 2]) << 16 |
		                          static_cast<std::uint32_t>(a_code[a_at + 3]) << 24;
		return a_base + a_next + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(static_cast<std::int32_t>(raw)));
	}

	// The engine's loop as the code has it (+15718B1..+157193B), with a cap on its rounds so the test ends: brings a_angle
	// into [-pi, pi). The rounds it took, or none when the cap ran out.
	std::optional<int> EngineWrap(float a_angle, int a_cap)
	{
		constexpr float PI = std::numbers::pi_v<float>;
		constexpr float TWO_PI = 2.0f * PI;
		constexpr float INV_TWO_PI = 1.0f / TWO_PI;
		float           a = a_angle;
		for (int round = 1; round <= a_cap; ++round) {
			const float k = std::floor((PI + a) * INV_TWO_PI);
			a -= TWO_PI * k;
			const float clamped = (std::max)(-PI, a < PI ? a : PI);
			if (!(clamped != a)) {  // cmpneqss: true for a NaN
				return round;
			}
		}
		return std::nullopt;
	}
}

TEST("havok nan: the engine's angle loop ends for every number, and never for a NaN or an infinity")
{
	for (const float angle : { 0.0f, 1.0f, -1.0f, 3.14159f, -3.14159f, 3.1416f, -3.1416f, 6.2832f, -100.0f, 12345.6f, -1.0e7f, 3.0e9f, 1.0e30f, -3.0e38f,
		     std::numeric_limits<float>::max(), std::numeric_limits<float>::lowest(), std::numeric_limits<float>::denorm_min() }) {
		const auto rounds = EngineWrap(angle, 64);
		CHECK(rounds.has_value());
		CHECK(rounds.value_or(99) <= 8);
	}
	// A NaN, and an infinity (which the first round makes NaN): the engine goes round forever. The detour's test.
	CHECK(!EngineWrap(std::numeric_limits<float>::quiet_NaN(), 100000).has_value());
	CHECK(!EngineWrap(std::numeric_limits<float>::infinity(), 100000).has_value());
	CHECK(!EngineWrap(-std::numeric_limits<float>::infinity(), 100000).has_value());
}

TEST("havok nan: the exit test is found at both copies as 1.11.240 has it, with where its jumps go")
{
	const auto bytes = Site();
	// The first copy: bytes at +1571929, the loop's top at +15718B1, what follows at +157193C (read from the dump).
	const auto first = HavokNan::SiteMatches(bytes, FUNCTION + HavokNan::SITES[0]);
	REQUIRE(first.has_value());
	CHECK(first->loop == BASE + 0x15718B1);
	CHECK(first->after == BASE + 0x157193C);
	CHECK(FUNCTION + HavokNan::SITES[0] + HavokNan::PATCH_AT == BASE + 0x1571932);  // the bt
	// The second: +15725F8, top +1572580, after +157260B.
	const auto second = HavokNan::SiteMatches(bytes, FUNCTION + HavokNan::SITES[1]);
	REQUIRE(second.has_value());
	CHECK(second->loop == BASE + 0x1572580);
	CHECK(second->after == BASE + 0x157260B);
}

TEST("havok nan: other bytes at a site (another version, a patch already there) are left alone")
{
	auto bytes = Site();
	bytes[HavokNan::PATCH_AT] = 0xE9;  // our own jmp, or anybody's
	CHECK(!HavokNan::SiteMatches(bytes, FUNCTION + HavokNan::SITES[0]).has_value());
	bytes = Site();
	bytes[3] = 0xC4;  // the compare on another register
	CHECK(!HavokNan::SiteMatches(bytes, FUNCTION + HavokNan::SITES[0]).has_value());
	bytes = Site();
	bytes[15] = 0x70;  // the loop longer: another build
	CHECK(!HavokNan::SiteMatches(bytes, FUNCTION + HavokNan::SITES[0]).has_value());
}

TEST("havok nan: the detour keeps the game's test, ends the loop on a NaN, counts it, and jumps where the site did")
{
	const HavokNan::Targets targets{ .loop = BASE + 0x15718B1, .after = BASE + 0x157193C };
	const std::uintptr_t    detour = BASE - 0x10000;  // the trampoline sits next to the game
	const auto              code = HavokNan::Detour(detour, targets);
	REQUIRE(code.has_value());
	const auto& c = *code;
	// bt eax,0 and jnc +1A: in range, out as the game goes.
	CHECK(c[0] == 0x0F && c[1] == 0xBA && c[2] == 0xE0 && c[3] == 0x00);
	CHECK(c[4] == 0x73 && 0x06 + c[5] == 0x1A);
	// ucomiss xmm5,xmm5 and jp +10: the NaN test on the loop's angle.
	CHECK(c[6] == 0x0F && c[7] == 0x2E && c[8] == 0xED);
	CHECK(c[9] == 0x7A && 0x0B + c[10] == 0x10);
	// jmp loop: round again.
	CHECK(c[0x0B] == 0xE9);
	CHECK(JumpTarget(c, detour, 0x0C, 0x10) == targets.loop);
	// lock inc dword [rip+9]: the count, at +20.
	CHECK(c[0x10] == 0xF0 && c[0x11] == 0xFF && c[0x12] == 0x05);
	CHECK(JumpTarget(c, detour, 0x13, 0x17) == detour + HavokNan::DETOUR_COUNT);
	// xorps xmm5,xmm5, then jmp after.
	CHECK(c[0x17] == 0x0F && c[0x18] == 0x57 && c[0x19] == 0xED);
	CHECK(c[0x1A] == 0xE9);
	CHECK(JumpTarget(c, detour, 0x1B, 0x1F) == targets.after);
	// The count starts at nought.
	CHECK(c[0x20] == 0 && c[0x21] == 0 && c[0x22] == 0 && c[0x23] == 0);
}

TEST("havok nan: no detour and no patch when a jump can't reach (memory over 2 GB away)")
{
	const HavokNan::Targets targets{ .loop = BASE + 0x15718B1, .after = BASE + 0x157193C };
	CHECK(!HavokNan::Detour(BASE + 0x1'0000'0000, targets).has_value());
	CHECK(!HavokNan::Patch(BASE + 0x1571932, BASE + 0x1571932 + 0x8000'0000ull + 5).has_value());
	CHECK(HavokNan::Patch(BASE + 0x1571932, BASE + 0x1571932 + 0x7FFF'FFFFull + 5).has_value());
	CHECK(HavokNan::Rel32(0x1000, 0x1000 - 0x8000'0000ull) == std::optional<std::int32_t>{ INT32_MIN });
	CHECK(!HavokNan::Rel32(0x1000, 0x1000 - 0x8000'0001ull).has_value());
}

TEST("havok nan: the patch over bt and jb is a jmp to the detour and a nop")
{
	const std::uintptr_t at = BASE + 0x1571932;
	const std::uintptr_t detour = BASE - 0x10000;
	const auto           patch = HavokNan::Patch(at, detour);
	REQUIRE(patch.has_value());
	CHECK(patch->size() == HavokNan::PATCH_SIZE);
	CHECK((*patch)[0] == 0xE9);
	CHECK(JumpTarget(*patch, at, 1, 5) == detour);
	CHECK((*patch)[5] == 0x0F && (*patch)[6] == 0x1F && (*patch)[7] == 0x44 && (*patch)[8] == 0x00 && (*patch)[9] == 0x00);
}

TEST("havok nan: finite is every float a number")
{
	const std::array<float, 3> fine{ 1.0f, -75078.35f, 0.0f };
	CHECK(HavokNan::Finite(fine));
	const std::array<float, 3> nan{ 1.0f, std::numeric_limits<float>::quiet_NaN(), 0.0f };
	CHECK(!HavokNan::Finite(nan));
	const std::array<float, 2> inf{ std::numeric_limits<float>::infinity(), 0.0f };
	CHECK(!HavokNan::Finite(inf));
	CHECK(HavokNan::Finite(std::span<const float>{}));
}

TEST("havok nan: a bad reference is disabled and enabled again a moment later, once")
{
	Cleanup    cleanup;
	const auto now = Cleanup::Clock::time_point{} + 1h;
	CHECK(cleanup.Decide({ .refId = 0x000BDD9A }, now) == Action::kReset);
	CHECK(cleanup.HasPending());
	// Seen again before its enable (the next frames' looks): nothing more.
	CHECK(cleanup.Decide({ .refId = 0x000BDD9A }, now + 100ms) == Action::kNone);
	CHECK(cleanup.TakeDue(now + Cleanup::RESET_GAP - 1ms).empty());
	const auto due = cleanup.TakeDue(now + Cleanup::RESET_GAP);
	REQUIRE(due.size() == 1);
	CHECK(due[0] == 0x000BDD9A);
	CHECK(!cleanup.HasPending());
	CHECK(cleanup.TakeDue(now + 2s).empty());
}

TEST("havok nan: one that goes bad again right after its reset is left disabled; much later it is reset again")
{
	Cleanup    cleanup;
	const auto now = Cleanup::Clock::time_point{} + 1h;
	CHECK(cleanup.Decide({ .refId = 0x1234 }, now) == Action::kReset);
	CHECK(cleanup.TakeDue(now + 1s).size() == 1);
	CHECK(cleanup.Decide({ .refId = 0x1234 }, now + 10s) == Action::kDisable);
	CHECK(!cleanup.HasPending());
	// Another, reset a while ago: reset again.
	CHECK(cleanup.Decide({ .refId = 0x5678 }, now) == Action::kReset);
	CHECK(cleanup.TakeDue(now + 1s).size() == 1);
	CHECK(cleanup.Decide({ .refId = 0x5678 }, now + Cleanup::AGAIN + 1s) == Action::kReset);
}

TEST("havok nan: one whose place is not finite is disabled and left so (enabling would put it back there)")
{
	Cleanup    cleanup;
	const auto now = Cleanup::Clock::time_point{} + 1h;
	CHECK(cleanup.Decide({ .refId = 0x42, .placeFinite = false }, now) == Action::kDisable);
	CHECK(!cleanup.HasPending());
}

TEST("havok nan: the player and the session's own stand-ins are only logged, once a minute")
{
	Cleanup    cleanup;
	const auto now = Cleanup::Clock::time_point{} + 1h;
	CHECK(cleanup.Decide({ .refId = 0x14, .player = true }, now) == Action::kLeave);
	CHECK(cleanup.Decide({ .refId = 0x14, .player = true }, now + 5s) == Action::kNone);
	CHECK(cleanup.Decide({ .refId = 0x14, .player = true, .placeFinite = false }, now + Cleanup::LEAVE_LOG) == Action::kLeave);
	CHECK(cleanup.Decide({ .refId = 0xFF000801, .ours = true }, now) == Action::kLeave);
	CHECK(!cleanup.HasPending());
}

TEST("havok nan: the main thread looks at every node when a NaN angle was counted, at the roots every few seconds")
{
	using HavokNan::DEEP_EVERY;
	using HavokNan::Plan;
	using HavokNan::Scan;
	using HavokNan::SCAN_EVERY;
	CHECK(Plan(3, 2, 0s, DEEP_EVERY) == Scan::kDeep);
	CHECK(Plan(3, 2, SCAN_EVERY, 1h) == Scan::kDeep);
	CHECK(Plan(3, 2, 0s, DEEP_EVERY - 1ms) == Scan::kNone);  // counted again within a second of the last deep look: waits
	CHECK(Plan(3, 2, SCAN_EVERY, DEEP_EVERY - 1ms) == Scan::kRoots);
	CHECK(Plan(3, 3, 1s, 1h) == Scan::kNone);
	CHECK(Plan(3, 3, SCAN_EVERY, 1h) == Scan::kRoots);
	CHECK(Plan(0, 0, SCAN_EVERY - 1ms, 1h) == Scan::kNone);
}
