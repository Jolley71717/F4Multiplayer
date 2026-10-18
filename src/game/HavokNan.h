#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

// Keeps a physics body gone NaN from freezing the game (the host's hang of 2026-10-11; docs/HANG-20261011.md
// has the dump). Havok's constraint setup (hknpSolverData's setup of a constraint's angular limits, game offset
// +156F5C0 in 1.11.240) brings the angle between two bodies into [-pi, pi) with a loop that runs while the angle differs
// from itself clamped to that range: `do { a -= 2pi * floor((a + pi) / 2pi); } while (clamp(a) != a)`. A NaN differs
// from everything, itself clamped included, so with a constrained body whose transform is NaN that loop never ends: the
// job thread running it spins, the physics step never finishes, the main thread waits for the step forever, and the game
// stops answering with no crash and no log. In the dump the bodies were a ragdoll skeleton lying in the world (a
// moveable static), whose bodies had blown up to infinity and then NaN; nothing of the plugin was on any thread.
//
// Two things, both here as decisions free of the game (unit tested), both done by HavokGuard.cpp:
// - The loop's exit test, at its two copies in that function, goes through a few bytes of ours (the detour) that end
//   the loop when the angle is NaN (the result is NaN, as the body is; the engine carries on with it) and count it. The
//   detour goes in only where the game has the very bytes read from 1.11.240 (SiteMatches).
// - Once a frame, on the main thread: when the count grew, and every few seconds anyway, the loaded references whose
//   3D (every node of it after a count, the root alone otherwise) or place is not finite are looked for and put right
//   the way the console's `disable` and `enable` do (the reference's 3D and physics made again from its place, which
//   stays finite): the player and the session's own stand-ins are left alone (logged), and one that goes bad again
//   soon after is left disabled (Cleanup).
namespace HavokNan
{
	// --- the loop's exit test ---

	// The bytes the exit test is, at both copies: `cmpneqss xmm0,xmm5; movd eax,xmm0; bt eax,0; jb loop`. The patch
	// replaces the last two instructions (the 10 bytes from PATCH_AT), so the loop's own compare stays the game's.
	inline constexpr std::array<std::uint8_t, 19> SITE_BYTES{
		0xF3, 0x0F, 0xC2, 0xC5, 0x04,        // cmpneqss xmm0, xmm5
		0x66, 0x0F, 0x7E, 0xC0,              // movd eax, xmm0
		0x0F, 0xBA, 0xE0, 0x00,              // bt eax, 0
		0x0F, 0x82, 0x75, 0xFF, 0xFF, 0xFF,  // jb rel32 (back to the loop's top)
	};
	inline constexpr std::size_t PATCH_AT = 9;     // the bt, from the start of SITE_BYTES
	inline constexpr std::size_t PATCH_SIZE = 10;  // bt and jb

	// Where the two copies' SITE_BYTES begin, from the start of the function (Address Library ID 2263389, +156F5C0).
	inline constexpr std::uint32_t FUNCTION_ID = 2263389;
	inline constexpr std::array<std::uintptr_t, 2> SITES{ 0x2369, 0x3038 };

	// The patch site's two jump targets: the loop's top (where jb goes) and the code right after it.
	struct Targets
	{
		std::uintptr_t loop = 0;
		std::uintptr_t after = 0;

		friend constexpr bool operator==(const Targets&, const Targets&) = default;
	};

	// Whether a_bytes, read at a_at, are the exit test as 1.11.240 has it; then where its jb goes and what follows it.
	// Anything else (another version, another plugin's patch) is left as it is.
	constexpr std::optional<Targets> SiteMatches(std::span<const std::uint8_t, SITE_BYTES.size()> a_bytes, std::uintptr_t a_at)
	{
		if (!std::ranges::equal(a_bytes, SITE_BYTES)) {
			return std::nullopt;
		}
		const std::uint32_t raw = static_cast<std::uint32_t>(a_bytes[15]) | static_cast<std::uint32_t>(a_bytes[16]) << 8 | static_cast<std::uint32_t>(a_bytes[17]) << 16 |
		                          static_cast<std::uint32_t>(a_bytes[18]) << 24;
		const auto after = a_at + SITE_BYTES.size();
		return Targets{ .loop = after + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(static_cast<std::int32_t>(raw))), .after = after };
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

