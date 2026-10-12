#pragma once

#include <chrono>
#include <cstdint>

// Whether a puppet's animation graph is told to start or stop moving. With its AI off nothing else tells
// it, and what we last told it is not always what it does: a stop sent mid-jump, mid-landing or
// mid-stagger is refused (only a locomotion state takes "moveStop"), and a graph back from a jump, a
// stagger, sneaking or a weapon change starts again in idle or locomotion by iSyncIdleLocomotion, which
// nothing kept for an actor with no AI. A friend's stand-in then walked on the spot while they stood
// still (the walk plays at WalkSpeedMult whatever Speed says, and its root motion pulled it off its spot
// every frame), and no stop was ever sent again. So while nothing changes, the graph is told again every
// RESYNC; a graph that agrees refuses it (standing takes only "moveStart", moving only "moveStop").
// Kept free of the game so it can be tested.
namespace MoveSync
{
	using Clock = std::chrono::steady_clock;

	// Locomotion starts above START and stops below STOP (units/second).
	constexpr float START = 25.0f;
	constexpr float STOP = 10.0f;
	// While a puppet keeps moving or standing, its graph is told so again this often.
	constexpr auto RESYNC = std::chrono::milliseconds(500);

	enum class Event
	{
		kStart,  // "moveStart"
		kStop,   // "moveStop"
	};

	struct Step
	{
		bool              moving = false;  // what the graph was told last, and as far as we can tell does
		Clock::time_point nextResync{};    // when it is told again
		const char*       resent = nullptr;  // a resend the graph took (it was in the other state), for the log
	};

	// One frame: a_moving and a_nextResync as the last step left them, a_speed the owner's copy's speed.
	// a_notify(Event) sends the event and says whether the graph took it.
	template <class Notify>
	Step Drive(bool a_moving, Clock::time_point a_nextResync, float a_speed, Clock::time_point a_now, Notify&& a_notify)
	{
		Step       step{ a_moving, a_nextResync };
		const bool due = a_now >= a_nextResync;
		bool       resync = false;
		if (!a_moving && a_speed > START) {
			step.moving = a_notify(Event::kStart);
			// A graph that walks already refuses the start too (a stand-in back in its walk, a mirror its own AI
			// had walking when mirroring began), and it went on walking at a walk's pace (Speed 0) while its
			// owner's copy ran: a stop it takes shows that, and it starts again. A graph held by an idle (a dog
			// sitting) refuses the stop as well, only a locomotion state takes it, and is left to IdleRelease.
			if (!step.moving && due) {
				resync = true;
				if (a_notify(Event::kStop)) {
					step.moving = a_notify(Event::kStart);
					step.resent = "moveStop and moveStart";
				}
			}
		} else if (a_moving && a_speed < STOP) {
			// Taken for done even if refused (mid-jump): the resync below sends it again.
			a_notify(Event::kStop);
			step.moving = false;
		} else if (due) {
			resync = true;
			if (a_notify(a_moving ? Event::kStart : Event::kStop)) {
				step.resent = a_moving ? "moveStart" : "moveStop";
			}
		}
		if (resync || step.moving != a_moving) {
			step.nextResync = a_now + RESYNC;
		}
		return step;
	}

	// Whether the a_count-th time something keeps happening is worth a log line: the first few, then every
	// 50th (one that keeps coming back means something else drives the graph).
	constexpr bool Loggable(std::uint32_t a_count)
	{
		return a_count <= 5 || a_count % 50 == 0;
	}
}
