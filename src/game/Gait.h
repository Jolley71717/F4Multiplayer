#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

// Walk or run for a humanoid puppet (a friend's stand-in, a mirrored person). The humanoid graph does not pick
// its gait by Speed: MTLocomotionBlend_SM (weapon away) leaves Walk for Run, and comes back, only on the
// events "Run" and "Walk" (the jog's, if it has one, is not known: see Event), and the weapon-drawn
// locomotion on "walkStart"/"runStart". The engine's AI movement sends
// them from the AnimationSpeedInfo data; with the AI off nothing did, so a stand-in moving at a run's pace
// played the walk cycle (its animation speed read 88 where the player's own read 373). Each clip then plays at
// speed over its own pace (WalkSpeedMult, RunSpeedMult and the weapon-drawn ones), as the engine's contour
// does, so the feet match the ground. A player walks (about 90 to 130 units/s) or runs (373, a sprint more):
// the graph's jog is left out. Kept free of the game so it can be tested.
namespace Gait
{
	// The values are the weapon-away graph's states (its sync variable, iSyncLocomotionSpeed). A stand-in never
	// jogs (Pick leaves it out); a mirrored person jogs when its owner's graph does (MirrorGait.h).
	enum class Kind : std::uint8_t
	{
		kWalk = 0,
		kJog = 1,
		kRun = 2,
	};

	// The clips' own paces, units/second (AnimationSpeedInfo for MTBehavior.hkb/MTDefault and
	// WeaponBehavior.hkb/RifleReady_SM).
	inline constexpr float WALK_CLIP = 92.4f;
	inline constexpr float RUN_CLIP = 373.0f;
	inline constexpr float WEAPON_WALK_CLIP = 133.5f;
	inline constexpr float WEAPON_RUN_CLIP = 360.0f;

	// A walk turns into a run above RUN_UP, and a run back into a walk below RUN_DOWN: well clear of a walk's
	// pace, and far enough apart that a speed wavering between them keeps the gait it has.
	inline constexpr float RUN_UP = 200.0f;
	inline constexpr float RUN_DOWN = 160.0f;

	// The speed the gait and the playback go by: the owner's reported speed, smoothed over about TAU seconds. A
	// slow game's reports swing from frame to frame (live, a 16 fps friend running read 88 to 747 units/s, 350
	// on average), which switched the gait back and forth.
	inline constexpr float TAU = 0.4f;

	// The speed a state carries. The owner's own animation speed (MiddleHighProcessData::animationSpeed: what
	// its legs play, live a steady 373 running, 92.6 walking, 500 sprinting and 0 standing, however unevenly the
	// owner's frames come) while it has one. The distance covered since the last sample only when it has none
	// (no process data, or moved by a script, a lift or a vehicle with its legs still): over uneven frames that
	// read 88 to 747 for a steady run.
	constexpr float Reported(float a_animationSpeed, float a_travelled)
	{
		return a_animationSpeed > 0.0f ? a_animationSpeed : a_travelled;
	}
	inline float Smooth(float a_previous, float a_speed, float a_seconds)
	{
		const float keep = std::exp(-std::clamp(a_seconds, 0.0f, 0.25f) / TAU);
		return a_previous * keep + a_speed * (1.0f - keep);
	}

	// Smooth, but from the speed itself when the smoothed speed says the friend was standing (below STANDING): it
	// decays toward 0 there, and a run that set off from it played its clips at the slowest rate (MIN_PLAYBACK)
	// for most of a second while the body already moved at full pace. Judged by the speed, not by whether the
	// graph took a moveStart, which it may refuse for a while with the friend running.
	inline constexpr float STANDING = 25.0f;  // MoveSync::START
	inline float Smoothed(float a_previous, float a_speed, float a_seconds)
	{
		return Smooth(a_previous < STANDING ? a_speed : a_previous, a_speed, a_seconds);
	}

