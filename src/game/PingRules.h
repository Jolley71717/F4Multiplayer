#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <vector>

#include "Protocol.h"

// Pings ("look at this"): what the crosshair is on, how long a ping is shown, and how often a player may ping. Kept
// free of the game so it can be tested; Party.cpp casts the ray, lists what is near it and draws the result.
namespace PingRules
{
	using namespace std::chrono_literals;
	using Clock = std::chrono::steady_clock;

	// How long a ping's compass marker stays up. A player's next ping replaces their last one.
	inline constexpr auto LIFETIME = 15s;
	// The least time between two of a player's pings: the server's own limit (Server.cpp, handlePing), so a ping our
	// player sees on their own compass is one the server passes on too.
	inline constexpr auto MIN_GAP = 1s;
	// How far the crosshair reaches (about 140 m). Past that, or at the sky, the ping is where the player stands.
	inline constexpr float REACH = 10000.0f;
	// Something nearer the camera than this is the player's own body or weapon, not a target.
	inline constexpr float MIN_ALONG = 40.0f;
	// The crosshair counts as on a target within this angle of its middle, however small or far it is.
	inline constexpr float MIN_ANGLE_DEGREES = 2.0f;
	// A target this far past the surface the ray hit is behind it (the ray often hits the target's own collision,
	// its front a little short of its middle).
	inline constexpr float BEHIND_SLACK = 60.0f;
	// The radii targets are taken to have around their middle: a person (and most creatures), an item on the ground.
	inline constexpr float ACTOR_RADIUS = 45.0f;
	inline constexpr float ITEM_RADIUS = 18.0f;
	// The middle of a person or creature, above its feet.
	inline constexpr float ACTOR_MIDDLE = 60.0f;
	inline constexpr float UNITS_TO_METERS = 0.0142875f;

	struct Vec
	{
		float x = 0.0f;
		float y = 0.0f;
		float z = 0.0f;
	};

	[[nodiscard]] inline bool IsFinite(const Vec& a_v)
	{
		return std::isfinite(a_v.x) && std::isfinite(a_v.y) && std::isfinite(a_v.z);
	}

	// Where the crosshair looks: from the camera, along a unit direction.
	struct Ray
	{
		Vec from;
		Vec direction;
	};

	// The ray of a player's look: their heading (radians, 0 along +y, a quarter turn along +x) and their angle x
	// (radians, looking down is positive), as the engine turns them into a direction. Nonsense looks level and ahead.
	[[nodiscard]] inline Ray LookRay(const Vec& a_from, float a_heading, float a_angleX)
	{
		const float heading = std::isfinite(a_heading) ? a_heading : 0.0f;
		const float pitch = -(std::isfinite(a_angleX) ? std::clamp(a_angleX, -1.55f, 1.55f) : 0.0f);
		const float flat = std::cos(pitch);
		return { a_from, { std::sin(heading) * flat, std::cos(heading) * flat, std::sin(pitch) } };
	}

	[[nodiscard]] inline Vec Along(const Ray& a_ray, float a_distance)
	{
		return { a_ray.from.x + a_ray.direction.x * a_distance, a_ray.from.y + a_ray.direction.y * a_distance, a_ray.from.z + a_ray.direction.z * a_distance };
	}

	// Something the crosshair may be on: an NPC, an item or a friend, by its middle and its size.
	struct Candidate
	{
		Protocol::PingKind kind = Protocol::PingKind::kActor;
		std::uint32_t      id = 0;  // the caller's (a reference's form id)
		Vec                middle;
		float              radius = ACTOR_RADIUS;
	};

