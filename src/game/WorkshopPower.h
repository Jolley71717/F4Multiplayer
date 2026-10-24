#pragma once

#include "Protocol.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <utility>
#include <vector>

// Settlement power across games: the game-free half (WorkshopSync.cpp runs it against the engine, and
// tests/WorkshopPowerTests.cpp against stand-ins).
//
// A friend's piece in our game is a copy: a plain reference, deleted when the session ends or its builder scraps it.
// The game's power grids must never name one. A grid (a settlement's PowerUtils::PowerGrid) is saved with the
// workbench, and a deleted copy's ID, left in a grid, is later handed to something else: the grid then links whatever
// that is. So:
//
// - Copies can't be picked in workshop mode (WorkshopSync gives each the workshop's "interaction blocked" keyword):
//   no wire, store, scrap or move of a friend's piece.
// - Should one end up on a grid anyway (a save from before this, or a way around the keyword), it is taken off again
//   (Audit, BeforeRemove): when it would be deleted, before every save, and every few seconds in a session. One the
//   game won't let go of is never deleted: it is only switched off, so its ID stays its own (Removal::kRetire).
//
// Whether a piece is powered is each builder's own game's say: the builder's game reports which of its pieces are
// powered (Reporter), and every other game shows its copies so (Mirror): lit, running, or dark.
namespace WorkshopPower
{
	using Clock = std::chrono::steady_clock;
	using namespace std::chrono_literals;
	using Entry = Protocol::PowerState;

	// ---- Grids -----------------------------------------------------------------------------------

	// One link of a grid: the node is joined to connection, through connector (a wire, or 0 for a piece snapped on).
	struct Edge
	{
		std::uint32_t node = 0;
		std::uint32_t connection = 0;
		std::uint32_t connector = 0;
	};

	// A settlement's power grid as the game holds it: its nodes, their links, and the nodes it powers.
	struct Grid
	{
		std::vector<std::uint32_t> nodes;
		std::vector<Edge>          edges;
		std::vector<std::uint32_t> powered;
	};

	// Every reference the grid names (sorted, each once).
	[[nodiscard]] inline std::vector<std::uint32_t> Named(const Grid& a_grid)
	{
		std::vector<std::uint32_t> ids = a_grid.nodes;
		ids.insert(ids.end(), a_grid.powered.begin(), a_grid.powered.end());
		for (const auto& edge : a_grid.edges) {
			ids.push_back(edge.node);
			ids.push_back(edge.connection);
			ids.push_back(edge.connector);
		}
		std::erase(ids, 0u);
		std::ranges::sort(ids);
		ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
		return ids;
	}

	// The references the grids name that a_which picks (sorted, each once): the copies among them (what a save must
	// never hold), or the ones that are gone.
	template <class Which>
	[[nodiscard]] std::vector<std::uint32_t> NamedWhere(std::span<const Grid> a_grids, Which a_which)
	{
		std::vector<std::uint32_t> found;
		for (const auto& grid : a_grids) {
			for (const auto id : Named(grid)) {
				if (a_which(id)) {
					found.push_back(id);
				}
			}
		}
		std::ranges::sort(found);
		found.erase(std::unique(found.begin(), found.end()), found.end());
		return found;
	}

	// ---- Taking a copy off the grid --------------------------------------------------------------

	// What ties a copy into our settlement's power.
	struct Links
	{
		bool grid = false;   // a grid of its workbench names it
		bool wires = false;  // it has power links (ExtraPowerLinks): wires strung to it, or pieces snapped on
		bool bench = false;  // it is linked to a workbench as one of its workshop items

		[[nodiscard]] bool Any() const { return grid || wires || bench; }
	};

	enum class Removal
	{
		kDelete,  // nothing ties it in (any more): deleted like any copy
		kRetire,  // the game still ties it in: switched off, never deleted, so the grid's ID still names it
	};

	struct Counts
	{
		std::uint32_t found = 0;     // copies found tied in
		std::uint32_t detached = 0;  // ...and taken off
		std::uint32_t retired = 0;   // ...that the game kept tied in (switched off, not deleted)
	};

