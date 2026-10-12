#pragma once

#include <chrono>

// When a mirrored NPC whose animation graph refuses to start moving is told to end the idle holding
// it. Dogmeat's graph only takes "moveStart" from its plain standing idle, and a dog left standing
// sits, lies down or turns on the spot by itself: the AI would end that idle before walking off, but
// a mirror's AI is off, so the dog slid after its owner's copy sitting down. Pure bookkeeping, so it
// can be tested without the game.
namespace IdleRelease
{
	using Clock = std::chrono::steady_clock;

	// Between tries: the graph takes the stop only once the idle is under way (a dog sitting down
	// finishes sitting first), and getting up plays for about a second, refusing "moveStart" meanwhile.
	constexpr auto RETRY = std::chrono::milliseconds(500);

	struct Gate
	{
		Clock::time_point next{};

		// a_refused: the NPC should move and its graph refused "moveStart" this frame. True when the
		// idle should be told to stop now: at once on the first refusal, then every RETRY while they go
		// on. A frame where the graph took the start arms it again for the next time.
		bool Due(bool a_refused, Clock::time_point a_now)
		{
			if (!a_refused) {
				next = {};
				return false;
			}
			if (a_now < next) {
				return false;
			}
			next = a_now + RETRY;
			return true;
		}
	};
}
