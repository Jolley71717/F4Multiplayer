#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

// Which of the game's created references SaveGuard keeps out of a save (no game needed, so it is tested).
namespace SaveStrays
{
	// A created reference SaveGuard looked at: an actor, or anything else placed at runtime (a stand-in's
	// frame copy or light, or one of the game's own).
	struct Actor
	{
		bool inCell = false;
		bool hasBase = false;  // its base form still exists
		bool deleted = false;
		bool ours = false;     // a stand-in, frame copy or light this run placed, or one marked or placed as stand-ins are (see StandIn)
		bool anchored = true;  // the engine's save code finds a cell or a worldspace for it (see Doomed)
	};

	// One the engine cannot save: for a created reference with a record of changes, the save (and the
	// engine unloading it, as a demoted actor's cell goes) asks for its cell, or else its worldspace, and
	// reads that without a check. With neither, it crashes, whoever made the reference and whether or not it
	// is deleted. So it is a stray whoever made it: losing its record beats a save that cannot be written.
	constexpr bool Doomed(const Actor& a_actor)
	{
		return !a_actor.anchored;
	}

	// In no cell, and deleted or without its base. Whether it is a stray or the game's own depends on whose it
	// is (see Stray).
	constexpr bool Gone(const Actor& a_actor)
	{
		return !a_actor.inCell && (!a_actor.hasBase || a_actor.deleted);
	}

	// A created reference is a stray when it is doomed (see Doomed), or in no cell and its base is gone (a
	// stand-in an earlier version saved, on a face copy no save keeps) or ours and deleted. The rest are
	// left alone: the game keeps created actors in no cell (leveled ones whose cell unloaded, disabled ones a
	// quest holds back), and one the game or a script deleted keeps its record on purpose (its id stays
	// taken while the record exists, so a script still holding it never finds another form under that id).
	constexpr bool Stray(const Actor& a_actor)
	{
		return Doomed(a_actor) || (Gone(a_actor) && (!a_actor.hasBase || a_actor.ours));
	}

	// Form IDs the game hands out at runtime (FF......).
	constexpr bool IsCreatedId(std::uint32_t a_id)
	{
		return (a_id >> 24) == 0xFF;
	}

	// Every stand-in is put in the player's faction at this rank when it is placed (RemotePlayers::Spawn,
	// every version since 0.4.0, with the console's addtofaction). On a base that is not unique (every
	// stand-in's but a unique iPuppetBaseForm's) the engine keeps that change on the reference itself
	// (ExtraFactionChanges, under change flag 0x80000000) and saves and loads it with the reference's other
	// extra data, deleted or not. That is read from the game's code (1.11.240) and not yet seen in a game.
	// So it marks a stand-in an earlier version saved without its friend's look having to be known.
	inline constexpr std::uint32_t STAND_IN_FACTION = 0x0001C21C;  // PlayerFaction
	inline constexpr int           STAND_IN_RANK = 1;

	// Whether a faction change on a reference (the faction's form ID, the rank) is the stand-ins' one. One
	// of the game's created actors that is deleted, in no cell and carries it is unlikely (a player's
	// console addtofaction could give it one); it would lose only the record of an actor already gone.
	constexpr bool StandInFaction(std::uint32_t a_faction, int a_rank)
	{
		return a_faction == STAND_IN_FACTION && a_rank == STAND_IN_RANK;
	}

	// What tells whose a created actor is. The base it was placed from: a leveled actor's own base is a
	// copy the game makes for it; what was placed is in its leveled data (0 when that names nothing). And
	// whether the reference carries the stand-ins' faction change (see StandInFaction).
	struct Origin
	{
		std::uint32_t base = 0;
		bool          leveled = false;
		std::uint32_t leveledFrom = 0;
		bool          standInFaction = false;
	};

	constexpr std::uint32_t PlacedFrom(const Origin& a_origin)
	{
		return a_origin.leveled ? a_origin.leveledFrom : a_origin.base;
	}

	// Whether an actor not placed this run was placed as stand-ins are, by an earlier version or before
	// a load: from a base stand-ins are placed from (a_standInBase says which: the default settlers, the
	// ini's iPuppetBaseForm, the looks friends asked for this run), or from a base made at runtime (a face
	// copy). The game makes runtime bases of its own for leveled actors; those count by what was placed (a
	// leveled list or an NPC of the game's), so the game's leveled actors are not ours. A stand-in placed
	// straight on a friend's look is known this way only once that friend has joined this run.
	template <class F>
	constexpr bool FromStandInBase(const Origin& a_origin, F&& a_standInBase)
	{
		const auto from = PlacedFrom(a_origin);
		return from != 0 && (IsCreatedId(from) || a_standInBase(from));
	}

