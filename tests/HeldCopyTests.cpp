// A friend's frame copy the engine unloads with its cell (HeldCopy, SaveStrays::Book::KeepForgotten), and the copy an
// earlier session left where one is about to be placed (HeldCopy::Leftover): what made the 0.9.1 report's frames pile up
// in Sanctuary, one more each time the player came back, and what keeps one copy per friend's frame now.

#include "Test.h"
#include "game/HeldCopy.h"
#include "game/SaveStrays.h"

#include <map>
#include <optional>
#include <set>
#include <vector>

namespace
{
	constexpr std::uint32_t FRAME = 0x0017F6E4;  // a power armor frame every game has
	constexpr std::uint32_t OTHER = 0x0020A578;  // anything else
}

TEST("held copy: nothing held, or a handle that still resolves, is left alone")
{
	using HeldCopy::Look;
	using HeldCopy::Lost;
	CHECK(Look({}, false, {}) == Lost::kNone);
	CHECK(Look({ .id = 0xFF000F00, .base = FRAME }, true, {}) == Lost::kNone);
}

TEST("held copy: the same copy back under its id with its cell is held again; anything else is forgotten")
{
	using HeldCopy::Look;
	using HeldCopy::Lost;
	const HeldCopy::Ids held{ .id = 0xFF000F00, .base = FRAME };
	CHECK(Look(held, false, { .baseNow = FRAME, .deleted = false, .ours = true }) == Lost::kBack);
	CHECK(Look(held, false, { .baseNow = std::nullopt }) == Lost::kForget);                           // unloaded: no form under the id
	CHECK(Look(held, false, { .baseNow = OTHER, .ours = false }) == Lost::kForget);                  // the id names something else now
	CHECK(Look(held, false, { .baseNow = 0 }) == Lost::kForget);                                      // a form that is no reference
	CHECK(Look(held, false, { .baseNow = FRAME, .deleted = true, .ours = true }) == Lost::kForget);   // on its way out
	CHECK(Look(held, false, { .baseNow = FRAME, .deleted = false, .ours = false }) == Lost::kForget);  // a frame not ours under the id
}

TEST("held copy: a leftover is a runtime frame of the same base at the very spot, enabled, emptied, nobody's, and not one this run placed")
{
	using HeldCopy::Leftover;
	const HeldCopy::Standing leftover{ .created = true, .sameBase = true, .enabled = true, .emptied = true, .distance = 0.1f };
	CHECK(Leftover(leftover));
	auto with = [&](auto a_change) {
		auto standing = leftover;
		a_change(standing);
		return Leftover(standing);
	};
	CHECK(!with([](auto& s) { s.created = false; }));   // a pre-placed suit (the museum's): the game's own
	CHECK(!with([](auto& s) { s.sameBase = false; }));  // another frame
	CHECK(!with([](auto& s) { s.enabled = false; }));   // disabled or deleted: on its way out
	CHECK(!with([](auto& s) { s.emptied = false; }));   // pieces in it, or its inventory never made: a suit somebody has
	CHECK(!with([](auto& s) { s.quest = true; }));      // a quest of ours holds it
	CHECK(!with([](auto& s) { s.used = true; }));       // our player's own, or somebody here is in it
	CHECK(!with([](auto& s) { s.placed = true; }));     // a copy this run placed: another friend's, or the graveyard's
	CHECK(!with([](auto& s) { s.distance = HeldCopy::LEFTOVER_RADIUS + 0.5f; }));
	CHECK(with([](auto& s) { s.distance = HeldCopy::LEFTOVER_RADIUS; }));
}

