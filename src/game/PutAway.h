#pragma once

#include "BuiltWalk.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

// A friend's own pieces at a settlement the host owns, put away for the session (docs/DESIGN.md, "Whose settlement it is").
//
// A friend and the host who both own Sanctuary each have their own version of it. In a session everyone sees the host's: the
// host's pieces are our copies (SharedWorkshop), so our own, built before the session, would stand through them. For the
// session they are hidden and walk-through: the scene-graph node is culled and the collision of its bodies is switched off.
// Never disabled, deleted, moved, unlinked from the workbench or given a keyword: neither a node's culled flag nor a body's
// collision switch is part of a saved reference, so a save made meanwhile (or a crash) holds every piece as before, and
// nothing has to be undone in a save hook. Leaving the session (or the host losing the settlement) shows them again.
//
// A piece the friend builds during the session is not put away: at a settlement the host owns it is the host's game's (a
// copy), and if the host was away, it stays the friend's own and stands.
//
// This header is the bookkeeping: which pieces count, which the engine was told about (and for which 3D), and which are to be
// shown again. The engine's side (the 3D, the collision) is WorkshopSync.cpp's.
namespace PutAway
{
	// How many pieces are put away (or shown again) in one frame: a settlement is hundreds of pieces.
	inline constexpr std::size_t PER_FRAME = 48;

	// A reference found in a host-owned workbench's list.
	struct Piece
	{
		std::uint32_t ref = 0;
		std::uint32_t bench = 0;
		bool          buildable = false;  // its object is one workshop mode builds
		bool          copy = false;       // one of our copies of a friend's piece
		bool          off = false;        // switched off or deleted: stored, scrapped
		bool          madeNow = false;    // built or handed over in this session: stands (see above)
	};

	// Whether the friend's game puts pieces away at all: the local switch (bPutAwayOwnSettlements), a friend's game (not the
	// host's) in a session whose shared workshop is the rule and whose host is there. Anything else shows all.
	[[nodiscard]] constexpr bool Active(bool a_switch, bool a_handsOver)
	{
		return a_switch && a_handsOver;
	}

	// Whether the piece is one of ours to put away: built before the session by our player at a workbench of the host's (the
	// one BuiltWalk counts), and not one made in this session.
	[[nodiscard]] constexpr bool Eligible(const Piece& a_piece, bool a_hostsBench)
	{
		const BuiltWalk::Piece built{ .ref = a_piece.ref, .bench = a_piece.bench, .buildable = a_piece.buildable, .copy = a_piece.copy, .off = a_piece.off };
		return a_hostsBench && !a_piece.madeNow && BuiltWalk::Built(built);
	}

	// What a round of looking at the workbenches asked to do.
	struct Release
	{
		std::uint32_t ref = 0;
		std::uint32_t bench = 0;
		std::uintptr_t root = 0;  // the 3D the engine was told about (0: none)
	};

	class Away
	{
	public:
		// A round of the host's settlements (ascending), each of which is looked at (the loaded ones) or not.
		void Begin()
		{
			++round;
		}

		// A piece found this round, with the 3D it has now (an opaque value, 0 for none: a piece in a cell not loaded). True
		// when the engine is to be told: it is new, or its 3D is not the one told. The piece is then taken as told.
		bool Want(std::uint32_t a_ref, std::uint32_t a_bench, std::uintptr_t a_root)
		{
			auto& entry = kept[a_ref];
			entry.bench = a_bench;
			entry.seen = round;
			if (a_root == 0 || a_root == entry.root) {
				return false;
			}
			entry.root = a_root;
			return true;
		}

		// The round is over. a_looked: the workbenches whose list was read (loaded ones); a_hosted: the host's settlements
		// (ascending) as of now. A piece goes back to shown if its workbench is no longer the host's, or its workbench was
		// read and no longer lists it as one to put away (scrapped, stored, handed over).
		template <class Looked>
		std::vector<Release> End(const Looked& a_looked, const std::vector<std::uint32_t>& a_hosted)
		{
			std::vector<Release> out;
			for (auto it = kept.begin(); it != kept.end();) {
				const auto bench = it->second.bench;
				const bool hosted = std::ranges::binary_search(a_hosted, bench);
				const bool listed = it->second.seen == round;
				const bool looked = std::ranges::find(a_looked, bench) != a_looked.end();
				if (!hosted || (looked && !listed)) {
					out.push_back({ it->first, bench, it->second.root });
					it = kept.erase(it);
				} else {
					++it;
				}
			}
			std::ranges::sort(out, {}, &Release::ref);
			return out;
		}

		// Every piece (leaving the session, the switch turned off): all go back to shown.
		std::vector<Release> All()
		{
			std::vector<Release> out;
			for (const auto& [ref, entry] : kept) {
				out.push_back({ ref, entry.bench, entry.root });
			}
			std::ranges::sort(out, {}, &Release::ref);
			kept.clear();
			counted.clear();
			return out;
		}

		// Put away: not to be picked in workshop mode, not to stand for the host's piece at its spot (OursAt).
		[[nodiscard]] bool Has(std::uint32_t a_ref) const { return kept.contains(a_ref); }
		[[nodiscard]] std::size_t Size() const { return kept.size(); }

		[[nodiscard]] std::size_t At(std::uint32_t a_bench) const
		{
			return static_cast<std::size_t>(std::ranges::count_if(kept, [&](const auto& a_entry) { return a_entry.second.bench == a_bench && a_entry.second.root != 0; }));
		}

		// The workbenches whose put-away count differs from the last time this was asked, with the count (for one log line each).
		std::vector<std::pair<std::uint32_t, std::size_t>> Changed()
		{
			std::vector<std::pair<std::uint32_t, std::size_t>> out;
			std::unordered_map<std::uint32_t, std::size_t>     now;
			for (const auto& [ref, entry] : kept) {
				if (entry.root != 0) {
					++now[entry.bench];
				}
			}
			for (const auto& [bench, count] : now) {
				if (!counted.contains(bench) || counted[bench] != count) {
					out.emplace_back(bench, count);
				}
			}
			for (const auto& [bench, count] : counted) {
				if (!now.contains(bench)) {
					out.emplace_back(bench, 0);
				}
			}
			counted = std::move(now);
			std::ranges::sort(out);
			return out;
		}

	private:
		struct Entry
		{
			std::uint32_t bench = 0;
			std::uintptr_t root = 0;  // the 3D the engine was told about last
			std::uint64_t seen = 0;
		};

		std::unordered_map<std::uint32_t, Entry>       kept;
		std::unordered_map<std::uint32_t, std::size_t> counted;
		std::uint64_t                                  round = 0;
	};
}