	// Before a copy goes, or when an audit finds it: whether it may be deleted, after taking it off the grid if it is on
	// one. The Engine gives:
	//
	//   Links LinksOf(std::uint32_t copy)
	//   void  Detach(std::uint32_t copy)        takes it off its grids, its wires and its workbench, through the game
	template <class Engine>
	Removal Untie(Engine& a_engine, std::uint32_t a_copy, Counts& a_counts)
	{
		if (!a_engine.LinksOf(a_copy).Any()) {
			return Removal::kDelete;
		}
		++a_counts.found;
		a_engine.Detach(a_copy);
		if (a_engine.LinksOf(a_copy).Any()) {
			++a_counts.retired;
			return Removal::kRetire;
		}
		++a_counts.detached;
		return Removal::kDelete;
	}

	// A copy going (its builder scrapped it, the session ended): deleted if nothing ties it in, after taking it off the
	// grid if it was on one; else only switched off, for good. The Engine gives Untie's, and:
	//
	//   void Delete(std::uint32_t copy)    switched off and deleted (the save drops it)
	//   void Retire(std::uint32_t copy)    switched off and kept: its ID stays its own, so the grid names no stranger
	template <class Engine>
	Removal Remove(Engine& a_engine, std::uint32_t a_copy, Counts& a_counts)
	{
		const auto removal = Untie(a_engine, a_copy, a_counts);
		if (removal == Removal::kDelete) {
			a_engine.Delete(a_copy);
		} else {
			a_engine.Retire(a_copy);
		}
		return removal;
	}

	// Looks for copies tied into our power (named by a grid, with power links of their own, or linked to a workbench) and
	// takes them off; the ones the game kept tied in. Before every save, and every AUDIT_EVERY in a session. The Engine
	// gives Untie's, and:
	//
	//   std::vector<Grid>          Grids()                   the loaded settlements' grids
	//   std::vector<std::uint32_t> Copies()                  our copies of friends' pieces
	//   bool                       IsCopy(std::uint32_t)
	//
	// a_skip(copy): copies not to try again (the session's audit passes those the game kept tied in before: taking one off
	// again every AUDIT_EVERY re-ran the game's grid and wire code on it for the rest of the session, and the game kept it
	// all the same). They are still named among the stuck ones.
	template <class Engine, class Skip>
	std::vector<std::uint32_t> Audit(Engine& a_engine, Counts& a_counts, Skip&& a_skip)
	{
		const auto grids = a_engine.Grids();
		auto       suspects = NamedWhere(std::span<const Grid>{ grids }, [&](std::uint32_t a_id) { return a_engine.IsCopy(a_id); });
		for (const auto copy : a_engine.Copies()) {
			const auto links = a_engine.LinksOf(copy);
			if (links.wires || links.bench) {
				suspects.push_back(copy);
			}
		}
		std::ranges::sort(suspects);
		suspects.erase(std::unique(suspects.begin(), suspects.end()), suspects.end());
		std::vector<std::uint32_t> stuck;
		for (const auto copy : suspects) {
			if (a_skip(copy) || Untie(a_engine, copy, a_counts) == Removal::kRetire) {
				stuck.push_back(copy);
			}
		}
		return stuck;
	}

	template <class Engine>
	std::vector<std::uint32_t> Audit(Engine& a_engine, Counts& a_counts)
	{
		return Audit(a_engine, a_counts, [](std::uint32_t) { return false; });
	}

	inline constexpr auto AUDIT_EVERY = 2s;

	// ---- The builder's side: which of our pieces are powered ---------------------------------------

	inline constexpr auto        POLL = 500ms;        // how often our pieces are looked at
	inline constexpr auto        SNAPSHOT = 30s;      // all of them sent again this often (a lost or late joiner catches up)
	inline constexpr std::size_t MAX_TRACKED = 4096;  // pieces of ours with power (a big settlement has a few hundred)

	// Our pieces that take, make or carry power, and what was last sent for each.
	class Reporter
	{
	public:
		// False if it isn't one a friend's game could have a copy of (not made at runtime), or too many are tracked.
		bool Track(std::uint32_t a_ref)
		{
			if (!Protocol::IsRuntimeRef(a_ref) || (tracked.size() >= MAX_TRACKED && !tracked.contains(a_ref))) {
				return false;
			}
			tracked.try_emplace(a_ref);
			return true;
		}

