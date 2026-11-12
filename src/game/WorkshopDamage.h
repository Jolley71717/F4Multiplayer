#pragma once

#include "Protocol.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

// Settlement damage and repair (docs/DESIGN.md, "Settlement damage and repair"): the rules, free of the game so they can
// be tested. WorkshopSync does them in the game, with Destructibles' engine calls.
//
// A settlement piece's damage is its builder's game's say alone. The engine keeps it as the reference's health (none
// while it was never damaged) and its destroyed flag (what workshop mode's Repair goes by), and a game that built pieces
// reports both for each of its own pieces that can be damaged (Reporter: what changed, and all of them now and then). A
// game that holds copies of those pieces shows what the builder's game says on them and nothing else (Mirror): a copy
// less damaged is wrecked to it, and a copy more damaged (its own game's raiders, a stray shot of its own player) is
// mended back to it. The engine has no veto for an object's damage, so a copy's own damage is undone at the next look,
// not prevented.
//
// A repair is the builder's game's too. A friend's Accept on a wrecked copy of the host's piece asks the host
// (WorkshopRequestOp::kRepair, the piece named as a scrap names it); the host's game takes what vanilla's repair costs
// out of the settlement's workbench (Pay) and mends the real piece with the engine's own repair, and the mended state
// comes back to every copy through the mirror.
namespace WorkshopDamage
{
	using Clock = std::chrono::steady_clock;
	using Entry = Protocol::PieceDamage;
	using Why = Protocol::WorkshopAnswer::Why;

	// ---- A piece's damage ----------------------------------------------------------------------------------

	struct State
	{
		float health = Protocol::NO_PIECE_HEALTH;  // negative: none (never damaged, or repaired)
		bool  destroyed = false;

		bool operator==(const State&) const = default;
	};

	// Two healths this close are one (a float that crossed the wire, the engine's own rounding).
	inline constexpr float HEALTH_SLACK = 0.5f;

	[[nodiscard]] constexpr bool HasHealth(const State& a_state) { return !(a_state.health < 0.0f); }
	[[nodiscard]] constexpr bool Whole(const State& a_state) { return !a_state.destroyed && !HasHealth(a_state); }

	[[nodiscard]] inline bool Same(const State& a_a, const State& a_b)
	{
		if (a_a.destroyed != a_b.destroyed || HasHealth(a_a) != HasHealth(a_b)) {
			return false;
		}
		return !HasHealth(a_a) || std::fabs(a_a.health - a_b.health) <= HEALTH_SLACK;
	}

	[[nodiscard]] inline State StateOf(const Entry& a_entry)
	{
		return { a_entry.health < 0.0f ? Protocol::NO_PIECE_HEALTH : a_entry.health, a_entry.destroyed };
	}

	[[nodiscard]] inline Entry EntryOf(std::uint32_t a_ref, const State& a_state)
	{
		return { a_ref, HasHealth(a_state) ? a_state.health : Protocol::NO_PIECE_HEALTH, a_state.destroyed };
	}

	// ---- The builder's side ---------------------------------------------------------------------------------

	inline constexpr auto        POLL = std::chrono::milliseconds(500);
	inline constexpr auto        SNAPSHOT = std::chrono::seconds(30);  // all of them again: a friend who missed one, or whose copy came late
	inline constexpr std::size_t MAX_TRACKED = 4096;

	// Our own pieces that can be damaged, and what we last said of each. Engine: `std::optional<State> Read(ref)`, none
	// once the piece is gone (it is forgotten: its scrap is the workshop stream's to tell).
	class Reporter
	{
	public:
		// A piece of ours to report from now on: false if it is not one a message can name, or there are too many.
		bool Track(std::uint32_t a_ref)
		{
			if (!Protocol::IsRuntimeRef(a_ref)) {
				return false;
			}
			if (tracked.contains(a_ref)) {
				return true;
			}
			if (tracked.size() >= MAX_TRACKED) {
				return false;
			}
			tracked.emplace(a_ref, std::nullopt);
			lost.erase(a_ref);
			return true;
		}

		void Forget(std::uint32_t a_ref)
		{
			tracked.erase(a_ref);
			lost.erase(a_ref);
		}

		// Everything is said again at the next poll (a player joined, a save loaded).
		void SendAll() { all = true; }