	// Whether an actor not placed this run is a stand-in all the same: it carries the stand-ins' faction
	// change (which needs no friend to have joined), or it was placed from a stand-in base.
	template <class F>
	constexpr bool StandIn(const Origin& a_origin, F&& a_standInBase)
	{
		return a_origin.standInFaction || FromStandInBase(a_origin, a_standInBase);
	}

	// The stand-ins, their frame copies and their lights placed this game run, by form ID, with the base each was
	// placed from. The game hands ids out again (after a load it counts on from the save's last one, and an id whose
	// form and record are gone can be handed out again), so an id is only ours while the form under it has the base
	// ours had, and only until a load.
	class Placed
	{
	public:
		void Add(std::uint32_t a_id, std::uint32_t a_base)
		{
			bases[a_id] = a_base;
			any = true;
		}

		[[nodiscard]] bool Ours(std::uint32_t a_id, std::uint32_t a_base) const
		{
			const auto it = bases.find(a_id);
			return it != bases.end() && it->second == a_base;
		}

		// The base the id was placed from, while it is ours.
		[[nodiscard]] std::optional<std::uint32_t> BaseOf(std::uint32_t a_id) const
		{
			const auto it = bases.find(a_id);
			return it != bases.end() ? std::optional{ it->second } : std::nullopt;
		}

		// Whether this run ever placed one (a load does not change that).
		[[nodiscard]] bool Any() const { return any; }

		// One whose form is gone: its id may be handed out again.
		void Erase(std::uint32_t a_id) { bases.erase(a_id); }

		// A load or a new game: what was placed before it is gone (no save keeps it), and its ids name the loaded
		// game's references now, a frame copy's or a light's often with the very base ours had.
		void Clear() { bases.clear(); }

		[[nodiscard]] std::size_t Size() const { return bases.size(); }

	private:
		std::unordered_map<std::uint32_t, std::uint32_t> bases;
		bool                                             any = false;
	};

	// When SaveGuard looks for strays: before a save, or right after a load.
	enum class When
	{
		kSave,
		kLoad,
	};

	enum class Verdict
	{
		kLeave,    // not a stray
		kExclude,  // its record is dropped
		kTell,     // only logged
	};

	// A stray of ours is kept out of every save, and dropped from a save that brought it back. One of the game's is
	// kept out of a save made once this run has placed a stand-in, frame copy or light (a_session): the session may
	// have left it so (its cells cleared under it), and a save written with it cannot be loaded. Before that, a
	// doomed one of the game's is only logged, its record left as the game has it, as it is without the mod. Right
	// after a load one of the game's is only logged: it may be in the middle of getting its cell back as the load
	// settles, and is left to the sweep before the next save.
	constexpr Verdict Judge(const Actor& a_actor, When a_when, bool a_session)
	{
		if (!Stray(a_actor)) {
			return Verdict::kLeave;
		}
		if (a_actor.ours) {
			return Verdict::kExclude;
		}
		if (a_when == When::kLoad || (Doomed(a_actor) && !a_session)) {
			return Verdict::kTell;
		}
		return Verdict::kExclude;
	}

	// References of ours the game took from under us (their handle stopped resolving) while their form may still be
	// there: kept out of saves for as long as it is, and is the one we placed (the game hands ids out again). The
	// oldest go past MAX; a load forgets them all (the ids name other forms then).
	class Forgotten
	{
	public:
		static constexpr std::size_t MAX = 64;

		void Add(std::uint32_t a_id, std::uint32_t a_base)
		{
			if (a_id == 0 || std::ranges::find(entries, std::pair{ a_id, a_base }) != entries.end()) {
				return;
			}
			if (entries.size() >= MAX) {
				entries.erase(entries.begin());
			}
			entries.emplace_back(a_id, a_base);
		}

		// Keeps the ones whose form is still there with the base it was placed from: a_baseNow(id) gives the base of
		// the form under the id now, or nothing when there is none. Calls a_kept(id) for each one kept, and a_gone(id)
		// for each one let go of.
		template <class F, class G, class H>
		void Keep(F&& a_baseNow, G&& a_kept, H&& a_gone)
		{
			std::erase_if(entries, [&](const std::pair<std::uint32_t, std::uint32_t>& a_entry) {
				const std::optional<std::uint32_t> base = a_baseNow(a_entry.first);
				if (!base || *base != a_entry.second) {
					a_gone(a_entry.first);
					return true;
				}
				a_kept(a_entry.first);
				return false;
			});
		}

