#include "game/GameClock.h"

#include <format>
#include <mutex>

namespace GameClock
{
	namespace
	{
		// Stepped on the main thread, read from any (a hit event's stamp comes on whatever thread the engine sends it).
		std::mutex    lock;
		Timeline      timeline;
		std::uint64_t steps = 0;

		const RE::BSTimer* Timer()
		{
			static const RE::BSTimer* timer = RE::BSTimer::GetSingleton();
			return timer;
		}
	}

	void Frame()
	{
		const auto timer = Timer();
		if (!timer) {
			return;
		}
		const auto wall = Clock::now();
		std::scoped_lock l{ lock };
		const auto before = timeline.mark;
		timeline.Step(wall, timer->lastTime, timer->delta, timer->realTimeDelta);
		if (timeline.mark != before) {
			++steps;
		}
	}

	Clock::time_point Now()
	{
		const auto wall = Clock::now();
		std::scoped_lock l{ lock };
		return timeline.Now(wall);
	}

	float RateNow()
	{
		std::scoped_lock l{ lock };
		return timeline.started ? timeline.rate : 1.0f;
	}

	std::string Describe()
	{
		const auto wall = Clock::now();
		std::scoped_lock l{ lock };
		const auto behind = timeline.started ? std::chrono::duration<float>(wall - timeline.Now(wall)).count() : 0.0f;
		return std::format("gameClock: rate={:.2f} behind={:.1f}s steps={}", timeline.started ? timeline.rate : 1.0f, behind, steps);
	}
}
