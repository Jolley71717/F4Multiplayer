#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

// The watch on the frame chain (main.cpp). The chain: the F4SE task runs on the game's job threads and
// posts a frame message once a drawn frame; the window procedure takes it on the main thread; a frame's
// game work runs unless a gate stops it (a frame of ours already under way, the engine's own message
// pump, a loading screen, a save or load). Any link can stop for good without a crash or a freeze, and
// nothing said so: a host's game drew on and answered its window for a quarter of an hour after a coc
// with no frame of ours, every dev command timing out, the hang watch quiet and the log silent (the
// main thread sat in the engine's pump, where every frame message is put off). So a thread of ours
// looks at the chain once a second: when the game is alive (it draws, or the task runs) but no game
// work has run for a while, it says which link stopped and where the main thread is, and re-arms what
// it can (posts the frame message itself, hooks a window the engine made anew). The judgement is free
// of the game, so it is unit tested.
namespace FrameWatch
{
	using Clock = std::chrono::steady_clock;

	// How the last frame message the window procedure got ended (main.cpp records it; a save's or
	// load's wake is not a frame message).
	enum class Outcome : std::uint8_t
	{
		kNone,           // none yet
		kWorked,         // a frame's game work ran
		kSkipped,        // inside a frame of ours already (NestedPump::Action::kSkip)
		kDeferred,       // inside the engine's pump (NestedPump::Action::kDefer)
		kQuit,           // the game is quitting (NestedPump::Action::kQuit)
		kLoadingScreen,  // RunFrame: a loading screen is up
		kHeld,           // RunFrame: a save or load is under way
	};

	// Which link of the chain stopped.
	enum class Stop : std::uint8_t
	{
		kNone,           // game work ran lately
		kIdle,           // the game neither draws nor runs the task: paused, minimised or frozen (the hang watch's); nothing to run
		kQuitting,       // the game is quitting: nothing more runs, by design
		kNoWindow,       // no window hooked (not made yet, or the hook failed): no main thread to run on
		kNoTask,         // the F4SE task has not run: nothing posts the frame message
		kNotDispatched,  // posted, but the window procedure never got one: the window changed, or the message is lost
		kInFrame,        // the main thread is still inside a frame of ours (an engine call in it pumps messages)
		kDeferred,       // every frame message was put off: the main thread is inside the engine's pump
		kLoadingScreen,  // a loading screen is up, as far as the plugin heard
		kHeld,           // a save or load is under way
		kUnknown,        // the window procedure ran a frame lately, yet no game work came of it
	};

	// What main.cpp reads off the chain, once a tick.
	struct Sample
	{
		Clock::time_point now{};
		std::uint32_t     drawn = 0;  // the renderer's frame count
		bool              quitting = false;
		bool              hooked = false;
		Clock::time_point task{};        // when the F4SE task last ran
		Clock::time_point dispatched{};  // when the window procedure last got a frame message
		Clock::time_point worked{};      // when a frame's game work last ran
		Outcome           last = Outcome::kNone;
	};

	// Which link stopped, going along the chain: the first one that has not run within a_lately of
	// a_sample.now. The task is looked at before the game work: with the task dead, the frames that
	// run are the watch's own posts, and the task is still what is wrong.
	constexpr Stop Diagnose(const Sample& a_sample, Clock::duration a_lately)
	{
		const auto lately = [&](Clock::time_point a_when) { return a_sample.now - a_when < a_lately; };
		if (a_sample.quitting) {
			return Stop::kQuitting;
		}
		if (!a_sample.hooked) {
			return Stop::kNoWindow;
		}
		if (!lately(a_sample.task)) {
			return Stop::kNoTask;
		}
		if (lately(a_sample.worked)) {
			return Stop::kNone;
		}
		if (!lately(a_sample.dispatched)) {
			return Stop::kNotDispatched;
		}
		switch (a_sample.last) {
		case Outcome::kSkipped:
			return Stop::kInFrame;
		case Outcome::kDeferred:
			return Stop::kDeferred;
		case Outcome::kQuit:
			return Stop::kQuitting;
		case Outcome::kLoadingScreen:
			return Stop::kLoadingScreen;
		case Outcome::kHeld:
			return Stop::kHeld;
		default:
			return Stop::kUnknown;
		}
	}

