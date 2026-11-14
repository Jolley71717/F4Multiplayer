#pragma once

#include "Protocol.h"
#include "game/AimPoint.h"
#include "game/InertShot.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// A player's own shot says where they aimed as it left and which projectile their gun fires (Protocol::Shot::heading,
// pitch and projectile). Without them a friend's game went by what it had: the stand-in's heading and its friend's
// pitch as the position updates last gave them (a tenth of a second old, and smoothed: a friend who turned and fired at
// once shot where they had been looking), and the projectile of the stand-in's own copy of the gun, which has none of
// its friend's mods. The decisions, free of the game, are here (unit tested); WeaponFire catches the aim and InertShots
// puts it on the launch.
namespace ShotAim
{
	// What one shot carries of its shooter's aim. pitch NO_AIM: none (a swing, an NPC's shot, a shot whose aim was not
	// caught), and the receiver goes by the stand-in's own (Puppets::AimOf).
	struct Sent
	{
		float       heading = 0.0f;                // radians, 0 along +y, a quarter turn along +x, within one turn
		std::int8_t pitch = Protocol::NO_AIM;      // whole degrees, up positive

		friend constexpr bool operator==(const Sent&, const Sent&) = default;
	};

	constexpr bool Known(const Sent& a_aim)
	{
		return a_aim.pitch >= -90 && a_aim.pitch <= 90;
	}

	// The aim of a shot leaving now, from the shooter's angles as the game keeps them (z the heading, x the look, down
	// positive; radians). The heading is brought within one turn (the decoder refuses more, Protocol::MAX_SHOT_HEADING).
	// Angles that are no numbers give no aim.
	inline Sent Snapshot(float a_angleZ, float a_angleX)
	{
		if (!std::isfinite(a_angleZ) || !std::isfinite(a_angleX)) {
			return {};
		}
		constexpr float TURN = 2.0f * 3.14159265358979f;
		float           heading = std::fmod(a_angleZ, TURN);
		if (heading < 0.0f) {
			heading += TURN;
		}
		return { .heading = heading, .pitch = Protocol::PackPitch(AimPoint::PitchFromLooking(a_angleX)) };
	}

	// The aims caught and not yet sent are kept in the order their shots left, at most this many (a minigun's, between
	// two sends: a handful).
	inline constexpr std::size_t MAX_QUEUED = 64;

	// One aim for each shot reported this send: a_fires gunshots, then a_swings swings or punches sent as shots (never
	// aimed: nothing flies). The shots are counted and their aims queued a moment apart, on another thread, so the two
	// can differ by one either way: a queue that is short is missing its newest (that shot goes with no aim), and one
	// that is long holds a leftover at its front, so the newest a_fires of it are the ones.
	inline std::vector<Sent> Pair(std::size_t a_fires, std::size_t a_swings, std::span<const Sent> a_queued)
	{
		std::vector<Sent> out;
		out.reserve(a_fires + a_swings);
		const auto have = std::min(a_fires, a_queued.size());
		const auto from = a_queued.size() - have;
		for (std::size_t i = 0; i < have; ++i) {
			out.push_back(a_queued[from + i]);
		}
		out.resize(a_fires + a_swings);
		return out;
	}

	// The launch angles a received shot's aim comes to (InertShot::LaunchAngles), or none when it carries none.
	inline std::optional<InertShot::Angles> AnglesOf(const Sent& a_aim)
	{
		if (!Known(a_aim) || !std::isfinite(a_aim.heading)) {
			return std::nullopt;
		}
		return InertShot::LaunchAngles(a_aim.heading, static_cast<float>(a_aim.pitch));
	}

	// How long a replayed shot's aim and record stand for its stand-in's launches: the firing animation's own
	// "WeaponFire" comes within a few frames of the replay (every pellet of a shotgun's in the one event), so half a
	// second covers it, and a launch later than that (a flamer's stream running on, a gun that winds up) goes by the
	// stand-in's own aim again, as before.
	inline constexpr std::chrono::milliseconds EXPECT_FOR{ 500 };

	constexpr bool Fresh(std::chrono::milliseconds a_sinceReplay)
	{
		return a_sinceReplay >= std::chrono::milliseconds::zero() && a_sinceReplay <= EXPECT_FOR;
	}

	// Whether a replayed shot's launch is made from its shooter's record (a_sent*) in place of the stand-in's own gun's
	// (a_own*): only when the shooter's is known here (a_sentKnown: the id translated and the form found), is another
	// record (a_same false), the stand-in's own launch is one the hook makes from an inert copy anyway (InertShot::Swaps:
	// a plain bullet or beam, or a record that goes off), and the shooter's flies as a bullet, a missile or a beam
	// (InertShot::IsPlainTrail: no hitscan, whose hit may be settled inside the launch, no stream, no lobbed grenade). The
	// launch is of the shooter's record's inert copy, which has no explosion, no force and no damage whatever the record
	// names (an Explosive legendary's round flies as itself and goes off nowhere: its blast comes as its shooter's game's
	// report, as before). So the swap only ever changes how a harmless shot looks (the caliber's tracer, a laser's colour,
	// a mod's own round): what is held and what is launched inert stay decided by the stand-in's own gun.
	constexpr bool UsesSentRecord(bool a_sentKnown, bool a_same, std::uint32_t a_ownFlags, bool a_ownExplodes, std::uint32_t a_sentFlags)
	{
		return a_sentKnown && !a_same && InertShot::Swaps(true, true, a_ownFlags, a_ownExplodes) && InertShot::IsPlainTrail(a_sentFlags);
	}

	// Whether our own launch of a trail (InertShots::Launch) is made from the shooter's record too: only one that goes
	// off nowhere, since a direct launch flies as a trail or not at all.
	constexpr bool DirectUsesSentRecord(bool a_used, std::uint32_t a_sentFlags, bool a_sentExplodes)
	{
		return a_used && !InertShot::GoesOff(a_sentFlags, a_sentExplodes);
	}
}
