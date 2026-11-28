#pragma once

// A puppet (a friend's stand-in, an NPC another game runs) is driven by us: the engine's no-AI update puts it where its
// owner's game has it and our animation drive plays what its owner's does. The engine's combat update (Actor::UpdateCombat,
// run on its job threads for every actor in a fight) is AI too: its behaviour tree aims the gun, sights a foe, throws a
// grenade, and does so by performing actions on the actor's animation graph, off the main thread, while that graph is
// built, swapped or driven by us. It crashed hosts three times inside the engine, with none of our frames on the stack
// (hkbStateMachine::getStateIndex under PerformAction(ActionSighted) as a friend joined; BSFixedString::operator= under the
// aim controller of a pipe gun in Corvega). So a puppet's combat update does not run, except while its AI is let run
// (Puppets::AllowAI: getting into power armor). Pure, so it is tested without the game.
namespace PuppetCombat
{
	// Whether the engine's combat update runs for an actor: any actor that is no puppet, and a puppet only while its own AI
	// is let run.
	constexpr bool Runs(bool a_puppet, bool a_aiAllowed)
	{
		return !a_puppet || a_aiAllowed;
	}
}