	constexpr Kind Pick(float a_speed, Kind a_current)
	{
		return a_speed > (a_current == Kind::kRun ? RUN_DOWN : RUN_UP) ? Kind::kRun : Kind::kWalk;
	}

	// The owner's own gait, sent with every state as its ActorState::moveMode (live: 0x66 walking, 0xA6
	// running, 0x1A6 sprinting, 0 standing; the low bits are the direction). What the owner's game plays, so
	// it goes before the speed, which a slow game reports unevenly. A sprint is the run played faster. With
	// none of the bits (an older build, a mirrored NPC moved by a script) the gait goes by the speed.
	inline constexpr std::uint16_t MOVE_WALKING = 0x40;
	inline constexpr std::uint16_t MOVE_RUNNING = 0x80;
	inline constexpr std::uint16_t MOVE_SPRINTING = 0x100;
	constexpr Kind Pick(float a_speed, Kind a_current, std::uint16_t a_moveMode)
	{
		if (a_moveMode & (MOVE_RUNNING | MOVE_SPRINTING)) {
			return Kind::kRun;
		}
		if (a_moveMode & MOVE_WALKING) {
			return Kind::kWalk;
		}
		return Pick(a_speed, a_current);
	}

	// A clip's playback rate for a speed: the engine's own (speed over the clip's pace), held to a range
	// that still looks like the gait (a sprint plays the run at most 1.6 times as fast).
	inline constexpr float MIN_PLAYBACK = 0.5f;
	inline constexpr float MAX_PLAYBACK = 1.6f;
	constexpr float Playback(float a_speed, float a_clip)
	{
		return std::clamp(a_speed / a_clip, MIN_PLAYBACK, MAX_PLAYBACK);
	}

	// The weapon-away graph's event, and the value of its sync variable, which picks the state a moveStart
	// enters. A jog has no event (null): the sync variable, set first, enters it at a start, and a change
	// into the jog while moving waits for the next start. "Jog" was a guess that nothing names (not the
	// engine's headers, not a reading of the graph), where "Run" is seen to be taken; and "Walk" in its
	// place, sent right after the start as every gait's event is, would pull a copy out of the jog its
	// start entered.
	constexpr const char* Event(Kind a_gait)
	{
		return a_gait == Kind::kRun ? "Run" : a_gait == Kind::kWalk ? "Walk" : nullptr;
	}
	constexpr std::int32_t SyncValue(Kind a_gait)
	{
		return static_cast<std::int32_t>(a_gait);
	}

	// The weapon-drawn graph's (iLocomotionSpeedState 0 and 1). It has no jog; a jog is nearer its run.
	constexpr const char* WeaponEvent(Kind a_gait)
	{
		return a_gait == Kind::kWalk ? "walkStart" : "runStart";
	}
	constexpr std::int32_t WeaponState(Kind a_gait)
	{
		return a_gait == Kind::kWalk ? 0 : 1;
	}

	// The weapon-drawn graph's sneak (SneakWPNLocomotion_SM), walk or run: its start state by the sync variable
	// iSyncSneakWalkRun (0 walk, 1 run, as WeaponState), and while moving these events. The weapon-away sneak has
	// one locomotion, a blend by Speed, and needs neither.
	constexpr const char* SneakEvent(Kind a_gait)
	{
		return a_gait == Kind::kWalk ? "sneakWalkStart" : "sneakRunStart";
	}

	// Whether the gait's events go this frame: while moving, when the gait changed, when the graph has just
	// started moving (a moveStart enters by the sync variable, which a graph that was moving before may have
	// kept), or when the graph was found out of step and told again (a jump, a stagger or a weapon change put
	// it back in its walk). Not every frame: an event to the state the graph is in could start its cycle over.
	constexpr bool Send(bool a_moving, bool a_wasMoving, Kind a_gait, Kind a_previous, bool a_resent)
	{
		return a_moving && (a_gait != a_previous || !a_wasMoving || a_resent);
	}
}
