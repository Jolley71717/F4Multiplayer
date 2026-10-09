#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>

// Frame-time bookkeeping for the dev channel: how long frames take and how many hitched, so a
// stutter can be told apart from a slow recording without a camera. Pure, so it is unit tested.
namespace FrameStats
{
	// A frame longer than this counts as a hitch (a 60 fps game takes 17 ms; 100 ms is visible).
	constexpr auto HITCH = std::chrono::milliseconds(100);

	struct Window
	{
		std::uint32_t frames = 0;
		std::uint32_t hitches = 0;
		std::chrono::steady_clock::duration total{};
		std::chrono::steady_clock::duration worst{};

		void Add(std::chrono::steady_clock::duration a_frame)
		{
			++frames;
			total += a_frame;
			worst = (std::max)(worst, a_frame);
			if (a_frame >= HITCH) {
				++hitches;
			}
		}

		// Mean frame time in milliseconds (0 without frames).
		float MeanMs() const
		{
			return frames ? std::chrono::duration<float, std::milli>(total).count() / static_cast<float>(frames) : 0.0f;
		}

		float WorstMs() const { return std::chrono::duration<float, std::milli>(worst).count(); }

		// "frames=N mean=X.Xms worst=Y.Yms hitches=H"
		std::string Describe() const
		{
			return "frames=" + std::to_string(frames) + " mean=" + std::to_string(static_cast<int>(MeanMs() * 10.0f) / 10.0f).substr(0, 4) + "ms worst=" +
			       std::to_string(static_cast<int>(WorstMs())) + "ms hitches=" + std::to_string(hitches);
		}
	};

	// Call once per frame from the game thread; Take() returns what accumulated since the last
	// Take() and starts over. The first call only sets the clock.
	class Tracker
	{
	public:
		void Frame(std::chrono::steady_clock::time_point a_now)
		{
			if (last != std::chrono::steady_clock::time_point{}) {
				window.Add(a_now - last);
			}
			last = a_now;
		}

		Window Take()
		{
			Window out = window;
			window = Window{};
			return out;
		}

	private:
		std::chrono::steady_clock::time_point last{};
		Window window;
	};

	Tracker& Global();
}
