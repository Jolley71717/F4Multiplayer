#include "Test.h"

#include "game/StrandedDialogue.h"

using namespace std::chrono_literals;

TEST("stranded dialogue: a conversation that is fine is never ended")
{
	StrandedDialogue::Timer timer;
	const auto              start = StrandedDialogue::Clock::now();
	CHECK(!timer.Due(false, start));
	CHECK(!timer.Due(false, start + 10s));
	CHECK(timer.Ends() == 0);
}

TEST("stranded dialogue: ended once it has been without its speaker for the grace time")
{
	StrandedDialogue::Timer timer;
	const auto              start = StrandedDialogue::Clock::now();
	CHECK(!timer.Due(true, start));
	CHECK(!timer.Due(true, start + 1900ms));
	CHECK(timer.Due(true, start + StrandedDialogue::GRACE));
	CHECK(timer.Ends() == 1);
}

TEST("stranded dialogue: a speaker that comes back in time starts the wait over")
{
	StrandedDialogue::Timer timer;
	const auto              start = StrandedDialogue::Clock::now();
	CHECK(!timer.Due(true, start));
	CHECK(!timer.Due(false, start + 1s));
	CHECK(!timer.Due(true, start + 1500ms));
	CHECK(!timer.Due(true, start + 3s));
	CHECK(timer.Due(true, start + 3500ms));
}

TEST("stranded dialogue: an end that didn't take is tried again after another grace time")
{
	StrandedDialogue::Timer timer;
	const auto              start = StrandedDialogue::Clock::now();
	CHECK(!timer.Due(true, start));
	CHECK(timer.Due(true, start + 2s));
	CHECK(!timer.Due(true, start + 2s + 100ms));
	CHECK(!timer.Due(true, start + 3900ms));
	CHECK(timer.Due(true, start + 4s));
	CHECK(timer.Ends() == 2);
	CHECK(!timer.Due(false, start + 5s));
	CHECK(timer.Ends() == 0);
}