TEST("save strays: a forgotten one is orphaned only when no form is under its id, not when the id names another")
{
	SaveStrays::Forgotten forgotten;
	forgotten.Add(0xFF000010, FRAME);
	forgotten.Add(0xFF000011, FRAME);
	forgotten.Add(0xFF000012, FRAME);
	std::map<std::uint32_t, bool> gone;
	std::vector<std::uint32_t>    kept;
	forgotten.Keep(
		[](std::uint32_t a_id) -> std::optional<std::uint32_t> {
			if (a_id == 0xFF000010) {
				return std::nullopt;
			}
			return a_id == 0xFF000011 ? OTHER : FRAME;
		},
		[&](std::uint32_t a_id) { kept.push_back(a_id); }, [&](std::uint32_t a_id, std::uint32_t a_base, bool a_orphan) {
			CHECK(a_base == FRAME);
			gone[a_id] = a_orphan;
		});
	CHECK(kept == std::vector<std::uint32_t>{ 0xFF000012 });
	CHECK(gone.size() == 2);
	CHECK(gone[0xFF000010]);
	CHECK(!gone[0xFF000011]);
}

TEST("save strays: only one of ours is forgotten, and its record is dropped once its form is gone")
{
	SaveStrays::Book book;
	book.Track(0xFF000020, FRAME);
	CHECK(book.Forget(0xFF000020));
	CHECK(!book.Forget(0xFF000021));  // never ours
	std::vector<std::uint32_t> orphaned;
	std::vector<std::uint32_t> kept;
	book.KeepForgotten([](std::uint32_t) -> std::optional<std::uint32_t> { return std::nullopt; }, [&](std::uint32_t a_id) { kept.push_back(a_id); },
		[&](std::uint32_t a_id) { orphaned.push_back(a_id); });
	CHECK(kept.empty());
	CHECK(orphaned == std::vector<std::uint32_t>{ 0xFF000020 });
	CHECK(!book.IsPlaced(0xFF000020, FRAME));
	CHECK(book.TakeGone().size() == 1);
	// The same through a save.
	book.Track(0xFF000022, FRAME);
	book.Forget(0xFF000022);
	orphaned.clear();
	book.BeforeSave([](std::uint32_t) -> std::optional<std::uint32_t> { return std::nullopt; }, [](std::uint32_t) {}, [&](std::uint32_t a_id) { orphaned.push_back(a_id); }, {},
		[](std::uint32_t, std::uint32_t) { return false; });
	CHECK(orphaned == std::vector<std::uint32_t>{ 0xFF000022 });
}

namespace
{
	// Sanctuary as the engine keeps it, for a friend's parked frame copy (RemotePlayers::UpdateParked): a reference
	// placed at runtime and never made persistent goes with its cell as the player walks off (the form freed, its record of
	// changes kept under the cell), and the record brings it back under the same id when the cell loads again. A save
	// writes every record there is; a load brings back every reference with one. SaveGuard's part is the real
	// SaveStrays::Book, called as SaveGuard calls it.
	struct Sanctuary
	{
		struct Ref
		{
			std::uint32_t base = FRAME;
			bool          emptied = true;
		};
		std::map<std::uint32_t, Ref> loaded;    // the forms there are now
		std::map<std::uint32_t, Ref> buffered;  // records kept under the cell while it is unloaded
		std::set<std::uint32_t>      records;   // ids with a record of changes
		std::uint32_t                nextId = 0xFF000F00;
		bool                         cellLoaded = true;
		SaveStrays::Book             book;
		bool                         fixed = true;  // RemotePlayers and SaveGuard as they are now, or as 0.9.1 had them

		// The friend's parked copy RemotePlayers holds.
		HeldCopy::Ids held;

		std::uint32_t Place()
		{
			const auto id = nextId++;
			loaded[id] = {};
			records.insert(id);  // placed and moved: the engine notes its changes
			book.Track(id, FRAME);
			return id;
		}

		void UnloadCell()
		{
			cellLoaded = false;
			for (const auto& [id, ref] : loaded) {
				if (records.contains(id)) {
					buffered[id] = ref;
				}
			}
			loaded.clear();
		}

		void LoadCell()
		{
			cellLoaded = true;
			for (const auto& [id, ref] : buffered) {
				if (records.contains(id)) {
					loaded[id] = ref;
				}
			}
			buffered.clear();
		}

