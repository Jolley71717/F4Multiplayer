#include "FrameDriver.h"

#include <Windows.h>

// wingdi.h defines ERROR, which collides with REX::ERROR.
#undef ERROR

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#	define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002  // Windows 10 1803 on
#endif

namespace FrameDriver
{
	namespace
	{
		// A frame at 60 fps is 16.7 ms: a tick every millisecond sees each new frame within a millisecond of its drawing,
		// as the task did once a frame.
		constexpr LONGLONG TICK_100NS = -10'000;  // relative, in 100 ns units
		constexpr DWORD    TICK_MS = 1;

		void Run(void (*a_tick)(), bool (*a_done)())
		{
			// A high-resolution waitable timer keeps the tick near a millisecond whatever the system's timer resolution;
			// where there is none (older Windows, some Wine), Sleep(1), which the game's own timer resolution decides.
			HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
			REX::INFO("Frame: old-gen F4SE has no permanent task; a thread of ours posts the frame message instead ({})",
				timer ? "a high-resolution timer" : "Sleep");
			for (;;) {
				// An exception must never end this thread (std::terminate would take the game with it).
				try {
					if (a_done()) {
						break;
					}
					a_tick();
				} catch (const std::exception& e) {
					REX::ERROR("Frame driver: {}", e.what());
				}
				LARGE_INTEGER due{};
				due.QuadPart = TICK_100NS;
				if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
					WaitForSingleObject(timer, INFINITE);
				} else {
					Sleep(TICK_MS);
				}
			}
			if (timer) {
				CloseHandle(timer);
			}
		}
	}

	bool Start(void (*a_tick)(), bool (*a_done)())
	{
		try {
			std::thread{ Run, a_tick, a_done }.detach();
			return true;
		} catch (const std::exception& e) {
			REX::ERROR("Frame: couldn't start the frame driver ({}); multiplayer is off this run", e.what());
			return false;
		}
	}
}
