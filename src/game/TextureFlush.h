#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>

// Keeps the engine's texture wait from waiting on requests nobody will hand over (the host's hang of 2026-10-17,
// docs/HANG-20261017.md). When the player is moved (a coc, a door, fast travel), the main thread pumps window messages
// in PositionPlayerJob::Run while a job thread positions the player, and that job ends in the texture update's blocking
// wait: `while (pending && done < total) Sleep(1)`, no timeout. The per-frame texture walk counts every request it adds
// to its batch into `total` at once, but hands the batch to the streamer only when the streamer's pool of requests runs
// out (and then it sets `pending`, and goes on adding) or at its Done stage. A move that starts after such a hand-over
// and before the next finds `pending` set with requests counted that sit in the main thread's batch: the wait sets that
// batch aside, keeps its count, and waits for the streamer to finish them; the streamer never got them, and the main
// thread, the only one that would hand them over, is in the pump waiting for the job.
//
// So right before Main::OnIdle calls PositionPlayerJob::Run, the main thread does what the walk would have done next: it
// hands the streamer the batch it owes (the engine's SubmitBatch, as the walk's own pool-full path and the Done stage
// do), under the texture update's lock. Not earlier (when a console command asks for the move): the walk runs on in the
// frames before the move starts and can count new requests in the meantime; from the pump on it does not run until the
// job is done. Only with `pending` set: with it clear the wait releases the batch itself (its first step counts every
// request in it as done and starts over), and an empty batch owes nothing. The decisions only, free of the game, so
// they are unit tested; TextureWait.cpp reads the engine and calls them.
namespace TextureFlush
{
	// The texture update's counters (game offset +32FF859, +32FF85C, +32FF870 in 1.11.240).
	struct Counters
	{
		bool          pending = false;  // a batch was handed to the streamer and not yet released
		std::uint32_t done = 0;         // requests finished (or released)
		std::uint32_t total = 0;        // requests counted into the batches since the last reset

		friend constexpr bool operator==(const Counters&, const Counters&) = default;
	};

	// The blocking wait's loop condition, as the engine has it.
	constexpr bool Waiting(const Counters& a_counters)
	{
		return a_counters.pending && a_counters.done < a_counters.total;
	}

	// Whether the main thread's batch must be handed over before a move. a_owed: requests linked into the main thread's
	// current batch, not handed to the streamer. Not pending: the wait releases them itself (and handing them over then
	// would let their late finishes count against the job's own requests, ending its wait early).
	constexpr bool NeedsFlush(const Counters& a_counters, std::uint32_t a_owed)
	{
		return a_counters.pending && a_owed > 0;
	}

	// Where the five bytes at a_at call: `E8 rel32` (a near call, relative to the next instruction), or nothing when they
	// are something else. The hand-over goes in only where the game has the call it expects.
	constexpr std::optional<std::uintptr_t> CallTarget(std::span<const std::uint8_t, 5> a_bytes, std::uintptr_t a_at)
	{
		if (a_bytes[0] != 0xE8) {
			return std::nullopt;
		}
		const std::uint32_t raw = static_cast<std::uint32_t>(a_bytes[1]) | static_cast<std::uint32_t>(a_bytes[2]) << 8 | static_cast<std::uint32_t>(a_bytes[3]) << 16 |
		                          static_cast<std::uint32_t>(a_bytes[4]) << 24;
		const auto rel = static_cast<std::int32_t>(raw);
		return a_at + 5 + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(rel));
	}

	// Whether any of a_callers (return addresses on a thread's stack) is inside [a_begin, a_end): that function is on it.
	constexpr bool OnStack(std::span<const std::uintptr_t> a_callers, std::uintptr_t a_begin, std::uintptr_t a_end)
	{
		return a_begin < a_end && std::ranges::any_of(a_callers, [&](std::uintptr_t a_at) { return a_at >= a_begin && a_at < a_end; });
	}
}