		// What to send now: nothing between polls; else each piece whose damage changed since we last said it, or every
		// piece when all are due.
		template <class Engine>
		[[nodiscard]] std::vector<Entry> Poll(Engine& a_engine, Clock::time_point a_now)
		{
			std::vector<Entry> out;
			if (a_now < nextPoll) {
				return out;
			}
			nextPoll = a_now + POLL;
			const bool everything = all || a_now >= nextSnapshot;
			if (everything) {
				all = false;
				nextSnapshot = a_now + SNAPSHOT;
			}
			for (auto it = tracked.begin(); it != tracked.end();) {
				const std::optional<State> now = a_engine.Read(it->first);
				if (!now) {
					// Gone for now (scrapped; or off for a moment, its model loading again). One we last said damaged is
					// remembered: back and whole, its copies still show the damage.
					if (it->second && !Whole(*it->second) && lost.size() < MAX_TRACKED) {
						lost.insert(it->first);
					}
					it = tracked.erase(it);
					continue;
				}
				if (everything || !it->second || !Same(*it->second, *now)) {
					out.push_back(EntryOf(it->first, *now));
					it->second = *now;
				}
				++it;
			}
			return out;
		}

		[[nodiscard]] std::size_t Size() const { return tracked.size(); }
		[[nodiscard]] bool        Tracks(std::uint32_t a_ref) const { return tracked.contains(a_ref); }
		// A piece that went away while we had said it damaged: to be tracked again when it is back, whatever its state
		// (a piece with nothing to damage and no damage is otherwise not worth tracking).
		[[nodiscard]] bool LostDamaged(std::uint32_t a_ref) const { return lost.contains(a_ref); }
		// What we last said of a piece: none if it is not tracked, or nothing was said yet.
		[[nodiscard]] std::optional<State> Said(std::uint32_t a_ref) const
		{
			const auto it = tracked.find(a_ref);
			return it != tracked.end() ? it->second : std::nullopt;
		}

		void Clear()
		{
			tracked.clear();
			lost.clear();
			all = true;
			nextPoll = {};
			nextSnapshot = {};
		}

	private:
		std::map<std::uint32_t, std::optional<State>> tracked;
		std::set<std::uint32_t>                       lost;  // gone while said damaged (LostDamaged)
		bool                                          all = true;
		Clock::time_point                             nextPoll{};
		Clock::time_point                             nextSnapshot{};
	};

	// The entries as packets' worth: a_max (a message's MAX_DAMAGE_ENTRIES) at most in each.
	[[nodiscard]] inline std::vector<std::vector<Entry>> Chunks(const std::vector<Entry>& a_entries, std::size_t a_max = Protocol::MAX_DAMAGE_ENTRIES)
	{
		std::vector<std::vector<Entry>> out;
		const auto                      max = (std::max<std::size_t>)(a_max, 1);
		for (std::size_t i = 0; i < a_entries.size(); i += max) {
			const auto end = (std::min)(a_entries.size(), i + max);
			out.emplace_back(a_entries.begin() + static_cast<std::ptrdiff_t>(i), a_entries.begin() + static_cast<std::ptrdiff_t>(end));
		}
		return out;
	}

	// ---- The copies' side -----------------------------------------------------------------------------------

	using Key = std::pair<std::uint32_t, std::uint32_t>;  // the builder, and the piece's id in their game

	inline constexpr auto        REFRESH = std::chrono::milliseconds(500);
	inline constexpr std::size_t MAX_MIRRORED = 8192;
	// A copy is changed this often at most: one our own game keeps wrecking (a fire it stands in) is left wrecked for the
	// rest of the window rather than mended twice a second.
	inline constexpr std::uint32_t MAX_SHOWS = 6;
	inline constexpr auto          SHOW_WINDOW = std::chrono::seconds(60);
	// Shows a frame at most: each reloads a copy's 3D (some switch it off and on for the model), so an explosion that wrecked
	// forty copies is put back over a few frames, not in one. The rest wait for the next frame, from where this one stopped.
	inline constexpr std::size_t MAX_SHOWS_PER_FRAME = 4;