	// Which candidate the crosshair is on (its index), if any. a_surface: how far along the ray it hit the world, if it
	// did (nothing: the sky, or beyond REACH). A candidate counts when it is in front of the camera, within reach, not
	// behind that surface, and the ray passes within its radius of its middle, or within MIN_ANGLE_DEGREES of it for one
	// that small or far. Of those, the one the ray passes nearest its middle (for its size) wins; at a tie the nearer.
	[[nodiscard]] inline std::optional<std::size_t> ChooseTarget(const Ray& a_ray, std::optional<float> a_surface, std::span<const Candidate> a_candidates)
	{
		const float minTan = std::tan(MIN_ANGLE_DEGREES * 3.14159265f / 180.0f);
		std::optional<std::size_t> best;
		float                      bestScore = 0.0f;
		float                      bestAlong = 0.0f;
		for (std::size_t i = 0; i < a_candidates.size(); ++i) {
			const auto& c = a_candidates[i];
			if (!IsFinite(c.middle) || !std::isfinite(c.radius) || c.radius <= 0.0f) {
				continue;
			}
			const Vec   v{ c.middle.x - a_ray.from.x, c.middle.y - a_ray.from.y, c.middle.z - a_ray.from.z };
			const float along = v.x * a_ray.direction.x + v.y * a_ray.direction.y + v.z * a_ray.direction.z;
			if (along < MIN_ALONG || along > REACH) {
				continue;
			}
			if (a_surface && along - c.radius > *a_surface + BEHIND_SLACK) {
				continue;
			}
			const Vec   off{ v.x - a_ray.direction.x * along, v.y - a_ray.direction.y * along, v.z - a_ray.direction.z * along };
			const float miss = std::sqrt(off.x * off.x + off.y * off.y + off.z * off.z);
			const float allowed = (std::max)(c.radius, along * minTan);
			if (miss > allowed) {
				continue;
			}
			const float score = miss / allowed;
			if (!best || score < bestScore - 0.01f || (std::abs(score - bestScore) <= 0.01f && along < bestAlong)) {
				best = i;
				bestScore = score;
				bestAlong = along;
			}
		}
		return best;
	}

	// Where a ping with no target goes: the surface the crosshair is on, else (the sky, or too far) where the player
	// stands.
	[[nodiscard]] inline Vec SpotFor(const Ray& a_ray, std::optional<float> a_surface, const Vec& a_standing)
	{
		if (a_surface && std::isfinite(*a_surface) && *a_surface >= 0.0f && *a_surface <= REACH) {
			return Along(a_ray, *a_surface);
		}
		return a_standing;
	}

	// One player's ping as shown: until when, and where the target is now (Party moves it with an NPC it follows).
	struct Mark
	{
		Protocol::Ping    ping;
		Clock::time_point until{};
	};

	// The pings shown, at most one per player: a player's next ping replaces their last, and each goes after
	// LIFETIME.
	class Board
	{
	public:
		void Add(const Protocol::Ping& a_ping, Clock::time_point a_now)
		{
			marks[a_ping.playerId] = { a_ping, a_now + LIFETIME };
		}

		void Expire(Clock::time_point a_now)
		{
			std::erase_if(marks, [&](const auto& a_entry) { return a_entry.second.until <= a_now; });
		}

		// A player left: their ping goes with them.
		void Forget(std::uint32_t a_playerId)
		{
			marks.erase(a_playerId);
		}

		void Clear()
		{
			marks.clear();
		}

		// The pings still up, by player (call Expire first).
		[[nodiscard]] std::map<std::uint32_t, Mark>& Marks()
		{
			return marks;
		}

		[[nodiscard]] const std::map<std::uint32_t, Mark>& Marks() const
		{
			return marks;
		}

	private:
		std::map<std::uint32_t, Mark> marks;
	};

	// Our own player's pings: one per MIN_GAP. A press sooner is let go (the server would drop it anyway).
	class Limiter
	{
	public:
		[[nodiscard]] bool Try(Clock::time_point a_now)
		{
			if (last && a_now - *last < MIN_GAP) {
				++refused;
				return false;
			}
			last = a_now;
			return true;
		}

		void Reset()
		{
			last.reset();
		}

		[[nodiscard]] std::uint32_t Refused() const
		{
			return refused;
		}

	private:
		std::optional<Clock::time_point> last;
		std::uint32_t                    refused = 0;
	};

	// Which of the eight compass points (0 N, 1 NE ... 7 NW) an offset points to: dx east, dy north. 0 for an offset
	// that is not a number.
	[[nodiscard]] inline std::size_t BearingIndex(float a_dx, float a_dy)
	{
		if (!std::isfinite(a_dx) || !std::isfinite(a_dy)) {
			return 0;
		}
		float degrees = std::atan2(a_dx, a_dy) * 180.0f / 3.14159265f;
		if (degrees < 0.0f) {
			degrees += 360.0f;
		}
		return static_cast<std::size_t>(std::lround(degrees / 45.0f)) % 8;
	}

	// A distance in whole meters, for the HUD.
	[[nodiscard]] inline long Meters(float a_units)
	{
		return std::isfinite(a_units) ? std::lround(std::abs(a_units) * UNITS_TO_METERS) : 0;
	}
}
