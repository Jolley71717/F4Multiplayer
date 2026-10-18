#pragma once

#include "game/RemoteEdits.h"
#include "game/StandInHold.h"

#include <chrono>
#include <cstddef>
#include <cstdint>

// A power armor frame standing in the world, shared between games the way an NPC is (NpcSync): one game runs it (its
// owner, granted by the server on a claim) and tells where it stands; every other game moves its own reference of
// that frame there. A frame is furniture, not an actor (TESFurniture with the engine's power armor keyword), so it is
// found by a walk of the loaded cells rather than among the actors, has no AI to turn off and nothing of its own to
// mirror: it stands where it was left until somebody gets into it and walks off in it. Its pieces are its own game's
// (the loot rules keep a frame's inventory per player: LootScope), so nothing of them is shared here.
// Which frames: the ones a plugin placed (the museum's suit in Concord), the same reference in every game. One made at
// runtime (0xFF......) is another reference, or none, in the other games: it is shared as a runtime NPC is instead,
// under a spawn id, and the others place a copy of it (SharesMade, below).
// Who runs one: whoever last got into or out of it in this session (the last to use it has it, wherever it stands
// now), else the host's game, whose world is the session's: a frame nobody has used stands where the host's world has
// it, and a joiner's copy, standing where the joiner's own world left it (on the museum roof, while the host's is in
// a station at Sanctuary), is brought to it. Kept free of the game so the rules can be tested.
namespace FrameShare
{
	using Duration = std::chrono::steady_clock::duration;
	using RemoteEdits::Spot;

	// Our reference of a frame, as our game has it.
	struct Ours
	{
		bool  found = false;     // a reference of that ID is here and not deleted
		bool  created = false;   // one made in this game (0xFF......): not the one the session means
		bool  frame = false;     // its base is a power armor frame
		bool  off = false;       // disabled: the engine disables a frame while an actor wears it
		bool  loaded = false;    // its 3D is in
		bool  quest = false;     // it fills a quest alias of ours (the museum's suit is the main quest's objective)
		bool  inUse = false;     // an actor of ours is in it
		bool  wornHere = false;  // our own player is in it (off, too)
		bool  copy = false;      // one the session placed here (a friend's stand-in's, the one they left, a copy of a
		                         // frame their game made): never ours to share
		Spot  at;                // where it stands here
		float heading = 0.0f;
	};

	// ---- Running one: what our game shares -------------------------------------------------------------------

	enum class Share
	{
		kNo,      // not here, made here, not a frame, or disabled by something other than our player's wear: let go
		kStands,  // standing, loaded: offered when nobody runs it (Offers), kept and reported while we run it
		kWorn,    // our player is in it: kept and reported from where it was as they got in (the engine disables it
		          // there, and puts it back where they get out), so a friend arriving mid-wear has it where we got in
		kAway,    // ours, in a cell no longer loaded here: kept and reported from where our world has it
	};

	constexpr Share ChooseShare(const Ours& a_ours)
	{
		if (!a_ours.found || a_ours.created || !a_ours.frame) {
			return Share::kNo;
		}
		if (a_ours.wornHere) {
			return Share::kWorn;
		}
		if (a_ours.off) {
			return Share::kNo;
		}
		return a_ours.loaded ? Share::kStands : Share::kAway;
	}

	// Whether our game goes on running a frame it runs.
	constexpr bool Keeps(Share a_share)
	{
		return a_share != Share::kNo;
	}

	// Whether our game offers to run a frame nobody runs: any game for one its player used (got into or out of in
	// this session, or had left last as the session began: the last to use it has it, wherever it stands), and the
	// host's game for any standing in its loaded cells (its world is the session's). A joiner's game never offers one
	// it has not used: its copy stands where its own world left it, which is nobody else's business until the host's
	// game, or a user, says where it stands.
	constexpr bool Offers(Share a_share, bool a_hosting, bool a_usedHere)
	{
		if (a_share == Share::kNo) {
			return false;
		}
		return a_usedHere || (a_hosting && a_share == Share::kStands);
	}

	// The frame our player used, as NpcSync's scan sees it: a_worn is the frame they are in now (0: none), a_wornBefore
	// the one they were in at the last scan, a_last the one they are in or left last (PlayerCharacter::lastUsedPowerArmor,
	// which the save keeps). Getting in or out of one is a use of it, taken over from whoever runs it; otherwise the
	// last used one, which the caller offers only the first time it sees it.
	constexpr std::uint32_t Used(std::uint32_t a_wornBefore, std::uint32_t a_worn, std::uint32_t a_last)
	{
		if (a_worn != a_wornBefore) {
			return a_worn ? a_worn : a_wornBefore;
		}
		return a_last;
	}

	// A frame's state goes once a second while it stands (a joiner hears of it within the second), and at once when
	// it moved or turned: the owner got out of it somewhere else, or the physics knocked it over. NPCs send twenty a
	// second, which a frame that never moves on its own has no use for.
	constexpr auto  STATE_INTERVAL = std::chrono::seconds(1);
	constexpr float MOVED = 1.0f;   // game units
	constexpr float TURNED = 0.02f; // radians