	// What a copy needs to show what its builder's game says.
	enum class Step : std::uint8_t
	{
		kNothing,        // it shows it already
		kLower,          // less damaged than the builder's: wrecked to it
		kMend,           // the builder's is whole: mended
		kMendThenLower,  // more damaged than the builder's, which is damaged too: mended, then wrecked to it
	};

	[[nodiscard]] constexpr const char* Name(Step a_step)
	{
		switch (a_step) {
		case Step::kNothing:
			return "nothing";
		case Step::kLower:
			return "wrecked";
		case Step::kMend:
			return "mended";
		case Step::kMendThenLower:
			return "mended and wrecked";
		}
		return "?";
	}

	[[nodiscard]] inline Step PlanFor(const State& a_shown, const State& a_wanted)
	{
		if (Same(a_shown, a_wanted)) {
			return Step::kNothing;
		}
		if (Whole(a_wanted)) {
			return Step::kMend;
		}
		// The engine only ever lowers a health and sets the destroyed flag: a copy further gone than the builder's piece in
		// either starts again from whole.
		const bool tooDestroyed = a_shown.destroyed && !a_wanted.destroyed;
		const bool tooLow = HasHealth(a_shown) && (!HasHealth(a_wanted) || a_shown.health < a_wanted.health - HEALTH_SLACK);
		return tooDestroyed || tooLow ? Step::kMendThenLower : Step::kLower;
	}

	// A copy its own game's damage took away (a stage that disables the piece: the builder's piece doesn't, or it would not
	// be told) is switched back on once a show mended it and what its builder says doesn't take it away too.
	[[nodiscard]] constexpr bool BringsBack(bool a_takenAway, Step a_step, bool a_wantedTakesAway)
	{
		return a_takenAway && (a_step == Step::kMend || a_step == Step::kMendThenLower) && !a_wantedTakesAway;
	}

	// What each builder's game says of its pieces, and our copies kept to it. Engine: `std::optional<State> Shown(key)`
	// (our copy's damage as the engine has it; none while we have no live copy of that piece) and
	// `bool Show(key, step, wanted)` (false: it could not be done now).
	class Mirror
	{
	public:
		// The builder's game says so: false if there is no room for another piece.
		bool Set(std::uint32_t a_player, const Entry& a_entry)
		{
			const Key key{ a_player, a_entry.refId };
			const auto it = wants.find(key);
			if (it != wants.end()) {
				const auto state = StateOf(a_entry);
				if (!Same(it->second.state, state)) {
					// A new word: the copy is shown it at once, whatever its window spent on the old one (a copy left wrecked
					// by the cap is mended as soon as the host repairs the piece).
					it->second.shows = 0;
					it->second.capped = false;
				}
				it->second.state = state;
				return true;
			}
			if (wants.size() >= MAX_MIRRORED) {
				++dropped;
				return false;
			}
			wants.emplace(key, Want{ StateOf(a_entry) });
			return true;
		}

		void Forget(const Key& a_key) { wants.erase(a_key); }

		// A builder left: their game says nothing more, and their copies stay as they are.
		void ForgetPlayer(std::uint32_t a_player)
		{
			std::erase_if(wants, [&](const auto& a_want) { return a_want.first.first == a_player; });
		}

		// Our copies were made again (a save loaded): each may be shown afresh.
		void Invalidate()
		{
			for (auto& [key, want] : wants) {
				want.shows = 0;
				want.capped = false;
			}
			nextRefresh = {};
			resumeAt.reset();
		}

		void Clear()
		{
			wants.clear();
			nextRefresh = {};
			resumeAt.reset();
		}

		// Our copies looked at, and each that differs from what its builder's game says shown as that, MAX_SHOWS_PER_FRAME
		// a frame at most: a look that runs out goes on at the next frame from the copy it stopped at, and the next look
		// after the last copy comes REFRESH later. How many were changed.
		template <class Engine>
		std::size_t Refresh(Engine& a_engine, Clock::time_point a_now)
		{
			if (a_now < nextRefresh) {
				return 0;
			}
			nextRefresh = a_now + REFRESH;
			std::size_t changed = 0;
			std::size_t shows = 0;
			auto        it = resumeAt ? wants.lower_bound(*resumeAt) : wants.begin();
			resumeAt.reset();
			for (; it != wants.end(); ++it) {
				auto& [key, want] = *it;
				const std::optional<State> shown = a_engine.Shown(key);
				if (!shown) {
					continue;
				}
				const auto step = PlanFor(*shown, want.state);
				if (step == Step::kNothing) {
					continue;
				}
				if (shows >= MAX_SHOWS_PER_FRAME) {
					resumeAt = key;
					nextRefresh = a_now;  // the next frame
					++deferred;
					break;
				}
				if (!MayShow(want, a_now)) {
					continue;
				}
				++shows;
				if (!a_engine.Show(key, step, want.state)) {
					++failed;
					continue;
				}
				++changed;
				++(step == Step::kLower ? lowered : mended);
			}
			return changed;
		}

