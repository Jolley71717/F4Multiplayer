#pragma once

#include <chrono>
#include <cstdint>

// When a mirrored NPC whose animation graph refuses to start moving is told to end the idle holding
// it. Dogmeat's graph only takes "moveStart" from its plain standing idle, and a dog left standing
// sits, lies down or turns on the spot by itself: the AI would end that idle before walking off, but
// a mirror's AI is off, so the dog slid after its owner's copy sitting down. Pure bookkeeping, so it
// can be tested without the game.
namespace IdleRelease
{
	using Clock = std::chrono::steady_clock;

	// The first stop goes at once, and normally frees it (for Dogmeat, its "turnStop" ends a sit or a
	// lie-down at once, with no get-up). A graph still refusing after it is held by something the stops did
	// not end at once (a flavor idle that ends by itself, a human's cower exit playing out), so the next
	// waits RETRY, and each one after twice as long as the one before, up to MAX_RETRY.
	constexpr auto RETRY = std::chrono::milliseconds(1500);
	constexpr auto MAX_RETRY = std::chrono::seconds(12);
	// Stops sent in one stuck spell. A graph that refuses them all is held by something the stops do not
	// reach, and is left alone until it next moves or its owner's copy next stops and goes again.
	constexpr std::uint32_t TRIES = 6;
	// The NPC is asked to move every frame while its owner's copy moves; no refusal for this long means its
	// owner's copy stood still meanwhile, and the next refusal starts a new spell (it may have sat again).
	constexpr auto SPELL_GAP = std::chrono::seconds(1);

	// How long after the a_tries-th stop the next one may go.
	constexpr Clock::duration Backoff(std::uint32_t a_tries)
	{
		Clock::duration wait = RETRY;
		for (std::uint32_t i = 1; i < a_tries && wait < MAX_RETRY; ++i) {
			wait *= 2;
		}
		return wait < MAX_RETRY ? wait : Clock::duration{ MAX_RETRY };
	}

	struct Gate
	{
		Clock::time_point next{};
		Clock::time_point lastRefusal{};
		std::uint32_t     tries = 0;  // stops sent in this spell

		// a_refused: the NPC should move and its graph refused "moveStart" this frame. True when the idle
		// should be told to stop now: at once on the first refusal of a spell, then after RETRY, twice that,
		// and so on, TRIES times at most. A frame where the graph took the start ends the spell.
		bool Due(bool a_refused, Clock::time_point a_now)
		{
			if (!a_refused) {
				*this = {};
				return false;
			}
			if (a_now - lastRefusal > SPELL_GAP) {
				tries = 0;
				next = {};
			}
			lastRefusal = a_now;
			if (tries >= TRIES || a_now < next) {
				return false;
			}
			++tries;
			next = a_now + Backoff(tries);
			return true;
		}

		// The stop just sent was this spell's last.
		[[nodiscard]] bool Spent() const { return tries >= TRIES; }
	};
}
