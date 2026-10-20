#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

// Walk, trot or run for a mirrored dog. Dogmeat's graph (DogmeatDefault.hkx) does not pick its gait by Speed: its
// MovingStateMachine leaves the walk only on the events "ToTrotForward", "ToTrotFastForward", "ToRunForward" (and
// "ToWalkForward", "ToWalkBackwards" back), enters the state iSyncLocomotionRangeID names on "moveStart", and plays
// each clip at its own variable (fWalkClipMult and the rest), which nothing in the graph computes. The engine's AI
// movement sends the events and writes the rates from the AnimationSpeedInfo data; a mirror's AI is off, so nothing
// did: a running dog's copy played its walk, at whatever rate the walk last had, while it was carried along at
// about five times the walk's pace (a run is 458 units/s, the walk 95). Gait.h is the humanoid graph's, whose
// events and variables a dog's graph does not have. Kept free of the game so it can be tested.
//
// Two things the graph's own data says (read from DogmeatDefault.hkx and DogmeatRoot.hkx), which the copy goes by:
// - Nothing in the graph writes iSyncLocomotionRangeID. Its MovingStateMachine has no sync variable (the state
//   machines that keep theirs, such as CombatNonCombatBehavior's iSyncCombatState, name one); the variable is only
//   read, as the state a "moveStart" enters. So the owner's value is whatever its engine last put there, which tells
//   the gait its dog set off in, and only the one it plays now if the engine writes it again as the gait changes
//   while moving, which nothing in the graph needs (it changes gait by the events alone). Then a dog that sets off at
//   a walk after its player and breaks into a run to keep up is reported walking, and its copy walked while carried
//   along at the run's pace, which looks like a dog squatting along rather than running. A gait whose clip cannot be
//   played at the speed reported (Fits) is taken for that, and the copy goes by the speed.
// - The root graph starts fTrotFastClipMult at 0, where the other rates start at 1: a graph in the fast trot that
//   nobody gave a rate stands frozen in the clip's first pose. Every rate is now written (Rates), not just the one
//   the copy was told to play.
// The ComputePlaybackMultipliers expression in the graph computes only the body's roll into a turn, despite its name.
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

	// Whether a gait's clip can be what carries a dog at a_speed: no more than FIT times the clip's own pace (the engine
	// plays each clip at the speed over its pace, live 0.98 and 1.00 for a dog walking at 93 and 95 units/s, and
	// changes gait long before that). A walk at 458 units/s, or a walk backwards, is a gait the owner's graph was
	// started in and has left since (see the top). Not the other way: a gait faster than the speed is believed, as the
	// owner's graph may go on playing its run slowed for a moment as it slows down.
	inline constexpr float FIT = 2.0f;
	constexpr bool Fits(const Range& a_range, float a_speed)
	{
		return !(a_speed > a_range.pace * FIT);
	}

	// The gait a copy's graph plays and its rate. a_own: the state carries the owner's graph's gait (a_ownRange and
	// a_ownRate as sent), which goes before the speed while its clip can carry the dog at that speed (Fits).
	constexpr Plan Choose(bool a_own, std::uint8_t a_ownRange, std::uint8_t a_ownRate, float a_speed)
	{
		Plan         plan;
		const Range* own = a_own ? Find(a_ownRange) : nullptr;
		if (own && !Fits(*own, a_speed)) {
			own = nullptr;  // and its rate with it, which is that clip's
		}
		plan.range = own ? own : &BySpeed(a_speed);
		const float rate = own && a_ownRate != 0 ? UnpackRate(a_ownRate) : a_speed / plan.range->pace;
		plan.rate = std::clamp(rate, MIN_RATE, MAX_RATE);
		return plan;
	}

	// Every clip's rate, as written each frame: the plan's gait at its rate, the others at the speed over their own pace
	// (held to the same limits), so whichever state the graph plays has a sane rate even when it is not the one the copy
	// was told (the fast trot's starts at 0, see the top). The walk and the walk backwards share theirs: the plan's goes.
	struct RateWrite
	{
		const char* variable = nullptr;
		float       rate = 1.0f;
	};
	inline constexpr std::size_t RATE_VARIABLES = 4;  // RANGES' forward gaits, each with its own variable
	static_assert(RANGES.size() == RATE_VARIABLES + 1 && RANGES[RATE_VARIABLES].id == WALK_BACK);
	constexpr std::array<RateWrite, RATE_VARIABLES> Rates(const Plan& a_plan, float a_speed)
	{
		std::array<RateWrite, RATE_VARIABLES> out{};
		for (std::size_t i = 0; i < RATE_VARIABLES; ++i) {
			const auto& range = RANGES[i];
			const bool  planned = a_plan.range && std::string_view{ range.rateVariable } == a_plan.range->rateVariable;
			out[i] = { range.rateVariable, planned ? a_plan.rate : std::clamp(a_speed / range.pace, MIN_RATE, MAX_RATE) };
		}
		return out;
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
