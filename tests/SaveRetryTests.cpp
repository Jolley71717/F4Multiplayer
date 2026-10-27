#include "SaveRetry.h"
#include "Test.h"

using namespace std::chrono_literals;

namespace
{
	const SaveRetry::Clock::time_point T0 = SaveRetry::Clock::time_point{} + 1h;
}

TEST("save retry: nothing is due while no save has failed")
{
	SaveRetry retry{ 15s };
	CHECK(!retry.Failing());
	CHECK(!retry.Due(T0));
	CHECK(!retry.Due(T0 + 24h));
	CHECK(!retry.Succeeded());  // nothing to say about a save that never failed
}

TEST("save retry: a failed save is due again after one interval, whatever else changed")
{
	SaveRetry retry{ 15s };
	CHECK(retry.Failed(T0));
	CHECK(retry.Failing());
	CHECK(!retry.Due(T0));
	CHECK(!retry.Due(T0 + 14s));
	CHECK(retry.Due(T0 + 15s));
}

TEST("save retry: the wait doubles after each failure and stops growing at eight intervals")
{
	SaveRetry retry{ 1s };
	auto      now = T0;
	const std::chrono::seconds expected[] = { 1s, 2s, 4s, 8s, 8s, 8s };
	for (const auto wait : expected) {
		retry.Failed(now);
		CHECK(retry.Wait() == wait);
		CHECK(!retry.Due(now + wait - 1ms));
		CHECK(retry.Due(now + wait));
		now += wait;
	}
}

TEST("save retry: a file that stays shut is tried once a minute at most")
{
	SaveRetry retry{ 15s };
	for (int i = 0; i < 100; ++i) {
		retry.Failed(T0);
	}
	CHECK(retry.Wait() == 60s);
	CHECK(!retry.Due(T0 + 59s));
	CHECK(retry.Due(T0 + 60s));
}

TEST("save retry: a streak of failures is said once, and so is its end")
{
	SaveRetry retry{ 1s };
	CHECK(retry.Failed(T0));
	CHECK(!retry.Failed(T0 + 1s));
	CHECK(!retry.Failed(T0 + 3s));
	CHECK(retry.Succeeded());
	CHECK(!retry.Failing());
	CHECK(!retry.Due(T0 + 1h));
	CHECK(!retry.Succeeded());

	// The next streak is said again, and starts from the short wait.
	CHECK(retry.Failed(T0 + 2h));
	CHECK(retry.Wait() == 1s);
}

TEST("save retry: a zero interval still waits and still never overflows")
{
	SaveRetry retry{ 0ms };
	retry.Failed(T0);
	CHECK(retry.Wait() == 1ms);
	for (int i = 0; i < 1000; ++i) {
		retry.Failed(T0);
	}
	CHECK(retry.Wait() == 8ms);
}

TEST("save retry: an interval longer than the longest wait is waited as it is, not cut short")
{
	SaveRetry retry{ 120s };
	retry.Failed(T0);
	CHECK(retry.Wait() == 120s);
	retry.Failed(T0);
	CHECK(retry.Wait() == 120s);
	CHECK(!retry.Due(T0 + 119s));
	CHECK(retry.Due(T0 + 120s));
}
