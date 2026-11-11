#pragma once

#include "Protocol.h"
#include "game/WorkshopCells.h"
#include "game/WorkshopInbox.h"
#include "game/WorkshopMove.h"

#include <cstdint>
#include <map>
#include <optional>
#include <utility>
#include <vector>

// What another player's report does to our world when its turn comes (WorkshopSync::Frame): a scrap
// takes the copy away, and a piece of the world itself with it; anything else moves the copy, takes over
// one the save left at the spot, or makes one; what can't be done yet waits for its cell. The engine
// is behind World (WorkshopSync.cpp's, or a test's stand-in), so these decisions are the ones the tests
// drive. A World gives:
//
//   using Copy = ...;                                    a copy as we keep it (a handle)
//   bool Loaded(const Protocol::WorkshopItem&)           the cell it stands in is loaded here (what stands at its spot is seen)
//   WorkshopCells::Where WhereOf(item), WhereScrapped(item)   where it waits until it is
//   bool InReach(item)                                   it stands within its workbench's reach
//   std::optional<Standing> StandingOf(const Copy&)      none if the copy is gone
//   void MoveTo(const Copy&, item)                       moves it within its cell
//   void Keep(const Copy&, item)                         ours from now on, as the copy of that report (its workbench)
//   void Discard(const Copy&)                            deleted, in time
//   Placeable CanPlace(item)
//   std::optional<Copy> FindExisting(item)               a copy an earlier session left at the spot (never a piece of ours)
//   bool MadeByUs(const Copy&)                           a copy of this kind made it (it carries the keyword that keeps
//                                                        workshop mode off our copies, saved with it) and holds nothing, not
//                                                        the player's own
//   bool OursAt(item)                                    a piece of ours, the same object, stands at the spot
//   bool CopyAt(item)                                    one of our copies, the same object, stands near the spot (TwinOf's quick look)
//   std::optional<Copy> LeftoverAt(item)                 a copy of it no session knows, at the spot, beside the one kept (or a piece of ours)
//   std::optional<Copy> Create(item)
//   void Empty(const Copy&)                              takes out what a container's base came with (Placeable::kStocked)
//   bool ScrapWorldRef(item)                             false until the reference is loaded here
//   bool OwnLoaded(item)                                 a piece of the world itself (item.refId): ours is in memory
//   std::optional<Copy> OwnPiece(item)                   ours, if a friend may move it (none: refused)
namespace WorkshopApply
{
	struct Counts
	{
		std::uint32_t placed = 0;
		std::uint32_t moved = 0;
		std::uint32_t scrapped = 0;
		std::uint32_t adopted = 0;
		std::uint32_t refused = 0;       // outside their workbench's reach
		std::uint32_t emptied = 0;       // copies of containers that come stocked, emptied as the workshop builds them
		std::uint32_t recreated = 0;     // copies made again where their piece was moved (to another cell)
		std::uint32_t baseMismatch = 0;  // copies replaced by another object under the same ID (their builder loaded a save)
		std::uint32_t movedOwn = 0;      // pieces of the world itself another player moved: ours moved
		std::uint32_t ownRefused = 0;    // ... but not one a friend may move (see OwnPiece)
		std::uint32_t oursThere = 0;     // no copy made: a piece of ours, the same object, stands at the spot
		std::uint32_t leftovers = 0;     // copies a save held twice at one spot, or over a piece of ours: gone
		std::uint32_t framesCleared = 0;  // copies of a power armor frame an older version made, found at the spot and removed
		std::uint32_t twinned = 0;       // no copy made: a copy of another friend's, the same object, stands just so at the spot
		std::uint32_t untwinned = 0;     // ... and placed after all: that copy went, or moved
	};

	inline constexpr int MAX_LEFTOVERS = 8;  // removed at one spot in one go: a guard, should one not go at once

	// A copy is a reference made at runtime in its cell, not a persistent one: when the cell unloads, the game writes it to
	// the cell's record of changes and frees it, and makes it again from that record when the cell loads again, under the same
	// form ID but as a new reference. The handle kept for it then names nothing, for good (the rig, after a trip to another
	// settlement and back: every copy's handle 00000000, the references standing under their IDs). So a copy is kept with its
	// form ID and base too, and the reference under that ID is taken for it again when: an ID made at runtime (FF), the same
	// object, and not deleted. A created ID is given out again only after its reference is deleted and let go, and a copy
	// is deleted only by us, as it leaves our list.
	constexpr bool SameCopy(std::uint32_t a_id, std::uint32_t a_baseNow, std::uint32_t a_base, bool a_deleted)
	{
		return (a_id >> 24) == 0xFF && a_base != 0 && a_baseNow == a_base && !a_deleted;
	}

