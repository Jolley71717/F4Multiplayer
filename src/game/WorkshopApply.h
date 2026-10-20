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
//   bool Loaded(const Protocol::WorkshopItem&)           its settlement is loaded here
//   WorkshopCells::Where WhereOf(item), WhereScrapped(item)   where it waits until it is
//   bool InReach(item)                                   it stands within its workbench's reach
//   std::optional<Standing> StandingOf(const Copy&)      none if the copy is gone
//   void MoveTo(const Copy&, item)                       moves it within its cell
//   void Keep(const Copy&) / void Discard(const Copy&)   ours from now on / deleted, in time
//   Placeable CanPlace(item)
//   std::optional<Copy> FindExisting(item)               a copy an earlier session left at the spot
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
	};

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

	// Places (or moves) the copy; false if its settlement isn't loaded yet.
	template <class World, class Key>
	bool Place(World& a_world, std::map<Key, typename World::Copy>& a_copies, Counts& a_counts, const Key& a_key, const Protocol::WorkshopItem& a_item)
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
				a_world.Keep(*existing);
				++a_counts.adopted;
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
			a_world.Keep(*made);
			++a_counts.placed;
		}
		return true;
	}

	// Another player's report, its turn come: placed, moved, or scrapped here, or filed until its cell
	// loads. Whatever waited under its key is older (a report waits only after its turn came, and one
	// arriving later is behind it in line): left waiting, it would undo this one when its cell attaches
	// (a scrapped piece standing again, a moved one going back).
	template <class World, class Key>
	void Apply(World& a_world, std::map<Key, typename World::Copy>& a_copies, WorkshopInbox::Inbox<Key, Protocol::WorkshopItem>& a_inbox, Counts& a_counts, const Key& a_key,
		const Protocol::WorkshopItem& a_item)
	{
		a_inbox.Forget(a_key);
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
			return;
		}
		if (!Place(a_world, a_copies, a_counts, a_key, a_item)) {
			a_inbox.Wait(a_world.WhereOf(a_item), a_key, a_item);
		}
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
	void RemoveAll(World& a_world, std::map<Key, typename World::Copy>& a_copies, WorkshopInbox::Inbox<Key, Protocol::WorkshopItem>& a_inbox)
	{
		for (const auto& [key, copy] : a_copies) {
			a_world.Discard(copy);
		}
		a_copies.clear();
		a_inbox.Clear();
	}
}