		void Forget(std::uint32_t a_ref) { tracked.erase(a_ref); }

		// The next poll sends every piece's state (someone joined; a save was loaded).
		void SendAll() { all = true; }

		// What to send now: the pieces whose state changed since it was last sent, or all of them (SendAll, and every
		// SNAPSHOT). The Engine gives:
		//
		//   std::optional<bool> Powered(std::uint32_t ref)   none if it's no piece of ours with power now (forgotten)
		template <class Engine>
		std::vector<Entry> Poll(Engine& a_engine, Clock::time_point a_now)
		{
			std::vector<Entry> out;
			if (a_now < nextPoll) {
				return out;
			}
			nextPoll = a_now + POLL;
			const bool full = all || a_now >= nextSnapshot;
			if (full) {
				all = false;
				nextSnapshot = a_now + SNAPSHOT;
			}
			for (auto it = tracked.begin(); it != tracked.end();) {
				const auto powered = a_engine.Powered(it->first);
				if (!powered) {
					it = tracked.erase(it);
					continue;
				}
				if (full || it->second != powered) {
					out.push_back({ it->first, *powered });
					it->second = powered;
				}
				++it;
			}
			return out;
		}

		[[nodiscard]] std::size_t Size() const { return tracked.size(); }
		[[nodiscard]] bool        Tracks(std::uint32_t a_ref) const { return tracked.contains(a_ref); }

		void Clear()
		{
			tracked.clear();
			all = true;
			nextPoll = {};
			nextSnapshot = {};
		}

	private:
		std::map<std::uint32_t, std::optional<bool>> tracked;  // the state last sent (none: not sent yet)
		bool                                         all = true;
		Clock::time_point                            nextPoll{};
		Clock::time_point                            nextSnapshot{};
	};

	// The entries in packets of at most a_max.
	[[nodiscard]] inline std::vector<std::vector<Entry>> Chunks(const std::vector<Entry>& a_entries, std::size_t a_max = Protocol::MAX_POWER_ENTRIES)
	{
		std::vector<std::vector<Entry>> chunks;
		for (std::size_t i = 0; i < a_entries.size(); i += a_max) {
			chunks.emplace_back(a_entries.begin() + static_cast<std::ptrdiff_t>(i), a_entries.begin() + static_cast<std::ptrdiff_t>((std::min)(i + a_max, a_entries.size())));
		}
		return chunks;
	}

	// ---- A friend's side: their copies shown as their game powers them ------------------------------

	using Key = std::pair<std::uint32_t, std::uint32_t>;  // (builder, their reference ID): a copy's key in WorkshopSync

	inline constexpr std::size_t MAX_MIRRORED = 8192;
	inline constexpr auto        REFRESH = 250ms;  // how often the copies are looked at (a copy just drawn, a graph not ready)
	inline constexpr std::uint8_t MAX_TRIES = 8;   // a copy whose game won't take it is let be (until it is drawn anew)
	// A copy drawn anew more often than this in REDRAW_WINDOW is let be until the window is over: each show switches its
	// lights and sends its graph "Powered" or "Unpowered", which a switch, a lamp or a machine answers with its sound, and
	// a copy whose 3D the game kept making anew was shown again four times a second with no end.
	inline constexpr std::uint8_t MAX_REDRAWS = 6;
	inline constexpr auto         REDRAW_WINDOW = 60s;

	// What each builder's game says of its pieces, and whether each copy shows it yet. A copy shows it once its 3D is
	// there and the game took it (a graph not loaded yet doesn't), and again whenever it is drawn anew (it came back into
	// a loaded cell, or was made again): a new 3D starts as the model has it, unpowered.
	class Mirror
	{
	public:
		// False if it's a piece not seen before and too many are kept.
		bool Set(std::uint32_t a_player, const Entry& a_entry)
		{
			const Key key{ a_player, a_entry.refId };
			const auto it = wants.find(key);
			if (it == wants.end()) {
				if (wants.size() >= MAX_MIRRORED) {
					++dropped;
					return false;
				}
				wants.emplace(key, Want{ a_entry.powered });
				return true;
			}
			if (it->second.powered != a_entry.powered) {
				it->second = Want{ a_entry.powered };
			} else if (it->second.frozen) {
				it->second.frozen = false;  // their game is back: shown afresh
				it->second.shown = false;
				it->second.tries = 0;
			}
			return true;
		}

