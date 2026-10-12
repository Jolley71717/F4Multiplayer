#pragma once

#include <chrono>

// When a stand-in may be deleted. Deleting a reference whose 3D is still being loaded crashes the
// game when the loader thread finishes (it builds the animation graph of a freed actor), so a
// stand-in is only deleted once its 3D is in, or once the load has had ample time to finish.
namespace DeferredDelete
{
	constexpr auto MAX_WAIT = std::chrono::seconds(15);  // long enough for a load stall; a reference whose 3D never came is deleted after this

	constexpr bool Safe(bool a_has3D, std::chrono::steady_clock::duration a_sinceSpawn)
	{
		return a_has3D || a_sinceSpawn >= MAX_WAIT;
	}

	// A stand-in in the player's conversation (the one speaking to them, or in the scene they are in)
	// is kept until the conversation ends: with its speaker deleted the engine never ends it (see
	// StrandedDialogue). Not for ever, though: a stand-in may not outlive its player for long.
	constexpr auto MAX_HOLD = std::chrono::seconds(60);

	constexpr bool Held(bool a_inConversation, std::chrono::steady_clock::duration a_sinceBuried)
	{
		return a_inConversation && a_sinceBuried < MAX_HOLD;
	}
}
