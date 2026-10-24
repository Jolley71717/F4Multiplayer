#pragma once

#include <cstdint>

// Whether our player is kept out of a power armor frame: one that stands in our game only because the session placed a
// copy of a friend's frame here. Getting into such a copy made it ours ("it is theirs now") while the friend's own frame
// stayed theirs in their game, and their game shared it again after their next load or rejoin: a second copy was placed
// on the same spot, two suits where the friend has one. Until borrowing a friend's suit is made (the frame staying the
// owner's, the friend only using it), our player is told whose it is instead (Equipment's frame use, before the engine
// starts getting them in).
// Kept narrow, so nobody is left without a suit: only the copies of the frame a friend used last (where they parked it
// as their save had it, or got out of it this session), never a frame from a plugin (every game has its own reference
// of those, the museum's suit in Concord among them, wherever FrameShare has moved ours), never a runtime frame no
// friend used (the host's game shares the ones standing in its loaded cells too), never our player's own, and never one
// a quest alias of ours holds. Kept free of the game so the rule can be tested.
namespace FriendFrame
{
	struct Facts
	{
		bool          ours = false;     // our player is in it, getting in, or got out of it last
		bool          quest = false;    // it fills a quest alias of ours
		bool          created = false;  // made at runtime (0xFF......): no plugin placed it
		std::uint32_t parkedFor = 0;    // the friend whose parked frame this is our copy of (RemotePlayers' parked copy)
		std::uint32_t leftFor = 0;      // the friend whose stand-in this copy was placed for: worn by it, or left where they got out
		std::uint32_t sharedBy = 0;     // the friend whose game shares the frame this is our copy of (Spawns)
		bool          sharedUsed = false;  // and it is the one that friend got out of last: it stands where they say they parked
	};

	// The friend whose frame it is, when our player is kept out of it; 0 lets them in.
	constexpr std::uint32_t KeptOutFor(const Facts& a_facts)
	{
		if (a_facts.ours || a_facts.quest || !a_facts.created) {
			return 0;
		}
		if (a_facts.parkedFor != 0) {
			return a_facts.parkedFor;
		}
		if (a_facts.leftFor != 0) {
			return a_facts.leftFor;
		}
		return a_facts.sharedUsed ? a_facts.sharedBy : 0;
	}
}
