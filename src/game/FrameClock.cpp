#include "game/FrameClock.h"

#include "game/EngineClock.h"

#include <atomic>
#include <mutex>

namespace FrameClock
{
	namespace
	{
		using namespace std::chrono_literals;

		// The timer is read from any thread the engine updates actors on: the lock keeps one frame's step to one reader.
		std::mutex            lock;
		EngineClock::Timeline timeline;
		std::uint64_t         seenTime = 0;      // the timer's time when the timeline last stepped
		Clock::time_point     steppedAt{};       // when that was, on the wall clock
		Clock::time_point     previous{};        // the timeline's time a step before
		std::atomic<Pace>     pace{ Pace::kFrame };

		// A timer whose time stops for longer than this is not one we can go by (it is not running, or its layout is
		// not the one we read): the wall clock is used until it moves again.
		constexpr auto STOPPED = 250ms;

		const RE::BSTimer* Timer()
		{
			static const RE::BSTimer* timer = RE::BSTimer::GetSingleton();
			return timer;
		}
	}

	Clock::time_point Now()
	{
		const auto wall = Clock::now();
		const auto timer = Timer();
		if (!timer) {
			return wall;
		}
		std::scoped_lock l{ lock };
		const auto time = timer->lastTime;
		if (time != seenTime) {
			seenTime = time;
			steppedAt = wall;
			previous = timeline.started ? timeline.at : wall;
			return timeline.Advance(wall, timer->delta);
		}
		if (wall - steppedAt > STOPPED) {
			timeline.started = false;  // set to the wall clock when it moves again
			return wall;
		}
		return timeline.at;
	}

	Raw Read()
	{
		const auto timer = Timer();
		return timer ? Raw{ timer->delta, timer->realTimeDelta, timer->lastTime, true } : Raw{};
	}

	Clock::time_point Previous()
	{
		const auto now = Now();
		std::scoped_lock l{ lock };
		return timeline.started && previous != Clock::time_point{} && previous <= now ? previous : now;
	}

	void SetPace(Pace a_pace) { pace.store(a_pace, std::memory_order_relaxed); }

	Pace GetPace() { return pace.load(std::memory_order_relaxed); }

	const char* PaceName(Pace a_pace)
	{
		switch (a_pace) {
		case Pace::kWall:
			return "wall";
		case Pace::kPrevious:
			return "previous";
		default:
			return "frame";
		}
	}

	Clock::time_point StampAt(Clock::time_point a_now)
	{
		switch (GetPace()) {
		case Pace::kWall:
			return a_now;
		case Pace::kPrevious:
			return Previous();
		default:
			return Now();
		}
	}

	Clock::time_point PlaceAt(Clock::time_point a_now) { return GetPace() == Pace::kWall ? a_now : Now(); }
}