	constexpr void PutRel32(std::span<std::uint8_t> a_out, std::size_t a_at, std::int32_t a_rel)
	{
		const auto raw = static_cast<std::uint32_t>(a_rel);
		for (std::size_t i = 0; i < 4; ++i) {
			a_out[a_at + i] = static_cast<std::uint8_t>(raw >> (8 * i));
		}
	}

	// The detour, written at a_base (memory of ours within 2 GB of the game's code), for a site whose jumps go to
	// a_targets:
	//
	//   +00  bt eax, 0                  the game's own test: the angle is in range
	//   +04  jnc +1A                    ... then out, as the game goes
	//   +06  ucomiss xmm5, xmm5         the angle is NaN?
	//   +09  jp +10
	//   +0B  jmp loop                   no: round again, as the game goes
	//   +10  lock inc dword [count]     yes: counted,
	//   +17  xorps xmm5, xmm5           the loop's copy of the angle zeroed (the result, kept apart, stays NaN),
	//   +1A  jmp after                  and out
	//   +20  count                      (4 bytes, at DETOUR_COUNT)
	//
	// rax and the flags are free at both sites (the code after sets eax before reading it). None when a jump can't
	// reach.
	inline constexpr std::size_t DETOUR_SIZE = 0x28;
	inline constexpr std::size_t DETOUR_COUNT = 0x20;

	constexpr std::optional<std::array<std::uint8_t, DETOUR_SIZE>> Detour(std::uintptr_t a_base, const Targets& a_targets)
	{
		std::array<std::uint8_t, DETOUR_SIZE> code{
			0x0F, 0xBA, 0xE0, 0x00,                    // +00 bt eax, 0
			0x73, 0x14,                                // +04 jnc +1A
			0x0F, 0x2E, 0xED,                          // +06 ucomiss xmm5, xmm5
			0x7A, 0x05,                                // +09 jp +10
			0xE9, 0x00, 0x00, 0x00, 0x00,              // +0B jmp loop
			0xF0, 0xFF, 0x05, 0x09, 0x00, 0x00, 0x00,  // +10 lock inc dword [rip+9] (+17 + 9 = +20)
			0x0F, 0x57, 0xED,                          // +17 xorps xmm5, xmm5
			0xE9, 0x00, 0x00, 0x00, 0x00,              // +1A jmp after
			0xCC,                                      // +1F
			0x00, 0x00, 0x00, 0x00,                    // +20 count
			0xCC, 0xCC, 0xCC, 0xCC,                    // +24
		};
		const auto loop = Rel32(a_base + 0x10, a_targets.loop);
		const auto after = Rel32(a_base + 0x1F, a_targets.after);
		if (!loop || !after) {
			return std::nullopt;
		}
		PutRel32(code, 0x0C, *loop);
		PutRel32(code, 0x1B, *after);
		return code;
	}

	// What goes over the site's bt and jb: a jmp to the detour, and a 5-byte nop (never run: the detour jumps past it).
	constexpr std::optional<std::array<std::uint8_t, PATCH_SIZE>> Patch(std::uintptr_t a_patchAt, std::uintptr_t a_detour)
	{
		const auto rel = Rel32(a_patchAt + 5, a_detour);
		if (!rel) {
			return std::nullopt;
		}
		std::array<std::uint8_t, PATCH_SIZE> code{ 0xE9, 0, 0, 0, 0, 0x0F, 0x1F, 0x44, 0x00, 0x00 };
		PutRel32(code, 1, *rel);
		return code;
	}

	// --- what is put right ---

	// Whether every float of a_values is a number (not NaN, not infinite).
	inline bool Finite(std::span<const float> a_values)
	{
		return std::ranges::all_of(a_values, [](float a_value) { return std::isfinite(a_value); });
	}

	// A loaded reference found with its 3D (or its place) not finite.
	struct Found
	{
		std::uint32_t refId = 0;
		bool          player = false;      // the player: never disabled
		bool          ours = false;        // a stand-in or mirrored NPC of the session's: its own code places it
		bool          placeFinite = true;  // its place (data.location, data.angle): where enable puts it back
	};

