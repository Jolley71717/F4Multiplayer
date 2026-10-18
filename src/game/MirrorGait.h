#pragma once

#include "game/CreatureGait.h"
#include "game/Gait.h"

#include <algorithm>
#include <cstdint>
#include <optional>

// What a mirrored NPC's legs play, from its owner's copy's own animation graph. The owner's game runs the NPC's AI, and
// the engine's AI movement picks its graph's gait (walk, jog or run for a person, a dog's walk, trot or run, a
// creature's locomotion range) and the rate each clip plays at; the copies have their AI off, so nothing did that for
// them. A copy guessed from the speed and moveMode instead: a person whose owner's copy jogged played the run slowed
// down (Gait.h has no jog), a creature set off in whatever state its graph last had, and an NPC carried with its legs
// still (a lift, a push: its position moving) walked in every other game. The owner's game now
// reads what its graph plays and sends it with each state (Protocol::ActorState::gait and gaitRate, flagged kOwnGait),
// and the copy's graph is told the same. Which graph an NPC has is told from the variables it answers, the same way in
// both games (its race is the same in both). A dog's own gait is CreatureGait.h's. Kept free of the game so it can be
// tested.
namespace MirrorGait
{
	// The variables that tell a graph's kind. Only a person's graph has the walk's playback rate; a creature keeps its
	// locomotion state in one of the sync variables (a dog's graph is told by CreatureGait::MARKER_VARIABLE).
	inline constexpr const char* PERSON_MARKER = "WalkSpeedMult";
	inline constexpr const char* CREATURE_VARIABLES[] = { CreatureGait::RANGE_VARIABLE, "iSyncLocomotionSpeed" };

	// A person's: the weapon-away graph's state (Gait::Kind), the weapon-drawn graph's, and the clips' playback rates.
	inline constexpr const char* GAIT_VARIABLE = "iSyncLocomotionSpeed";
	inline constexpr const char* WEAPON_GAIT_VARIABLE = "iLocomotionSpeedState";
	inline constexpr const char* WALK_RATE = "WalkSpeedMult";
	inline constexpr const char* RUN_RATE = "RunSpeedMult";
	inline constexpr const char* WEAPON_WALK_RATE = "fLocomotionWalkPlaybackSpeed";
	inline constexpr const char* WEAPON_RUN_RATE = "fLocomotionRunPlaybackSpeed";

	// 1 while the graph is in its locomotion state, 0 while it stands (what the engine keeps for an actor with AI).
	inline constexpr const char* IDLE_VARIABLE = "iSyncIdleLocomotion";

	enum class Family : std::uint8_t
	{
		kNone,      // no graph, or one with none of the variables: Speed and Direction only
		kPerson,    // the humanoid graph (Gait.h)
		kDog,       // Dogmeat's (CreatureGait.h)
		kCreature,  // another creature's: its locomotion state in CREATURE_VARIABLES[creatureVariable]
	};

	// a_creatureVariable: the index of the first of CREATURE_VARIABLES the graph has, -1 none.
	constexpr Family Identify(bool a_personMarker, bool a_dogMarker, int a_creatureVariable)
	{
		if (a_personMarker) {
			return Family::kPerson;
		}
		if (a_dogMarker && a_creatureVariable == 0) {
			return Family::kDog;
		}
		return a_creatureVariable >= 0 ? Family::kCreature : Family::kNone;
	}

	// ---- The owner's game: what a state carries.

	// A person's gait as a state carries it: the weapon-drawn graph's state while the weapon is out (only walk or run),
	// the weapon-away graph's otherwise (walk, jog or run). None for a value the graph does not have.
	constexpr std::optional<Gait::Kind> PersonGait(bool a_weaponDrawn, std::int32_t a_state)
	{
		if (a_weaponDrawn) {
			if (a_state == 0 || a_state == 1) {
				return a_state == 0 ? Gait::Kind::kWalk : Gait::Kind::kRun;
			}
			return std::nullopt;
		}
		if (a_state >= 0 && a_state <= 2) {
			return static_cast<Gait::Kind>(a_state);
		}
		return std::nullopt;
	}

