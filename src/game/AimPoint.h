#pragma once

#include <algorithm>
#include <cmath>
#include <optional>

// Where a friend's stand-in points its gun. A humanoid's arms and gun do not follow the graph's Pitch (in
// RaiderRootBehavior its blend, CameraPitchOffsetBlend, hangs under a child weighted by IsPlayer, 0 on an NPC)
// nor AimPitchCurrent (the aim modifier's own output, an offset from its target, which it writes every frame).
// WeaponBehavior's BSDirectAtModifier nodes (RifleReady_DirectAt and the cover and grenade ones) turn the spine
// and arms toward a point they ask the engine for (BSDirectAtModifierInterface, target type 3). For the player
// that point is REACH units along their heading and looking angle; for an NPC it is the one
// Actor::SetLookAtTarget left in its movement state (the combat code sets it to what the NPC shoots at), or else
// its ExtraDirectAtTarget's place. With no point the modifier blends out and the gun is held level. A stand-in's
// AI is off, so nothing ever gave it one: it is given the point its owner's own game aims the owner's arms at.
// Kept free of the game so it can be tested.
namespace AimPoint
{
	struct Point
	{
		float x = 0.0f;
		float y = 0.0f;
		float z = 0.0f;
	};

	// How far ahead the engine puts the player's own aim point (the player's branch of the direct-at target).
	inline constexpr float REACH = 4000.0f;
	inline constexpr float PI = 3.14159265358979f;
	inline constexpr float TO_DEGREES = 180.0f / PI;

	// The pitch a player aims at, degrees up (+) or down (-), from their angle x in radians (looking down is
	// positive), for PlayerState::pitch.
	inline float PitchFromLooking(float a_angleX)
	{
		return std::isfinite(a_angleX) ? -a_angleX * TO_DEGREES : 0.0f;
	}

	// The point REACH units from a_from along a heading (radians, 0 along +y, a quarter turn along +x) and a pitch
	// (degrees up), the way the engine turns the player's angles into a direction. Not-a-number counts as level
	// and ahead; straight up or down is held a degree short, where the heading still means something.
	inline Point Along(const Point& a_from, float a_heading, float a_pitch)
	{
		const float heading = std::isfinite(a_heading) ? a_heading : 0.0f;
		const float pitch = (std::isfinite(a_pitch) ? std::clamp(a_pitch, -89.0f, 89.0f) : 0.0f) / TO_DEGREES;
		const float flat = std::cos(pitch) * REACH;
		return { a_from.x + std::sin(heading) * flat, a_from.y + std::cos(heading) * flat, a_from.z + std::sin(pitch) * REACH };
	}

	// What an aim point says from where an actor stands: the pitch (degrees up) and the heading (radians, 0 to
	// a full turn) it aims at.
	inline float PitchTo(const Point& a_from, const Point& a_to)
	{
		const float dx = a_to.x - a_from.x;
		const float dy = a_to.y - a_from.y;
		return std::atan2(a_to.z - a_from.z, std::sqrt(dx * dx + dy * dy)) * TO_DEGREES;
	}
	inline float HeadingTo(const Point& a_from, const Point& a_to)
	{
		const float heading = std::atan2(a_to.x - a_from.x, a_to.y - a_from.y);
		return heading < 0.0f ? heading + 2.0f * PI : heading;
	}

	inline bool IsFinite(const Point& a_point)
	{
		return std::isfinite(a_point.x) && std::isfinite(a_point.y) && std::isfinite(a_point.z);
	}

	// The aim point a puppet is given each frame, or none. A friend's stand-in aims where its owner aims, from
	// where it stands. A mirrored NPC is not (ForMirror gives it its owner's copy's aim), nor is a puppet whose
	// place is not known.
	inline std::optional<Point> ForPuppet(bool a_standIn, const Point& a_position, float a_heading, float a_pitch)
	{
		if (!a_standIn || !IsFinite(a_position)) {
			return std::nullopt;
		}
		return Along(a_position, a_heading, a_pitch);
	}

	// A mirrored NPC's aim is measured and given from this high above its feet, about a person's shoulders, where
	// its arms turn from. Its owner's copy aims at what it shoots at, often near: a bloatfly 300 units up and 200
	// out is 56 degrees up from the feet but 44 from the shoulders, and the copy's point is REACH away, where the
	// two hardly differ.
	inline constexpr float MIRROR_HEIGHT = 110.0f;

	// The pitch (degrees up) an NPC its owner runs aims at, for ActorState::aimPitch: from MIRROR_HEIGHT above its
	// feet to the point the engine gives its weapon aim (the one its combat AI set), or none while it has none.
	inline std::optional<float> OwnerPitch(const Point& a_feet, const std::optional<Point>& a_aimPoint)
	{
		if (!a_aimPoint || !IsFinite(*a_aimPoint) || !IsFinite(a_feet)) {
			return std::nullopt;
		}
		const float pitch = PitchTo({ a_feet.x, a_feet.y, a_feet.z + MIRROR_HEIGHT }, *a_aimPoint);
		return std::isfinite(pitch) ? std::optional<float>{ pitch } : std::nullopt;
	}

	// What a mirrored NPC's aim point is made each frame. Its AI is off, so nothing else gives it one.
	struct MirrorOrder
	{
		enum class Act
		{
			kLeave,  // nothing to do
			kSet,    // give it point
			kClear,  // forget its point (Actor::ClearLookAtTarget)
		};
		Act   act = Act::kLeave;
		Point point{};
	};

	// While its owner's copy aims (a_ownerPitch, from ActorState::aimPitch), the copy is given a point REACH units
	// along its own heading at that pitch, from MIRROR_HEIGHT above where it stands. While the owner's copy aims at
	// nothing, a point the copy still has (a_hasPoint: its last one from us, or one its own AI set before it was
	// taken over) is forgotten, so an NPC that stopped fighting there lowers its gun here rather than holding the
	// aim. A copy whose place is not known is left as it is.
	inline MirrorOrder ForMirror(std::optional<float> a_ownerPitch, bool a_hasPoint, const Point& a_feet, float a_heading)
	{
		if (!IsFinite(a_feet)) {
			return {};
		}
		if (a_ownerPitch) {
			return { MirrorOrder::Act::kSet, Along({ a_feet.x, a_feet.y, a_feet.z + MIRROR_HEIGHT }, a_heading, *a_ownerPitch) };
		}
		return { a_hasPoint ? MirrorOrder::Act::kClear : MirrorOrder::Act::kLeave, {} };
	}
}
