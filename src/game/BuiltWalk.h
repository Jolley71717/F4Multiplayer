#pragma once

#include "Protocol.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <unordered_set>
#include <utility>

// What the host built before the session: the walls, wires and turrets of a settlement built in an
// earlier game, before the mod was installed, or while hosting someone who has since left with the
// session file gone. The workshop's events only tell of building done while we watch, so a friend who
// joins would never see any of it. The host's game looks over the references of the cells as they
// attach, and the workshop items its workbenches list, and reports each piece it built once a session,
// as if it had just been placed: the server keeps it for whoever joins later, and a friend's game that
// has a copy already (from the session file) leaves it where it stands.
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

	// Whether the piece is one the host built: see above.
	[[nodiscard]] constexpr bool Built(const Piece& a_piece)
	{
		return a_piece.ref != 0 && a_piece.ref != Protocol::PLAYER_REF_ID && !Protocol::IsShareableRef(a_piece.ref) && a_piece.bench != 0 && a_piece.buildable &&
		       !a_piece.copy && !a_piece.off;
	}

	class Walk
	{
	public:
		// A reference found in a cell, or in a workbench's list: queued to be reported, once a session.
		// False if it isn't one the host built, or it was offered already.
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
