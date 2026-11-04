#pragma once

#include "Protocol.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <span>
#include <utility>
#include <vector>

// A friend's workshop pieces counted in our settlement: the game-free half (WorkshopResources.cpp runs it against the
// engine, tests/ResourceMirrorTests.cpp against stand-ins).
//
// A settlement's food, water, defense, beds and power are actor values on its workbench, which the game sums from the pieces
// linked to it as its workshop items. A friend's piece in our game is a copy, linked to nothing (WorkshopPower.h: a copy
// linked to a workbench, on a grid or known to the workshop scripts is how a save gets corrupted), so it counts for nobody
// but its builder. So, as power is mirrored:
//
// - The builder's game sends what each of its own pieces counts for in its settlement (Reporter): pieces it built (made at
//   runtime), linked to one of its workbenches, as its game counts them.
// - A friend's game keeps that per (builder, piece) (Tally), sums it over the copies it has of those pieces for each of
//   its workbenches (a piece both saves hold, of which it made no copy, already counts there), and puts the sums on the
//   workbench as a modifier of its actor values (Applier): never by linking a copy.
// - The modifier is the session's alone: taken off before every save (Hush) and put back after, taken off when the friend
//   leaves and when the session ends.
namespace ResourceMirror
{
	using Clock = std::chrono::steady_clock;
	using namespace std::chrono_literals;

	inline constexpr std::size_t KINDS = Protocol::RESOURCE_KINDS;
	using Values = std::array<float, KINDS>;
	using Entry = Protocol::ResourceState;

	// The values, in the wire's order.
	enum Kind : std::size_t
	{
		kFood,
		kWater,
		kSafety,
		kBeds,
		kPower,        // power made
		kPowerNeeded,  // power the pieces take
	};

	inline constexpr std::array<const char*, KINDS> NAMES{ "food", "water", "safety", "beds", "power", "powerNeeded" };

	[[nodiscard]] inline bool Any(const Values& a_values)
	{
		return std::ranges::any_of(a_values, [](float a_v) { return a_v != 0.0f; });
	}

	inline constexpr float EPSILON = 0.001f;  // a value or a change smaller than this is none

	// What one piece counts for: each value a count (a negative one, one under EPSILON, or one that is no number counts for
	// nothing), at most its kind's Protocol::MAX_RESOURCE_PIECE.
	[[nodiscard]] inline Values Clean(Values a_values)
	{
		for (std::size_t k = 0; k < KINDS; ++k) {
			auto& value = a_values[k];
			value = std::isfinite(value) && value >= EPSILON ? (std::min)(value, Protocol::MAX_RESOURCE_PIECE[k]) : 0.0f;
		}
		return a_values;
	}

	// The most a workbench holds of friends' pieces of each kind, from one friend and from all of them: past what any honest
	// settlement reaches within the game's build limit (a water farm of 40 industrial purifiers makes 1600 water). What a
	// modified game says past it counts as this.
	inline constexpr Values MAX_BENCH{ 500.0f, 2000.0f, 1000.0f, 200.0f, 2000.0f, 1000.0f };

	inline void CapBench(Values& a_values)
	{
		for (std::size_t k = 0; k < KINDS; ++k) {
			a_values[k] = (std::min)(a_values[k], MAX_BENCH[k]);
		}
	}

	// ---- The builder's side: what each of our pieces counts for ---------------------------------------

	inline constexpr auto        POLL = 1s;           // how often our pieces are looked at
	inline constexpr auto        SNAPSHOT = 30s;      // all of them sent again this often (a lost or late joiner catches up)
	inline constexpr std::size_t MAX_TRACKED = 4096;  // pieces of ours that count for something (a big settlement has a few hundred)

	// What a look at one of our pieces finds (the Engine's Look).
	struct Look
	{
		enum class State
		{
			kHere,  // loaded: it counts for `values` now (all 0: for nothing)
			kAway,  // not loaded (its cell isn't): it counts as it did, and nothing is sent
			kGone,  // no piece of ours any more (scrapped, stored, deleted, disabled, unlinked): forgotten, sent as nothing
		};
		State  state = State::kGone;
		Values values{};
	};

