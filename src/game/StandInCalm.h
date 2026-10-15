#pragma once

#include <chrono>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <vector>

#include "game/SaveStrays.h"

// A friend's stand-in never fights us. It runs no AI, so a combat the engine starts on it never ends: our hits
// on it (a punch, friendly fire) turned it on us, red name and health bar, and kept us in combat for good (no
// fast travel, no waiting) until the console's stopcombat and stopcombatalarm. It is set up so hits do not anger
// it (SetupCommands), and each frame any combat it is in against us or another stand-in is ended (Frame), and then
// ours, once nothing real fights us (OursDue). Pure bookkeeping, so it is tested without the game.
namespace StandInCalm
{
	using Clock = std::chrono::steady_clock;

	// What a stand-in's combat is against.
	enum class Foe : std::uint8_t
	{
		kNobody,   // no target the engine names
		kUs,       // the player in our game
		kStandIn,  // another friend's stand-in
		kOther,    // anyone else: an enemy whose fight with it is the enemy's own
	};

	// What the game says of a stand-in this frame.
	struct Facts
	{
		bool inCombat = false;
		Foe  target = Foe::kNobody;
		bool angry = false;  // flagged angry with us or to attack us on sight
	};

	// A stand-in still in combat with us after its combat was ended is ended again at most this often: ending it
	// also ends the fight of everyone fighting it, which an enemy then takes up again.
	constexpr auto STOP_GAP = std::chrono::milliseconds(500);
	// After a stand-in's fight with us was ended, our own combat is given this long to end by itself, then ended
	// when nothing real fights us, until OURS_WINDOW after that stand-in's.
	constexpr auto OURS_SETTLE = std::chrono::seconds(1);
	constexpr auto OURS_WINDOW = std::chrono::seconds(10);

	// What to do to a stand-in this frame.
	struct Calm
	{
		bool forgive = false;  // clear its anger with us
		bool stop = false;     // end its combat, and the alarm about it
	};

	// One stand-in's ends.
	struct Track
	{
		Clock::time_point next{};  // no stop before then (STOP_GAP)
		std::uint32_t     stops = 0;
	};

	// In combat with us or another stand-in: a fight it can never end, which keeps us (or that friend) in combat.
	constexpr bool Hostile(const Facts& a_facts)
	{
		return a_facts.inCombat && (a_facts.target == Foe::kUs || a_facts.target == Foe::kStandIn);
	}

	// Our side: the combat a stand-in's fight put us in. Ended once nothing real fights us; a fight we are in with
	// anyone else is left alone, and so is our combat when no stand-in's fight with us was ended lately.
	struct Ours
	{
		std::optional<Clock::time_point> stoppedAt;  // a stand-in's fight with us was last ended then
		std::uint32_t                    clears = 0;
	};

	// Its anger is cleared whenever it is there (the engine reads it to tell whether it is hostile to us); a combat
	// against us or another stand-in is ended at once, and again STOP_GAP later if the engine kept it up, and one
	// against us has our own combat watched (OursDue). A fight with an enemy is left alone: the enemy is fighting
	// it, and its side of that keeps nobody else in combat.
	inline Calm Frame(Track& a_track, Ours& a_ours, const Facts& a_facts, Clock::time_point a_now)
	{
		Calm calm{ .forgive = a_facts.angry };
		if (Hostile(a_facts) && a_now >= a_track.next) {
			calm.stop = true;
			++a_track.stops;
			a_track.next = a_now + STOP_GAP;
			if (a_facts.target == Foe::kUs) {
				a_ours.stoppedAt = a_now;
			}
		}
		return calm;
	}

	// a_inCombat: the game has us in combat; a_realFoe: someone besides a stand-in is in combat with us. True when
	// our combat should be ended now: once, OURS_SETTLE to OURS_WINDOW after a stand-in's fight with us was ended.
	inline bool OursDue(Ours& a_ours, bool a_inCombat, bool a_realFoe, Clock::time_point a_now)
	{
		if (!a_ours.stoppedAt) {
			return false;
		}
		const auto since = a_now - *a_ours.stoppedAt;
		if (since > OURS_WINDOW) {
			a_ours.stoppedAt.reset();
			return false;
		}
		if (since < OURS_SETTLE || !a_inCombat || a_realFoe) {
			return false;
		}
		a_ours.stoppedAt.reset();
		++a_ours.clears;
		return true;
	}

	// Each stand-in is placed with these console commands. Its faction: enemies treat it like the player, and hitting
	// it is no crime (that faction and rank also tell SaveGuard a stand-in an earlier version saved, so they stay as
	// every version since 0.4.0 set them). And hits from friends (us, our companions, another stand-in) do not anger
	// it, as a companion's do not: it still takes them, so friendly fire still reaches its friend.
	inline std::vector<std::string> SetupCommands(std::uint32_t a_formId)
	{
		return {
			std::format("{:08X}.addtofaction {:08X} {}", a_formId, SaveStrays::STAND_IN_FACTION, SaveStrays::STAND_IN_RANK),
			std::format("{:08X}.setignorefriendlyhits 1", a_formId),
		};
	}

	// And with these actor values (unaggressive, helping nobody): it starts no fight, and joins none.
	inline constexpr float AGGRESSION = 0.0f;
	inline constexpr float ASSISTANCE = 0.0f;
}