	// The variable holding the playback rate of the clip a person's gait plays; none for the jog, whose rate the engine
	// keeps in no variable of ours (its own default is left to it).
	constexpr const char* PersonRate(bool a_weaponDrawn, Gait::Kind a_gait)
	{
		if (a_weaponDrawn) {
			return a_gait == Gait::Kind::kWalk ? WEAPON_WALK_RATE : WEAPON_RUN_RATE;
		}
		return a_gait == Gait::Kind::kWalk ? WALK_RATE : a_gait == Gait::Kind::kRun ? RUN_RATE : nullptr;
	}

	// A creature's locomotion state as a state carries it (a byte): none for one out of its range.
	constexpr std::optional<std::uint8_t> CreatureState(std::int32_t a_state)
	{
		if (a_state < 0 || a_state > 0xFF) {
			return std::nullopt;
		}
		return static_cast<std::uint8_t>(a_state);
	}

	// The speed a state carries (Gait::Reported), but none while a person's graph stands and its legs play nothing: the
	// distance it covered then is a lift, a script or a push carrying it, which the copy follows without walking.
	// a_graphStands: its graph answered IDLE_VARIABLE with 0, read only while the engine animates it (it has the process
	// data its animation speed comes from). Only a person's: the engine is known to keep that variable for a humanoid
	// with AI, and a creature's graph that has it but never had it kept would have its copy slide along standing.
	constexpr float Reported(float a_animationSpeed, float a_travelled, Family a_family, bool a_graphStands)
	{
		if (a_family == Family::kPerson && a_graphStands && !(a_animationSpeed > 0.0f)) {
			return 0.0f;
		}
		return Gait::Reported(a_animationSpeed, a_travelled);
	}

	// ---- The copy's game: what its graph is told.

	// What a mirrored person's graph is told: the gait it walks, jogs or runs in (the weapon-drawn graph's state follows
	// from it), and the rate of each clip.
	struct PersonPlan
	{
		Gait::Kind gait = Gait::Kind::kWalk;
		float      walkRate = 1.0f;
		float      runRate = 1.0f;
		float      weaponWalkRate = 1.0f;
		float      weaponRunRate = 1.0f;
	};

	// a_own: the state carries its owner's graph's gait (a_ownGait and a_ownRate as sent), which goes before the speed
	// and moveMode a stand-in goes by (Gait::Pick). Each clip plays at the smoothed speed over its pace, as a stand-in's;
	// the one the owner's graph plays at the rate it plays there, when sent.
	constexpr PersonPlan Person(bool a_own, std::uint8_t a_ownGait, std::uint8_t a_ownRate, bool a_weaponDrawn, float a_speed, Gait::Kind a_current, std::uint16_t a_moveMode)
	{
		PersonPlan plan;
		plan.walkRate = Gait::Playback(a_speed, Gait::WALK_CLIP);
		plan.runRate = Gait::Playback(a_speed, Gait::RUN_CLIP);
		plan.weaponWalkRate = Gait::Playback(a_speed, Gait::WEAPON_WALK_CLIP);
		plan.weaponRunRate = Gait::Playback(a_speed, Gait::WEAPON_RUN_CLIP);
		const auto own = a_own ? PersonGait(false, a_ownGait) : std::nullopt;
		plan.gait = own ? *own : Gait::Pick(a_speed, a_current, a_moveMode);
		if (own && a_ownRate != 0) {
			const float rate = std::clamp(CreatureGait::UnpackRate(a_ownRate), Gait::MIN_PLAYBACK, Gait::MAX_PLAYBACK);
			if (a_weaponDrawn) {
				(plan.gait == Gait::Kind::kWalk ? plan.weaponWalkRate : plan.weaponRunRate) = rate;
			} else if (plan.gait == Gait::Kind::kWalk) {
				plan.walkRate = rate;
			} else if (plan.gait == Gait::Kind::kRun) {
				plan.runRate = rate;
			}
		}
		return plan;
	}
}
