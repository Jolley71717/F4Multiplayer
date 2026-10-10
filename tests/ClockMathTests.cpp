#include "Test.h"

#include "game/ClockMath.h"

TEST("clock: the hour gap is the shortest way round the clock")
{
	CHECK(std::abs(ClockMath::HourGap(12.0, 9.0) - 3.0) < 1e-9);
	CHECK(std::abs(ClockMath::HourGap(9.0, 12.0) + 3.0) < 1e-9);
	CHECK(std::abs(ClockMath::HourGap(1.0, 23.0) - 2.0) < 1e-9);
	CHECK(std::abs(ClockMath::HourGap(23.0, 1.0) + 2.0) < 1e-9);
	CHECK(std::abs(ClockMath::HourGap(10.0, 10.0)) < 1e-9);
}

TEST("clock: lining up moves forward, or back within the day")
{
	CHECK(std::abs(ClockMath::LineUp(12.0, 9.0) - 3.0) < 1e-9);   // forward 3
	CHECK(std::abs(ClockMath::LineUp(9.0, 12.0) + 3.0) < 1e-9);   // back 3, same day
	CHECK(std::abs(ClockMath::LineUp(23.0, 1.0) - 22.0) < 1e-9);  // never back across midnight: forward 22
}