		[[nodiscard]] std::optional<State> Wanted(const Key& a_key) const
		{
			const auto it = wants.find(a_key);
			return it != wants.end() ? std::optional<State>{ it->second.state } : std::nullopt;
		}

		[[nodiscard]] std::size_t   Size() const { return wants.size(); }
		[[nodiscard]] std::uint32_t Dropped() const { return dropped; }
		[[nodiscard]] std::uint32_t Lowered() const { return lowered; }
		[[nodiscard]] std::uint32_t Mended() const { return mended; }   // mends, with or without a wreck after
		[[nodiscard]] std::uint32_t Failed() const { return failed; }
		[[nodiscard]] std::uint32_t Capped() const { return capped; }   // windows in which a copy was left as it was
		[[nodiscard]] std::uint32_t Deferred() const { return deferred; }  // looks that ran out of shows for the frame

	private:
		struct Want
		{
			State             state;
			std::uint32_t     shows = 0;
			Clock::time_point windowStart{};
			bool              capped = false;
		};

		// Counts a show against the copy's window; false once the window is spent (counted once a window).
		bool MayShow(Want& a_want, Clock::time_point a_now)
		{
			if (a_want.shows == 0 || a_now - a_want.windowStart >= SHOW_WINDOW) {
				a_want.windowStart = a_now;
				a_want.shows = 0;
				a_want.capped = false;
			}
			if (a_want.shows >= MAX_SHOWS) {
				if (!std::exchange(a_want.capped, true)) {
					++capped;
				}
				return false;
			}
			++a_want.shows;
			return true;
		}

		std::map<Key, Want> wants;
		Clock::time_point   nextRefresh{};
		std::optional<Key>  resumeAt;  // where a look that ran out of shows goes on
		std::uint32_t       deferred = 0;
		std::uint32_t       dropped = 0;
		std::uint32_t       lowered = 0;
		std::uint32_t       mended = 0;
		std::uint32_t       failed = 0;
		std::uint32_t       capped = 0;
	};

	// ---- A friend's repair: the friend's side ----------------------------------------------------------------

	// The menu's Accept on a copy of the host's piece (vanilla: Accept on a wrecked piece repairs it, on any other grabs it).
	enum class Press : std::uint8_t
	{
		kNotDamaged,  // the grab, refused as ever
		kPending,     // something is asked of the host for this piece already
		kOff,         // the host doesn't let friends edit their pieces
		kAsk,         // the repair is asked of the host
	};

	[[nodiscard]] constexpr Press OnAccept(bool a_damaged, bool a_pending, bool a_editAllowed)
	{
		if (!a_damaged) {
			return Press::kNotDamaged;
		}
		if (a_pending) {
			return Press::kPending;
		}
		return a_editAllowed ? Press::kAsk : Press::kOff;
	}

	inline constexpr const char* PENDING_NOTICE = "workshop.shared.repair_pending";
	inline constexpr const char* OFF_NOTICE = "workshop.shared.repair_off";
	inline constexpr const char* REFUSED_NOTICE = "workshop.shared.repair_refused";

	// What the friend's game makes of the host's answer: the notice to show, and whether the answer says the settlement
	// is not the host's (learned, as a build's answer teaches it). Nothing is done to the copy either way: the mended
	// state comes with the builder's next report.
	struct Verdict
	{
		const char* notice = nullptr;
		bool        benchNotOwned = false;
	};