		std::optional<std::uint32_t> BaseNow(std::uint32_t a_id) const
		{
			const auto it = loaded.find(a_id);
			return it != loaded.end() ? std::optional{ it->second.base } : std::nullopt;
		}

		// SaveGuard::Forget: and, now, the record of one whose form is gone dropped at once (DropOrphan).
		void Forget(std::uint32_t a_id)
		{
			if (book.Forget(a_id) && fixed && !loaded.contains(a_id)) {
				records.erase(a_id);
			}
		}

		// A frame of RemotePlayers::Update with the friend parked here (UpdateParked, after Refresh).
		void Frame()
		{
			const bool resolves = held.id != 0 && loaded.contains(held.id);
			if (fixed) {
				const auto base = BaseNow(held.id);
				const HeldCopy::Facts facts{ .baseNow = base, .ours = base && book.IsPlaced(held.id, *base) };
				if (HeldCopy::Look(held, resolves, facts) == HeldCopy::Lost::kForget) {
					Forget(held.id);
					held = {};
				}
			} else if (held.id != 0 && !resolves) {
				Forget(held.id);  // 0.9.1: let go of; its record left where the engine keeps it
				held = {};
			}
			// SaveGuard::Frame
			book.KeepForgotten([&](std::uint32_t a_id) { return BaseNow(a_id); }, [&](std::uint32_t a_id) { records.erase(a_id); },
				[&](std::uint32_t a_id) {
					if (fixed) {
						records.erase(a_id);
					}
				});
			if (cellLoaded && held.id == 0) {
				held = { .id = PlaceCopy(), .base = FRAME };
			}
		}

		// RemotePlayers::PlaceCopy: a leftover at the spot is taken over (now), or a new one placed.
		std::uint32_t PlaceCopy()
		{
			if (fixed) {
				for (const auto& [id, ref] : loaded) {
					const HeldCopy::Standing standing{
						.created = true, .sameBase = ref.base == FRAME, .enabled = true, .emptied = ref.emptied, .placed = book.IsPlaced(id, ref.base), .distance = 0.0f
					};
					if (HeldCopy::Leftover(standing)) {
						book.Track(id, FRAME);
						return id;
					}
				}
			}
			return Place();
		}

		// The friend leaves, or a save is made: DespawnAll takes the copy away (deleted, its record dropped).
		void Despawn()
		{
			if (held.id != 0 && loaded.contains(held.id)) {
				loaded.erase(held.id);
				records.erase(held.id);
				book.Release(held.id);
			}
			held = {};
		}

		std::size_t Frames() const { return loaded.size(); }
	};

	// The player walks off and back a_times with the friend parked in Sanctuary.
	std::size_t WalkOffAndBack(bool a_fixed, int a_times)
	{
		Sanctuary sanctuary;
		sanctuary.fixed = a_fixed;
		sanctuary.Frame();
		for (int i = 0; i < a_times; ++i) {
			sanctuary.UnloadCell();
			sanctuary.Frame();
			sanctuary.Frame();
			sanctuary.LoadCell();
			sanctuary.Frame();
			sanctuary.Frame();
		}
		return sanctuary.Frames();
	}
}

TEST("held copy: with the friend parked in Sanctuary, each walk off and back left 0.9.1 one more frame; now there is one")
{
	CHECK(WalkOffAndBack(false, 1) == 2);
	CHECK(WalkOffAndBack(false, 3) == 4);
	CHECK(WalkOffAndBack(true, 1) == 1);
	CHECK(WalkOffAndBack(true, 3) == 1);
}

