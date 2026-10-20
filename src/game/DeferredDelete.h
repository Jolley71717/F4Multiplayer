#pragma once

#include <chrono>
#include <optional>

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

	// A reference of ours goes in two steps: disabled, so the engine takes its 3D and collision out at its own safe
	// point, and marked for deletion only this long after. Both in one frame crashed a physics job still handling a
	// collision with the freed actor (CharacterCollisionHandler -> Actor::GetDesiredProcessLevel -> Get3D): in play,
	// and after a save, whose flush (RemotePlayers::DespawnAll) once did both at once.
	constexpr auto DELETE_AFTER_DISABLE = std::chrono::seconds(1);

	// Whether a reference disabled a_sinceDisabled ago (nothing: not disabled yet) may be marked for deletion now.
	constexpr bool MayDelete(std::optional<std::chrono::steady_clock::duration> a_sinceDisabled)
	{
		return a_sinceDisabled && *a_sinceDisabled >= DELETE_AFTER_DISABLE;
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
