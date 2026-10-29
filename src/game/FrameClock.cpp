#include "game/FrameClock.h"

#include "game/EngineClock.h"

#include <atomic>
#include <mutex>

namespace FrameClock
{
	namespace
	{
		// The timer is read from any thread the engine updates actors on: the lock keeps one frame's step to one reader.
		std::mutex          lock;
		EngineClock::Frames frames;
		std::atomic<Stamp>  stamp{ Stamp::kPrevious };
		std::atomic<Place>  place{ Place::kWall };

		const RE::BSTimer* Timer()
		{
			static const RE::BSTimer* timer = RE::BSTimer::GetSingleton();
			return timer;
		}

		// The timer's mark of the frame: the 64-bit field at +18, the one this has always read (BSTimer::lastTime in the
		// CommonLibF4 it was written against). The library now names +18 realDeltaMs on 1.11.240 and puts lastTime at +20
		// there (+18 on old-gen and next-gen, BSTimer::QLastTime); read by offset, 1.11.240 reads what it always did.
		// Which of the two is the better mark there is for a test on the rig, not for a library switch.
		constexpr std::size_t MARK_OFFSET = 0x18;

		std::uint64_t Mark(const RE::BSTimer* a_timer)
		{
			return *reinterpret_cast<const std::uint64_t*>(reinterpret_cast<const std::byte*>(a_timer) + MARK_OFFSET);
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
		return frames.Now(wall, Mark(timer), EngineClock::FrameSeconds(timer->delta, timer->realTimeDelta));
	}

	Clock::time_point Previous()
	{
		const auto wall = Clock::now();
		const auto timer = Timer();
		if (!timer) {
			return wall;
		}
		std::scoped_lock l{ lock };
		return frames.Previous(wall, Mark(timer), EngineClock::FrameSeconds(timer->delta, timer->realTimeDelta));
	}

	Raw Read()
	{
		const auto timer = Timer();
		return timer ? Raw{ timer->delta, timer->realTimeDelta, Mark(timer), true } : Raw{};
	}

	void SetStamp(Stamp a_stamp) { stamp.store(a_stamp, std::memory_order_relaxed); }

	Stamp GetStamp() { return stamp.load(std::memory_order_relaxed); }

	const char* StampName(Stamp a_stamp)
	{
		switch (a_stamp) {
		case Stamp::kWall:
			return "wall";
		case Stamp::kFrame:
			return "frame";
		default:
			return "previous";
		}
	}

	void SetPlace(Place a_place) { place.store(a_place, std::memory_order_relaxed); }

	Place GetPlace() { return place.load(std::memory_order_relaxed); }

	const char* PlaceName(Place a_place) { return a_place == Place::kFrame ? "frame" : "wall"; }

	Clock::time_point StampAt(Clock::time_point a_now)
	{
		switch (GetStamp()) {
		case Stamp::kWall:
			return a_now;
		case Stamp::kFrame:
			return Now();
		default:
			return Previous();
		}
	}

	Clock::time_point PlaceAt(Clock::time_point a_now) { return GetPlace() == Place::kFrame ? Now() : a_now; }
}
