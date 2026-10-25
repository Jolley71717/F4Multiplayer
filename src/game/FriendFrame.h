#pragma once

#include <chrono>
#include <cstdint>

// Whether our player (or a companion of theirs) is kept out of a power armor frame: one that stands in our game only
// because the session placed a copy of a friend's frame here. Getting into such a copy made it ours ("it is theirs now")
// while the friend's own frame stayed theirs in their game, and their game shared it again after their next load or
// rejoin: a second copy was placed on the same spot, two suits where the friend has one. Until borrowing a friend's
// suit is made (the frame staying the owner's, the friend only using it), our player is told whose it is instead
// (Equipment's frame gate: getting in, before the engine starts getting them in; Transfer, whose menu is closed again).
// A companion ordered into one is sent out of it again (SendsOut, RemotePlayers).
// Every copy the session placed is kept out, whoever used it last: each game makes its own runtime frames (an
// encounter's, a script's), a joiner's game shares a frame only when its player used it, and Spawns places only copies
// of what a friend's game shared, so a frame our own world made is never one of these copies. What our player loses is
// only a suit that only the friend's world has, which is borrowing. Never kept out: a frame from a plugin (every game
// has its own reference of those, the museum's suit in Concord among them, wherever FrameShare has moved ours), our
// player's own (in it, getting in, or got out of it last: a copy taken before this stays usable), and one a quest
// alias of ours holds. Kept free of the game so the rule can be tested.
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
		return a_facts.sharedBy;
	}

	// Who is in a frame our player is kept out of, as far as sending them out goes.
	struct Occupant
	{
		bool player = false;    // our player (kept out as they get in: never found in one)
		bool standIn = false;   // a friend's stand-in, switched into its frame copy by the plugin
		bool runsHere = false;  // our game runs the actor (no friend's game drives it here)
	};

	// Whether someone in a frame is sent out of it again (Actor.SwitchToPowerArmor with None) rather than the frame given to
	// them as theirs: a companion ordered into a friend's frame copy, mostly. The engine's command walks them to it and has
	// them get in without asking the frame, so it cannot be turned down beforehand. a_keptOutFor: KeptOutFor's answer.
	constexpr bool SendsOut(const Occupant& a_occupant, std::uint32_t a_keptOutFor)
	{
		return a_keptOutFor != 0 && !a_occupant.player && !a_occupant.standIn && a_occupant.runsHere;
	}

	// A frame has no 3D while someone is in it. Got out of, it gets it back only once their way out is over, some seconds
	// on. Until then the copy is left as it is (RemotePlayers::BeingSentOut): without its 3D it would be taken for one that
	// never loaded and placed again. For a moment at least, so whatever looks at it next sees the 3D that came back; never
	// longer than SETTLE_LIMIT, so one that never gets it back is placed again in the end.
	constexpr auto SETTLE_MIN = std::chrono::seconds(1);
	constexpr auto SETTLE_LIMIT = std::chrono::seconds(30);

	// Whether a frame copy got out of a_since ago is still left as it is. a_back: its 3D is in (or the frame is gone).
	constexpr bool Settling(bool a_back, std::chrono::steady_clock::duration a_since)
	{
		return a_since <= SETTLE_MIN || (!a_back && a_since <= SETTLE_LIMIT);
	}
}
