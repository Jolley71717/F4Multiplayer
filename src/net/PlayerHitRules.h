#pragma once

#include <cstdint>
#include <optional>

// A hit on our stand-in in a friend's game comes as kPlayerDamaged (Session::ApplyDamageToPlayer): an NPC their game
// runs struck it there, so our player takes the damage here. Our copy of that NPC never hurts us itself: a copy another
// game runs plays its moves and its attacks are disabled (Puppets), so every hit by an enemy a friend's game runs that
// doesn't get through this is a hit our player never takes, and that enemy can't hurt us at all.
//
// Up to 0.9.2 the hit was taken only if our copy of the NPC was there to see: loaded, alive, in our cell or worldspace
// and within RANGE. It was ignored when our copy was a corpse (the session had it dead and the host's game brought it
// back with a cell reset, ours not yet), stood outside while the friend and the NPC went through a door, had not loaded
// here, or was an NPC of a plugin our game lacks (the hit named nothing: FormTranslate makes such an attacker 0). The
// friend saw the enemy fight us; we took nothing.
//
// So a hit whose attacker isn't to be seen here is taken all the same when the friend who reported it is near us: the NPC
// stands within weapon range of our stand-in in their game, and their game runs only NPCs loaded around their player, so
// their player is within REPORTER_RANGE of us. A hit from a friend nowhere near is still ignored: their game has an NPC
// of theirs fighting a stand-in that stands nowhere near our player. Pure, so it can be tested without the game.
namespace PlayerHits
{
	constexpr float RANGE = 10000.0f;           // about 140 m: a sniper rifle's reach
	constexpr float REPORTER_RANGE = 20000.0f;  // the NPC within RANGE of our stand-in there, their player within RANGE of the NPC

	// Our copy of the NPC that hit our stand-in.
	enum class Attacker : std::uint8_t
	{
		kUnnamed,    // the hit names no NPC: one of a plugin our game (or the session's table) lacks
		kNotHere,    // no such actor here, or not loaded
		kDead,       // dead here (the session had it dead; the friend's game has it alive again)
		kElsewhere,  // in another cell or worldspace
		kFar,        // farther than RANGE
		kHere,       // loaded, alive, in our cell or worldspace, within RANGE
	};

	struct Facts
	{
		bool                 byPlayer = false;  // the friend hit us themselves (friendly fire): always taken
		Attacker             attacker = Attacker::kNotHere;
		std::optional<float> reporterApart;  // how far the reporting friend's stand-in is from our player, if in our cell or worldspace
	};

	enum class Verdict : std::uint8_t
	{
		kTake,
		kTakeUnseen,  // taken, though the attacker isn't to be seen here: the reporter is near
		kIgnore,
	};

	constexpr Verdict Judge(const Facts& a_facts)
	{
		if (a_facts.byPlayer || a_facts.attacker == Attacker::kHere) {
			return Verdict::kTake;
		}
		if (a_facts.reporterApart && *a_facts.reporterApart < REPORTER_RANGE) {
			return Verdict::kTakeUnseen;
		}
		return Verdict::kIgnore;
	}

	constexpr const char* Name(Attacker a_attacker)
	{
		switch (a_attacker) {
		case Attacker::kUnnamed:
			return "an NPC our game can't name";
		case Attacker::kNotHere:
			return "not here";
		case Attacker::kDead:
			return "dead here";
		case Attacker::kElsewhere:
			return "in another cell";
		case Attacker::kFar:
			return "too far";
		case Attacker::kHere:
			return "here";
		}
		return "?";
	}
}
