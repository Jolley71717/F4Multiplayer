// A friend's frame copy the engine unloads with its cell (HeldCopy, SaveStrays::Book::KeepForgotten), and one still on
// its way out as a load comes (SaveStrays::Remains, Book::AfterLoad): what made the 0.9.1 report's frames pile up in
// Sanctuary, and what keeps one copy per friend's frame now without ever taking a frame of the player's for ours.

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
	constexpr HeldCopy::Spot AT{ .cell = 0x0000DD9F, .x = -68367.0f, .y = 81042.0f, .z = 7357.0f };  // where a copy stands
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
	const HeldCopy::Ids held{ .id = 0xFF000F00, .base = FRAME, .at = AT };
	CHECK(Look(held, false, { .baseNow = FRAME, .deleted = false, .ours = true, .atNow = AT }) == Lost::kBack);
	CHECK(Look(held, false, { .baseNow = std::nullopt }) == Lost::kForget);                           // unloaded: no form under the id
	CHECK(Look(held, false, { .baseNow = OTHER, .ours = false }) == Lost::kForget);                  // the id names something else now
	CHECK(Look(held, false, { .baseNow = 0 }) == Lost::kForget);                                      // a form that is no reference
	CHECK(Look(held, false, { .baseNow = FRAME, .deleted = true, .ours = true }) == Lost::kForget);   // on its way out
	CHECK(Look(held, false, { .baseNow = FRAME, .deleted = false, .ours = false }) == Lost::kForget);  // a frame not ours under the id
}

