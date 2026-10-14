#include "Test.h"

#include "game/Prediction.h"

#include <random>
#include <vector>

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

TEST("prediction: the sender clock stamps states as far apart as they were sent, however late each came")
{
	Prediction::SenderClock clock;
	const auto start = std::chrono::steady_clock::time_point{} + 10s;
	std::mt19937 random{ 7 };
	std::vector<std::chrono::steady_clock::time_point> stamps;
	// Sent every 30 to 50 ms, starting just before their clock wraps; each takes 30 ms and up to 80 ms more.
	std::uint32_t sentMs = 0xFFFFFF00u;
	auto          sentAt = start;
	std::vector<int> gaps;
	for (int i = 0; i < 200; ++i) {
		const int gap = 30 + static_cast<int>(random() % 21);
		sentMs += static_cast<std::uint32_t>(gap);
		sentAt += std::chrono::milliseconds(gap);
		gaps.push_back(gap);
		stamps.push_back(clock.Next(sentAt + 30ms + std::chrono::milliseconds(random() % 80), sentMs));
	}
	// Once it has seen the quickest trips (two seconds), each stamp is its gap after the last, give or take what
	// the shift may move in that time.
	for (std::size_t i = 70; i < stamps.size(); ++i) {
		const auto gap = std::chrono::duration<float, std::milli>(stamps[i] - stamps[i - 1]).count();
		CHECK(std::abs(gap - static_cast<float>(gaps[i])) < 2.5f);
	}
	// And they sit near the quickest trip.
	const auto trip = std::chrono::duration<float, std::milli>(stamps.back() - sentAt).count();
	CHECK(trip > 25.0f && trip < 45.0f);
}

TEST("prediction: the sender clock follows a slower route within seconds, and a burst of late states leaves it")
{
	Prediction::SenderClock clock;
	const auto start = std::chrono::steady_clock::time_point{} + 10s;
	std::uint32_t sentMs = 5000;
	auto          sentAt = start;
	auto          stamp = start;
	const auto send = [&](std::chrono::milliseconds a_trip) {
		sentMs += 33;
		sentAt += 33ms;
		stamp = clock.Next(sentAt + a_trip, sentMs);
	};
	for (int i = 0; i < 100; ++i) {
		send(30ms);
	}
	// Ten states held up together for 300 ms, then the rest as before: the stamps go on 33 ms apart.
	const auto before = stamp;
	for (int i = 0; i < 10; ++i) {
		send(std::chrono::milliseconds(330 - 33 * i));
	}
	CHECK(std::abs(std::chrono::duration<float, std::milli>(stamp - before).count() - 330.0f) < 1.0f);
	// The route takes 100 ms longer from now on: within ten seconds the stamps are that much later.
	for (int i = 0; i < 300; ++i) {
		send(130ms);
	}
	const auto trip = std::chrono::duration<float, std::milli>(stamp - sentAt).count();
	CHECK(trip > 125.0f && trip < 135.0f);
}
