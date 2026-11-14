#pragma once

#include <algorithm>
#include <chrono>
#include <optional>
#include <utility>

// XP our player gained before the kill that earned it was seen (Party::TrackXp). A kill's XP lands at the lethal blow;
// its death event comes at the end of the victim's fall, so the XP waits here to be claimed by that event. XP no kill
// claims within the window was not a kill's (a quest's, a lock's, a discovery's) and is dropped.
//
// A menu that stops our game (the pause menu, the Pip-Boy) holds the victim's fall, and so its death event, for as long
// as it is up: the window does not run while the game is stopped, and runs again in full once it plays. XP gained while
// the game was stopped from one frame to the next is never a kill's (nothing dies in a stopped world): it is not held,
// so a death event that comes later can't claim a terminal's or a lock's XP as its own.
//
// Kept free of the game, so it is tested.
class XpClaim
{
public:
	using Time = std::chrono::steady_clock::time_point;
	using Duration = std::chrono::steady_clock::duration;

	struct Held
	{
		float    xp = 0.0f;
		Time     since{};    // its first gain
		Duration stopped{};  // how long the game stood stopped while it was held
	};

	explicit XpClaim(Duration a_window) :
		window(a_window)
	{}

	// Once a frame, before Gain and Claim. a_stopped: the game is stopped this frame (GameStop::Now). XP that ran out
	// unclaimed, for the log; it is dropped.
	std::optional<Held> Tick(Time a_now, bool a_stopped)
	{
		stoppedThrough = a_stopped && stoppedBefore;
		stoppedBefore = a_stopped;
		if (held.xp <= 0.0f) {
			stopStart.reset();
			return std::nullopt;
		}
		if (a_stopped) {
			if (!stopStart) {
				stopStart = a_now;
			}
			until = (std::max)(until, a_now + window);
			return std::nullopt;
		}
		if (stopStart) {
			held.stopped += a_now - *stopStart;
			stopStart.reset();
		}
		if (a_now < until) {
			return std::nullopt;
		}
		const auto out = held;
		held = {};
		return out;
	}

	// XP gained this frame outside a kill's window. False when the game was stopped since the last frame: not a kill's,
	// and not held.
	bool Gain(float a_xp, Time a_now)
	{
		if (a_xp <= 0.0f || stoppedThrough) {
			return false;
		}
		if (held.xp <= 0.0f) {
			held = { 0.0f, a_now, {} };
		}
		held.xp += a_xp;
		until = a_now + window;
		return true;
	}

	// A kill's death event: the XP held (none: 0), now the kill's.
	Held Claim()
	{
		stopStart.reset();
		return std::exchange(held, {});
	}

	// A load: the XP held was the old game's.
	void Reset()
	{
		held = {};
		until = {};
		stopStart.reset();
		stoppedBefore = false;
		stoppedThrough = false;
	}

	[[nodiscard]] float Xp() const { return held.xp; }

private:
	Duration            window;
	Held                held;
	Time                until{};
	std::optional<Time> stopStart;  // the frame a stop began while XP was held
	bool                stoppedBefore = false;  // the last Tick's a_stopped
	bool                stoppedThrough = false;  // stopped this frame and the last
};