	// A reference of ours that goes (a copy, a wire we strung, a piece of ours the host took over or never built) is deleted
	// with its record of changes dropped. The engine's own delete (TESObjectREFR::SetDelete) drops the record of a created
	// reference only while it has not been written out to the cell's buffer; one written out already (BGSSaveLoadGame::UnloadForm,
	// run by a persistence demotion or an earlier unload of the cell) is left as it is, the cell's unload does not write it
	// again, and the cell's next load makes the piece again from it: alive, unblocked, unlinked, under the same id (a friend's
	// generator copy on the rig, deleted, back as a plain generator after a trip away). BGSSaveLoadGame::ClearChanges frees a
	// record buffered or not. Only for a reference made at runtime: a piece of the world switched off (a friend's scrap of a
	// plugin reference) is a change the save must keep. And only while it is not persistent: the engine's delete of a
	// persistent created reference skips the drop and leaves it a record with the deleted flag, which the load frees and the
	// save writes as deleted, its id taken while the record lives (SaveStrays.h); with that record dropped it would have none,
	// a load would leave it in memory, and the engine takes a change on a persistent reference, so the next change would make
	// it a fresh record without the deleted flag, written by the next save (the 0.9.1 "came back drawn and whole"). A
	// persistent one keeps the engine's own record. (Its record is not the buffered kind: a demotion, which buffers it, makes
	// it non-persistent first.)
	constexpr bool DropsRecord(std::uint32_t a_id, bool a_persistent)
	{
		return (a_id >> 24) == 0xFF && !a_persistent;
	}

	// Whether the base of a reference that goes with its record dropped (DropsRecord) loses its record too: an actor's base the
	// engine made at runtime (a templated turret's, ExtraLeveledCreature) is that one actor's alone, and has a record of its
	// own. Dropped with the actor's, as SaveGuard::HandToLoad does for a stand-in's face copy: an actor with no record stays in
	// memory through a load, a created base with a record is freed by it, and the actor, freed some frames later
	// (DeferredDeleter), reads its base as it goes; the load after that crashed (Actor::LoadGame, the process lists,
	// RevertAllChanges). With neither record the load leaves both in memory, nobody's, and no save writes either.
	constexpr bool DropsBaseRecord(bool a_actor, std::uint32_t a_base)
	{
		return a_actor && (a_base >> 24) == 0xFF;
	}

	// What dropping the record did, for the count and the log.
	enum class RecordDrop
	{
		kNotOurs,     // a plugin reference: its record is the game's
		kPersistent,  // persistent: the engine's own deleted-flag record is kept (see DropsRecord)
		kNone,        // it had no record to drop (deleted before any change, or dropped already)
		kDropped,
		kKept,        // the engine still holds a record after the drop: the piece may come back with its cell (said in the log)
	};

	constexpr RecordDrop DropOf(std::uint32_t a_id, bool a_persistent, bool a_hadRecord, bool a_hasRecordNow)
	{
		if ((a_id >> 24) != 0xFF) {
			return RecordDrop::kNotOurs;
		}
		if (a_persistent) {
			return RecordDrop::kPersistent;
		}
		if (a_hasRecordNow) {
			return RecordDrop::kKept;
		}
		return a_hadRecord ? RecordDrop::kDropped : RecordDrop::kNone;
	}

	// The base a piece is reported and kept under. An actor placed from a templated NPC (a Machinegun Turret) gets a base
	// the engine makes at runtime (ExtraLeveledCreature names the original); that base exists in this game alone, so the
	// report names the original, and the copy, whose own game makes it a runtime base of its own, is known by the original
	// too. A piece whose base is the game's own is reported as it is. Zero: nothing to report (a runtime base with no original).
	constexpr std::uint32_t ReportedBase(std::uint32_t a_base, std::uint32_t a_original)
	{
		if ((a_base >> 24) != 0xFF) {
			return a_base;
		}
		return (a_original >> 24) == 0xFF ? 0 : a_original;
	}

	// A save may hold copies that doubled what stood at the spot: an older version placed a piece once its workbench was
	// in our worldspace, its own cell loaded or not, and so over what stood there unseen (the save's copy,
	// after a load away from the settlement, or a piece of ours). Two at one spot flicker. All but the one kept go.
	template <class World>
	void DiscardLeftovers(World& a_world, const Protocol::WorkshopItem& a_item, Counts& a_counts)
	{
		for (int i = 0; i < MAX_LEFTOVERS; ++i) {
			const auto leftover = a_world.LeftoverAt(a_item);
			if (!leftover) {
				return;
			}
			a_world.Discard(*leftover);
			++a_counts.leftovers;
		}
	}

