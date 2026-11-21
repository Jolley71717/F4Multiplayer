#pragma once

#include "Protocol.h"

#include <cstdint>
#include <string>
#include <vector>

// What a death effect made of a body, made the same in every game (the rules, and why: src/game/BodyEffects.h). Our game
// tells the others what its dead bodies show once the effect has ended (kReportBodyState: the engine's critical stage, and
// the base of the pile attached), and makes of its own body of that actor what another game told (kBodyState) and ours
// lacks: an ash or goo pile with the body unseen, a frozen body. Only ever a body that already lies dead here, loaded;
// what is told of one that isn't waits. With the console's own commands, as the game's scripts do it with theirs: no
// script object is made for the actor. Main thread only, but for NoteDeath.
namespace BodyStates
{
	// Every frame while welcomed, in the world and out of the opening.
	void Frame();

	std::vector<std::vector<std::uint8_t>> TakeOutgoing();

	// From the server: what a friend's game made of a body.
	void OnHeard(const Protocol::BodyState& a_state);

	// One of our game's NPCs is dying or has died (WorldSync's death watch; any thread): what its body shows is looked
	// for over the next minute.
	void NoteDeath(std::uint32_t a_local);

	// A friend joined, who heard none of it: the bodies around us are told again.
	void OnPlayerJoined();

	// A game was loaded: our bodies are as the save had them, so what friends told is made of them anew.
	void OnGameLoaded();

	// The session ended.
	void Reset();

	std::string Describe();

	// For the dev commands: the dead bodies loaded around our player and what each shows; all that is known of one
	// reference; and one made to show a stage and a pile here and now, as if a friend's game had told it (no session
	// needed, nothing is sent).
	std::string List();
	std::string Why(std::uint32_t a_refId);
	std::string DevApply(std::uint32_t a_refId, std::uint8_t a_stage, std::uint32_t a_pile);
}