	// Whether a_stop is worth reporting. The others are the game's own doing and end on their own
	// (or never, with nothing to be done: a window that could not be hooked was logged once).
	constexpr bool Reportable(Stop a_stop)
	{
		return a_stop != Stop::kNone && a_stop != Stop::kIdle && a_stop != Stop::kQuitting && a_stop != Stop::kNoWindow;
	}

	// Whether a_stop is one the watch can do something about: post the frame message itself (and hook
	// the game's window again). Done once a tick while the stop lasts past its limit.
	constexpr bool Rearmable(Stop a_stop)
	{
		return a_stop == Stop::kNoTask || a_stop == Stop::kNotDispatched;
	}

	// A loading screen, a save or a load hold frames by design, for as long as they take: their limit
	// is the long one.
	constexpr bool ByDesign(Stop a_stop)
	{
		return a_stop == Stop::kLoadingScreen || a_stop == Stop::kHeld;
	}

	constexpr const char* Name(Stop a_stop)
	{
		switch (a_stop) {
		case Stop::kNone:
			return "none";
		case Stop::kIdle:
			return "idle";
		case Stop::kQuitting:
			return "quitting";
		case Stop::kNoWindow:
			return "no window";
		case Stop::kNoTask:
			return "no task";
		case Stop::kNotDispatched:
			return "not dispatched";
		case Stop::kInFrame:
			return "in frame";
		case Stop::kDeferred:
			return "deferred";
		case Stop::kLoadingScreen:
			return "loading screen";
		case Stop::kHeld:
			return "held";
		default:
			return "unknown";
		}
	}

	struct Limits
	{
		Clock::duration lately{};     // a link that ran within this is alive
		Clock::duration stall{};      // a stop this long is reported
		Clock::duration longStall{};  // the same for a stop by design (ByDesign)
		Clock::duration again{};      // how often a stop is reported again while it lasts
	};

	class Watch
	{
	public:
		enum class Event : std::uint8_t
		{
			kNone,
			kStalled,  // a stop has lasted its limit (or another `again` since the last report)
			kResumed,  // game work runs again after a reported stop
		};

		struct Result
		{
			Event           event = Event::kNone;
			Stop            stop = Stop::kNone;  // the stop now (kNone: game work ran lately)
			bool            past = false;        // the stop has lasted its limit: re-arm (Rearmable) each tick
			Clock::duration stalled{};           // how long game work has been (or, on kResumed, was) stopped while the game was alive
			std::uint32_t   drawn = 0;           // frames the renderer drew meanwhile
		};

		explicit Watch(const Limits& a_limits) :
			limits(a_limits)
		{}

		Result Tick(const Sample& a_sample)
		{
			const auto drawnNow = lastDrawn && a_sample.drawn != *lastDrawn ? a_sample.drawn - *lastDrawn : 0u;
			const auto sinceLast = lastTick ? a_sample.now - *lastTick : Clock::duration{};
			lastDrawn = a_sample.drawn;
			lastTick = a_sample.now;
			// Alive: the renderer draws, or the task runs (a game paused in the background draws nothing,
			// yet serves its connection; one that does neither is frozen or shut down).
			const bool alive = drawnNow != 0 || a_sample.now - a_sample.task < limits.lately;
			auto       stop = Diagnose(a_sample, limits.lately);
			if (stop != Stop::kNone && !alive) {
				stop = Stop::kIdle;
			}
			if (stop == Stop::kNone) {
				Result result{ .stop = stop };
				if (reported) {
					result = { Event::kResumed, stop, false, stalled, drawn };
				}
				reported.reset();
				stalled = {};
				drawn = 0;
				return result;
			}
			if (!Reportable(stop)) {
				return { .stop = stop, .stalled = stalled, .drawn = drawn };  // not counted: the game's own doing
			}
			stalled += sinceLast;
			drawn += drawnNow;
			const bool past = stalled >= (ByDesign(stop) ? limits.longStall : limits.stall);
			if (past && (!reported || a_sample.now - *reported >= limits.again)) {
				reported = a_sample.now;
				return { Event::kStalled, stop, true, stalled, drawn };
			}
			return { Event::kNone, stop, past, stalled, drawn };
		}

	private:
		Limits                           limits;
		std::optional<std::uint32_t>     lastDrawn;
		std::optional<Clock::time_point> lastTick;
		std::optional<Clock::time_point> reported;  // when the stop was last reported
		Clock::duration                  stalled{};
		std::uint32_t                    drawn = 0;
	};
}