		void Forget(const Key& a_key) { wants.erase(a_key); }

		// The builder left: their copies keep what they show, and nothing powers or unpowers them, nor shows them again,
		// until their game says so again (Set).
		void Freeze(std::uint32_t a_player)
		{
			for (auto& [key, want] : wants) {
				if (key.first == a_player) {
					want.frozen = true;
				}
			}
		}

		// Every copy is shown again (a save was loaded: the copies are new references).
		void Invalidate()
		{
			for (auto& [key, want] : wants) {
				want.shown = false;
				want.tries = 0;
			}
		}

		void Clear()
		{
			wants.clear();
			nextRefresh = {};
		}

		// Shows the copies that don't show what their builder's game says yet; how many it showed. The Engine gives:
		//
		//   std::uintptr_t Drawn(const Key&)            the copy's 3D (any value that changes when it is drawn anew); 0
		//                                               if there's no copy here, or it isn't drawn
		//   bool           Show(const Key&, bool on)    false if the game didn't take it (try again)
		template <class Engine>
		std::size_t Refresh(Engine& a_engine, Clock::time_point a_now)
		{
			if (a_now < nextRefresh) {
				return 0;
			}
			nextRefresh = a_now + REFRESH;
			std::size_t shown = 0;
			for (auto& [key, want] : wants) {
				if (want.frozen) {
					continue;
				}
				const auto drawn = a_engine.Drawn(key);
				if (drawn == 0 || (want.shown && want.drawn == drawn)) {
					continue;
				}
				if (want.drawn != drawn) {
					const bool anew = want.drawn != 0;
					want.drawn = drawn;
					want.tries = 0;
					if (anew) {
						if (a_now - want.redrawsSince >= REDRAW_WINDOW) {
							want.redrawsSince = a_now;
							want.redraws = 0;
						}
						if (++want.redraws > MAX_REDRAWS) {
							want.shown = true;  // let be until the window is over and it is drawn anew again
							++redrawsCapped;
							continue;
						}
					}
				}
				if (a_engine.Show(key, want.powered)) {
					want.shown = true;
					++shown;
				} else if (++want.tries >= MAX_TRIES) {
					want.shown = true;  // the game won't take it (no graph, or already so): not asked again until drawn anew
					++givenUp;
				}
			}
			return shown;
		}

		[[nodiscard]] std::optional<bool> Wanted(const Key& a_key) const
		{
			const auto it = wants.find(a_key);
			return it == wants.end() ? std::nullopt : std::optional{ it->second.powered };
		}

		[[nodiscard]] std::size_t Size() const { return wants.size(); }
		[[nodiscard]] std::size_t Dropped() const { return dropped; }
		[[nodiscard]] std::size_t GivenUp() const { return givenUp; }
		[[nodiscard]] std::size_t RedrawsCapped() const { return redrawsCapped; }

		// How many copies show what their builder's game says (or were given up on).
		[[nodiscard]] std::size_t Shown() const
		{
			return static_cast<std::size_t>(std::ranges::count_if(wants, [](const auto& a_want) { return a_want.second.shown; }));
		}

	private:
		struct Want
		{
			bool           powered = false;
			bool           shown = false;
			std::uintptr_t drawn = 0;
			std::uint8_t   tries = 0;
			bool           frozen = false;          // its builder left (Freeze)
			std::uint8_t   redraws = 0;             // drawn anew since redrawsSince
			Clock::time_point redrawsSince{};
		};

		std::map<Key, Want> wants;
		std::size_t         dropped = 0;
		std::size_t         givenUp = 0;
		std::size_t         redrawsCapped = 0;
		Clock::time_point   nextRefresh{};
	};
}
