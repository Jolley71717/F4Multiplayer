#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

// When a conversation left open without its speaker is ended. The engine ends a conversation through
// its speaker, so once the speaker is gone (a friend's stand-in deleted in the middle of it) nothing
// ends it, and the player has no weapons, Pip-Boy or workshop until they talk to someone else. It is
// only ended after a moment of that, since a speaker can briefly fail to resolve while a conversation
// starts or its NPC reloads; and only when the speaker is one of ours (a friend's stand-in, or an NPC
// the session shares), since one the game itself left open is the game's.
namespace StrandedDialogue
{
	using Clock = std::chrono::steady_clock;

	constexpr auto GRACE = std::chrono::seconds(2);

	class Timer
	{
	public:
		// Whether to end the conversation now: it has been stranded for GRACE without a break. If it
		// is still stranded GRACE after that (the end didn't take), again.
		bool Due(bool a_stranded, Clock::time_point a_now)
		{
			if (!a_stranded) {
				Reset();
				return false;
			}
			if (!since) {
				since = a_now;
				return false;
			}
			if (a_now - *since < GRACE) {
				return false;
			}
			since = a_now;
			++ends;
			return true;
		}

		// The wait starts over: the conversation is fine, or over, or a loading screen is up (it may last
		// longer than GRACE, and is no part of the wait).
		void Reset()
		{
			since.reset();
			ends = 0;
		}

		// How often Due() said so since the conversation was last fine: 1 the first time.
		[[nodiscard]] std::uint32_t Ends() const { return ends; }

	private:
		std::optional<Clock::time_point> since;
		std::uint32_t                    ends = 0;
	};

	// Whether the conversation's speaker is one of ours, remembered while the conversation lasts: a
	// friend's stand-in deleted in the middle of it no longer resolves, or no longer counts as ours by
	// what it is (it was let go as it was switched off), and it is still the one that was ours.
	class Speaker
	{
	public:
		// a_speaker: the speaker's form ID, 0 when its handle doesn't resolve. a_ours: whether it is one of
		// ours by what it is now (ignored for 0).
		bool Ours(std::uint32_t a_speaker, bool a_ours)
		{
			if (a_speaker != 0 && (a_ours || a_speaker != last)) {
				last = a_ours ? a_speaker : 0;  // another speaker, not ours, ends what was known
			}
			return last != 0;
		}

		// The conversation is over.
		void Reset() { last = 0; }

	private:
		std::uint32_t last = 0;  // the speaker last seen while it was ours
	};
}