	// A report that made no copy because another friend's copy stands for it (TwinOf), kept with the key of that copy: if it
	// goes or moves, the piece this report names still stands in its builder's game, and is placed here then (Apply).
	template <class Key>
	struct Twin
	{
		Key                    of;
		Protocol::WorkshopItem item;
	};

	template <class Key>
	using Twins = std::map<Key, Twin<Key>>;

	// Where a copy stands here, for following its piece.
	struct Standing
	{
		std::uint32_t                                         base = 0;
		WorkshopMove::Transform                               at;
		std::optional<std::pair<std::int32_t, std::int32_t>> cell;  // its exterior cell; none in an interior
	};

	enum class Placeable
	{
		kYes,
		kNo,       // nothing we can show (a mod we don't have), or nothing we would (not something workshop mode builds)
		kStocked,  // a container whose base holds things of its own: its copy is emptied (World::Empty), or it would be loot
		kFrame,    // a power armor frame (see Shared): no copy, and one an older version made at the spot goes
	};

	// What a copy is, as far as what it holds goes.
	enum class Kind
	{
		kContainer,
		kActor,  // a turret, a robot
		kOther,
	};

	// Whether what a copy holds comes to the player before it goes: only a container's, what they kept
	// in it. A turret's or a robot's inventory is the game's own, filled from its record when it loads
	// (its weapon, its ammo, its parts): handed over, it would be free gear on every scrap, every move to
	// another cell and every session's end.
	constexpr bool Rescues(Kind a_kind, bool a_holdsItems)
	{
		return a_kind == Kind::kContainer && a_holdsItems;
	}

	// Whether a piece of this object is the workshop's to share: one workshop mode builds, and never a power armor
	// frame. Workshop mode moves a parked frame like a piece of its own, but a frame is its player's, shared worn,
	// parked or left by RemotePlayers. Up to 0.9.1 a frame moved there (the player's own, or our copy of a friend's)
	// was reported as a piece too: each friend's game made a workshop copy of it where it was set down, beside the
	// copy of the frame itself, and the server kept that copy for every later session and load (the 0.9.1 report:
	// frames piling up in Sanctuary). Neither reported nor placed now, whoever sends it (Placeable::kFrame).
	constexpr bool Shared(bool a_buildable, bool a_frame)
	{
		return a_buildable && !a_frame;
	}

	// Moves the copy to where the report puts it (see WorkshopMove); false if it has to be made again
	// there instead (in another cell, or as another object).
	template <class World>
	bool Follow(World& a_world, const typename World::Copy& a_copy, const Standing& a_now, const Protocol::WorkshopItem& a_item, Counts& a_counts)
	{
		switch (WorkshopMove::HowToFollow(a_now.base, a_now.at, a_now.cell, a_item)) {
		case WorkshopMove::How::kStay:
			return true;
		case WorkshopMove::How::kRecreate:
			return false;
		case WorkshopMove::How::kInPlace:
			break;
		}
		a_world.MoveTo(a_copy, a_item);
		++a_counts.moved;
		return true;
	}

	// The key of another player's copy standing just where the report puts the same object, turned the same way: two friends
	// whose saves share a past each report the piece built before they parted. One of the reporter's own is another piece of
	// theirs (two of one object at one spot, crossed, or turned about a shared end), and so is one turned otherwise.
	template <class World, class Key>
	std::optional<Key> TwinOf(World& a_world, const std::map<Key, typename World::Copy>& a_copies, const Key& a_key, const Protocol::WorkshopItem& a_item)
	{
		if (!a_world.CopyAt(a_item)) {
			return std::nullopt;  // the usual case, without going over every copy
		}
		const auto to = WorkshopMove::Of(a_item);
		for (const auto& [key, copy] : a_copies) {
			if (key.first == a_key.first) {
				continue;
			}
			const auto now = a_world.StandingOf(copy);
			if (now && now->base == a_item.base && WorkshopMove::Same(now->at, to)) {
				return key;
			}
		}
		return std::nullopt;
	}

