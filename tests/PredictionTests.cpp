#include "Test.h"

#include "game/Prediction.h"

#include <algorithm>
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

TEST("prediction: a friend's first state that came late leaves the sender clock's stamps late for a moment, not for seconds")
{
	for (const auto late : { 300ms, 1000ms }) {
		Prediction::SenderClock clock;
		const auto start = std::chrono::steady_clock::time_point{} + 10s;
		std::uint32_t sentMs = 5000;
		auto          sentAt = start;
		auto          stamp = start;
		std::vector<std::chrono::steady_clock::duration> steppedBack;
		const auto send = [&](std::chrono::milliseconds a_trip) {
			sentMs += 33;
			sentAt += 33ms;
			stamp = clock.Next(sentAt + a_trip, sentMs);
			steppedBack.push_back(clock.steppedBack);
		};
		// Our game hitched over the welcome: the first state is handled `late` after the 30 ms trip, the rest on time.
		send(30ms + late);
		for (int i = 0; i < 30; ++i) {
			send(30ms);
		}
		// A second on, the stamps sit at the real trip again, not `late` behind it.
		const auto trip = std::chrono::duration<float, std::milli>(stamp - sentAt).count();
		CHECK(trip > 25.0f && trip < 35.0f);
		// They stepped back once, by as much as the first state was late, so the stamps already given can follow.
		CHECK(std::ranges::count_if(steppedBack, [](auto a_back) { return a_back > 0ms; }) == 1);
		CHECK(std::abs(std::chrono::duration<float, std::milli>(steppedBack[1] - late).count()) < 1.0f);
	}
}

TEST("prediction: once the sender clock has its window of trips, a quicker route is still followed gently")
{
	Prediction::SenderClock clock;
	const auto start = std::chrono::steady_clock::time_point{} + 10s;
	std::uint32_t sentMs = 5000;
	auto          sentAt = start;
	auto          stamp = start;
	auto          arrival = start;
	const auto send = [&](std::chrono::milliseconds a_trip) {
		sentMs += 33;
		sentAt += 33ms;
		arrival = (std::max)(sentAt + a_trip, arrival);  // our game handles them in order
		stamp = clock.Next(arrival, sentMs);
	};
	for (int i = 0; i < 100; ++i) {
		send(130ms);
	}
	// The route gets 100 ms quicker: the stamps keep coming 33 ms apart, less what the shift may move, instead of
	// stepping back at once.
	auto before = stamp;
	for (int i = 0; i < 10; ++i) {
		send(30ms);
		const auto gap = std::chrono::duration<float, std::milli>(stamp - before).count();
		CHECK(gap > 31.0f && gap < 34.0f);
		CHECK(clock.steppedBack == std::chrono::steady_clock::duration::zero());
		before = stamp;
	}
	// And within ten seconds they are that much earlier.
	for (int i = 0; i < 300; ++i) {
		send(30ms);
	}
	const auto trip = std::chrono::duration<float, std::milli>(stamp - sentAt).count();
	CHECK(trip > 25.0f && trip < 35.0f);
}

TEST("prediction: the speed window closes at exactly a tenth of a second, not before")
{
	Prediction::SpeedWindow w;
	// A step that leaves the window short only adds up: no new speed, and the call says so.
	CHECK(!w.Add(5.0f, 0.04f));
	CHECK(w.speed == 0.0f);
	CHECK(w.distance == 5.0f && w.seconds == 0.04f);
	CHECK(!w.Add(0.0f, 0.0f));  // the duplicate call of a frame that set its target twice
	CHECK(w.speed == 0.0f);

	// Exactly WINDOW seconds in one step closes it: the speed is half the way to what was covered (10 units in a
	// tenth of a second, 100 a second, smoothed from standing still), and the next window starts empty.
	Prediction::SpeedWindow exact;
	CHECK(exact.Add(10.0f, Prediction::SpeedWindow::WINDOW));
	CHECK(std::abs(exact.speed - 50.0f) < 0.001f);
	CHECK(exact.distance == 0.0f && exact.seconds == 0.0f);

	// The same reached in two steps closes on the second, not the first.
	Prediction::SpeedWindow split;
	CHECK(!split.Add(4.0f, 0.05f));
	CHECK(split.Add(6.0f, 0.05f));
	CHECK(split.speed > 0.0f);
}

TEST("prediction: the arrival clock keeps a state exactly RESYNC off the beat on its timeline, and starts over past it")
{
	using Clock = Prediction::ArrivalClock::Clock;
	const auto start = Clock::time_point{} + 10s;
	const auto resync = std::chrono::duration_cast<Clock::duration>(Prediction::ArrivalClock::RESYNC);

	// A sender whose usual gap is half a second: the state after the first is expected 500 ms on. One that comes
	// RESYNC early is not that far off the beat yet (it is not less than RESYNC after the last either, so the usual
	// gap stays what it was): it is eased an eighth of the way to when it came, not stamped when it came.
	{
		Prediction::ArrivalClock clock;
		clock.Next(start);
		clock.gap = 0.5f;
		CHECK(clock.Next(start + resync) == start + 500ms - 31250000ns);
	}
	// A nanosecond earlier is a restart: stamped when it came.
	{
		Prediction::ArrivalClock clock;
		clock.Next(start);
		clock.gap = 0.5f;
		const auto arrived = start + resync - 1ns;
		CHECK(clock.Next(arrived) == arrived);
	}
	// And the same on the late side: exactly RESYNC late is eased, a nanosecond more is a restart.
	{
		Prediction::ArrivalClock clock;
		clock.Next(start);
		clock.gap = 0.5f;
		CHECK(clock.Next(start + 500ms + resync) == start + 500ms + 31250000ns);
	}
	{
		Prediction::ArrivalClock clock;
		clock.Next(start);
		clock.gap = 0.5f;
		const auto arrived = start + 500ms + resync + 1ns;
		CHECK(clock.Next(arrived) == arrived);
	}
}