TEST("held copy: what 0.9.1 left came back in every session after; now the copy goes with the friend, and an old leftover is taken over")
{
	// 0.9.1: the copies that came back with the cell were nobody's, and a save kept them.
	Sanctuary old;
	old.fixed = false;
	old.Frame();
	old.UnloadCell();
	old.Frame();
	old.LoadCell();
	old.Frame();
	CHECK(old.Frames() == 2);
	old.Despawn();  // the friend leaves: only the one held goes
	CHECK(old.Frames() == 1);
	const auto left = old.loaded;
	CHECK(old.records.contains(left.begin()->first));

	// The next session, from that save: the friend is back, parked where they were.
	Sanctuary next;
	next.loaded = left;
	next.records = old.records;
	next.nextId = old.nextId;
	next.Frame();
	CHECK(next.Frames() == 1);  // the leftover is the copy now
	CHECK(next.held.id == left.begin()->first);
	next.Despawn();  // and it goes with the friend, out of the save
	CHECK(next.Frames() == 0);
	CHECK(next.records.empty());

	// Without the takeover, the next session placed a second on it.
	Sanctuary before;
	before.fixed = false;
	before.loaded = left;
	before.records = old.records;
	before.nextId = old.nextId;
	before.Frame();
	CHECK(before.Frames() == 2);
}

TEST("held copy: a frame with pieces in it, or one this run placed for another friend, is never taken over")
{
	Sanctuary sanctuary;
	sanctuary.loaded[0xFF000100] = { .base = FRAME, .emptied = false };  // a runtime suit somebody filled
	sanctuary.Frame();
	CHECK(sanctuary.Frames() == 2);
	CHECK(sanctuary.held.id != 0xFF000100);
	const auto first = sanctuary.held.id;
	sanctuary.held = {};  // a second friend parked at the same spot
	sanctuary.Frame();
	CHECK(sanctuary.held.id != first);
	CHECK(sanctuary.Frames() == 3);
}

TEST("save strays: a copy still on its way out as a load came is forgotten anew, kept out of every save after it, and let go of once gone")
{
	SaveStrays::Book book;
	book.Track(0xFF001814, FRAME);  // the parked copy, disabled and deleted by DespawnAll just before the load
	book.Track(0xFF001993, FRAME);  // another, gone with the load
	book.Track(0xFF001A00, OTHER);  // its id names the loaded game's own reference now
	const auto lingers = [](std::uint32_t a_id, std::uint32_t a_base) { return a_id == 0xFF001814 && a_base == FRAME; };
	book.AfterLoad({}, [](std::uint32_t, std::uint32_t) { return false; }, lingers);
	CHECK(book.PlacedCount() == 0);  // nothing placed before the load is ours to place or hold
	CHECK(book.ForgottenCount() == 1);
	std::vector<std::uint32_t> excluded;
	const auto baseNow = [](std::uint32_t a_id) -> std::optional<std::uint32_t> {
		return a_id == 0xFF001814 ? std::optional{ FRAME } : std::nullopt;
	};
	book.BeforeSave(baseNow, [&](std::uint32_t a_id) { excluded.push_back(a_id); }, [](std::uint32_t) {}, {}, [](std::uint32_t, std::uint32_t) { return false; });
	CHECK(excluded == std::vector<std::uint32_t>{ 0xFF001814 });  // 0.9.1 wrote it into the next save, and a fresh game brought it back whole
	excluded.clear();
	book.BeforeSave(baseNow, [&](std::uint32_t a_id) { excluded.push_back(a_id); }, [](std::uint32_t) {}, {}, [](std::uint32_t, std::uint32_t) { return false; });
	CHECK(excluded.size() == 1);  // every save, while it is there
	// Freed at last: let go of, its record dropped.
	std::vector<std::uint32_t> orphaned;
	book.KeepForgotten([](std::uint32_t) -> std::optional<std::uint32_t> { return std::nullopt; }, [](std::uint32_t) {}, [&](std::uint32_t a_id) { orphaned.push_back(a_id); });
	CHECK(orphaned == std::vector<std::uint32_t>{ 0xFF001814 });
	CHECK(book.ForgottenCount() == 0);
	// Without a lingering check, a load forgets them all, as before.
	SaveStrays::Book plain;
	plain.Track(0xFF001814, FRAME);
	plain.AfterLoad({}, [](std::uint32_t, std::uint32_t) { return false; });
	CHECK(plain.ForgottenCount() == 0);
}
