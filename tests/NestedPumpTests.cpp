#include "Test.h"

#include "NestedPump.h"
#include "game/SaveLoadHold.h"

#include <array>
#include <atomic>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using NestedPump::Action;
using NestedPump::Range;
using NestedPump::Seen;

namespace
{
	// Made-up addresses laid out as in the game: the engine's pump while it moves the player, the game's message loop,
	// and Main::Run, which calls it.
	constexpr Range                PUMP{ 0x140C3B030, 0x140C3B11F };
	constexpr std::array<Range, 2> OUTER{ Range{ 0x140C336B0, 0x140C33943 }, Range{ 0x140C314F0, 0x140C316F7 } };
	constexpr std::uintptr_t       USER32 = 0x7FF8'1234'5678;  // DispatchMessage and the window procedure's own frames

	// The return addresses from our window procedure outward, for a message the game's own loop dispatched, and for one
	// the engine's pump dispatched while it moved the player (called from the game's loop, which is on the stack too).
	const std::vector<std::uintptr_t> ORDINARY{ USER32, USER32 + 0x40, 0x140C33790, 0x140C31523 };
	const std::vector<std::uintptr_t> IN_PUMP{ USER32, USER32 + 0x40, 0x140C3B0E6, 0x140A10000, 0x140C33800, 0x140C31523 };

	// What main.cpp's window procedure does with our message, with the hold it uses: the game's work for a frame is
	// counted, a wake runs what a save's or load's thread handed over.
	struct Window
	{
		SaveLoadHold::Hold& hold;
		bool                inFrame = false;
		int                 framesRun = 0;
		int                 deferred = 0;

		void Dispatch(bool a_wake, const std::vector<std::uintptr_t>& a_stack)
		{
			switch (NestedPump::Choose(a_wake, inFrame, NestedPump::Classify(a_stack, PUMP, OUTER))) {
			case Action::kSkip:
				break;
			case Action::kRunPosted:
				hold.RunPosted();
				break;
			case Action::kRunFrame:
				hold.RunPosted();
				if (const SaveLoadHold::Frame frame{ hold, SaveLoadHold::Clock::now() }) {
					++framesRun;
				}
				break;
			case Action::kDefer:
				++deferred;
				break;
			}
		}
	};
}

TEST("nested pump: a frame message dispatched inside the engine's pump is told from an ordinary one, whatever else is on the stack")
{
	CHECK(NestedPump::Classify(ORDINARY, PUMP, OUTER) == Seen::kOuter);
	CHECK(NestedPump::Classify(IN_PUMP, PUMP, OUTER) == Seen::kNested);
	CHECK(NestedPump::Classify(std::vector<std::uintptr_t>{ USER32 }, PUMP, OUTER) == Seen::kNeither);  // the walk stopped short
	CHECK(NestedPump::Classify(std::vector<std::uintptr_t>{ PUMP.end, OUTER[0].begin - 1 }, PUMP, OUTER) == Seen::kNeither);  // just past each
	CHECK(NestedPump::Classify(std::vector<std::uintptr_t>{ PUMP.begin }, PUMP, OUTER) == Seen::kNested);
	// A pump that could not be found in the game's unwind table is never found on the stack either.
	CHECK(NestedPump::Classify(IN_PUMP, Range{}, OUTER) == Seen::kOuter);
}

TEST("nested pump: a door taken while a load waits: the load's wake is served inside the engine's pump, its frames are put off, and play resumes after")
{
	SaveLoadHold::Hold hold{ 60s };
	Window             window{ hold };
	window.Dispatch(false, ORDINARY);
	CHECK(window.framesRun == 1);

	// A load begins on its own thread and hands the main thread the stand-ins to take out, waiting for it.
	std::atomic<bool> tookOut = false;
	const auto        number = hold.Begin(SaveLoadHold::Clock::now());
	hold.Post([&] { tookOut = true; });
	std::atomic<bool> served = false;
	std::thread       load{ [&] { served = hold.WaitForFrames(5s); } };

	// The main thread is inside the engine's pump, moving the player. Our frame messages there are put off and do no
	// game work; the load's wake is served at once.
	window.Dispatch(false, IN_PUMP);
	window.Dispatch(false, IN_PUMP);
	CHECK(window.deferred == 2);
	CHECK(!tookOut);
	window.Dispatch(true, IN_PUMP);
	CHECK(tookOut);
	load.join();
	CHECK(served);  // the load did not wait for the main-thread fallback

	// The load ends; no frame was held for it (none was run inside the pump), and the next ordinary frame runs.
	const auto held = hold.End(number);
	REQUIRE(held.has_value());
	CHECK(*held == 0u);
	window.Dispatch(false, ORDINARY);
	CHECK(window.framesRun == 2);
	CHECK(window.deferred == 2);
}

TEST("nested pump: inside a frame of ours nothing more runs, a wake included; outside it a wake never waits for the stack check")
{
	CHECK(NestedPump::Choose(true, true, Seen::kOuter) == Action::kSkip);
	CHECK(NestedPump::Choose(false, true, Seen::kNested) == Action::kSkip);
	CHECK(NestedPump::Choose(true, false, Seen::kNested) == Action::kRunPosted);
	CHECK(NestedPump::Choose(false, false, Seen::kNeither) == Action::kRunFrame);  // a walk that cannot tell changes nothing
}

TEST("nested pump: the first frame message decides whether the check works, once")
{
	auto check = NestedPump::Check::kUnknown;
	CHECK(NestedPump::Decide(check, Seen::kOuter));
	CHECK(check == NestedPump::Check::kOk);
	CHECK(!NestedPump::Decide(check, Seen::kNeither));
	CHECK(check == NestedPump::Check::kOk);
	check = NestedPump::Check::kUnknown;
	CHECK(NestedPump::Decide(check, Seen::kNeither));
	CHECK(check == NestedPump::Check::kBroken);
	CHECK(std::string_view{ NestedPump::Name(check) } == "broken");
	check = NestedPump::Check::kUnknown;
	CHECK(NestedPump::Decide(check, Seen::kNested));  // found inside the pump: the walk works
	CHECK(check == NestedPump::Check::kOk);
}
