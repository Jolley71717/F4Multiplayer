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
// - Once a frame, on the main thread: when the count grew (backing off while that finds nothing to put right: Looks),
//   and every few seconds anyway, the loaded references whose 3D (every node of it after a count, the root alone
//   otherwise) or place is not finite are looked for and put right by a disable and an enable (the reference's 3D
//   and physics made again from its place, which stays finite), when they are references nothing else has a stake
//   in (Traits); every other one, and one that goes bad again soon after, is left as it is, logged
//   (Cleanup). Nothing is ever left disabled.
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

	// What a reference found bad is, as far as a reset goes. A reset is the engine's Disable and, RESET_GAP later,
	// Enable, both done at once (the console's `disable` is queued, and a save could come before it and keep it): the
	// engine drops the reference's 3D and physics and makes them again from its place. Only a reference a plugin file
	// placed in the world that nothing else has a stake in is ever reset (the dump's skeleton, a moveable static, is
	// one); everything else is left as it is, logged: an actor of any kind (the player, the session's
	// stand-ins and mirrored NPCs, companions, essential and protected NPCs: disabling one runs its scripts' and the
	// AI's handling of that, and an NPC's disabled state goes to the other games), anything the session places or
	// shares (a power armor frame's state goes to the other games too), anything made at runtime (dropped items,
	// workshop builds, spawns), a persistent reference, one in a quest's alias, and one in an enable parent's chain
	// (disabling a parent disables its children; enabling a child its parent keeps off would undo the quest's work).
	struct Traits
	{
		bool player = false;
		bool actor = false;
		bool session = false;     // the session placed it, runs it or mirrors it
		bool frame = false;       // a power armor frame (FrameShare shares them)
		bool created = false;     // made at runtime (0xFF......)
		bool persistent = false;  // the reference's persistent flag
		bool quest = false;       // fills a quest's alias
		bool enableLinked = false;  // has an enable parent, or is one
	};

	// Why a reference is never reset, or null when it may be.
	constexpr const char* KeptBecause(const Traits& a_traits)
	{
		if (a_traits.player) {
			return "the player";
		}
		if (a_traits.actor) {
			return "an actor";
		}
		if (a_traits.session) {
			return "the session's";
		}
		if (a_traits.frame) {
			return "a power armor frame";
		}
		if (a_traits.created) {
			return "made at runtime";
		}
		if (a_traits.persistent) {
			return "persistent";
		}
		if (a_traits.quest) {
			return "in a quest alias";
		}
		if (a_traits.enableLinked) {
			return "enable-linked";
		}
		return nullptr;
	}

	// A loaded reference found with its 3D (or its place) not finite.
	struct Found
	{
		std::uint32_t refId = 0;
		const char*   kept = nullptr;      // KeptBecause: never reset
		bool          placeFinite = true;  // its place (data.location, data.angle): where enable puts it back
	};

	// Nothing is ever left disabled: a reference is reset (disabled, then enabled RESET_GAP later) or left as it is.
	enum class Action : std::uint8_t
	{
		kNone,      // being reset already, or left alone and logged lately
		kReset,     // disable now, enable after RESET_GAP: its 3D and physics are made again from its place
		kKept,      // one never reset (Found::kept): logged only
		kPlaceBad,  // its place is not finite either (enabling would put it back there): logged only
		kAgain,     // it went bad again soon after a reset: logged only, and never reset again until a load
	};

	constexpr const char* Name(Action a_action)
	{
		switch (a_action) {
		case Action::kReset:
			return "reset (disabled and enabled again)";
		case Action::kKept:
			return "left as it is";
		case Action::kPlaceBad:
			return "left as it is (its place is not finite either)";
		case Action::kAgain:
			return "left as it is (bad again soon after its reset)";
		default:
			return "none";
		}
	}

	class Cleanup
	{
	public:
		using Clock = std::chrono::steady_clock;

		static constexpr auto RESET_GAP = std::chrono::milliseconds(500);  // between the disable and the enable
		static constexpr auto AGAIN = std::chrono::seconds(60);            // bad again this soon after a reset: given up
		static constexpr auto LEAVE_LOG = std::chrono::minutes(10);        // a left-alone one is logged again after this

		// What to do with a_found, seen at a_now. A reset is remembered until its enable is due (TakeDue), and for AGAIN
		// after; one given up on, until Forget.
		Action Decide(const Found& a_found, Clock::time_point a_now)
		{
			if (std::ranges::any_of(pending, [&](const Enable& a_pending) { return a_pending.refId == a_found.refId; })) {
				return Action::kNone;
			}
			auto leave = Action::kNone;
			if (a_found.kept) {
				leave = Action::kKept;
			} else if (!a_found.placeFinite) {
				leave = Action::kPlaceBad;
			} else if (const auto last = resets.find(a_found.refId); last != resets.end() && (last->second.gaveUp || a_now - last->second.at < AGAIN)) {
				last->second.gaveUp = true;
				leave = Action::kAgain;
			}
			if (leave != Action::kNone) {
				auto& at = left[a_found.refId];
				if (at != Clock::time_point{} && a_now - at < LEAVE_LOG) {
					return Action::kNone;
				}
				at = a_now;
				return leave;
			}
			resets[a_found.refId] = { a_now, false };
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

		// Every reset still waiting for its enable, taken off the list (before a save or a load: enabled at once).
		std::vector<std::uint32_t> TakeAll()
		{
			std::vector<std::uint32_t> all;
			for (const auto& one : pending) {
				all.push_back(one.refId);
			}
			pending.clear();
			return all;
		}

		// What is no longer needed at a_now: resets older than AGAIN (not given up on), left-alone ones logged longer
		// than LEAVE_LOG ago.
		void Prune(Clock::time_point a_now)
		{
			std::erase_if(resets, [&](const auto& a_entry) { return !a_entry.second.gaveUp && a_now - a_entry.second.at >= AGAIN; });
			std::erase_if(left, [&](const auto& a_entry) { return a_now - a_entry.second >= LEAVE_LOG; });
		}

		// After a load or a new game: its ids name other references, or the same ones as its save has them.
		void Forget()
		{
			pending.clear();
			resets.clear();
			left.clear();
		}

		[[nodiscard]] bool        HasPending() const { return !pending.empty(); }
		[[nodiscard]] std::size_t Pending() const { return pending.size(); }
		[[nodiscard]] std::size_t Remembered() const { return resets.size() + left.size(); }

	private:
		struct Enable
		{
			std::uint32_t     refId = 0;
			Clock::time_point enableAt{};
		};

		struct Reset
		{
			Clock::time_point at{};
			bool              gaveUp = false;
		};

		std::vector<Enable>                                   pending;
		std::unordered_map<std::uint32_t, Reset>             resets;  // when each was last reset, and whether it is given up on
		std::unordered_map<std::uint32_t, Clock::time_point> left;    // when each left-alone one was last logged
	};

	// How the main thread looks for bad references.
	enum class Scan : std::uint8_t
	{
		kNone,
		kRoots,  // root nodes and places
		kDeep,   // every node, and places
	};

	// When the main thread looks. Each loaded reference's root node and place, every ROOTS_EVERY: cheap, and finds one
	// blown up as a whole. Every node of each loaded reference's 3D (a ragdoll's bones are its children) when NaN angles
	// were counted since the last such look: the count grows on every physics step while a bad body is in the world.
	// A deep look that resets nothing (the bodies are the player's, a stand-in's, or anything else left alone, or their
	// nodes are finite) waits twice as long before the next (DEEP_FIRST, 2x, 4x ..., at most DEEP_MOST), and after
	// DEEP_TRIES of them in a row there are no more deep looks until the player is in another space (an interior, a
	// worldspace: a loading screen), a game is loaded (Rearm), or the count, still for QUIET, grows again.
	class Looks
	{
	public:
		using Clock = std::chrono::steady_clock;

		static constexpr auto ROOTS_EVERY = std::chrono::seconds(5);
		static constexpr auto DEEP_FIRST = std::chrono::seconds(1);
		static constexpr auto DEEP_MOST = std::chrono::seconds(8);
		static constexpr int  DEEP_TRIES = 5;
		static constexpr auto QUIET = std::chrono::seconds(30);

		// This frame's look: a_count is the NaN angles counted so far.
		Scan Plan(std::uint64_t a_count, Clock::time_point a_now)
		{
			if (a_count != lastCount) {
				if (gaveUp && a_now - lastGrowth >= QUIET) {
					Arm(a_now);  // a new burst: the count was still a while
				}
				lastCount = a_count;
				lastGrowth = a_now;
			}
			if (!gaveUp && a_count != seen && a_now >= nextDeep) {
				return Scan::kDeep;
			}
			return a_now - lastRoots >= ROOTS_EVERY ? Scan::kRoots : Scan::kNone;
		}

		// A look was done at a_now; a_count as Plan had it, a_fruitful when it reset something. Whether this deep look
		// was the last before giving up.
		bool Looked(Scan a_scan, std::uint64_t a_count, bool a_fruitful, Clock::time_point a_now)
		{
			lastRoots = a_now;
			if (a_scan != Scan::kDeep) {
				return false;
			}
			seen = a_count;
			if (a_fruitful) {
				tries = 0;
				gap = DEEP_FIRST;
				nextDeep = a_now + DEEP_FIRST;
				return false;
			}
			if (++tries >= DEEP_TRIES) {
				gaveUp = true;
				return true;
			}
			nextDeep = a_now + gap;
			gap = (std::min)(gap * 2, std::chrono::duration_cast<Clock::duration>(DEEP_MOST));
			return false;
		}

		// The player is somewhere else, or a game was loaded: the cuts counted so far are of the world as it was, and
		// the looks start over from the first gap.
		void Rearm(std::uint64_t a_count, Clock::time_point a_now)
		{
			Arm(a_now);
			seen = lastCount = a_count;
		}

		[[nodiscard]] bool GaveUp() const { return gaveUp; }
		[[nodiscard]] int  Tries() const { return tries; }

	private:
		void Arm(Clock::time_point a_now)
		{
			gaveUp = false;
			tries = 0;
			gap = DEEP_FIRST;
			nextDeep = a_now;
		}

		std::uint64_t     seen = 0;       // the count at the last deep look
		std::uint64_t     lastCount = 0;  // the count Plan saw last
		Clock::time_point lastGrowth{};   // when Plan saw it grow
		Clock::time_point lastRoots{};
		Clock::time_point nextDeep{};
		Clock::duration   gap = DEEP_FIRST;
		int               tries = 0;  // deep looks in a row that reset nothing
		bool              gaveUp = false;
	};

	// A deep look stops walking a reference's 3D after this many nodes (a reference has hundreds at most).
	inline constexpr std::size_t NODE_LIMIT = 4096;
	// A look takes at most this many loaded references (all of the loaded cells', together), and puts right at most
	// BAD_LIMIT of the ones it finds bad.
	inline constexpr std::size_t REF_LIMIT = 16384;
	inline constexpr std::size_t BAD_LIMIT = 64;
}