	constexpr bool Due(bool a_moved, Duration a_sinceSent)
	{
		return a_moved || a_sinceSent >= STATE_INTERVAL;
	}

	// Whether where it stands now is news against what was last sent of it (none sent: a_sent false).
	inline bool Moved(bool a_sent, const Spot& a_sentAt, float a_sentHeading, const Spot& a_now, float a_heading)
	{
		if (!a_sent) {
			return true;
		}
		const float distance = RemoteEdits::Distance(a_sentAt, a_now);
		return !RemoteEdits::Within(distance, MOVED) || StandInHold::AngleBetween(a_sentHeading, a_heading) > TURNED;
	}

	// ---- One our game made: shared as a spawn --------------------------------------------------------------------

	// A frame our game made at runtime (0xFF......: placed by a script, a quest or an encounter rather than by a plugin,
	// such as the frame of a power armor NPC the game made) is another reference, or none, in every other game.
	// So it is shared as a runtime NPC is (Spawns, SpawnBook.h): under a spawn id the server grants, run by our game for
	// good, its state sent as a pre-placed one's is (Due), and every other game places a copy of it where ours stands
	// (SpawnBook::Theirs::DecideFrame). A friend's suit parked before we joined is told to us so, as the server tells a
	// joiner every spawn shared so far.
	// Shared: our player's own (got into or out of in this session, or left last as it began) wherever our world still
	// has it, their suit parked at home while they roam; and, from the host's game, any standing loaded there (its world
	// is the session's, as for a pre-placed frame: Offers). A joiner's game never shares a stranger's: each game makes
	// its own frames at runtime (an encounter's, a script's), and a joiner's sent to the host would stand a second suit
	// beside the host's own in the same spot. One that only stood in a cell since unloaded is let go, and the copies go
	// with it. Never while somebody is in it: the engine disables a worn frame, and the wearer's stand-in shows it then;
	// the copies go and it is shared afresh where they get out. Never a copy the session placed here (that would send a
	// friend's own frame back to them).
	constexpr bool SharesMade(const Ours& a_ours, bool a_hosting, bool a_usedHere)
	{
		if (!a_ours.found || !a_ours.created || !a_ours.frame || a_ours.copy) {
			return false;
		}
		if (a_ours.wornHere || a_ours.off || a_ours.inUse) {
			return false;
		}
		return a_usedHere || (a_hosting && a_ours.loaded);
	}

	// Of the spawns one game may share at once (Protocol::MAX_SPAWNS_PER_PLAYER), frames take this many at most: the
	// rest are for a fight's NPCs, which come and go. Our player's own are offered first.
	constexpr std::size_t MAX_MADE = 12;

	constexpr bool RoomForMade(std::size_t a_shared)
	{
		return a_shared < MAX_MADE;
	}

	// ---- A frame another game runs: ours is moved to where theirs stands ---------------------------------------

	enum class Move
	{
		kThere,      // ours stands where theirs does: nothing to do
		kMissing,    // not here (or made here): nothing to move
		kNotFrame,   // another kind of reference by that ID (another plugin in that slot)
		kOff,        // disabled here (our player wears it): never moved, never switched on by a friend's word
		kQuest,      // a quest of ours holds it: our player may have to find it or get into it where our story put it
		kInUse,      // an actor of ours is in it
		kOutOfView,  // where theirs stands is not in the area our engine draws: ours stays where it is for now
		kNudge,      // loaded in that cell or world, near: moved in place
		kBring,      // elsewhere, far, or not loaded: moved through the console, as an NPC's copy is brought in
	};

	// Nearer than this to where theirs stands, loaded in the same cell or world, ours is moved in place; farther, or
	// in another place, it is moved as a reference is between cells.
	constexpr float NUDGE_LIMIT = 256.0f;

	// a_theirs: where the owner's game has it. a_inView: that spot is in the area our engine draws around our player
	// (our interior, or an exterior cell attached around ours).
	inline Move Decide(const Ours& a_ours, const Spot& a_theirs, float a_theirHeading, bool a_inView)
	{
		if (!a_ours.found || a_ours.created) {
			return Move::kMissing;
		}
		if (!a_ours.frame) {
			return Move::kNotFrame;
		}
		if (a_ours.off) {
			return Move::kOff;
		}
		if (a_ours.quest) {
			return Move::kQuest;
		}
		if (a_ours.inUse) {
			return Move::kInUse;
		}
		const float distance = RemoteEdits::Distance(a_ours.at, a_theirs);  // NaN in another place
		if (a_ours.loaded && RemoteEdits::Within(distance, MOVED) && StandInHold::AngleBetween(a_ours.heading, a_theirHeading) <= TURNED) {
			return Move::kThere;
		}
		if (!a_inView) {
			return Move::kOutOfView;
		}
		return a_ours.loaded && RemoteEdits::Within(distance, NUDGE_LIMIT) ? Move::kNudge : Move::kBring;
	}
}
