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
	// A copy shown again (drawn anew, or settled: SETTLE) more often than this in REDRAW_WINDOW is let be until the window
	// is over: each show switches its lights and sends its graph "Powered" or "Unpowered", which a switch, a lamp or a
	// machine answers with its sound, and a copy whose 3D the game kept making anew was shown again four times a second
	// with no end.
	inline constexpr std::uint8_t MAX_REDRAWS = 6;
	inline constexpr auto         REDRAW_WINDOW = 60s;
	// A new drawing is shown once more when it has been there this long: the game's own setup of a 3D just made (its
	// cell attaching) may still run after the first show. A graph loading after the 3D is waited for (Drawing), not left
	// to this one try. A 3D made anew sooner than this is never shown twice.
	inline constexpr auto SETTLE = 2s;
	// A copy shown powered is shown unpowered while a save is written (Mirror::Hush) and powered again once it is over
	// (SaveDone), or this long after, should no message say the save is over.
	inline constexpr auto HUSH_LIMIT = 5s;

	// What a copy is drawn as, for Mirror (the Engine's Drawn): a value that changes whenever the game makes anew what
	// shows its power, and 0 while there is nothing to show it on yet.
	//
	// Only a light (LIGH) is lit by switching its light (TESObjectLIGH::SwitchLight does nothing for any other form:
	// 1.11.240 returns at once unless the base is a LIGH). Every other workshop light is an activator whose light, glow and
	// motion are its animation graph's (the game's generic workshop graphs, "Powered" / "Unpowered"): the construction
	// light, the light box, the high-tech lights, the strobe, the fan light, the mirror ball. The game loads that graph
	// after the 3D, on its own, and drops an event sent before it is there. When a graph is loaded the game shows a powered
	// workshop item of its own as powered (a graph load's init, 1.11.240 +529120), but does nothing for a reference that
	// isn't a workshop item, such as our copy: a graph made anew starts unpowered whatever the copy was shown before.
	//
	// So a copy whose model names a graph (a_namesGraph) is drawn only once the graph is loaded (a_graph), and is drawn
	// anew whenever the graph is (a_graph changes) as well as the model (a_model).
	[[nodiscard]] inline std::uintptr_t Drawing(std::uintptr_t a_model, bool a_inScene, bool a_namesGraph, std::uintptr_t a_graph)
	{
		if (a_model == 0 || !a_inScene || (a_namesGraph && a_graph == 0)) {
			return 0;
		}
		if (a_graph == 0) {
			return a_model;
		}
		const std::uintptr_t mixed = a_model ^ (a_graph * static_cast<std::uintptr_t>(0x9E3779B97F4A7C15ull) + (a_graph >> 7));
		return mixed != 0 ? mixed : 1;
	}

	// What we last told a copy's loaded graph, as far as it took it: nothing (a graph new to us: made anew, or brought back,
	// with what it showed, by a load or by its cell loading again), "Powered" or "Unpowered".
	enum class Told : std::uint8_t
	{
		kNothing,
		kPowered,
		kUnpowered,
	};

	// A loaded graph refused "Powered" or "Unpowered" (a_on): it has no such move from where it stands. Whether that means
	// it shows what was asked already (shown: no try spent), or not yet (tried again, a try spent):
	//
	// - "Unpowered" refused: dark already (a new graph starts unpowered, and one told "Unpowered" took it), but not one we
	//   told "Powered": it may still be turning on, and a lit copy left lit is a friend's generator running in our game.
	// - "Powered" refused: lit already (we told it so, or it came back lit: a save keeps what a graph showed, and so does a
	//   cell that unloads and loads again), but not one we told "Unpowered": it may still be turning off. A graph new to us
	//   that refuses "Powered" is taken as lit: a graph made anew takes "Powered" at once (every first show on the rig took
	//   it), so a refusal is one that came back lit. Should it be one not ready yet after all, the copy is only left dark
	//   where its builder's is lit (and is shown once more when it settles, SETTLE), never lit where it should be dark; and
	//   giving up after MAX_TRIES left it as it was, too.
	[[nodiscard]] constexpr bool RefusalShows(bool a_on, Told a_told)
	{
		return a_on ? a_told != Told::kUnpowered : a_told != Told::kPowered;
	}

	// What showing a copy powered or not took (WorkshopSync's ShowPoweredHow), from what the show found on it (ShowParts).
	enum class Took : std::uint8_t
	{
		kNoModel,   // no 3D: nothing shown
		kLight,     // a light's light switched
		kGraph,     // the graph took the event
		kRefused,   // the graph is loaded and didn't take it (a try spent)
		kNothing,   // no light, belt or graph: nothing to show it on
		kDark,      // a graph not told Powered by us refused Unpowered: dark already
		kLit,       // a graph not told Unpowered by us refused Powered: lit already
		kBelt,      // a conveyor belt set running or stopped
	};

	// What a show found on a copy, and what its graph made of the event.
	//
	// A conveyor belt (Contraptions Workshop's belts and lifts) runs by the engine's own conveyor on its reference
	// (BGSConveyorBelt::ConveyorBelt, one of its init actions, ExtraInitActions), not by its graph or a light: running or
	// stopped is only ever set by Papyrus (ObjectReference.ConveyorBeltOn, whose native nothing else in 1.11.240 calls),
	// so by the belt's own script on the power events, which a copy never gets (the power handler isn't run for copies:
	// their scripts would take them for ours). A show sets it as ConveyorBeltOn would, without the events.
	struct ShowParts
	{
		bool model = false;         // its 3D is there
		bool light = false;         // a light (LIGH): its light was switched
		bool belt = false;          // it has a conveyor belt: set running or stopped
		bool graph = false;         // its graph is loaded
		bool graphTook = false;     // ...and took the event
		bool alreadyShows = false;  // ...or refused it, showing so already (GraphsTold::Refused)
	};

	[[nodiscard]] constexpr Took Outcome(const ShowParts& a_parts, bool a_on)
	{
		if (!a_parts.model) {
			return Took::kNoModel;
		}
		if (!a_parts.graph) {
			return a_parts.light ? Took::kLight : a_parts.belt ? Took::kBelt : Took::kNothing;
		}
		if (a_parts.graphTook) {
			return Took::kGraph;
		}
		if (a_parts.alreadyShows) {
			return a_parts.light ? Took::kLight : (a_on ? Took::kLit : Took::kDark);
		}
		// A refusing graph spends no try when the light or the belt is what shows the power: that was set.
		return a_parts.light ? Took::kLight : a_parts.belt ? Took::kBelt : Took::kRefused;
	}

	// Whether the show counts (Mirror's Show): not one with no 3D yet, nor one a loaded graph refused.
	[[nodiscard]] constexpr bool ShowCounts(Took a_took)
	{
		return a_took != Took::kNoModel && a_took != Took::kRefused;
	}

	// Whether a copy shown a_on, drawn the same since, still shows it (Mirror's Holds): its light there while shown on and
	// gone while shown off (a_lit, for a light), and its conveyor belt running while shown on and stopped while shown off
	// (a_beltRuns, for a piece with a belt: the copy's own script may set it again, its OnLoad with the copy unpowered here,
	// after our show). None of either: it holds (its graph keeps what it was told).
	[[nodiscard]] constexpr bool StillShows(std::optional<bool> a_lit, std::optional<bool> a_beltRuns, bool a_on)
	{
		return (!a_lit || *a_lit == a_on) && (!a_beltRuns || *a_beltRuns == a_on);
	}

	// What we last told each copy's graph (by the copy's form ID, with the graph's address: a graph made anew is told nothing
	// yet), for RefusalShows.
	class GraphsTold
	{
	public:
		// The graph took the event.
		void Took(std::uint32_t a_copy, std::uintptr_t a_graph, bool a_on) { told[a_copy] = { a_graph, a_on ? Told::kPowered : Told::kUnpowered }; }

		// The graph refused the event: true if it shows what was asked already (RefusalShows), and it counts as told so from
		// now on (a lit graph that refused "Powered" is one a later "Unpowered" must turn off: tried again if refused).
		bool Refused(std::uint32_t a_copy, std::uintptr_t a_graph, bool a_on)
		{
			if (!RefusalShows(a_on, Of(a_copy, a_graph))) {
				return false;
			}
			Took(a_copy, a_graph, a_on);
			return true;
		}

		[[nodiscard]] Told Of(std::uint32_t a_copy, std::uintptr_t a_graph) const
		{
			const auto it = told.find(a_copy);
			return it != told.end() && it->second.graph == a_graph ? it->second.what : Told::kNothing;
		}

		void Forget(std::uint32_t a_copy) { told.erase(a_copy); }
		void Clear() { told.clear(); }
		[[nodiscard]] std::size_t Size() const { return told.size(); }

	private:
		struct Entry
		{
			std::uintptr_t graph = 0;
			Told           what = Told::kNothing;
		};
		std::map<std::uint32_t, Entry> told;
	};

	// What each builder's game says of its pieces, and whether each copy shows it yet. A copy shows it once it is drawn
	// (its 3D there and the graph it names loaded: Drawing) and the game took it, once more when that drawing has settled
	// (SETTLE), and again whenever it is drawn anew (it came back into a loaded cell, was made again, or got a new graph):
	// a new 3D or graph starts as the model has it, unpowered.
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
				it->second.powered = a_entry.powered;  // shown again; a 3D already there and settled isn't shown twice
				it->second.hushed = false;
				it->second.shown = false;
				it->second.tries = 0;
				it->second.frozen = false;
			} else if (it->second.frozen) {
				it->second.frozen = false;  // their game is back: shown afresh
				it->second.hushed = false;
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

		// Every copy is shown again, and once more when settled (a save was loaded: the copies are new references).
		void Invalidate()
		{
			for (auto& [key, want] : wants) {
				want.shown = false;
				want.tries = 0;
				want.settle = true;
				want.hushed = false;
			}
		}

		// A save is about to be written. A copy's 3D keeps what it was shown: a graph told "Powered" is saved so, and the
		// game puts it back at the load with the copy, which the save holds. A friend's generator then ran its engine in a
		// game with no session to say it had stopped (or one whose friend has no such piece any more), and its copy can't
		// be scrapped (it carries the block keyword). So each copy shown powered is shown unpowered now, and powered again
		// after the save (SaveDone, or HUSH_LIMIT after if no message says it is over). How many it hushed.
		//
		// Only a copy drawn as it was when a frame last showed it (the same 3D and graph). One drawn anew since (its cell
		// came in with a travel, and the save is that travel's autosave, before any frame has run) is left as it is: the
		// game may still be setting that 3D and its graph up as the save comes, no frame touches a copy then (Refresh waits
		// for it), and the hush, calling on every such copy at once (a big settlement's hundreds), was the one thing that
		// did. The cost: such a copy may have come back lit (a cell that loads again keeps what its graph showed:
		// RefusalShows), and that save keeps it lit, a friend's machine running after a load of it, as before 0.10.1. That
		// is the trade for no call on a 3D the game is still setting up.
		template <class Engine>
		std::size_t Hush(Engine& a_engine, Clock::time_point a_now)
		{
			std::size_t hushed = 0;
			saveDone = false;
			for (auto& [key, want] : wants) {
				if (want.hushed) {
					want.hushedAt = a_now;  // still hushed from a save just before: this one's too
					continue;
				}
				if (!want.powered || !want.shown) {
					continue;
				}
				if (const auto drawn = a_engine.Drawn(key); drawn == 0 || drawn != want.drawn) {
					continue;  // not drawn, or drawn anew since its show: unpowered as its model has it
				}
				if (a_engine.Show(key, false)) {
					want.hushed = true;
					want.hushedAt = a_now;
					++hushed;
				}
			}
			hushedTotal += hushed;
			return hushed;
		}

		// The save is over: what it hushed is shown powered again at the next look.
		void SaveDone()
		{
			saveDone = true;
			nextRefresh = {};
		}

		void Clear()
		{
			wants.clear();
			nextRefresh = {};
			saveDone = false;
		}

		// Shows the copies that don't show what their builder's game says yet; how many it showed. The Engine gives:
		//
		//   std::uintptr_t Drawn(const Key&)            what the copy is drawn as (Drawing: changes when its 3D or its
		//                                               graph is made anew); 0 if there's no copy here, it isn't drawn, or
		//                                               its graph isn't loaded yet (waited for, no try spent)
		//   bool           Show(const Key&, bool on)    false if the game didn't take it (try again): a light's light
		//                                               switched, or the graph took the event
		//
		// and, if it has it:
		//
		//   bool           Holds(const Key&, bool on)   false if a copy shown so doesn't show it now, though drawn the same
		//                                               (a light's light gone while shown on): shown again (Lapses)
		template <class Engine>
		std::size_t Refresh(Engine& a_engine, Clock::time_point a_now)
		{
			if (a_now < nextRefresh) {
				return 0;
			}
			nextRefresh = a_now + REFRESH;
			std::size_t shown = 0;
			for (auto& [key, want] : wants) {
				if (want.hushed) {
					// Back to what its builder's game says (a frozen copy's too: it keeps what it showed), once the save is over.
					if (!saveDone && a_now - want.hushedAt < HUSH_LIMIT) {
						continue;
					}
					want.hushed = false;
					const auto now3D = a_engine.Drawn(key);
					if (now3D != 0 && a_engine.Show(key, want.powered)) {
						want.drawn = now3D;  // drawn anew meanwhile: this is the show for it
						++shown;
					} else {
						want.shown = false;  // not drawn, or the game won't take it yet: shown as any copy is, with the usual tries
						want.tries = 0;
					}
					continue;
				}
				if (want.frozen) {
					continue;
				}
				const auto drawn = a_engine.Drawn(key);
				if (drawn == 0) {
					continue;
				}
				if (want.shown && want.drawn == drawn) {
					if (!Holds(a_engine, key, want.powered)) {
						// Shown so, and drawn the same, but not showing it now (a light whose light the game's queued switch
						// took away after it was shown on): shown again, as often as a copy drawn anew is.
						++lapses;
						if (Redrawn(want, a_now) && a_engine.Show(key, want.powered)) {
							++shown;
						}
						continue;
					}
					if (!want.settle || a_now < want.settleAt) {
						continue;
					}
					want.settle = false;  // once, however the game takes it
					if (Redrawn(want, a_now) && a_engine.Show(key, want.powered)) {
						++shown;
					}
					continue;
				}
				if (want.drawn != drawn) {
					const bool anew = want.drawn != 0;
					want.drawn = drawn;
					want.tries = 0;
					want.settle = true;
					if (anew && !Redrawn(want, a_now)) {
						want.shown = true;  // let be until the window is over and it is drawn anew again
						want.settle = false;
						continue;
					}
				}
				if (a_engine.Show(key, want.powered)) {
					want.shown = true;
					want.settleAt = a_now + SETTLE;
					++shown;
				} else if (++want.tries >= MAX_TRIES) {
					want.shown = true;  // the game won't take it (no graph, or already so): not asked again until drawn anew
					want.settle = false;
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
		[[nodiscard]] std::size_t Hushed() const { return hushedTotal; }
		[[nodiscard]] std::size_t Lapses() const { return lapses; }

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
			bool           settle = false;          // its 3D is to be shown once more, at settleAt (SETTLE after its first show)
			Clock::time_point settleAt{};
			bool           hushed = false;          // shown unpowered for a save (Hush), to be shown powered again
			Clock::time_point hushedAt{};
		};

		// A show again of a copy already shown (drawn anew, or settled) counted in its window: false if that is one too
		// many (MAX_REDRAWS), and it is let be.
		bool Redrawn(Want& a_want, Clock::time_point a_now)
		{
			if (a_want.redraws == 0 || a_now - a_want.redrawsSince >= REDRAW_WINDOW) {  // a window starts with its first redraw, not at the clock's epoch
				a_want.redrawsSince = a_now;
				a_want.redraws = 0;
			}
			if (++a_want.redraws > MAX_REDRAWS) {
				++redrawsCapped;
				return false;
			}
			return true;
		}

		template <class Engine>
		static bool Holds(Engine& a_engine, const Key& a_key, bool a_on)
		{
			if constexpr (requires { a_engine.Holds(a_key, a_on); }) {
				return a_engine.Holds(a_key, a_on);
			} else {
				return true;
			}
		}

		std::map<Key, Want> wants;
		std::size_t         lapses = 0;  // copies found not showing what they were shown (Holds), shown again
		std::size_t         dropped = 0;
		std::size_t         givenUp = 0;
		std::size_t         redrawsCapped = 0;
		std::size_t         hushedTotal = 0;
		bool                saveDone = false;  // the save a hush was for is over (SaveDone)
		Clock::time_point   nextRefresh{};
	};
}