	// Places (or moves) the copy; false if its settlement isn't loaded yet.
	template <class World, class Key>
	bool Place(World& a_world, std::map<Key, typename World::Copy>& a_copies, Twins<Key>& a_twins, Counts& a_counts, const Key& a_key, const Protocol::WorkshopItem& a_item)
	{
		if (!a_world.Loaded(a_item)) {
			return false;
		}
		if (!a_world.InReach(a_item)) {
			++a_counts.refused;
			return true;  // dropped, and a copy already standing is left where it was
		}
		if (Protocol::IsShareableRef(a_item.refId)) {
			// A piece of the world itself (a bed or a crop the settlement came with), moved: every game has it,
			// so ours moves. A copy at the new spot would leave ours standing at the old one.
			if (!a_world.OwnLoaded(a_item)) {
				return false;  // waits for its cell
			}
			const auto own = a_world.OwnPiece(a_item);
			const auto now = own ? a_world.StandingOf(*own) : std::nullopt;
			if (!now || now->base != a_item.base) {
				++a_counts.ownRefused;
				return true;
			}
			switch (WorkshopMove::HowToMove(now->at, WorkshopMove::Of(a_item), now->cell)) {
			case WorkshopMove::How::kStay:
				break;
			case WorkshopMove::How::kInPlace:
				a_world.MoveTo(*own, a_item);
				++a_counts.movedOwn;
				break;
			case WorkshopMove::How::kRecreate:
				++a_counts.ownRefused;  // the engine moves a reference only within its cell, and ours can't be made again
				break;
			}
			return true;
		}
		if (const auto it = a_copies.find(a_key); it != a_copies.end()) {
			const auto now = a_world.StandingOf(it->second);
			if (now && Follow(a_world, it->second, *now, a_item, a_counts)) {
				return true;
			}
			if (now) {
				// It went to another cell, or its builder's game made the ID again for something else:
				// made again (or the new thing made), below.
				++(now->base == a_item.base ? a_counts.recreated : a_counts.baseMismatch);
			}
			a_world.Discard(it->second);
			a_copies.erase(it);  // (or the game dropped it)
		}
		const auto placeable = a_world.CanPlace(a_item);
		if (placeable == Placeable::kNo) {
			return true;
		}
		if (placeable == Placeable::kFrame) {
			// What 0.9.x's copy of the frame left in this save (the server still sends its report) goes; a frame of the
			// same base at the spot that no copy of ours made (the player's own suit) is left alone.
			if (const auto left = a_world.FindExisting(a_item); left && a_world.MadeByUs(*left)) {
				a_world.Discard(*left);
				++a_counts.framesCleared;
			}
			return true;
		}
		if (a_world.OursAt(a_item)) {
			// Our save and theirs share a past (one save, played on by both): the piece they built before it parted is ours
			// too, linked to our workbench, with its settler's job. A copy over it would double it, and taking it over would
			// make it a copy: out of workshop mode's reach, off our workbench, and deleted when the session ends.
			++a_counts.oursThere;
			DiscardLeftovers(a_world, a_item, a_counts);
			return true;
		}
		if (const auto twin = TwinOf(a_world, a_copies, a_key, a_item)) {
			// Two friends whose saves share a past each report the piece built before it parted: the first made the copy, and
			// the second's would double it. Kept, for when the first's copy goes or moves (Untwin).
			a_twins.insert_or_assign(a_key, Twin<Key>{ *twin, a_item });
			++a_counts.twinned;
			return true;
		}
		if (placeable == Placeable::kStocked) {
			// A copy a save kept (an older version made them full) may hold what its base came with: it goes
			// (what the player kept in it comes to them first), and an empty one is made below.
			if (const auto existing = a_world.FindExisting(a_item)) {
				a_world.Discard(*existing);
			}
		} else if (const auto existing = a_world.FindExisting(a_item)) {
			const auto now = a_world.StandingOf(*existing);
			if (now && Follow(a_world, *existing, *now, a_item, a_counts)) {
				a_copies.emplace(a_key, *existing);
				a_world.Keep(*existing, a_item);
				++a_counts.adopted;
				DiscardLeftovers(a_world, a_item, a_counts);
				return true;
			}
			a_world.Discard(*existing);  // at the spot, but a cell apart (on its edge): made again, below
			++a_counts.recreated;
		}
		if (const auto made = a_world.Create(a_item)) {
			if (placeable == Placeable::kStocked) {
				a_world.Empty(*made);  // as the workshop builds it: empty
				++a_counts.emptied;
			}
			a_copies.emplace(a_key, *made);
			a_world.Keep(*made, a_item);
			++a_counts.placed;
			DiscardLeftovers(a_world, a_item, a_counts);
		}
		return true;
	}

