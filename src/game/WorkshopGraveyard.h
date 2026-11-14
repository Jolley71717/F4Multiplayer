#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

// The copies of friends' workshop pieces waiting to be deleted (WorkshopSync's graveyard): their cell isn't loaded, or
// their 3D still is (DeferredDelete). The graveyard is looked through four times a second, each one not loaded looked up
// by its ID, until it goes or a load lets the whole list go. A copy is removed more than once (a friend's scrap, then
// leaving the session): it waits once, the newer entry in place of the older (an ID names one reference at a time, so
// the newer entry is the one the ID stands for), and no more than MAX_WAITING wait. Past that a copy is not kept: it
// stays in our world (as one left over by an earlier session would), which is said once and counted. Free of the game:
// tests/WorkshopGraveyardTests.cpp.
namespace WorkshopGraveyard
{
	inline constexpr std::size_t MAX_WAITING = 4096;

	enum class Admitted
	{
		kAdded,    // waits now
		kAlready,  // waited already: its entry is the newer one now
		kFull,     // MAX_WAITING wait: not kept
	};

	// a_copy to wait in a_waiting (its ID, never 0, by a_idOf).
	template <class Copy, class IdOf>
	Admitted Admit(std::vector<Copy>& a_waiting, const Copy& a_copy, IdOf a_idOf)
	{
		const std::uint32_t id = a_idOf(a_copy);
		const auto          it = std::ranges::find_if(a_waiting, [&](const Copy& a_other) { return a_idOf(a_other) == id; });
		if (it != a_waiting.end()) {
			*it = a_copy;
			return Admitted::kAlready;
		}
		if (a_waiting.size() >= MAX_WAITING) {
			return Admitted::kFull;
		}
		a_waiting.push_back(a_copy);
		return Admitted::kAdded;
	}
}
