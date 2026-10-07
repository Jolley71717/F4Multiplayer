#pragma once

#include <chrono>

// When a stand-in may be deleted. Deleting a reference whose 3D is still being loaded crashes the
// game when the loader thread finishes (it builds the animation graph of a freed actor), so a
// stand-in is only deleted once its 3D is in, or once the load has had ample time to finish.
namespace DeferredDelete
{
	constexpr auto MAX_WAIT = std::chrono::seconds(3);

	constexpr bool Safe(bool a_has3D, std::chrono::steady_clock::duration a_sinceSpawn)
	{
		return a_has3D || a_sinceSpawn >= MAX_WAIT;
	}
}