	// Our pieces and what was last sent for each.
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

		// The next poll sends every piece's values (someone joined; a save was loaded).
		void SendAll() { all = true; }

		// What to send now: the pieces whose values changed since they were last sent, or all of them (SendAll, and every
		// SNAPSHOT), each loaded one as it counts now. A piece gone is forgotten, and sent as counting for nothing if it last
		// counted for something: a script may disable it, or unlink it, with no scrap that takes its copy. The Engine gives:
		//
		//   Look Look(std::uint32_t ref)
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
				const auto look = a_engine.Look(it->first);
				if (look.state == Look::State::kGone) {
					if (it->second && Any(*it->second)) {
						out.push_back({ it->first, {} });
					}
					it = tracked.erase(it);
					continue;
				}
				if (look.state == Look::State::kHere) {
					const auto values = Clean(look.values);
					// One that counts for nothing and never did is not sent (most of a settlement: walls, floors, lights).
					const bool news = it->second ? *it->second != values : Any(values);
					if (news || (full && (it->second || Any(values)))) {
						out.push_back({ it->first, values });
						it->second = values;
					}
				} else if (full && it->second && Any(*it->second)) {
					out.push_back({ it->first, *it->second });  // away: as last sent, for a joiner
				}
				++it;
			}
			return out;
		}

		[[nodiscard]] std::size_t Size() const { return tracked.size(); }
		[[nodiscard]] bool        Tracks(std::uint32_t a_ref) const { return tracked.contains(a_ref); }

		// The pieces that count for something, as last sent.
		[[nodiscard]] std::size_t Counting() const
		{
			return static_cast<std::size_t>(std::ranges::count_if(tracked, [](const auto& a_piece) { return a_piece.second && Any(*a_piece.second); }));
		}

		void Clear()
		{
			tracked.clear();
			all = true;
			nextPoll = {};
			nextSnapshot = {};
		}

	private:
		std::map<std::uint32_t, std::optional<Values>> tracked;  // the values last sent (none: not sent yet)
		bool                                           all = true;
		Clock::time_point                              nextPoll{};
		Clock::time_point                              nextSnapshot{};
	};

	// The entries in packets of at most a_max.
	[[nodiscard]] inline std::vector<std::vector<Entry>> Chunks(const std::vector<Entry>& a_entries, std::size_t a_max = Protocol::MAX_RESOURCE_ENTRIES)
	{
		std::vector<std::vector<Entry>> chunks;
		for (std::size_t i = 0; i < a_entries.size(); i += a_max) {
			chunks.emplace_back(a_entries.begin() + static_cast<std::ptrdiff_t>(i), a_entries.begin() + static_cast<std::ptrdiff_t>((std::min)(i + a_max, a_entries.size())));
		}
		return chunks;
	}

	// ---- A friend's side: what their pieces count for, summed over our copies of them -------------------

	using Key = std::pair<std::uint32_t, std::uint32_t>;  // (builder, their reference ID): a copy's key in WorkshopSync

	inline constexpr std::size_t MAX_KEPT = 8192;

	// What each builder's game says its pieces count for.
	class Tally
	{
	public:
		// False if it's a piece not seen before and too many are kept.
		bool Set(std::uint32_t a_player, const Entry& a_entry)
		{
			const Key  key{ a_player, a_entry.refId };
			const auto values = Clean(a_entry.values);
			if (!Any(values)) {
				kept.erase(key);
				return true;
			}
			if (kept.size() >= MAX_KEPT && !kept.contains(key)) {
				++dropped;
				return false;
			}
			kept[key] = values;
			return true;
		}

		// The builder left (or their game loaded another world): their pieces count for nothing here until it says so again.
		void Forget(std::uint32_t a_player)
		{
			std::erase_if(kept, [&](const auto& a_piece) { return a_piece.first.first == a_player; });
		}

		void Clear() { kept.clear(); }

		// For each of our workbenches, what friends' pieces count for there: over the pieces we have a copy of, each at the
		// workbench its report named; each friend's sum, and the sum of them, at most MAX_BENCH. The Engine gives:
		//
		//   std::uint32_t BenchOf(const Key&)   the workbench of our copy of that piece; 0 if we have none (not placed yet,
		//                                       or one of our own stands there, or another friend's copy stands for it)
		template <class Engine>
		[[nodiscard]] std::map<std::uint32_t, Values> Totals(Engine& a_engine) const
		{
			std::map<std::pair<std::uint32_t, std::uint32_t>, Values> theirs;  // (workbench, friend)
			for (const auto& [key, values] : kept) {
				const auto bench = a_engine.BenchOf(key);
				if (bench == 0) {
					continue;
				}
				auto& total = theirs[{ bench, key.first }];
				for (std::size_t k = 0; k < KINDS; ++k) {
					total[k] += values[k];
				}
			}
			std::map<std::uint32_t, Values> totals;
			for (auto& [at, values] : theirs) {
				CapBench(values);
				auto& total = totals[at.first];
				for (std::size_t k = 0; k < KINDS; ++k) {
					total[k] += values[k];
				}
			}
			for (auto& [bench, values] : totals) {
				CapBench(values);
			}
			return totals;
		}

		[[nodiscard]] std::optional<Values> Of(const Key& a_key) const
		{
			const auto it = kept.find(a_key);
			return it == kept.end() ? std::nullopt : std::optional{ it->second };
		}

		[[nodiscard]] std::size_t Size() const { return kept.size(); }
		[[nodiscard]] std::size_t Dropped() const { return dropped; }

	private:
		std::map<Key, Values> kept;
		std::size_t           dropped = 0;
	};

	// ---- A friend's side: the totals on our workbenches ---------------------------------------------

	// The kinds put on a workbench. Not beds: the Pip-Boy shows a settlement's beds, and its daily update counts them, by
	// the value's base alone, which the game's recount sets (WorkshopResources.h). A modifier on beds would count as
	// sheltered beds a settlement doesn't have. The builder's game still sends them.
	inline constexpr std::array<bool, KINDS> APPLIED{ true, true, true, false, true, true };

	inline constexpr auto APPLY_EVERY = 1s;  // how often the totals are put on the workbenches
	inline constexpr auto HUSH_LIMIT = 5s;   // a save's hush is let go this long after the last, if no word says the save is over

	// What we put on each of our workbenches, so all of it can be taken off again: the game's own recount sets only what
	// the workbench's own pieces count for, and leaves ours be.
	class Applier
	{
	public:
		// Puts the totals on the workbenches: on each, what it holds of ours made what the totals say (a workbench not in
		// them: nothing of ours; beds: nothing, APPLIED). Not while a save's hush lasts. How many values it changed. The
		// Engine gives:
		//
		//   bool Add(std::uint32_t bench, Kind, float delta)   false if there's no such workbench here (nothing added)
		template <class Engine>
		std::size_t Apply(Engine& a_engine, const std::map<std::uint32_t, Values>& a_totals, Clock::time_point a_now)
		{
			if (hushes > 0) {
				if (a_now - hushedAt < HUSH_LIMIT) {
					return 0;
				}
				hushes = 0;
			}
			std::size_t changed = 0;
			for (const auto& [bench, want] : a_totals) {
				changed += Set(a_engine, bench, want);
			}
			for (auto it = on.begin(); it != on.end();) {
				const auto bench = it->first;
				++it;  // Set may erase it
				if (!a_totals.contains(bench)) {
					changed += Set(a_engine, bench, {});
				}
			}
			return changed;
		}

		// A save is about to be written: everything of ours taken off, so no save holds a friend's piece; put back at the
		// first Apply after the save (SaveDone), or HUSH_LIMIT after the last hush if no word comes. A second save before the
		// first one's word (a quicksave as an autosave is written) holds it off until both are done. How many values it
		// took off.
		template <class Engine>
		std::size_t Hush(Engine& a_engine, Clock::time_point a_now)
		{
			++hushes;
			hushedAt = a_now;
			return TakeAllOff(a_engine);
		}

		void SaveDone()
		{
			if (hushes > 0) {
				--hushes;
			}
		}

		// Everything of ours taken off (the session ended). How many values.
		template <class Engine>
		std::size_t TakeAllOff(Engine& a_engine)
		{
			std::size_t changed = 0;
			for (auto it = on.begin(); it != on.end();) {
				const auto bench = it->first;
				++it;
				changed += Set(a_engine, bench, {});
			}
			return changed;
		}

		// A save was loaded, or a new game started: the workbenches are the new world's, and what we had put on the old one's
		// is forgotten. A load was told first (TakeAllOff before it); a new game from the main menu is not, so what of ours is
		// still on a workbench of the same ID (its temporary modifier holds at least it: the engine did not set it back) is
		// taken off; one set back is let be. How many values it took off. The Engine gives too:
		//
		//   std::optional<float> Temporary(std::uint32_t bench, Kind)   the workbench's temporary modifier (none: no such one)
		template <class Engine>
		std::size_t ForgetWorld(Engine& a_engine)
		{
			std::size_t off = 0;
			for (const auto& [bench, have] : on) {
				for (std::size_t k = 0; k < KINDS; ++k) {
					if (have[k] == 0.0f) {
						continue;
					}
					const auto kind = static_cast<Kind>(k);
					const auto now = a_engine.Temporary(bench, kind);
					const bool still = now && (have[k] > 0.0f ? *now >= have[k] - EPSILON : *now <= have[k] + EPSILON);
					if (still && a_engine.Add(bench, kind, -have[k])) {
						++off;
					}
				}
			}
			on.clear();
			hushes = 0;
			return off;
		}

		[[nodiscard]] const std::map<std::uint32_t, Values>& On() const { return on; }
		[[nodiscard]] bool                                   Hushed() const { return hushes > 0; }

	private:
		template <class Engine>
		std::size_t Set(Engine& a_engine, std::uint32_t a_bench, const Values& a_want)
		{
			auto&       have = on[a_bench];
			std::size_t changed = 0;
			for (std::size_t k = 0; k < KINDS; ++k) {
				const float want = APPLIED[k] ? a_want[k] : 0.0f;
				// Taken off, it is all of what we added, however little: no remainder goes into a save.
				const float delta = want == 0.0f ? -have[k] : want - have[k];
				if (delta == 0.0f || (want != 0.0f && std::fabs(delta) < EPSILON) || !a_engine.Add(a_bench, static_cast<Kind>(k), delta)) {
					continue;
				}
				have[k] = want == 0.0f ? 0.0f : have[k] + delta;
				++changed;
			}
			if (!Any(have)) {
				on.erase(a_bench);
			}
			return changed;
		}

		std::map<std::uint32_t, Values> on;          // what we added to each workbench, of each value
		int                             hushes = 0;  // saves under way (Hush less SaveDone)
		Clock::time_point               hushedAt{};
	};

	// A loaded save's record (DecodeRecord) taken off its workbenches: how many values.
	template <class Engine>
	std::size_t TakeOffRecord(Engine& a_engine, const std::vector<std::pair<std::uint32_t, Values>>& a_held)
	{
		std::size_t off = 0;
		for (const auto& [bench, values] : a_held) {
			for (std::size_t k = 0; k < KINDS; ++k) {
				if (values[k] != 0.0f && std::isfinite(values[k]) && a_engine.Add(bench, static_cast<Kind>(k), -values[k])) {
					++off;
				}
			}
		}
		return off;
	}

	// ---- The save's record: what it holds of friends' pieces on our workbenches --------------------

	// Nothing, unless the hush before the save left some on (Applier::Hush: a workbench not found then), or didn't run. A
	// load takes what its save's record names off. Laid out: a count (u16), and for each workbench its form ID (u32, the
	// save's), a mask of the values it holds and a float for each.
	inline constexpr std::size_t MAX_RECORD_BENCHES = 512;  // the game has about 30 settlements, its DLCs and mods' a few hundred more

	[[nodiscard]] inline std::vector<std::uint8_t> EncodeRecord(const std::vector<std::pair<std::uint32_t, Values>>& a_benches)
	{
		std::vector<std::uint8_t> out;
		if (a_benches.empty()) {
			return out;
		}
		const auto put = [&](const void* a_data, std::size_t a_size) {
			const auto bytes = static_cast<const std::uint8_t*>(a_data);
			out.insert(out.end(), bytes, bytes + a_size);
		};
		const auto count = static_cast<std::uint16_t>((std::min)(a_benches.size(), MAX_RECORD_BENCHES));
		put(&count, sizeof(count));
		for (std::size_t i = 0; i < count; ++i) {
			const auto& [bench, values] = a_benches[i];
			std::uint8_t mask = 0;
			for (std::size_t k = 0; k < KINDS; ++k) {
				mask |= values[k] != 0.0f ? static_cast<std::uint8_t>(1u << k) : 0;
			}
			put(&bench, sizeof(bench));
			put(&mask, sizeof(mask));
			for (std::size_t k = 0; k < KINDS; ++k) {
				if (mask & (1u << k)) {
					put(&values[k], sizeof(float));
				}
			}
		}
		return out;
	}

	// Nothing if the record is cut short, too long, or holds a value that is no number.
	[[nodiscard]] inline std::vector<std::pair<std::uint32_t, Values>> DecodeRecord(std::span<const std::uint8_t> a_bytes)
	{
		std::size_t at = 0;
		const auto  take = [&](void* a_out, std::size_t a_size) {
			if (a_bytes.size() - at < a_size) {
				return false;
			}
			std::memcpy(a_out, a_bytes.data() + at, a_size);
			at += a_size;
			return true;
		};
		std::vector<std::pair<std::uint32_t, Values>> out;
		std::uint16_t                                 count = 0;
		if (!take(&count, sizeof(count)) || count > MAX_RECORD_BENCHES) {
			return {};
		}
		for (std::uint16_t i = 0; i < count; ++i) {
			std::uint32_t bench = 0;
			std::uint8_t  mask = 0;
			Values        values{};
			if (!take(&bench, sizeof(bench)) || !take(&mask, sizeof(mask)) || (mask >> KINDS) != 0) {
				return {};
			}
			for (std::size_t k = 0; k < KINDS; ++k) {
				if ((mask & (1u << k)) && (!take(&values[k], sizeof(float)) || !std::isfinite(values[k]))) {
					return {};
				}
			}
			out.emplace_back(bench, values);
		}
		return at == a_bytes.size() ? out : std::vector<std::pair<std::uint32_t, Values>>{};
	}

	// What each save's record says: made fresh for each save. The hush before it (Hushed) says what it left on; a save whose
	// hush never ran, or threw before it said, (Take with no Hushed since BeforeSave) is written with what is on now (Live),
	// never with an earlier save's.
	class SaveRecord
	{
	public:
		using Benches = std::vector<std::pair<std::uint32_t, Values>>;

		// What is on our workbenches now, whenever it changes.
		void Live(const std::map<std::uint32_t, Values>& a_on) { live.assign(a_on.begin(), a_on.end()); }

		// A save's hush starts: nothing of an earlier save's is kept.
		void BeforeSave()
		{
			forSave.clear();
			hushed = false;
		}

		// The hush is over (or threw): what it left on.
		void Hushed(const std::map<std::uint32_t, Values>& a_on)
		{
			forSave.assign(a_on.begin(), a_on.end());
			live = forSave;
			hushed = true;
		}

		// The save's record, as it is written (empty: none).
		[[nodiscard]] std::vector<std::uint8_t> Take()
		{
			auto bytes = EncodeRecord(hushed ? forSave : live);
			forSave.clear();
			hushed = false;
			return bytes;
		}

	private:
		Benches live;
		Benches forSave;
		bool    hushed = false;
	};
}