	[[nodiscard]] constexpr Verdict Judge(Why a_why)
	{
		switch (a_why) {
		case Why::kApplied:
			return { "workshop.shared.repaired" };
		case Why::kNotAllowed:
			return { OFF_NOTICE };
		case Why::kPieceBusy:
			return { "workshop.shared.piece_busy" };
		case Why::kNoSuchPiece:
			return { "workshop.shared.repair_no_piece" };
		case Why::kNoMaterials:
			return { "workshop.shared.repair_no_materials" };
		case Why::kNotDamaged:
			return { "workshop.shared.repair_not_damaged" };
		case Why::kBenchUnopened:
			return { "workshop.shared.repair_bench_unopened" };
		case Why::kNotHostOwned:
			return { REFUSED_NOTICE, true };
		default:
			return { REFUSED_NOTICE };
		}
	}

	// ---- A friend's repair: the host's side ------------------------------------------------------------------

	// What the host's game finds of a repair request (Refusal looks in this order, as a scrap's does).
	struct Facts
	{
		bool benchKnown = false;
		bool hostOwned = false;
		bool benchLoaded = false;
		bool benchOpened = false;       // the workbench has its workshop data (made when its workshop is first opened, saved with it)
		bool editAllowed = false;
		bool otherBenchOpen = false;    // the host's workshop mode is open at another workbench
		bool targetKnown = false;
		bool targetAtBench = false;     // one of this workbench's own pieces, built (made at runtime): not one the settlement came with, a copy, the bench or a wire
		bool targetRepairable = false;  // not an actor (a turret's repair is its own sync's), and something builds it (it has a recipe)
		bool targetLoaded = false;
		bool targetHeld = false;        // in the host's hands in workshop mode
		bool damaged = false;           // the engine's own test: destroyed
	};

	// Why the host's game refuses a repair, none if it may go on to the cost.
	[[nodiscard]] constexpr std::optional<Why> Refusal(const Facts& a_facts)
	{
		if (!a_facts.benchKnown || !a_facts.hostOwned) {
			return Why::kNotHostOwned;
		}
		if (!a_facts.benchLoaded) {
			return Why::kBenchNotLoaded;
		}
		if (!a_facts.benchOpened) {
			return Why::kBenchUnopened;
		}
		if (!a_facts.editAllowed) {
			return Why::kNotAllowed;
		}
		if (a_facts.otherBenchOpen) {
			return Why::kHostBusy;
		}
		if (!a_facts.targetKnown || !a_facts.targetAtBench) {
			return Why::kNoSuchPiece;
		}
		if (!a_facts.targetRepairable) {
			return Why::kNotWorkshopItem;
		}
		if (!a_facts.targetLoaded) {
			return Why::kBenchNotLoaded;
		}
		if (a_facts.targetHeld) {
			return Why::kPieceBusy;
		}
		if (!a_facts.damaged) {
			return Why::kNotDamaged;
		}
		return std::nullopt;
	}

	// ---- The cost ---------------------------------------------------------------------------------------------

	// One line of a cost: a component (or, for the rare recipe that names one, an item itself) and how many.
	struct Cost
	{
		std::uint32_t form = 0;
		std::uint32_t count = 0;

		bool operator==(const Cost&) const = default;
	};

	// Vanilla's repair cost of one recipe line: its count times fWorkshopRepairComponentMult, cut to a whole number, one at
	// least (Workshop::RequestRepairItem).
	[[nodiscard]] inline std::uint32_t RepairCount(std::uint32_t a_recipeCount, float a_mult)
	{
		if (!std::isfinite(a_mult) || a_mult <= 0.0f) {
			return 1;
		}
		const double scaled = std::floor(static_cast<double>(a_recipeCount) * static_cast<double>(a_mult));
		return scaled < 1.0 ? 1u : scaled > 4294967295.0 ? 4294967295u : static_cast<std::uint32_t>(scaled);
	}

	// A recipe's lines as a repair's cost: a form named twice is one line.
	[[nodiscard]] inline std::vector<Cost> RepairCost(const std::vector<Cost>& a_recipe, float a_mult)
	{
		std::vector<Cost> out;
		for (const auto& line : a_recipe) {
			if (line.form == 0) {
				continue;
			}
			const auto count = RepairCount(line.count, a_mult);
			const auto it = std::ranges::find(out, line.form, &Cost::form);
			if (it != out.end()) {
				it->count += count;
			} else {
				out.push_back({ line.form, count });
			}
		}
		return out;
	}

