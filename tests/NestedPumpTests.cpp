#include "Test.h"

#include "NestedPump.h"
#include "game/SaveLoadHold.h"

#include <array>
#include <atomic>
#include <functional>
#include <stdexcept>
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

	// The game's window as main.cpp drives it, with the real dispatch (NestedPump::Dispatch) and the hold main.cpp uses:
	// the F4SE task posts a frame message once a drawn frame while the last is not taken, the window procedure takes
	// each message from wherever it is dispatched (a_stack), and a frame does its game work under the hold.
	struct Window
	{
		SaveLoadHold::Hold&               hold;
		bool                              inFrame = false;
		bool                              posted = false;  // main.cpp's framePosted
		std::vector<std::uintptr_t>       stack;           // where the next message is dispatched from
		int                               framesRun = 0;
		int                               deferred = 0;
		int                               checks = 0;      // stack walks
		std::function<void()>             duringFrame;     // what the frame's game work does (an engine call may pump messages)
		bool                              quitting = false;  // the game is quitting (main.cpp's Quitting)

		// Calls (see NestedPump::Dispatch).
		void             Taken() { posted = false; }
		bool             Quitting() const { return quitting; }
		NestedPump::Seen Where()
		{
			++checks;
			return NestedPump::Classify(stack, PUMP, OUTER);
		}
		void Posted() { hold.RunPosted(); }
		void Frame()
		{
			hold.RunPosted();  // RunFrame: what a save or load handed over comes first
			if (const SaveLoadHold::Frame frame{ hold, SaveLoadHold::Clock::now() }) {
				++framesRun;
				if (duringFrame) {
					duringFrame();
				}
			}
		}
		void Deferred() { ++deferred; }

		// The F4SE task, once a drawn frame (OnFrame): posts a frame message unless one is waiting. True if it did.
		bool DrawnFrame()
		{
			return !std::exchange(posted, true);
		}

		NestedPump::Action Dispatch(bool a_wake, const std::vector<std::uintptr_t>& a_stack)
		{
			stack = a_stack;
			return NestedPump::Dispatch(a_wake, inFrame, *this);
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

TEST("nested pump: a frame message put off inside the engine's pump lets the next drawn frame post another, so play resumes the frame the move ends")
{
	SaveLoadHold::Hold hold{ 60s };
	Window             window{ hold };
	CHECK(window.DrawnFrame());
	CHECK(window.Dispatch(false, IN_PUMP) == Action::kDefer);
	// Still inside the move: each drawn frame posts one, and each is put off, none piling up.
	for (int i = 0; i < 3; ++i) {
		CHECK(window.DrawnFrame());
		CHECK(window.Dispatch(false, IN_PUMP) == Action::kDefer);
	}
	CHECK(window.framesRun == 0);
	// The move is done: the very next drawn frame's message runs (not one reposted after REPOST_AFTER).
	CHECK(window.DrawnFrame());
	CHECK(!window.DrawnFrame());  // one waiting already
	CHECK(window.Dispatch(false, ORDINARY) == Action::kRunFrame);
	CHECK(window.framesRun == 1);
	CHECK(window.deferred == 4);
}

TEST("nested pump: inside a frame of ours, what an engine call pumps (a frame message or a load's wake) runs nothing and walks no stack")
{
	SaveLoadHold::Hold hold{ 60s };
	Window             window{ hold };
	std::vector<Action> inside;
	window.duringFrame = [&] {
		inside.push_back(window.Dispatch(false, IN_PUMP));
		inside.push_back(window.Dispatch(true, IN_PUMP));
		inside.push_back(window.Dispatch(false, ORDINARY));
	};
	CHECK(window.Dispatch(false, ORDINARY) == Action::kRunFrame);
	const std::vector<Action> skipped{ Action::kSkip, Action::kSkip, Action::kSkip };
	CHECK(inside == skipped);
	CHECK(window.framesRun == 1);
	CHECK(window.checks == 1);  // only the outer message's
	CHECK(!window.inFrame);

	// A wake outside a frame runs the work handed over without a walk either.
	bool ran = false;
	hold.Post([&] { ran = true; });
	CHECK(window.Dispatch(true, ORDINARY) == Action::kRunPosted);
	CHECK(ran);
	CHECK(window.checks == 1);
}

TEST("nested pump: a frame whose game work throws leaves the window running the next frame, and no load waiting on it")
{
	SaveLoadHold::Hold hold{ 60s };
	Window             window{ hold };
	window.duringFrame = [] { throw std::runtime_error("game work failed"); };
	bool thrown = false;
	try {
		window.Dispatch(false, ORDINARY);
	} catch (const std::runtime_error&) {
		thrown = true;  // main.cpp's WndProc catches it, the stack check's too
	}
	CHECK(thrown);
	CHECK(!window.inFrame);
	CHECK(hold.WaitForFrames(0s));  // the frame left the hold: a load beginning now does not wait for it
	window.duringFrame = nullptr;
	CHECK(window.Dispatch(false, ORDINARY) == Action::kRunFrame);
	CHECK(window.framesRun == 2);
}

TEST("nested pump: once the game quits, no frame message or load's wake does game work or walks the stack, and a load's thread takes its work back")
{
	SaveLoadHold::Hold hold{ 60s };
	Window             window{ hold };
	CHECK(window.Dispatch(false, ORDINARY) == Action::kRunFrame);
	CHECK(window.framesRun == 1);
	CHECK(window.checks == 1);

	// Work a load handed over is waiting when the player quits (the menu's Quit, qqq). From then on the engine tears the
	// world down: whatever dispatches our messages (its loop's last turn, Main::Shutdown), nothing runs.
	bool tookOut = false;
	hold.Post([&] { tookOut = true; });
	window.quitting = true;
	CHECK(window.DrawnFrame());
	CHECK(window.Dispatch(false, ORDINARY) == Action::kQuit);
	CHECK(window.Dispatch(false, IN_PUMP) == Action::kQuit);
	CHECK(window.Dispatch(true, ORDINARY) == Action::kQuit);
	CHECK(window.framesRun == 1);
	CHECK(window.checks == 1);  // no stack walked either
	CHECK(window.deferred == 0);
	CHECK(!window.inFrame);
	CHECK(!tookOut);
	// The load's thread, after its wait, takes back what the main thread never ran and finds no frame working.
	const auto left = hold.TakeBack();
	CHECK(left.size() == 1u);
	CHECK(hold.WaitForFrames(0s));
}

TEST("nested pump: a quit that comes inside a frame of ours lets that frame finish, and what an engine call pumps after it runs nothing")
{
	SaveLoadHold::Hold hold{ 60s };
	Window             window{ hold };
	std::vector<Action> inside;
	window.duringFrame = [&] {
		window.quitting = true;  // e.g. a script's QuitGame, or the window closed
		inside.push_back(window.Dispatch(false, ORDINARY));
		inside.push_back(window.Dispatch(true, IN_PUMP));
	};
	CHECK(window.Dispatch(false, ORDINARY) == Action::kRunFrame);
	const std::vector<Action> quit{ Action::kQuit, Action::kQuit };
	CHECK(inside == quit);
	CHECK(window.framesRun == 1);
	CHECK(!window.inFrame);
	window.duringFrame = nullptr;
	CHECK(window.Dispatch(false, ORDINARY) == Action::kQuit);
	CHECK(window.framesRun == 1);
	CHECK(NestedPump::Choose(true, false, false, Seen::kOuter) == Action::kQuit);
	CHECK(NestedPump::Choose(true, true, true, Seen::kNested) == Action::kQuit);
}

TEST("nested pump: inside a frame of ours nothing more runs, a wake included; outside it a wake never waits for the stack check")
{
	CHECK(NestedPump::Choose(false, true, true, Seen::kOuter) == Action::kSkip);
	CHECK(NestedPump::Choose(false, false, true, Seen::kNested) == Action::kSkip);
	CHECK(NestedPump::Choose(false, true, false, Seen::kNested) == Action::kRunPosted);
	CHECK(NestedPump::Choose(false, false, false, Seen::kNeither) == Action::kRunFrame);  // a walk that cannot tell changes nothing
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