	enum class Action : std::uint8_t
	{
		kNone,     // being put right already (disabled, to be enabled)
		kReset,    // disable now, enable after RESET_GAP: its 3D and physics are made again from its place
		kDisable,  // disable and leave it so: its place is not finite either, or it went bad again right after a reset
		kLeave,    // the player or one of ours: logged only
	};

	constexpr const char* Name(Action a_action)
	{
		switch (a_action) {
		case Action::kReset:
			return "reset (disabled and enabled again)";
		case Action::kDisable:
			return "disabled and left so";
		case Action::kLeave:
			return "left alone";
		default:
			return "none";
		}
	}

	class Cleanup
	{
	public:
		using Clock = std::chrono::steady_clock;

		static constexpr auto RESET_GAP = std::chrono::milliseconds(500);  // between the disable and the enable
		static constexpr auto AGAIN = std::chrono::seconds(60);            // bad again this soon after a reset: left disabled
		static constexpr auto LEAVE_LOG = std::chrono::seconds(60);        // a left-alone one is logged again after this

		// What to do with a_found, seen at a_now. A reset is remembered until its enable is due (TakeDue) and for AGAIN after.
		Action Decide(const Found& a_found, Clock::time_point a_now)
		{
			if (std::ranges::any_of(pending, [&](const Enable& a_pending) { return a_pending.refId == a_found.refId; })) {
				return Action::kNone;
			}
			if (a_found.player || a_found.ours) {
				auto& at = left[a_found.refId];
				if (at != Clock::time_point{} && a_now - at < LEAVE_LOG) {
					return Action::kNone;
				}
				at = a_now;
				return Action::kLeave;
			}
			const auto last = resets.find(a_found.refId);
			if (!a_found.placeFinite || (last != resets.end() && a_now - last->second < AGAIN)) {
				resets.erase(a_found.refId);
				return Action::kDisable;
			}
			resets[a_found.refId] = a_now;
			pending.push_back({ a_found.refId, a_now + RESET_GAP });
			return Action::kReset;
		}

		// The resets whose enable is due at a_now, taken off the list.
		std::vector<std::uint32_t> TakeDue(Clock::time_point a_now)
		{
			std::vector<std::uint32_t> due;
			std::erase_if(pending, [&](const Enable& a_pending) {
				if (a_pending.enableAt > a_now) {
					return false;
				}
				due.push_back(a_pending.refId);
				return true;
			});
			return due;
		}

		[[nodiscard]] bool HasPending() const { return !pending.empty(); }

	private:
		struct Enable
		{
			std::uint32_t     refId = 0;
			Clock::time_point enableAt{};
		};

		std::vector<Enable>                                   pending;
		std::unordered_map<std::uint32_t, Clock::time_point> resets;  // when each was last reset
		std::unordered_map<std::uint32_t, Clock::time_point> left;    // when each left-alone one was last logged
	};

	// How the main thread looks for bad references. Each loaded reference's root node and place, every SCAN_EVERY: cheap,
	// and finds one blown up as a whole. Every node of each loaded reference's 3D (a ragdoll's bones are its children)
	// when a NaN angle was counted since the last such look, at most once each DEEP_EVERY (the count grows on every
	// physics step while a bad one is in the world, until it is put right).
	inline constexpr auto SCAN_EVERY = std::chrono::seconds(5);
	inline constexpr auto DEEP_EVERY = std::chrono::seconds(1);

	enum class Scan : std::uint8_t
	{
		kNone,
		kRoots,  // root nodes and places
		kDeep,   // every node, and places
	};

	// This frame's look: a_count is the NaN angles counted so far, a_seen the count at the last deep look; a_sinceRoots
	// and a_sinceDeep the time since the last look of either kind and since the last deep one.
	constexpr Scan Plan(std::uint64_t a_count, std::uint64_t a_seen, std::chrono::steady_clock::duration a_sinceRoots, std::chrono::steady_clock::duration a_sinceDeep)
	{
		if (a_count != a_seen && a_sinceDeep >= DEEP_EVERY) {
			return Scan::kDeep;
		}
		return a_sinceRoots >= SCAN_EVERY ? Scan::kRoots : Scan::kNone;
	}

	// A deep look stops walking a reference's 3D after this many nodes (a reference has hundreds at most).
	inline constexpr std::size_t NODE_LIMIT = 4096;
}
