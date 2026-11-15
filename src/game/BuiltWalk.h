#pragma once

#include "Protocol.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <unordered_set>
#include <utility>

// What a player built before the session (the host, and a guest when the host lets them): the walls, wires
// and turrets of a settlement built in an earlier game, before the mod was installed, or while hosting
// someone who has since left with the session file gone. The workshop's events only tell of building done
// while we watch, so a friend who joins would never see any of it. The host's game (and, with the host's
// bShareGuestBuilds, a guest's: Shares) looks over the references of the cells as they attach, and the
// workshop items its workbenches list, and reports each piece it built once a session, as if it had just
// been placed: the server keeps it for whoever joins later, and a friend's game that has a copy already
// (from the session file) leaves it where it stands. Building done in the session is reported by the
// workshop's events, from every game, whatever the rule.
//
// A piece counts if the game made it at runtime (a built piece's reference ID is 0xFF...), it is linked
// to a workbench as one of its workshop items, its object is something workshop mode builds, and it is
// no copy of a friend's piece. The engine is behind Engine (WorkshopSync.cpp's, or a test's stand-in), so
// the bookkeeping is what the tests drive. An Engine gives:
//
//   bool Report(const Piece&)   reported as placed; false if it couldn't be now (too many reports waiting)
namespace BuiltWalk
{
	inline constexpr std::size_t REPORT_PER_FRAME = 32;  // the send queue paces them on (SendPacer)
	inline constexpr std::size_t MAX_QUEUED = 20000;     // the server keeps no more built pieces than this anyway

	// A reference as the walk finds it.
	struct Piece
	{
		std::uint32_t ref = 0;
		std::uint32_t bench = 0;  // the workbench it is linked to as a workshop item; 0 if none
		bool          buildable = false;  // its object is one workshop mode builds
		bool          copy = false;       // one of our copies of a friend's piece
		bool          off = false;        // switched off or deleted: stored, scrapped, or on its way out
	};

	// Whether this game reports what its player built before the session: the host's always; a guest's only when the
	// host's rules say so (off by default: each player keeps their own save, and a guest's solo settlement would land on
	// top of the host's own building).
	[[nodiscard]] constexpr bool Shares(bool a_host, const Protocol::SessionRules& a_rules)
	{
		return a_host || a_rules.shareGuestBuilds;
	}

	// Whether the piece is one its player built: see above.
	[[nodiscard]] constexpr bool Built(const Piece& a_piece)
	{
		return a_piece.ref != 0 && a_piece.ref != Protocol::PLAYER_REF_ID && !Protocol::IsShareableRef(a_piece.ref) && a_piece.bench != 0 && a_piece.buildable &&
		       !a_piece.copy && !a_piece.off;
	}

	// ---- A turret or a robot is a piece, never a spawn too ------------------------------------------------
	//
	// A turret (and, with Automatron, a robot) is an actor workshop mode builds: its builder's game reports it as a piece and
	// every other game holds a copy of it, an actor too. Both are references made at run time, and a game shares the NPCs it
	// made at run time that fight as spawns (Spawns::Share). So a turret that fired was offered a second time, as a spawn: a
	// friend's game stood a second turret where its copy of the piece stood, and the friend's copy, once it fired back, went
	// to the builder's game as a spawn of the friend's. An actor that is shared as a piece (ours, or our copy of a friend's)
	// is never offered as a spawn.
	struct ActorFacts
	{
		std::uint32_t ref = 0;
		bool          copy = false;      // our copy of a friend's piece (standing, marked, or waiting to be deleted)
		bool          linked = false;    // linked to a settlement's workbench as one of its workshop items
		bool          builds = false;    // its object (the one it is reported under) is one workshop mode builds
		bool          shares = false;    // our game reports what its player built before the session (Shares)
		bool          reported = false;  // our game reported it placed in this session (built in it: reported whatever the rule)
	};

	enum class ActorPiece : std::uint8_t
	{
		kNo,    // an NPC as any other
		kCopy,  // a friend's piece, our copy
		kOurs,  // our own, which the others hold copies of
	};

	// What a runtime actor is to the workshop's sync. One our player built that no other game has a copy of (a guest's,
	// built before the session, without the host's bShareGuestBuilds) is an NPC as any other: seen while it fights, as a spawn.
	[[nodiscard]] constexpr ActorPiece PieceActor(const ActorFacts& a_facts)
	{
		if (a_facts.copy) {
			return ActorPiece::kCopy;
		}
		const bool ours = a_facts.ref != 0 && a_facts.ref != Protocol::PLAYER_REF_ID && !Protocol::IsShareableRef(a_facts.ref) && a_facts.linked && a_facts.builds &&
		                  (a_facts.shares || a_facts.reported);
		return ours ? ActorPiece::kOurs : ActorPiece::kNo;
	}

	class Walk
	{
	public:
		// A reference found in a cell, or in a workbench's list: queued to be reported, once a session.
		// False if it isn't one its player built, or it was offered already.
		bool Offer(const Piece& a_piece)
		{
			if (!Built(a_piece) || queued.size() >= MAX_QUEUED || !seen.insert(a_piece.ref).second) {
				return false;
			}
			queued.push_back(a_piece);
			return true;
		}

		// Reports up to a_max of what is queued, in the order found. One that can't be reported now stays
		// first in line, for the next frame.
		template <class Engine>
		std::size_t ReportSome(Engine& a_engine, std::size_t a_max = REPORT_PER_FRAME)
		{
			std::size_t done = 0;
			while (done < a_max && !queued.empty()) {
				if (!a_engine.Report(queued.front())) {
					break;
				}
				queued.pop_front();
				++done;
				++reported;
			}
			return done;
		}

		// A new session, or a save loaded: every piece is reported again.
		void Clear()
		{
			seen.clear();
			queued.clear();
		}

		[[nodiscard]] std::size_t   Queued() const { return queued.size(); }
		[[nodiscard]] std::uint32_t Reported() const { return reported; }

	private:
		std::unordered_set<std::uint32_t> seen;  // offered this session
		std::deque<Piece>                 queued;
		std::uint32_t                     reported = 0;  // ever, for the status line
	};
}
