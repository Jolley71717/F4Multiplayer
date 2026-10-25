#pragma once

#include <algorithm>
#include <cstdint>
#include <mutex>
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
	// is deleted. So it is a stray whoever made it, and named in the log; whether its record is dropped is Judge's.
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

		template <class F>
		void ForEach(F&& a_each) const
		{
			for (const auto& [id, base] : bases) {
				a_each(id, base);
			}
		}

	private:
		std::unordered_map<std::uint32_t, std::uint32_t> bases;
		bool                                             any = false;
	};

	// A load does not always free a reference of ours that was on its way out as it came (see Book::AfterLoad): its form
	// stays in memory under its id, nobody's from then on, and the next save writes it. The engine's load frees only what
	// has a record of changes (BGSSaveLoadGame::RevertAllChanges walks the records, and lets go of each created form it finds
	// there), and every one of ours on its way out had its record dropped before the load, so SaveGuard::BeforeLoad gives
	// each one its record back (see Handover); this is for one the engine would not take a record for. After the load the
	// game counts ids on from the save's last one, so the id may just as well name a reference the loaded game made since,
	// often of the very base ours had (a raider's frame, disabled while worn). So the form under the id is taken for our
	// old one only when it is provably the same form SaveGuard noted before the load (the same address and extra data,
	// which it held the whole time it was in memory, under the same base, at the very spot to the bit: a form made since
	// at that address is placed where its maker placed it), and only while it stays on its way out and nobody could want
	// it: disabled or deleted, and for a power armor frame, nobody in it or last out of it, with no owner it did not have
	// when noted, and nothing in it; for an actor (a friend's stand-in, a copy of a friend's NPC), not a puppet of ours.
	// Every frame copy of ours is emptied as it is placed; the one a friend's stand-in got into has had the stand-in's base
	// for its owner since then (Equipment::SwitchIntoPowerArmor), so an owner noted before the load is ours; the engine
	// gives a frame to whoever gets out of it. Asked again at every check (each second, and before each save): one that
	// fails once is left to the game for good.
	struct Remains
	{
		bool same = false;        // the very form noted before the load
		bool frame = false;       // a power armor frame
		bool actor = false;       // an actor (a stand-in, or a copy of a friend's NPC)
		bool puppet = false;      // an actor that is one of our puppets (a stand-in in use, or a mirrored NPC)
		bool off = false;         // disabled, or marked for deletion
		bool notedOn = false;     // it was neither when noted: buried while its 3D was still on its way (see LingerOf)
		bool worn = false;        // the player is in it or left it last, or an actor here is in it
		bool newOwner = false;    // it has an owner of its own, and had none when noted (or another ownership since)
		bool holdsItems = false;  // its inventory holds anything
	};

	constexpr bool StillOurs(const Remains& a_remains)
	{
		if (!a_remains.same || !a_remains.off) {
			return false;
		}
		if (a_remains.frame) {
			return !a_remains.worn && !a_remains.newOwner && !a_remains.holdsItems;
		}
		if (a_remains.actor) {
			return !a_remains.puppet;
		}
		return true;  // a light, or anything else Track was told of: nothing of the game's is ever the same form
	}

	// What SaveGuard does with what is under the id right after a load (see Remains).
	enum class Linger
	{
		kNo,       // not ours, or wanted: left to the game
		kYes,      // our old one, on its way out: kept out of saves while it stays so
		kTurnOff,  // our old one, buried before its 3D came in, so never disabled: disabled now, then as kYes
	};

	// One buried while its 3D was still on its way is not disabled (deleting or disabling a reference whose 3D is loading
	// crashes the loader as it finishes: DeferredDelete::Safe), and a load can come in that moment (a friend's parked
	// frame coming into view as the player quickloads, or dies and the game reloads). The loader is done with the old
	// world once the load is over, so it is disabled then, if it is otherwise all StillOurs asks.
	constexpr Linger LingerOf(const Remains& a_remains)
	{
		if (StillOurs(a_remains)) {
			return Linger::kYes;
		}
		Remains once = a_remains;
		once.off = true;
		return a_remains.notedOn && !a_remains.off && StillOurs(once) ? Linger::kTurnOff : Linger::kNo;
	}

	// Which of the references of ours still in the book SaveGuard::BeforeLoad hands to the load: each on its way out (buried,
	// being deleted, or lost from those lists with its form still there), each lingering from the last load, and each held
	// with no record. A held copy with a cell always has its record; one without is adrift (it lost its cell, and its record
	// is dropped each frame until it is buried), and kept through the load it would be in no cell and nobody's to judge.
	constexpr bool GoesToLoad(bool a_onItsWayOut, bool a_lingering, bool a_hasRecord)
	{
		return a_onItsWayOut || a_lingering || !a_hasRecord;
	}

	// What SaveGuard::BeforeLoad does with a reference of ours on its way out, so the load frees it with the old world
	// (see Remains): one with a record is left to the load; one without is given one (TESForm::AddChange), which the
	// engine refuses for a created reference that is deleted and not persistent; that one is let go of now, as the load
	// would have (kRelease), and noted all the same.
	enum class Handover
	{
		kHasRecord,
		kGiveRecord,
		kRelease,
	};

	constexpr Handover HandoverOf(bool a_hasRecord, bool a_recordGiven)
	{
		if (a_hasRecord) {
			return Handover::kHasRecord;
		}
		return a_recordGiven ? Handover::kGiveRecord : Handover::kRelease;
	}

	// References of ours on their way out whose handle stopped resolving while their form was still there (a reference
	// marked for deletion may no longer resolve through its handle while the engine still holds it). RemotePlayers' lists
	// let go of an entry whose handle fails, so these are kept here, by id and address, for RemotePlayers::OnItsWayOut to
	// still say yes at the load; an entry goes once the form under its id is gone or another, or at a load. Past MAX the
	// oldest go.
	class LostOnWayOut
	{
	public:
		static constexpr std::size_t MAX = 64;

		void Add(std::uint32_t a_id, const void* a_ref)
		{
			if (a_id == 0 || !a_ref || Has(a_id, a_ref)) {
				return;
			}
			if (entries.size() >= MAX) {
				entries.erase(entries.begin());
			}
			entries.emplace_back(a_id, a_ref);
		}

		[[nodiscard]] bool Has(std::uint32_t a_id, const void* a_ref) const
		{
			return a_ref && std::ranges::find(entries, std::pair{ a_id, a_ref }) != entries.end();
		}

		// a_now(id): the address of the form under the id now, or null when there is none.
		template <class F>
		void Keep(F&& a_now)
		{
			std::erase_if(entries, [&](const std::pair<std::uint32_t, const void*>& a_entry) { return a_now(a_entry.first) != a_entry.second; });
		}

		void Clear() { entries.clear(); }

		[[nodiscard]] std::size_t Size() const { return entries.size(); }

	private:
		std::vector<std::pair<std::uint32_t, const void*>> entries;
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

	// A stray of ours is kept out of every save, and dropped from a save that brought it back. One of the game's whose
	// base is gone (a stand-in an earlier version saved, on a face copy no save keeps) is kept out of a save: it could
	// only come back broken. A doomed one of the game's that still has its base is only logged, its record left as the
	// game has it, as it is without the mod, whether or not a friend's stand-in was placed this run: the crash is known
	// only for ours, and dropping the record of one of the game's (a quest's, or the player's own) could lose its
	// changes from the player's save for good. Right after a load one of the game's is only logged: it may be in the
	// middle of getting its cell back as the load settles, and is left to the sweep before the next save.
	constexpr Verdict Judge(const Actor& a_actor, When a_when)
	{
		if (!Stray(a_actor)) {
			return Verdict::kLeave;
		}
		if (a_actor.ours) {
			return Verdict::kExclude;
		}
		if (a_when == When::kLoad || (Doomed(a_actor) && a_actor.hasBase)) {
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
		// the form under the id now, or nothing when there is none. Calls a_kept(id) for each one kept, and a_gone(id, base,
		// orphaned) for each one let go of, with the base it was placed from: orphaned when no form is under the id at all
		// (see Book::KeepForgotten), not when the id names another form.
		template <class F, class G, class H>
		void Keep(F&& a_baseNow, G&& a_kept, H&& a_gone)
		{
			std::erase_if(entries, [&](const std::pair<std::uint32_t, std::uint32_t>& a_entry) {
				const std::optional<std::uint32_t> base = a_baseNow(a_entry.first);
				if (!base || *base != a_entry.second) {
					a_gone(a_entry.first, a_entry.second, !base.has_value());
					return true;
				}
				a_kept(a_entry.first);
				return false;
			});
		}

		template <class F, class G>
		void Keep(F&& a_baseNow, G&& a_kept)
		{
			Keep(a_baseNow, a_kept, [](std::uint32_t, std::uint32_t, bool) {});
		}

		void Clear() { entries.clear(); }

		[[nodiscard]] std::size_t Size() const { return entries.size(); }

		template <class F>
		void ForEach(F&& a_each) const
		{
			for (const auto& [id, base] : entries) {
				a_each(id, base);
			}
		}

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

		// Whether the reference under this id, of this base, is one of those we placed this run.
		[[nodiscard]] bool IsPlaced(std::uint32_t a_id, std::uint32_t a_base) const { return placed.Ours(a_id, a_base); }

		// One of ours the player took into their inventory (a copy of an item a friend dropped: Drops): the engine's pickup
		// deletes the reference, and the game hands its id out again, often to the item of that base the player drops next,
		// which is not ours. So it is let go of at once (not forgotten: that keeps the id out of saves while its base is).
		void Release(std::uint32_t a_id) { placed.Erase(a_id); }

		// One of ours whose form is gone already (the engine unloaded it with its cell): let go of at once, not forgotten
		// (Forgotten is for a form that may still be there). Its record is about to be dropped, which frees the id, and the
		// engine hands a freed id out again, often at once: one more second on the books here would take whatever gets it
		// next (a raider's frame of the same base) for ours. Whether it was ours (placed this run); what else holds it as
		// ours lets go of it too (TakeGone).
		bool ForgetGone(std::uint32_t a_id)
		{
			const auto base = placed.BaseOf(a_id);
			if (!base) {
				return false;
			}
			placed.Erase(a_id);
			gone.emplace_back(a_id, *base);
			return true;
		}

		// One of ours whose handle stopped resolving (see Forgotten). Whether it is ours (placed this run).
		bool Forget(std::uint32_t a_id)
		{
			if (const auto base = placed.BaseOf(a_id)) {
				forgotten.Add(a_id, *base);
				return true;
			}
			return false;
		}

		// The forgotten ones still there (a_kept(id) for each, its record to be dropped); one whose form is gone is let
		// go of, and is not ours any more (its id may be handed out again; see TakeGone).
		// a_orphaned(id) for each one with no form under its id at all: its record is to be dropped as it is let go of. The
		// engine unloads a reference it never made persistent with its cell: the form is freed and its record of changes kept
		// under that cell, which brings the reference back when the cell loads again. A friend's frame copy left in Sanctuary
		// as the player walked or travelled off so came back beside the one placed for them on their return, and every save
		// from then on kept it. The id stays taken while that record is there (TESDataHandler::GetNextID hands out an id only
		// when no form is under it, no record is, and it is no unloaded form's), so the record is ours; one whose id names
		// another form now is left alone. A forgotten one still there has its record dropped each second (a_kept): that
		// never frees its id, as the form itself still holds it; the id is free only once the form is gone.
		template <class B, class K, class O>
		void KeepForgotten(B&& a_baseNow, K&& a_kept, O&& a_orphaned)
		{
			forgotten.Keep(a_baseNow, a_kept, [&](std::uint32_t a_id, std::uint32_t a_base, bool a_orphan) {
				if (a_orphan) {
					a_orphaned(a_id);
				}
				placed.Erase(a_id);
				gone.emplace_back(a_id, a_base);
			});
		}

		template <class B, class K>
		void KeepForgotten(B&& a_baseNow, K&& a_kept)
		{
			KeepForgotten(a_baseNow, a_kept, [](std::uint32_t) {});
		}

		// The ids let go of since the last call (KeepForgotten, BeforeSave), their forms gone, with the base each was placed
		// from: what else holds them as ours lets go of them too (see Unpromoted). A load forgets them with the rest.
		[[nodiscard]] std::vector<std::pair<std::uint32_t, std::uint32_t>> TakeGone() { return std::exchange(gone, {}); }

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

		// Before a save: the forgotten ones still there (a_kept), the records of those whose form is gone (a_orphaned, see
		// KeepForgotten), then each stray with what the save does with it.
		template <class B, class K, class O, class S>
		std::vector<Judged> BeforeSave(B&& a_baseNow, K&& a_kept, O&& a_orphaned, std::vector<Found> a_found, S&& a_standIn)
		{
			KeepForgotten(a_baseNow, a_kept, a_orphaned);
			return JudgeAll(Strays(std::move(a_found), a_standIn), When::kSave);
		}

		template <class B, class K, class S>
		std::vector<Judged> BeforeSave(B&& a_baseNow, K&& a_kept, std::vector<Found> a_found, S&& a_standIn)
		{
			return BeforeSave(a_baseNow, a_kept, [](std::uint32_t) {}, std::move(a_found), a_standIn);
		}

		// Every id held as ours now: placed this run, forgotten, or lingering from before a load (for SaveGuard to note
		// before a load which forms are ours; see Remains).
		template <class F>
		void ForEachHeld(F&& a_each) const
		{
			placed.ForEach(a_each);
			forgotten.ForEach(a_each);
			for (const auto& [id, base] : lingering) {
				a_each(id, base);
			}
		}

		// Right after a load, or a new game: what was placed and forgotten before it is let go of first (see
		// Placed::Clear), then each stray the loaded game has, with what is done with it.
		// a_lingers(id, base): whether the form under the id is still the copy placed before the load, on its way out (see
		// Remains). The load does not always free those: one still marked for deletion as the load came (a copy
		// DespawnAll had just disabled or deleted) stayed in memory, nobody's from then on, and the next save wrote it;
		// loaded by a game started afresh, it came back drawn and whole, a frame nobody owned where a friend's copy had
		// stood, beside the copy placed for them then, and shared from then on as the host's own (0.9.1). So each of
		// those lingers: kept out of every save while it is still provably that copy and nobody's (KeepLingering), and let
		// go of for good once it is not. Never forgotten: its id is not ours after the load, and a record under it once its
		// form is gone may be the loaded game's, so none is ever dropped for it.
		template <class S, class L>
		std::vector<Judged> AfterLoad(std::vector<Found> a_found, S&& a_standIn, L&& a_lingers)
		{
			std::vector<std::pair<std::uint32_t, std::uint32_t>> before;
			ForEachHeld([&](std::uint32_t a_id, std::uint32_t a_base) { before.emplace_back(a_id, a_base); });
			placed.Clear();
			forgotten.Clear();
			lingering.clear();
			gone.clear();
			for (const auto& [id, base] : before) {
				if (std::ranges::find(lingering, std::pair{ id, base }) == lingering.end() && a_lingers(id, base)) {
					lingering.emplace_back(id, base);
				}
			}
			return JudgeAll(Strays(std::move(a_found), a_standIn), When::kLoad);
		}

		// The lingering ones (see AfterLoad) still provably our old copies, each asked again: a_still(id, base) says
		// whether it is; a_kept(id) for each that is (its record to be dropped), a_dropped(id) for each that no longer is,
		// let go of for good and left to the game.
		template <class St, class K, class D>
		void KeepLingering(St&& a_still, K&& a_kept, D&& a_dropped)
		{
			std::erase_if(lingering, [&](const std::pair<std::uint32_t, std::uint32_t>& a_entry) {
				if (!a_still(a_entry.first, a_entry.second)) {
					a_dropped(a_entry.first);
					return true;
				}
				a_kept(a_entry.first);
				return false;
			});
		}

		[[nodiscard]] std::size_t LingeringCount() const { return lingering.size(); }

		// Whether the id lingers under that base (see AfterLoad): off and on its way out by definition, so noted again
		// before the next load as well (SaveGuard::BeforeLoad), or a second load in a row would let go of it.
		[[nodiscard]] bool IsLingering(std::uint32_t a_id, std::uint32_t a_base) const
		{
			return std::ranges::find(lingering, std::pair{ a_id, a_base }) != lingering.end();
		}

		template <class S>
		std::vector<Judged> AfterLoad(std::vector<Found> a_found, S&& a_standIn)
		{
			return AfterLoad(std::move(a_found), a_standIn, [](std::uint32_t, std::uint32_t) { return false; });
		}

		// Each stray there is now, with what a save would do with it, and what a load would (changes nothing).
		template <class S>
		std::vector<std::pair<Judged, Verdict>> List(std::vector<Found> a_found, S&& a_standIn) const
		{
			std::vector<std::pair<Judged, Verdict>> out;
			for (auto& f : Strays(std::move(a_found), a_standIn)) {
				const auto save = Judge(f.seen, When::kSave);
				const auto load = Judge(f.seen, When::kLoad);
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
				const auto verdict = Judge(f.seen, a_when);
				out.push_back({ f, verdict });
			}
			return out;
		}

		Placed                                               placed;
		Forgotten                                            forgotten;
		std::vector<std::pair<std::uint32_t, std::uint32_t>> lingering;  // id, base: copies from before the last load (see AfterLoad)
		std::vector<std::pair<std::uint32_t, std::uint32_t>> gone;       // id, base
	};

	// The stand-ins the engine is not let keep persistent. Something of the game's (a quest alias filled with one, a
	// scene, an AI package aimed at it) asks the engine to promote a reference it holds (BGSDynamicPersistenceManager::
	// PromoteReference), which keeps it in memory past its cell. A stand-in held so when the player leaves (a door, a
	// fast travel, walking off: the crash leaving Red Rocket) is left in no cell, and when the holder lets go of it (a
	// quest stopping with the location left, an actor's AI moving on) the engine demotes it: an actor's demotion waits
	// for the end of the frame (DemoteQueuedActors), and one in no cell is then unloaded, its record of changes written
	// out under its cell or else its worldspace, read without a check. It has neither, and the game crashes
	// (BGSSaveLoadInitialData::SaveInitialData). That can come in the same engine update that cleared its cell, before
	// any frame of ours could drop its record (SaveGuard::ExcludeOurs). The engine asks the reference first whether it
	// may be promoted (TESObjectREFR::GetAllowPromoteToPersistent), for a promotion and for a demotion alike; SaveGuard
	// answers no for these, so a stand-in is neither promoted nor demoted, its holder keeps only a handle, and it is
	// unloaded with its cell like any reference the game placed and never promoted.
	//
	// The engine asks from any thread, so this has its own lock and calls nothing under it. An entry is the actor's
	// address, form ID and base: the game hands addresses and ids out again, often the ones just let go of, and an actor
	// of the game's placed under a stand-in's old address and id has another base, so it is still told yes. An entry
	// goes when its form is gone (Book::TakeGone) or at a load; past MAX the oldest go.
	class Unpromoted
	{
	public:
		static constexpr std::size_t MAX = 256;

		void Add(const void* a_ref, std::uint32_t a_id, std::uint32_t a_base)
		{
			if (!a_ref || a_id == 0) {
				return;
			}
			const std::scoped_lock l{ lock };
			std::erase_if(entries, [&](const Entry& a_entry) { return a_entry.ref == a_ref || a_entry.id == a_id; });
			if (entries.size() >= MAX) {
				entries.erase(entries.begin());
			}
			entries.push_back({ a_ref, a_id, a_base });
		}

		// The one placed under the id from the base, its form gone (an entry a newer stand-in made under the id stays).
		void Remove(std::uint32_t a_id, std::uint32_t a_base)
		{
			const std::scoped_lock l{ lock };
			std::erase_if(entries, [&](const Entry& a_entry) { return a_entry.id == a_id && a_entry.base == a_base; });
		}

		void Clear()
		{
			const std::scoped_lock l{ lock };
			entries.clear();
		}

		// The engine's question about the reference at a_ref, with its id and base now: whether to say no. Each no is
		// counted (Refused).
		[[nodiscard]] bool Refuses(const void* a_ref, std::uint32_t a_id, std::uint32_t a_base)
		{
			const std::scoped_lock l{ lock };
			const bool refuses = std::ranges::any_of(entries, [&](const Entry& a_entry) {
				return a_entry.ref == a_ref && a_entry.id == a_id && a_entry.base == a_base;
			});
			refused += refuses;
			return refuses;
		}

		[[nodiscard]] std::size_t Size() const
		{
			const std::scoped_lock l{ lock };
			return entries.size();
		}

		[[nodiscard]] std::uint32_t Refused() const
		{
			const std::scoped_lock l{ lock };
			return refused;
		}

	private:
		struct Entry
		{
			const void*   ref = nullptr;
			std::uint32_t id = 0;
			std::uint32_t base = 0;
		};

		mutable std::mutex lock;
		std::vector<Entry> entries;
		std::uint32_t      refused = 0;
	};
}