		template <class F, class G>
		void Keep(F&& a_baseNow, G&& a_kept)
		{
			Keep(a_baseNow, a_kept, [](std::uint32_t) {});
		}

		void Clear() { entries.clear(); }

		[[nodiscard]] std::size_t Size() const { return entries.size(); }

	private:
		std::vector<std::pair<std::uint32_t, std::uint32_t>> entries;  // id, base
	};

	// A created reference in no cell, as a sweep finds it (SaveGuard reads it from the game); whose it is, is filled
	// in by Book::Strays.
	struct Found
	{
		std::uint32_t id = 0;
		std::uint32_t base = 0;
		bool          actor = false;
		Actor         seen;
	};

	struct Judged
	{
		Found   found;
		Verdict verdict = Verdict::kLeave;
	};

	// What SaveGuard keeps between sweeps, and what a save and a load do with what a sweep finds, in the order the
	// game calls them. a_standIn(id, base) says whether an actor not placed this run was placed as stand-ins are (see
	// StandIn); it reads the game, so it is asked only of an actor in no cell that is deleted, has lost its base or
	// is doomed.
	class Book
	{
	public:
		// A stand-in, frame copy or light was placed.
		void Track(std::uint32_t a_id, std::uint32_t a_base) { placed.Add(a_id, a_base); }

		// One of ours whose handle stopped resolving (see Forgotten).
		void Forget(std::uint32_t a_id)
		{
			if (const auto base = placed.BaseOf(a_id)) {
				forgotten.Add(a_id, *base);
			}
		}

		// The forgotten ones still there (a_kept(id) for each, its record to be dropped); one whose form is gone is let
		// go of, and is not ours any more (its id may be handed out again).
		template <class B, class K>
		void KeepForgotten(B&& a_baseNow, K&& a_kept)
		{
			forgotten.Keep(a_baseNow, a_kept, [&](std::uint32_t a_id) { placed.Erase(a_id); });
		}

		// The strays among what a sweep found, each with whose it is.
		template <class S>
		std::vector<Found> Strays(std::vector<Found> a_found, S&& a_standIn) const
		{
			std::erase_if(a_found, [&](Found& a_f) {
				if (!Gone(a_f.seen) && !Doomed(a_f.seen)) {
					return true;  // held back by the game, and saved under its worldspace
				}
				a_f.seen.ours = a_f.seen.hasBase && (placed.Ours(a_f.id, a_f.base) || (a_f.actor && a_standIn(a_f.id, a_f.base)));
				return !Stray(a_f.seen);
			});
			return a_found;
		}

		// Before a save: the forgotten ones still there (a_kept), then each stray with what the save does with it.
		template <class B, class K, class S>
		std::vector<Judged> BeforeSave(B&& a_baseNow, K&& a_kept, std::vector<Found> a_found, S&& a_standIn)
		{
			KeepForgotten(a_baseNow, a_kept);
			return JudgeAll(Strays(std::move(a_found), a_standIn), When::kSave);
		}

		// Right after a load, or a new game: what was placed and forgotten before it is let go of first (see
		// Placed::Clear), then each stray the loaded game has, with what is done with it.
		template <class S>
		std::vector<Judged> AfterLoad(std::vector<Found> a_found, S&& a_standIn)
		{
			placed.Clear();
			forgotten.Clear();
			return JudgeAll(Strays(std::move(a_found), a_standIn), When::kLoad);
		}

		// Each stray there is now, with what a save would do with it, and what a load would (changes nothing).
		template <class S>
		std::vector<std::pair<Judged, Verdict>> List(std::vector<Found> a_found, S&& a_standIn) const
		{
			std::vector<std::pair<Judged, Verdict>> out;
			for (auto& f : Strays(std::move(a_found), a_standIn)) {
				const auto save = Judge(f.seen, When::kSave, placed.Any());
				const auto load = Judge(f.seen, When::kLoad, placed.Any());
				out.push_back({ { f, save }, load });
			}
			return out;
		}

		// Whether this run placed a stand-in, frame copy or light (a load does not change that).
		[[nodiscard]] bool Session() const { return placed.Any(); }
		[[nodiscard]] std::size_t PlacedCount() const { return placed.Size(); }
		[[nodiscard]] std::size_t ForgottenCount() const { return forgotten.Size(); }

	private:
		std::vector<Judged> JudgeAll(std::vector<Found> a_strays, When a_when) const
		{
			std::vector<Judged> out;
			for (auto& f : a_strays) {
				const auto verdict = Judge(f.seen, a_when, placed.Any());
				out.push_back({ f, verdict });
			}
			return out;
		}

		Placed    placed;
		Forgotten forgotten;
	};
}
