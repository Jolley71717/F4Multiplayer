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