	// One thing the workbench holds: how many, and the components one of it scraps into (none: not junk). A loose
	// component ("Steel") is an item of one part.
	struct Held
	{
		std::uint32_t                                        item = 0;
		std::uint32_t                                        count = 0;
		std::vector<std::pair<std::uint32_t, std::uint32_t>> parts;  // component, how many in one
	};

	// What paying does to the workbench: takes (negative) first, then what is put back (positive: the parts of the junk
	// broken up that the repair didn't use, as loose components).
	struct Payment
	{
		std::vector<std::pair<std::uint32_t, std::int64_t>> changes;  // item, count
		std::uint32_t                                       lost = 0;  // spare parts with no loose item to put back as
	};

	// How the workbench pays a cost, as the engine's own workshop does from its containers: the thing itself, then loose
	// components and junk that scraps into it (the item of fewest parts first), with the parts left over put back loose.
	// a_looseOf: each component's loose item. None if the workbench can't pay all of it: then nothing is taken.
	[[nodiscard]] inline std::optional<Payment> Pay(const std::vector<Held>& a_bench, const std::vector<Cost>& a_cost, const std::map<std::uint32_t, std::uint32_t>& a_looseOf)
	{
		std::map<std::uint32_t, std::uint64_t> left;   // item -> how many the bench still holds
		std::map<std::uint32_t, std::uint64_t> taken;  // item -> how many taken
		std::map<std::uint32_t, std::uint64_t> spare;  // component -> parts of broken junk not used yet
		for (const auto& held : a_bench) {
			left[held.item] += held.count;
		}
		// The junk, the item of fewest parts first (a loose component before a desk fan), then by id (the same on every run).
		std::vector<const Held*> junk;
		for (const auto& held : a_bench) {
			if (!held.parts.empty()) {
				junk.push_back(&held);
			}
		}
		const auto size = [](const Held* a_held) {
			std::uint64_t parts = 0;
			for (const auto& part : a_held->parts) {
				parts += part.second;
			}
			return parts;
		};
		std::ranges::stable_sort(junk, [&](const Held* a_a, const Held* a_b) {
			const auto a = size(a_a), b = size(a_b);
			return a != b ? a < b : a_a->item < a_b->item;
		});
		for (const auto& line : a_cost) {
			std::uint64_t need = line.count;
			const auto    use = [&](std::uint64_t& a_from) {
				const auto n = (std::min)(need, a_from);
				a_from -= n;
				need -= n;
				return n;
			};
			use(spare[line.form]);
			if (need > 0) {
				if (const auto it = left.find(line.form); it != left.end()) {
					taken[line.form] += use(it->second);  // the thing itself
				}
			}
			for (const auto held : junk) {
				if (need == 0) {
					break;
				}
				std::uint64_t each = 0;
				for (const auto& part : held->parts) {
					if (part.first == line.form) {
						each += part.second;
					}
				}
				auto& have = left[held->item];
				if (each == 0 || have == 0) {
					continue;
				}
				const auto items = (std::min)(have, (need + each - 1) / each);
				have -= items;
				taken[held->item] += items;
				for (const auto& part : held->parts) {
					spare[part.first] += static_cast<std::uint64_t>(part.second) * items;
				}
				use(spare[line.form]);
			}
			if (need > 0) {
				return std::nullopt;
			}
		}
		Payment payment;
		std::map<std::uint32_t, std::int64_t> net;
		for (const auto& [item, count] : taken) {
			net[item] -= static_cast<std::int64_t>(count);
		}
		for (const auto& [component, count] : spare) {
			if (count == 0) {
				continue;
			}
			const auto loose = a_looseOf.find(component);
			if (loose == a_looseOf.end() || loose->second == 0) {
				payment.lost += static_cast<std::uint32_t>((std::min<std::uint64_t>)(count, 0xFFFFFFFFu));
				continue;
			}
			net[loose->second] += static_cast<std::int64_t>(count);
		}
		// Takes first, so the workbench never holds more than it did between two changes.
		for (const auto& [item, count] : net) {
			if (count < 0) {
				payment.changes.emplace_back(item, count);
			}
		}
		for (const auto& [item, count] : net) {
			if (count > 0) {
				payment.changes.emplace_back(item, count);
			}
		}
		return payment;
	}
}