	// The reports twinned with a_key's copy (TwinOf) whose piece it no longer stands for, now that it went or moved: placed
	// for themselves (or one twinned with the first of them placed), or filed until their cell loads.
	template <class World, class Key>
	void Untwin(World& a_world, std::map<Key, typename World::Copy>& a_copies, Twins<Key>& a_twins, WorkshopInbox::Inbox<Key, Protocol::WorkshopItem>& a_inbox, Counts& a_counts,
		const Key& a_key)
	{
		if (a_twins.empty()) {
			return;
		}
		const auto it = a_copies.find(a_key);
		const auto now = it != a_copies.end() ? a_world.StandingOf(it->second) : std::nullopt;
		std::vector<std::pair<Key, Protocol::WorkshopItem>> loose;
		std::erase_if(a_twins, [&](const auto& a_twin) {
			if (a_twin.second.of != a_key || (now && now->base == a_twin.second.item.base && WorkshopMove::Same(now->at, WorkshopMove::Of(a_twin.second.item)))) {
				return false;
			}
			loose.emplace_back(a_twin.first, a_twin.second.item);
			return true;
		});
		for (const auto& [key, item] : loose) {
			++a_counts.untwinned;
			if (!Place(a_world, a_copies, a_twins, a_counts, key, item)) {
				a_inbox.Wait(a_world.WhereOf(item), key, item);
			}
		}
	}

	// Another player's report, its turn come: placed, moved, or scrapped here, or filed until its cell
	// loads. Whatever waited under its key is older (a report waits only after its turn came, and one
	// arriving later is behind it in line): left waiting, it would undo this one when its cell attaches
	// (a scrapped piece standing again, a moved one going back).
	template <class World, class Key>
	void Apply(World& a_world, std::map<Key, typename World::Copy>& a_copies, Twins<Key>& a_twins, WorkshopInbox::Inbox<Key, Protocol::WorkshopItem>& a_inbox, Counts& a_counts,
		const Key& a_key, const Protocol::WorkshopItem& a_item)
	{
		a_inbox.Forget(a_key);
		a_twins.erase(a_key);  // this report is newer than the one twinned
		if (a_item.op == Protocol::WorkshopOp::kScrapped) {
			if (const auto it = a_copies.find(a_key); it != a_copies.end()) {
				a_world.Discard(it->second);
				a_copies.erase(it);
				++a_counts.scrapped;
			}
			// A piece of the world itself goes from our world too (one its player had moved has a copy
			// here as well, gone above).
			if (Protocol::IsShareableRef(a_item.refId) && !a_world.ScrapWorldRef(a_item)) {
				a_inbox.Wait(a_world.WhereScrapped(a_item), a_key, a_item, a_item.refId);
			}
			Untwin(a_world, a_copies, a_twins, a_inbox, a_counts, a_key);
			return;
		}
		if (!Place(a_world, a_copies, a_twins, a_counts, a_key, a_item)) {
			a_inbox.Wait(a_world.WhereOf(a_item), a_key, a_item);
		}
		Untwin(a_world, a_copies, a_twins, a_inbox, a_counts, a_key);
	}

	// What waits nowhere (its workbench wasn't in this game) and has somewhere to wait now: a cell that
	// attached brought its workbench. One whose workbench this game doesn't have at all (a friend's mod
	// settlement) stays put, instead of going through the line again with every cell that attaches.
	template <class World, class Key>
	std::vector<Key> FoundSomewhere(World& a_world, const WorkshopInbox::Inbox<Key, Protocol::WorkshopItem>& a_inbox)
	{
		std::vector<Key> keys;
		for (const auto& key : a_inbox.Pending().Nowhere()) {
			const auto item = a_inbox.Pending().Find(key);
			if (item && (item->op == Protocol::WorkshopOp::kScrapped ? a_world.WhereScrapped(*item) : a_world.WhereOf(*item)) != WorkshopCells::Where{}) {
				keys.push_back(key);
			}
		}
		return keys;
	}

	// Leaving the session: every copy goes (what the player kept in one comes to them first), and so
	// does what waited to be placed.
	template <class World, class Key>
	void RemoveAll(World& a_world, std::map<Key, typename World::Copy>& a_copies, Twins<Key>& a_twins, WorkshopInbox::Inbox<Key, Protocol::WorkshopItem>& a_inbox)
	{
		for (const auto& [key, copy] : a_copies) {
			a_world.Discard(copy);
		}
		a_copies.clear();
		a_twins.clear();
		a_inbox.Clear();
	}
}
