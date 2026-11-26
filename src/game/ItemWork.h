#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>

// When an actor is safe for item work: giving, equipping, taking away or removing items. Every such change queues
// work in the engine for the actor (TaskQueueInterface::QueueInventoryItemStackUpdated, which later attaches the
// item's enchantment visuals to the actor's 3D), and the queue runs a frame or more after the change, holding the
// stacks as they were then. A stack taken out before the queue has run (the base outfit the engine puts on a stand-in or
// a copy as its 3D comes in, which our dressing empties out of it, or what the last dressing gave) is gone when the queue
// gets to it. A joining game crashed twice in that queue, most likely so (a call through a bad pointer in AttachEnchantmentVisuals, an
// actor of ours on the stack), once in a while, as the host's quests were caught up and the stand-in was dressed in the
// same moment (Out of Time's stage 38).
// So item work waits until the engine has the actor ready (a 3D, a process, an attached cell), its 3D has stood
// unchanged for SETTLE, and the last item work on it is QUIET behind. Equipment judges it for every change it makes to
// an actor, and whoever asked tries again a frame later. Kept free of the game so it can be tested.
namespace ItemWork
{
	using Clock = std::chrono::steady_clock;

	// How long an actor's 3D must stand unchanged before its inventory is worked on.
	constexpr auto SETTLE = std::chrono::milliseconds(250);

	// How long after item work on an actor the next waits: the queue the work filled has run by then, so a later change never
	// takes a stack that the queue still holds a pointer to (a gun taken away and given again, a stand-in dressed afresh).
	constexpr auto QUIET = std::chrono::milliseconds(200);

	// What the engine says of the actor.
	struct Facts
	{
		bool          exists = true;        // a reference here, not deleted
		std::uintptr_t root = 0;            // its 3D (the address, only compared); 0 when it has none
		bool          hasProcess = false;   // the engine's AI process for it
		bool          cellAttached = false; // its cell is attached to the world
	};

	// Per actor: the 3D it was last seen with, and since when.
	struct Track
	{
		std::uintptr_t root = 0;
		Clock::time_point since{};
		std::optional<Clock::time_point> workedAt;  // the last item work on it (Worked)
		bool          said = false;  // the deferral was logged (once for each stretch of waiting)
	};

	enum class Verdict : std::uint8_t
	{
		kReady,
		kGone,
		kNo3D,
		kNoProcess,
		kCellNotAttached,
		kSettling,
		kBusy,
	};

	inline Verdict Check(Track& a_track, const Facts& a_facts, Clock::time_point a_now)
	{
		if (!a_facts.exists) {
			a_track = {};
			return Verdict::kGone;
		}
		if (a_facts.root == 0) {
			a_track.root = 0;  // its 3D went: the next one is timed afresh
			return Verdict::kNo3D;
		}
		if (a_track.root != a_facts.root) {
			a_track.root = a_facts.root;
			a_track.since = a_now;
		}
		if (!a_facts.hasProcess) {
			return Verdict::kNoProcess;
		}
		if (!a_facts.cellAttached) {
			return Verdict::kCellNotAttached;
		}
		if (a_now - a_track.since < SETTLE) {
			return Verdict::kSettling;
		}
		if (a_track.workedAt && a_now - *a_track.workedAt < QUIET) {
			return Verdict::kBusy;
		}
		a_track.said = false;
		return Verdict::kReady;
	}

	// Item work on the actor was done now: the next waits QUIET.
	inline void Worked(Track& a_track, Clock::time_point a_now)
	{
		a_track.workedAt = a_now;
	}

	// True the first time for a stretch of waiting: the log line is written once, not each frame.
	inline bool FirstSay(Track& a_track)
	{
		if (a_track.said) {
			return false;
		}
		a_track.said = true;
		return true;
	}

	constexpr std::string_view Name(Verdict a_verdict)
	{
		switch (a_verdict) {
		case Verdict::kReady:
			return "ready";
		case Verdict::kGone:
			return "gone";
		case Verdict::kNo3D:
			return "no 3D yet";
		case Verdict::kNoProcess:
			return "no AI process yet";
		case Verdict::kCellNotAttached:
			return "its cell is not attached";
		case Verdict::kSettling:
			return "its 3D is new";
		case Verdict::kBusy:
			return "item work on it just ran";
		}
		return "?";
	}
}