TEST("held copy: the same id and base are no proof on their own: only a frame standing where ours was last seen is ours back")
{
	using HeldCopy::Look;
	using HeldCopy::Lost;
	const HeldCopy::Ids held{ .id = 0xFF000F00, .base = FRAME, .at = AT };
	const HeldCopy::Facts back{ .baseNow = FRAME, .ours = true, .atNow = AT };
	CHECK(Look(held, false, back) == Lost::kBack);
	auto moved = back;
	moved.atNow->x += 2.0f;  // within a few units: the record's own spot
	CHECK(Look(held, false, moved) == Lost::kBack);
	// A raider's frame of the same base made under the freed id, elsewhere in the cell or in another cell.
	auto raider = back;
	raider.atNow->x += 300.0f;
	CHECK(Look(held, false, raider) == Lost::kForget);
	auto elsewhere = back;
	elsewhere.atNow->cell = 0x0000DDFF;
	CHECK(Look(held, false, elsewhere) == Lost::kForget);
	auto noCell = back;
	noCell.atNow.reset();  // in no cell: nothing to compare
	CHECK(Look(held, false, noCell) == Lost::kForget);
	CHECK(Look({ .id = 0xFF000F00, .base = FRAME }, false, back) == Lost::kForget);  // never seen standing anywhere
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

		// SaveGuard::Forget: and, now, one whose form is gone taken off the books and its record dropped at once
		// (ForgetGone, DropOrphan).
		void Forget(std::uint32_t a_id)
		{
			if (fixed && !loaded.contains(a_id)) {
				if (book.ForgetGone(a_id)) {
					records.erase(a_id);
				}
				return;
			}
			book.Forget(a_id);
		}

		// A frame of RemotePlayers::Update with the friend parked here (UpdateParked, after Refresh).
		void Frame()
		{
			const bool resolves = held.id != 0 && loaded.contains(held.id);
			if (fixed) {
				const auto base = BaseNow(held.id);
				// A copy back from its record stands where it was last seen (the sim's copies never move).
				const HeldCopy::Facts facts{ .baseNow = base, .ours = base && book.IsPlaced(held.id, *base), .atNow = base ? std::optional{ AT } : std::nullopt };
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
				held = { .id = PlaceCopy(), .base = FRAME, .at = AT };
			}
		}

		// RemotePlayers::PlaceCopy: a new copy, always. A frame already at the spot is never taken over: nothing tells one an
		// earlier session left from one of the player's own.
		std::uint32_t PlaceCopy()
		{
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


TEST("held copy: a frame an earlier save left at the spot is never taken over, so one of the player's own there is safe")
{
	// The player stripped a suit of theirs at the very spot a friend's frame stands (pieces out, its inventory made and
	// empty, its owner unknown to us), or an earlier session's copy stands there: nothing tells the two apart.
	Sanctuary sanctuary;
	sanctuary.loaded[0xFF000100] = {};
	sanctuary.records.insert(0xFF000100);
	sanctuary.Frame();
	CHECK(sanctuary.held.id != 0xFF000100);
	CHECK(sanctuary.Frames() == 2);
	sanctuary.Despawn();  // the friend leaves: only their copy goes
	CHECK(sanctuary.Frames() == 1);
	CHECK(sanctuary.loaded.contains(0xFF000100));
	CHECK(sanctuary.records.contains(0xFF000100));  // and it is saved as it was
}

TEST("save strays: a reference under an id from before a load is our old copy only while it is provably the same form, a frame, off, and nobody's")
{
	using SaveStrays::StillOurs;
	const SaveStrays::Remains copy{ .same = true, .frame = true, .off = true };
	CHECK(StillOurs(copy));
	auto with = [&](auto a_change) {
		auto remains = copy;
		a_change(remains);
		return StillOurs(remains);
	};
	CHECK(!with([](auto& r) { r.same = false; }));      // a form the loaded game made since under the id (a raider's frame, disabled while worn)
	CHECK(!with([](auto& r) { r.frame = false; }));     // a stand-in, a light, an item copy: left as the game has it
	CHECK(!with([](auto& r) { r.off = false; }));       // enabled
	CHECK(!with([](auto& r) { r.worn = true; }));       // the player's or a companion's suit now
	CHECK(!with([](auto& r) { r.newOwner = true; }));   // somebody got out of it since: an owner it did not have when noted
	CHECK(!with([](auto& r) { r.holdsItems = true; })); // pieces or a core in it
}

TEST("save strays: a copy still on its way out as a load came is kept out of saves while it stays so, and left to the game once it does not")
{
	SaveStrays::Book book;
	book.Track(0xFF001814, FRAME);  // the parked copy, disabled and deleted by DespawnAll just before the load
	book.Track(0xFF001993, FRAME);  // another, gone with the load
	book.Track(0xFF001A00, FRAME);  // the id names a frame the loaded game made since (not the noted form)
	book.Track(0xFF001A01, FRAME);
	book.Forget(0xFF001A01);        // forgotten before the load
	std::set<std::uint32_t> held;  // a forgotten one is still placed too: it may come twice
	book.ForEachHeld([&](std::uint32_t a_id, std::uint32_t) { held.insert(a_id); });
	CHECK(held.size() == 4);  // all noted before the load (SaveGuard::BeforeLoad)

	std::map<std::uint32_t, SaveStrays::Remains> now{
		{ 0xFF001814, { .same = true, .frame = true, .off = true } },
		{ 0xFF001A00, { .same = false, .frame = true, .off = true } },
	};
	const auto still = [&](std::uint32_t a_id, std::uint32_t a_base) {
		const auto it = now.find(a_id);
		return a_base == FRAME && it != now.end() && SaveStrays::StillOurs(it->second);
	};
	book.AfterLoad({}, [](std::uint32_t, std::uint32_t) { return false; }, still);
	CHECK(book.PlacedCount() == 0);     // nothing placed before the load is ours to place or hold
	CHECK(book.ForgottenCount() == 0);  // nor forgotten: its id is not ours after the load
	CHECK(book.LingeringCount() == 1);

	std::vector<std::uint32_t> excluded;
	std::vector<std::uint32_t> dropped;
	const auto keep = [&] {
		book.KeepLingering(still, [&](std::uint32_t a_id) { excluded.push_back(a_id); }, [&](std::uint32_t a_id) { dropped.push_back(a_id); });
	};
	keep();
	keep();
	CHECK((excluded == std::vector<std::uint32_t>{ 0xFF001814, 0xFF001814 }));  // at every check and save while it is so
	CHECK(dropped.empty());

	// A second load with it still there: it lingers on, once.
	book.AfterLoad({}, [](std::uint32_t, std::uint32_t) { return false; }, still);
	CHECK(book.LingeringCount() == 1);

	// The player gets into it (or it is enabled, filled, owned): left to the game from then on, never excluded again, and
	// no record under its id is ever dropped for it (KeepForgotten has nothing of it).
	now[0xFF001814].worn = true;
	excluded.clear();
	keep();
	CHECK(excluded.empty());
	CHECK(dropped == std::vector<std::uint32_t>{ 0xFF001814 });
	now[0xFF001814].worn = false;
	keep();
	CHECK(excluded.empty());
	CHECK(book.LingeringCount() == 0);
	std::vector<std::uint32_t> orphaned;
	book.KeepForgotten([](std::uint32_t) -> std::optional<std::uint32_t> { return std::nullopt; }, [](std::uint32_t) {}, [&](std::uint32_t a_id) { orphaned.push_back(a_id); });
	CHECK(orphaned.empty());

	// Without a lingering check, a load forgets them all, as before.
	SaveStrays::Book plain;
	plain.Track(0xFF001814, FRAME);
	plain.AfterLoad({}, [](std::uint32_t, std::uint32_t) { return false; });
	CHECK(plain.LingeringCount() == 0);
	CHECK(plain.ForgottenCount() == 0);
}

TEST("save strays: the review's case: a raider's frame under our old id, stripped and got into by the player, is never kept out of a save")
{
	SaveStrays::Book book;
	book.Track(0xFF000A10, FRAME);  // our copy, placed after the save the player quickloads
	// After the quickload the engine hands 0xFF000A10 to a raider's frame, disabled while worn: same id, same base, not the
	// form noted before the load.
	SaveStrays::Remains raiders{ .same = false, .frame = true, .off = true };
	book.AfterLoad({}, [](std::uint32_t, std::uint32_t) { return false; }, [&](std::uint32_t, std::uint32_t) { return SaveStrays::StillOurs(raiders); });
	CHECK(book.LingeringCount() == 0);
	std::vector<std::uint32_t> excluded;
	book.KeepLingering([](std::uint32_t, std::uint32_t) { return true; }, [&](std::uint32_t a_id) { excluded.push_back(a_id); }, [](std::uint32_t) {});
	book.BeforeSave([](std::uint32_t) -> std::optional<std::uint32_t> { return FRAME; }, [&](std::uint32_t a_id) { excluded.push_back(a_id); }, [](std::uint32_t) {}, {},
		[](std::uint32_t, std::uint32_t) { return false; });
	CHECK(excluded.empty());
}

TEST("save strays: a copy unloaded with its cell is off the books at once, so a frame handed its freed id next is never taken for ours")
{
	SaveStrays::Book book;
	book.Track(0xFF000C40, FRAME);  // a friend's parked copy; the player walks off and the engine unloads it
	CHECK(book.ForgetGone(0xFF000C40));
	CHECK(!book.ForgetGone(0xFF000C41));  // never ours
	CHECK(!book.IsPlaced(0xFF000C40, FRAME));
	CHECK(book.ForgottenCount() == 0);
	CHECK(book.TakeGone().size() == 1);  // what else holds it as ours lets go of it too
	// The next cell attaches and a raider in power armor gets the freed id, on the same frame: nothing keeps it out.
	std::vector<std::uint32_t> excluded;
	book.KeepForgotten([](std::uint32_t) -> std::optional<std::uint32_t> { return FRAME; }, [&](std::uint32_t a_id) { excluded.push_back(a_id); });
	book.BeforeSave([](std::uint32_t) -> std::optional<std::uint32_t> { return FRAME; }, [&](std::uint32_t a_id) { excluded.push_back(a_id); }, [](std::uint32_t) {}, {},
		[](std::uint32_t, std::uint32_t) { return false; });
	CHECK(excluded.empty());
	CHECK(!book.IsPlaced(0xFF000C40, FRAME));
}

TEST("save strays: the copy a friend's stand-in wears as a load comes lingers too: the owner it was given before the load is ours")
{
	// Equipment::SwitchIntoPowerArmor gives the copy the stand-in's base for its owner; DespawnAll buries it before the
	// load, and a load that leaves it in memory would have the next save write it.
	const SaveStrays::Remains worn{ .same = true, .frame = true, .off = true, .newOwner = false };
	CHECK(SaveStrays::StillOurs(worn));
	auto taken = worn;
	taken.newOwner = true;  // somebody got out of it since, and owns it now
	CHECK(!SaveStrays::StillOurs(taken));
}

TEST("held copy: a copy handed to someone here (a companion got into it) is off the books, so no load notes it as ours")
{
	SaveStrays::Book book;
	book.Track(0xFF000F10, FRAME);  // the copy left where a friend got out
	book.Track(0xFF000F11, FRAME);  // their parked copy
	book.Release(0xFF000F10);       // "someone here is in it; leaving it" (RemotePlayers, Spawns: SaveGuard::Release)
	std::set<std::uint32_t> held;
	book.ForEachHeld([&](std::uint32_t a_id, std::uint32_t) { held.insert(a_id); });
	CHECK(held == std::set<std::uint32_t>{ 0xFF000F11 });
	CHECK(!book.IsPlaced(0xFF000F10, FRAME));
}
