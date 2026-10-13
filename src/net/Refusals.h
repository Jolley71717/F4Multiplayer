#pragma once

#include "Protocol.h"

#include <chrono>
#include <string>
#include <string_view>

// A server's refusals as the joining game takes them: when to try again, and whether to tell the player. Kept
// free of the game so it can be tested against the real server's refusals; Session applies it.
class Refusals
{
public:
	struct Answer
	{
		bool                 notify = false;  // tell the player
		std::chrono::seconds retryIn{};
	};

	// The host's game will be in soon (it is loading, or at the main menu): try again in a few seconds, and say so
	// once rather than at every try. Anything else is told every time, and tried again in a minute.
	Answer OnReject(std::string_view a_reason)
	{
		const bool hostNotReady = a_reason == Protocol::REJECT_HOST_NOT_READY;
		const bool notify = !hostNotReady || last != a_reason;
		last = a_reason;
		return { notify, hostNotReady ? std::chrono::seconds{ 5 } : std::chrono::seconds{ 60 } };
	}

	// Let in, or set to reach another server: the next refusal is news.
	void Clear() { last.clear(); }

	// Why the last try was refused ("" = it wasn't), and whether the next comes soon.
	[[nodiscard]] const std::string& Last() const { return last; }
	[[nodiscard]] bool                RetrySoon() const { return last == Protocol::REJECT_HOST_NOT_READY; }

private:
	std::string last;
};
