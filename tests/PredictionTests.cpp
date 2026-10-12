#include "Test.h"

#include "game/Prediction.h"

using namespace std::chrono_literals;

TEST("prediction: a late update is projected ahead, but never more than the cap")
{
	CHECK(Prediction::AheadSeconds(0ms) == 0.0f);
	CHECK(Prediction::AheadSeconds(-50ms) == 0.0f);
	CHECK(std::abs(Prediction::AheadSeconds(100ms) - 0.1f) < 0.001f);
	CHECK(std::abs(Prediction::AheadSeconds(150ms) - 0.15f) < 0.001f);
	CHECK(std::abs(Prediction::AheadSeconds(2s) - 0.15f) < 0.001f);
}

TEST("prediction: the animation speed follows the measured speed smoothly")
{
	float speed = 0.0f;
	for (int i = 0; i < 20; ++i) {
		speed = Prediction::SmoothSpeed(speed, 588.0f);
	}
	CHECK(speed > 580.0f);
	speed = Prediction::SmoothSpeed(speed, 0.0f);
	CHECK(speed < 300.0f && speed > 280.0f);  // one window of stopping halves it
}

TEST("prediction: the speed window measures distance over time, whatever the call pattern")
{
	Prediction::SpeedWindow w;
	// 30 frames at 33 ms, 12 units each (364 u/s), with a duplicate call (same target, no time) every frame.
	for (int i = 0; i < 30; ++i) {
		w.Add(12.0f, 0.033f);
		w.Add(0.0f, 0.0f);
	}
	CHECK(w.speed > 340.0f && w.speed < 380.0f);
	// Standing still brings it down window by window.
	for (int i = 0; i < 30; ++i) {
		w.Add(0.0f, 0.033f);
	}
	CHECK(w.speed < 5.0f);
	w.Reset(100.0f);
	CHECK(w.speed == 100.0f && w.seconds == 0.0f);
}

TEST("prediction: states read in bursts by a slow game are stamped evenly")
{
	// The sender makes a state every 33 ms; a 16 fps game reads them at 62 ms frames, one or two at a time.
	Prediction::ArrivalClock clock;
	const auto start = std::chrono::steady_clock::time_point{} + 10s;
	std::vector<std::chrono::steady_clock::time_point> stamps;
	auto lastArrival = start;
	for (int sent = 0, frame = 0; sent < 120; ++frame) {
		const auto now = start + frame * 62ms;
		while (sent < 120 && sent * 33ms <= frame * 62ms) {
			stamps.push_back(clock.Next(now));
			lastArrival = now;
			++sent;
		}
	}
	// After it settles, no two stamps are equal and the gaps stay near 33 ms (the frames alone give 0 and 62).
	for (std::size_t i = 40; i < stamps.size(); ++i) {
		const auto gap = std::chrono::duration<float, std::milli>(stamps[i] - stamps[i - 1]).count();
		CHECK(gap > 20.0f && gap < 46.0f);
	}
	// And it keeps up with the real arrivals instead of drifting away from them.
	const auto lag = std::chrono::duration<float, std::milli>(lastArrival - stamps.back()).count();
	CHECK(lag > -60.0f && lag < 60.0f);
}

TEST("prediction: the arrival clock starts over after a silence and counts lost states")
{
	Prediction::ArrivalClock clock;
	const auto start = std::chrono::steady_clock::time_point{} + 10s;
	for (int i = 0; i < 30; ++i) {
		clock.Next(start + i * 33ms);
	}
	const auto resumed = start + 5s;
	CHECK(clock.Next(resumed) == resumed);  // the game was paused: no easing across it
	const auto next = clock.Next(resumed + 99ms, 3);  // two states were lost on the way
	const auto gap = std::chrono::duration<float, std::milli>(next - resumed).count();
	CHECK(gap > 85.0f && gap < 105.0f);
}
