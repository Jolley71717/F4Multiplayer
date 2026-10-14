#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

// Walk, trot or run for a mirrored dog. Dogmeat's graph (DogmeatDefault.hkx) does not pick its gait by Speed: its
// MovingStateMachine leaves the walk only on the events "ToTrotForward", "ToTrotFastForward", "ToRunForward" (and
// "ToWalkForward", "ToWalkBackwards" back), enters the state iSyncLocomotionRangeID names on "moveStart", and plays
// each clip at its own variable (fWalkClipMult and the rest), which nothing in the graph computes. The engine's AI
// movement sends the events and writes the rates from the AnimationSpeedInfo data; a mirror's AI is off, so nothing
// did: a running dog's copy played its walk, at whatever rate the walk last had, while it was carried along at
// about five times the walk's pace (a run is 458 units/s, the walk 95). Gait.h is the humanoid graph's, whose
// events and variables a dog's graph does not have. Kept free of the game so it can be tested.
namespace CreatureGait
{
	// One of the dog graph's gaits: its MovingStateMachine state (the value of iSyncLocomotionRangeID), the event
	// that switches a moving graph to it, the variable its clip plays at, and the clip's own pace in units/second
	// (AnimationSpeedInfo for DogmeatDefault.hkb).
	struct Range
	{
		std::uint8_t id;
		const char*  event;
		const char*  rateVariable;
		float        pace;
	};

	inline constexpr std::uint8_t WALK = 0;
	inline constexpr std::uint8_t TROT = 1;
	inline constexpr std::uint8_t TROT_FAST = 2;
	inline constexpr std::uint8_t RUN = 3;
	inline constexpr std::uint8_t WALK_BACK = 10;

	inline constexpr std::array<Range, 5> RANGES{ {
		{ WALK, "ToWalkForward", "fWalkClipMult", 94.7f },
		{ TROT, "ToTrotForward", "fTrotClipMult", 219.0f },
		{ TROT_FAST, "ToTrotFastForward", "fTrotFastClipMult", 377.2f },
		{ RUN, "ToRunForward", "fRunClipMult", 458.0f },
		{ WALK_BACK, "ToWalkBackwards", "fWalkClipMult", 76.0f },
	} };

	// The graph variable holding the state, and one only the dog graph has, which tells its graph from another
	// creature's that has a variable of the same name.
	inline constexpr const char* RANGE_VARIABLE = "iSyncLocomotionRangeID";
	inline constexpr const char* MARKER_VARIABLE = "fTrotFastClipMult";

	constexpr const Range* Find(std::int32_t a_id)
	{
		const auto it = std::ranges::find(RANGES, a_id, &Range::id);
		return it != RANGES.end() ? &*it : nullptr;
	}

	// A clip's playback rate as a state carries it, in 64ths (0 to almost 4: the engine's own rates stay well
	// inside, a dog's run plays at about 1).
	constexpr std::uint8_t PackRate(float a_rate)
	{
		if (!(a_rate > 0.0f)) {
			return 0;  // also a NaN
		}
		return static_cast<std::uint8_t>(std::min(a_rate * 64.0f + 0.5f, 255.0f));
	}
	constexpr float UnpackRate(std::uint8_t a_rate)
	{
		return static_cast<float>(a_rate) / 64.0f;
	}

	// The gait for a speed when the owner's state says none (its graph was not readable): the gait whose clip's
	// pace is nearest, forward. Not what the owner's game plays, which a state normally says.
	constexpr const Range& BySpeed(float a_speed)
	{
		const Range* best = &RANGES[0];
		for (const auto& range : RANGES) {
			const float off = range.pace > a_speed ? range.pace - a_speed : a_speed - range.pace;
			const float bestOff = best->pace > a_speed ? best->pace - a_speed : a_speed - best->pace;
			if (range.id != WALK_BACK && off < bestOff) {
				best = &range;
			}
		}
		return *best;
	}

	// The rates a copy plays at: the owner's own, but never a frozen clip (a rate the owner had not set yet), nor
	// one so fast it blurs. With no rate from the owner, the speed over the clip's pace.
	inline constexpr float MIN_RATE = 0.25f;
	inline constexpr float MAX_RATE = 3.0f;

	struct Plan
	{
		const Range* range = nullptr;  // the gait the copy's graph is told
		float        rate = 1.0f;      // what range->rateVariable is set to
	};

	// The gait a copy's graph plays and its rate. a_own: the state carries the owner's graph's gait (a_ownRange and
	// a_ownRate as sent), which goes before the speed.
	constexpr Plan Choose(bool a_own, std::uint8_t a_ownRange, std::uint8_t a_ownRate, float a_speed)
	{
		Plan         plan;
		const Range* own = a_own ? Find(a_ownRange) : nullptr;
		plan.range = own ? own : &BySpeed(a_speed);
		const float rate = own && a_ownRate != 0 ? UnpackRate(a_ownRate) : a_speed / plan.range->pace;
		plan.rate = std::clamp(rate, MIN_RATE, MAX_RATE);
		return plan;
	}

	// Whether the gait's event goes this frame. a_moving and a_wasMoving: whether the copy's graph moves this frame
	// and did last frame (MoveSync); a_previous the gait it was last told (-1: none); a_resent, it was found in the
	// other state and told again. While moving, when the gait changed, when the graph has just started moving (a
	// "moveStart" enters the state iSyncLocomotionRangeID names, which a graph that moved before may have kept), or
	// when it was told again (a stagger or a jump put it back in its walk). Not every frame: the graph takes the event
	// from the state it is in too, and starts its clip over.
	constexpr bool Send(bool a_moving, bool a_wasMoving, std::int32_t a_range, std::int32_t a_previous, bool a_resent)
	{
		return a_moving && (a_range != a_previous || !a_wasMoving || a_resent);
	}
}
