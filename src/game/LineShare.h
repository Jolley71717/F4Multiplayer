#pragma once

// Which spoken lines a game reports to the session (Conversations::Watch). The player's own lines always; an NPC's when
// our game runs it (or our player talks to it: their copy is the one in the conversation). Combat barks and grunts are
// not shared: they would bury the conversations in notifications. A creature's line (a race our player can't talk to:
// a feral dog, a stingwing, a mutant hound) only when our own game has a recording of it: a creature's growls are sound
// descriptors named in its dialogue's responses with no voice file, so every hearer found nothing to play and the text
// was noise (the rig, 0.14.0); an eyebot's broadcasts and Liberty Prime's lines are recorded, and go. A talking race's
// line goes with or without a recording (shown as text then). Kept free of the game so it can be tested
// (tests/LineShareTests.cpp).
namespace LineShare
{
	constexpr bool Reported(bool a_inCombat, bool a_isPlayer, bool a_ours, bool a_creature, bool a_recorded)
	{
		if (a_inCombat) {
			return false;
		}
		if (a_isPlayer) {
			return true;
		}
		return a_ours && (!a_creature || a_recorded);
	}
}
