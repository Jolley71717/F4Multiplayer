#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

// When a conversation left open without its speaker is ended. The engine ends a conversation through
// its speaker, so once the speaker is gone (a friend's stand-in deleted in the middle of it) nothing
// ends it, and the player has no weapons, Pip-Boy or workshop until they talk to someone else. It is
// only ended after a moment of that, since a speaker can briefly fail to resolve while a conversation
// starts or its NPC reloads.
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
				since.reset();
				ends = 0;
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

		// How often Due() said so since the conversation was last fine: 1 the first time.
		[[nodiscard]] std::uint32_t Ends() const { return ends; }

	private:
		std::optional<Clock::time_point> since;
		std::uint32_t                    ends = 0;
	};
}
