#pragma once

#include <cstdint>
#include <span>

// Whether our frame message reached the window from inside the engine's own message pump. Moving the player to
// another place (a door, a coc, a fast travel) runs PositionPlayerJob::Run on the main thread, and that reads and
// dispatches window messages until the move is done: our frame message dispatched there ran a whole frame's game work
// in the middle of the engine's cell change. Such a frame message is put off; the next drawn frame posts another. The
// wake a save's or load's thread sends (main.cpp) is still served there: the load waits on it, and putting it off
// left every load to the main-thread wait's fallback.
// Told by the return addresses on the main thread's stack (main.cpp walks it). No game needed, so it is tested.
namespace NestedPump
{
	// A function's code, [begin, end): from the image's unwind table in the game.
	struct Range
	{
		std::uintptr_t begin = 0;
		std::uintptr_t end = 0;

		[[nodiscard]] constexpr bool Has(std::uintptr_t a_address) const { return a_address >= begin && a_address < end; }
	};

	enum class Seen
	{
		kNested,   // the engine's pump is on the stack
		kOuter,    // only the game's own message loop: an ordinary frame message
		kNeither,  // the walk reached neither (it stopped short: the check cannot tell)
	};

	// a_frames: return addresses from the window procedure outward. a_pump: the engine's nested pump; a_outer: the
	// game's message loop and what calls it (on the stack in both cases, so the pump is looked for first).
	constexpr Seen Classify(std::span<const std::uintptr_t> a_frames, Range a_pump, std::span<const Range> a_outer)
	{
		for (const auto address : a_frames) {
			if (a_pump.Has(address)) {
				return Seen::kNested;
			}
		}
		for (const auto address : a_frames) {
			for (const auto& outer : a_outer) {
				if (outer.Has(address)) {
					return Seen::kOuter;
				}
			}
		}
		return Seen::kNeither;
	}

	enum class Action
	{
		kSkip,       // a frame of ours is under way already (an engine call inside it pumped messages)
		kRunPosted,  // a save's or load's wake: only the work it handed over
		kRunFrame,
		kDefer,      // a frame message inside the engine's pump: the next drawn frame posts another
	};

	// What the window procedure does with our message. a_seen is only looked at for a frame message.
	constexpr Action Choose(bool a_wake, bool a_inFrame, Seen a_seen)
	{
		if (a_inFrame) {
			return Action::kSkip;
		}
		if (a_wake) {
			return Action::kRunPosted;
		}
		return a_seen == Seen::kNested ? Action::kDefer : Action::kRunFrame;
	}

	// Whether the walk works in this game: decided by the first frame message (ok when it found the game's message loop
	// or the engine's pump, broken when neither). A broken walk never finds the pump, so nothing is ever put off and
	// frames run as they did before the check; `frames` shows which (pumpCheck).
	enum class Check
	{
		kUnknown,
		kOk,
		kBroken,
	};

	// True when this decided the check (once).
	constexpr bool Decide(Check& a_check, Seen a_seen)
	{
		if (a_check != Check::kUnknown) {
			return false;
		}
		a_check = a_seen == Seen::kNeither ? Check::kBroken : Check::kOk;
		return true;
	}

	constexpr const char* Name(Check a_check)
	{
		switch (a_check) {
		case Check::kOk:
			return "ok";
		case Check::kBroken:
			return "broken";
		default:
			return "unknown";
		}
	}
}
