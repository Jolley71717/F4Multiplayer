#include "Test.h"
#include "game/WorkshopPower.h"

#include <map>
#include <set>
#include <vector>

// Settlement power across games: no grid in a save names a friend's copy, and each builder's game says what is powered.
namespace
{
	using namespace WorkshopPower;
	using namespace std::chrono_literals;

	constexpr std::uint32_t GENERATOR = 0xFF000A01;  // a friend's generator: our copy of it
	constexpr std::uint32_t PYLON = 0xFF000A02;      // another copy
	constexpr std::uint32_t LIGHT = 0xFF000B01;      // our own light
	constexpr std::uint32_t WIRE = 0xFF000B02;       // our wire, from our light to their generator's copy

	// A settlement's power as the game keeps it, and our copies in it: wiring to a copy puts it on the grid, gives it
	// power links and links it to the workbench, as the game does.
	struct Settlement
	{
		Grid                    grid;
		std::set<std::uint32_t> copies{ GENERATOR, PYLON };
		std::set<std::uint32_t> wired;    // with power links
		std::set<std::uint32_t> benched;  // linked to the workbench
		std::set<std::uint32_t> stubborn;  // what the game won't take off
		std::set<std::uint32_t> deleted;
		std::set<std::uint32_t> retired;

		void Wire(std::uint32_t a_from, std::uint32_t a_to, std::uint32_t a_wire)
		{
			for (const auto id : { a_from, a_to }) {
				if (std::ranges::find(grid.nodes, id) == grid.nodes.end()) {
					grid.nodes.push_back(id);
				}
				wired.insert(id);
				benched.insert(id);
			}
			grid.edges.push_back({ a_from, a_to, a_wire });
			grid.edges.push_back({ a_to, a_from, a_wire });
			grid.powered = { a_from, a_to };
		}

		// What a save of it would hold: every reference the grid names.
		[[nodiscard]] std::vector<std::uint32_t> Saved() const { return Named(grid); }

		// The engine, as WorkshopPower asks it.
		Links LinksOf(std::uint32_t a_id) const
		{
			const auto named = Named(grid);
			return { std::ranges::find(named, a_id) != named.end(), wired.contains(a_id), benched.contains(a_id) };
		}

		std::uint32_t detaches = 0;  // the game's grid and wire code run on a copy

		void Detach(std::uint32_t a_id)
		{
			++detaches;
			if (stubborn.contains(a_id)) {
				return;
			}
			std::erase(grid.nodes, a_id);
			std::erase(grid.powered, a_id);
			std::erase_if(grid.edges, [&](const Edge& a_edge) { return a_edge.node == a_id || a_edge.connection == a_id || a_edge.connector == a_id; });
			wired.erase(a_id);
			benched.erase(a_id);
		}

		void Delete(std::uint32_t a_id)
		{
			copies.erase(a_id);
			deleted.insert(a_id);
		}

		void Retire(std::uint32_t a_id)
		{
			copies.erase(a_id);
			retired.insert(a_id);
		}

		std::vector<Grid>          Grids() const { return { grid }; }
		std::vector<std::uint32_t> Copies() const { return { copies.begin(), copies.end() }; }
		bool                       IsCopy(std::uint32_t a_id) const { return copies.contains(a_id); }
	};

	// What a save may never hold: a grid naming a reference that was deleted.
	bool SaveIsSound(const Settlement& a_s)
	{
		for (const auto id : a_s.Saved()) {
			if (a_s.deleted.contains(id)) {
				return false;
			}
		}
		return true;
	}
}

TEST("workshop power: a grid names its nodes, their links and the wires between them, each once")
{
	Grid grid;
	grid.nodes = { LIGHT, GENERATOR };
	grid.edges = { { LIGHT, GENERATOR, WIRE }, { GENERATOR, LIGHT, WIRE }, { LIGHT, PYLON, 0 } };
	grid.powered = { LIGHT };
	CHECK((Named(grid) == std::vector<std::uint32_t>{ GENERATOR, PYLON, LIGHT, WIRE }));
	const std::vector<Grid> grids{ grid, grid };
	CHECK((NamedWhere(std::span<const Grid>{ grids }, [](std::uint32_t a_id) { return a_id == GENERATOR || a_id == PYLON; }) ==
		   std::vector<std::uint32_t>{ GENERATOR, PYLON }));
}

TEST("workshop power: a copy wired into our grid is taken off it before it is deleted, so no save names it")
{
	Settlement s;
	s.Wire(LIGHT, GENERATOR, WIRE);
	CHECK(!SaveIsSound([&] { auto gone = s; gone.Delete(GENERATOR); return gone; }()));  // deleted as before: the save would name it
	Counts counts;
	CHECK(Remove(s, GENERATOR, counts) == Removal::kDelete);
	CHECK(s.deleted.contains(GENERATOR));
	CHECK(SaveIsSound(s));
	CHECK(!s.LinksOf(GENERATOR).Any());
	CHECK(counts.found == 1);
	CHECK(counts.detached == 1);
	CHECK(counts.retired == 0);
	CHECK(s.LinksOf(LIGHT).Any());  // our own light stays on our grid
}

TEST("workshop power: a copy on no grid is deleted as it always was")
{
	Settlement s;
	Counts     counts;
	CHECK(Remove(s, PYLON, counts) == Removal::kDelete);
	CHECK(s.deleted.contains(PYLON));
	CHECK(counts.found == 0);
}

TEST("workshop power: a copy the game won't take off its grid is only switched off, never deleted")
{
	Settlement s;
	s.Wire(LIGHT, GENERATOR, WIRE);
	s.stubborn.insert(GENERATOR);
	Counts counts;
	CHECK(Remove(s, GENERATOR, counts) == Removal::kRetire);
	CHECK(!s.deleted.contains(GENERATOR));
	CHECK(s.retired.contains(GENERATOR));
	CHECK(SaveIsSound(s));  // the grid names it, and it is still there under its own ID
	CHECK(counts.retired == 1);
}

TEST("workshop power: the audit finds copies on a grid, or with power links or a workbench of their own, and takes them off")
{
	Settlement s;
	s.Wire(LIGHT, GENERATOR, WIRE);
	s.benched.insert(PYLON);  // linked to the workbench only (a wire half strung)
	Counts counts;
	CHECK(Audit(s, counts).empty());
	CHECK(!s.LinksOf(GENERATOR).Any());
	CHECK(!s.LinksOf(PYLON).Any());
	CHECK(counts.detached == 2);
	CHECK(s.copies.size() == 2);  // still copies: nothing is deleted by an audit
	CHECK(s.deleted.empty());
	for (const auto id : s.Saved()) {
		CHECK(!s.IsCopy(id));
	}
	Counts again;
	CHECK(Audit(s, again).empty());
	CHECK(again.found == 0);  // nothing left to do
}

TEST("workshop power: the audit names the copies the game kept tied in")
{
	Settlement s;
	s.Wire(LIGHT, GENERATOR, WIRE);
	s.stubborn.insert(GENERATOR);
	Counts counts;
	CHECK((Audit(s, counts) == std::vector<std::uint32_t>{ GENERATOR }));
}

TEST("workshop power: a copy the game kept tied in is not taken off again by every audit, but still named")
{
	Settlement s;
	s.Wire(LIGHT, GENERATOR, WIRE);
	s.stubborn.insert(GENERATOR);
	Counts                  counts;
	std::set<std::uint32_t> kept;
	const auto              skip = [&](std::uint32_t a_id) { return kept.contains(a_id); };
	auto                    stuck = Audit(s, counts, skip);
	CHECK((stuck == std::vector<std::uint32_t>{ GENERATOR }));
	kept.insert(stuck.begin(), stuck.end());
	const auto detaches = s.detaches;
	for (int i = 0; i < 30; ++i) {  // a minute of the session's audits
		CHECK((Audit(s, counts, skip) == std::vector<std::uint32_t>{ GENERATOR }));
	}
	CHECK(s.detaches == detaches);  // its grid and wires were left alone
	CHECK(counts.found == 1);
	// The audit before a save still tries it.
	CHECK((Audit(s, counts) == std::vector<std::uint32_t>{ GENERATOR }));
	CHECK(s.detaches == detaches + 1);
}

TEST("workshop power: a session's end with copies wired in leaves a save whose grids name none of them")
{
	Settlement s;
	s.Wire(LIGHT, GENERATOR, WIRE);
	s.Wire(GENERATOR, PYLON, 0xFF000B03);
	Counts counts;
	for (const auto copy : s.Copies()) {
		Remove(s, copy, counts);
	}
	CHECK(s.copies.empty());
	CHECK(SaveIsSound(s));
	CHECK((s.Saved() == std::vector<std::uint32_t>{ LIGHT }) || (s.Saved() == std::vector<std::uint32_t>{}));
}

namespace
{
	struct OwnGame
	{
		std::map<std::uint32_t, bool> powered;
		std::uint32_t                 asked = 0;

		std::optional<bool> Powered(std::uint32_t a_ref)
		{
			++asked;
			const auto it = powered.find(a_ref);
			return it == powered.end() ? std::nullopt : std::optional{ it->second };
		}
	};
}

TEST("workshop power: the builder sends all its pieces first, then only what changed, and all again now and then")
{
	Reporter reporter;
	OwnGame  game;
	game.powered = { { LIGHT, true }, { 0xFF000B05, false } };
	CHECK(reporter.Track(LIGHT));
	CHECK(reporter.Track(0xFF000B05));
	CHECK(!reporter.Track(0x0001F00D));  // a piece of the world: nobody has a copy of it
	const auto t0 = Clock::now();
	CHECK(reporter.Poll(game, t0).size() == 2);
	CHECK(reporter.Poll(game, t0 + 100ms).empty());  // not due
	CHECK(reporter.Poll(game, t0 + POLL).empty());   // nothing changed
	game.powered[LIGHT] = false;
	const auto changed = reporter.Poll(game, t0 + 2 * POLL);
	CHECK((changed == std::vector<Entry>{ { LIGHT, false } }));
	CHECK(reporter.Poll(game, t0 + SNAPSHOT).size() == 2);
	reporter.SendAll();  // someone joined
	CHECK(reporter.Poll(game, t0 + SNAPSHOT + POLL).size() == 2);
}

TEST("workshop power: a piece gone, or no longer ours, is forgotten")
{
	Reporter reporter;
	OwnGame  game;
	game.powered = { { LIGHT, true } };
	reporter.Track(LIGHT);
	reporter.Track(0xFF000B05);
	const auto t0 = Clock::now();
	CHECK(reporter.Poll(game, t0).size() == 1);
	CHECK(reporter.Size() == 1);
	CHECK(!reporter.Tracks(0xFF000B05));
}

TEST("workshop power: no more than MAX_TRACKED pieces are tracked")
{
	Reporter reporter;
	for (std::uint32_t i = 1; i <= MAX_TRACKED; ++i) {
		CHECK(reporter.Track(0xFF000000 + i));
	}
	CHECK(!reporter.Track(0xFF100000));
	CHECK(reporter.Track(0xFF000001));  // one tracked already
	CHECK(reporter.Size() == MAX_TRACKED);
}

TEST("workshop power: a big snapshot goes in packets of at most MAX_POWER_ENTRIES, each decoding back")
{
	std::vector<Entry> all;
	for (std::uint32_t i = 1; i <= 600; ++i) {
		all.push_back({ 0xFF000000 + i, i % 3 == 0 });
	}
	const auto chunks = Chunks(all);
	REQUIRE(chunks.size() == 3);
	std::vector<Entry> back;
	for (const auto& chunk : chunks) {
		CHECK(chunk.size() <= Protocol::MAX_POWER_ENTRIES);
		const auto decoded = Protocol::DecodePowerStates(Protocol::Encode(Protocol::PowerStates{ 7, chunk }, Protocol::MessageType::kWorkshopPower));
		REQUIRE(decoded.has_value());
		CHECK(decoded->playerId == 7u);
		back.insert(back.end(), decoded->entries.begin(), decoded->entries.end());
	}
	CHECK(back == all);
	CHECK(Chunks({}).empty());
}

namespace
{
	struct FriendGame
	{
		std::map<Key, std::uintptr_t> drawn;  // our copies' 3D
		std::map<Key, bool>           shown;
		bool                          graphReady = true;

		std::uintptr_t Drawn(const Key& a_key)
		{
			const auto it = drawn.find(a_key);
			return it == drawn.end() ? 0 : it->second;
		}

		bool Show(const Key& a_key, bool a_on)
		{
			if (!graphReady) {
				return false;
			}
			shown[a_key] = a_on;
			return true;
		}
	};

	constexpr std::uint32_t BUILDER = 2;
}

TEST("workshop power: a copy shows what its builder's game says, once it is drawn")
{
	Mirror     mirror;
	FriendGame game;
	const auto t0 = Clock::now();
	mirror.Set(BUILDER, { GENERATOR, true });
	CHECK(mirror.Refresh(game, t0) == 0);  // no copy here yet
	game.drawn[{ BUILDER, GENERATOR }] = 0x1000;
	CHECK(mirror.Refresh(game, t0 + 10ms) == 0);  // not due
	CHECK(mirror.Refresh(game, t0 + REFRESH) == 1);
	CHECK(game.shown.at({ BUILDER, GENERATOR }));
	CHECK(mirror.Refresh(game, t0 + 2 * REFRESH) == 0);  // shown: nothing to do
	mirror.Set(BUILDER, { GENERATOR, false });
	CHECK(mirror.Refresh(game, t0 + 3 * REFRESH) == 1);
	CHECK(!game.shown.at({ BUILDER, GENERATOR }));
}

TEST("workshop power: a copy drawn anew is shown again, and a graph not ready is tried again")
{
	Mirror     mirror;
	FriendGame game;
	const auto t0 = Clock::now();
	mirror.Set(BUILDER, { LIGHT, true });
	game.drawn[{ BUILDER, LIGHT }] = 0x1000;
	game.graphReady = false;
	CHECK(mirror.Refresh(game, t0) == 0);
	game.graphReady = true;
	CHECK(mirror.Refresh(game, t0 + REFRESH) == 1);
	game.shown.clear();
	game.drawn[{ BUILDER, LIGHT }] = 0x2000;  // its cell loaded again
	CHECK(mirror.Refresh(game, t0 + 2 * REFRESH) == 1);
	CHECK(game.shown.at({ BUILDER, LIGHT }));
	game.shown.clear();
	mirror.Invalidate();  // a save was loaded
	CHECK(mirror.Refresh(game, t0 + 3 * REFRESH) == 1);
}

TEST("workshop power: a copy whose game won't take it is let be after a few tries, until it is drawn anew")
{
	Mirror     mirror;
	FriendGame game;
	auto       t = Clock::now();
	mirror.Set(BUILDER, { PYLON, true });
	game.drawn[{ BUILDER, PYLON }] = 0x1000;
	game.graphReady = false;  // a piece with no graph: nothing to tell
	for (std::uint8_t i = 0; i < MAX_TRIES; ++i, t += REFRESH) {
		CHECK(mirror.Refresh(game, t) == 0);
	}
	CHECK(mirror.GivenUp() == 1);
	CHECK(mirror.Shown() == 1);
	game.graphReady = true;
	CHECK(mirror.Refresh(game, t) == 0);  // let be
	game.drawn[{ BUILDER, PYLON }] = 0x2000;
	CHECK(mirror.Refresh(game, t + REFRESH) == 1);  // drawn anew: tried again
}

TEST("workshop power: a copy the game keeps drawing anew is shown a few times, then let be for a while")
{
	Mirror     mirror;
	FriendGame game;
	auto       t = Clock::now();
	mirror.Set(BUILDER, { LIGHT, true });
	std::size_t shows = 0;
	std::uintptr_t model = 0x1000;
	for (int i = 0; i < 240; ++i, t += REFRESH) {  // a minute, a new 3D at every look
		game.drawn[{ BUILDER, LIGHT }] = model;
		model += 0x10;
		shows += mirror.Refresh(game, t);
	}
	CHECK(shows == 1u + MAX_REDRAWS);  // the first draw, then MAX_REDRAWS anew: no 3D lasted to be shown once more
	CHECK(mirror.RedrawsCapped() > 0);
	// Once the window is over it is shown again.
	t += REDRAW_WINDOW;
	game.drawn[{ BUILDER, LIGHT }] = model + 0x10;
	CHECK(mirror.Refresh(game, t) == 1);
}

TEST("workshop power: a copy drawn anew is shown once more when it has settled, so the game's own setup can't leave it dark")
{
	Mirror     mirror;
	FriendGame game;
	const Key  key{ BUILDER, LIGHT };
	auto       t = Clock::now();
	const auto showsFor = [&](Clock::duration a_for) {
		std::size_t shows = 0;
		for (const auto end = t + a_for; t < end; t += REFRESH) {
			shows += mirror.Refresh(game, t);
		}
		return shows;
	};
	mirror.Set(BUILDER, { LIGHT, true });  // a friend comes to a camp already lit
	game.drawn[key] = 0x1000;
	CHECK(showsFor(REFRESH) == 1);
	game.shown.clear();                 // the game's setup of the new 3D left it dark
	CHECK(showsFor(SETTLE - REFRESH) == 0);  // not settled yet
	CHECK(showsFor(REFRESH) == 1);
	CHECK(game.shown.at(key));
	CHECK(showsFor(2min) == 0);  // once: no more for the same 3D, nor for the snapshots saying the same
	mirror.Set(BUILDER, { LIGHT, true });
	CHECK(showsFor(SETTLE * 2) == 0);
	// Their generator goes off: the same 3D is shown so at once, with no show after.
	mirror.Set(BUILDER, { LIGHT, false });
	CHECK(showsFor(SETTLE * 2) == 1);
	CHECK(!game.shown.at(key));
	// Its cell loaded again, and a save loaded: each a show, and one more when settled.
	game.drawn[key] = 0x2000;
	CHECK(showsFor(SETTLE * 2) == 2);
	mirror.Invalidate();
	CHECK(showsFor(SETTLE * 2) == 2);
}

TEST("workshop power: a copy made anew just slower than it settles is shown no more than the redraw cap allows")
{
	Mirror     mirror;
	FriendGame game;
	auto       t = Clock::now();
	mirror.Set(BUILDER, { LIGHT, true });
	std::size_t    shows = 0;
	std::uintptr_t model = 0x1000;
	const auto     every = (SETTLE + REFRESH) / REFRESH;
	for (int i = 0; i < 200; ++i, t += REFRESH) {  // 50 s, under a window: a new 3D each SETTLE + REFRESH
		if (i % every == 0) {
			game.drawn[{ BUILDER, LIGHT }] = model;
			model += 0x10;
		}
		shows += mirror.Refresh(game, t);
	}
	CHECK(shows == 1u + MAX_REDRAWS);  // the first draw, then MAX_REDRAWS shows again, settled ones counted
	CHECK(mirror.RedrawsCapped() > 0);
}

TEST("workshop power: a copy given up on is not shown once more when it settles")
{
	Mirror     mirror;
	FriendGame game;
	auto       t = Clock::now();
	mirror.Set(BUILDER, { PYLON, true });
	game.drawn[{ BUILDER, PYLON }] = 0x1000;
	game.graphReady = false;
	for (std::uint8_t i = 0; i < MAX_TRIES; ++i, t += REFRESH) {
		CHECK(mirror.Refresh(game, t) == 0);
	}
	CHECK(mirror.GivenUp() == 1);
	game.graphReady = true;
	std::size_t shows = 0;
	for (const auto end = t + SETTLE * 2; t < end; t += REFRESH) {
		shows += mirror.Refresh(game, t);
	}
	CHECK(shows == 0u);
}

TEST("workshop power: a builder who left has their copies shown no more, until their game speaks again")
{
	Mirror     mirror;
	FriendGame game;
	auto       t = Clock::now();
	mirror.Set(BUILDER, { LIGHT, true });
	mirror.Set(3, { PYLON, true });
	game.drawn[{ BUILDER, LIGHT }] = 0x1000;
	game.drawn[{ 3, PYLON }] = 0x2000;
	CHECK(mirror.Refresh(game, t) == 2);
	mirror.Freeze(BUILDER);
	game.drawn[{ BUILDER, LIGHT }] = 0x1100;  // its cell loaded again
	game.drawn[{ 3, PYLON }] = 0x2100;
	t += REFRESH;
	CHECK(mirror.Refresh(game, t) == 1);  // only the other builder's
	CHECK(mirror.Wanted({ BUILDER, LIGHT }) == std::optional{ true });  // kept for when they come back
	mirror.Set(BUILDER, { LIGHT, true });  // back: their join's snapshot
	t += REFRESH;
	CHECK(mirror.Refresh(game, t) == 1);
	CHECK(game.shown.at({ BUILDER, LIGHT }));
}

TEST("workshop power: two builders' pieces with the same ID are told apart; a scrapped piece is forgotten")
{
	Mirror     mirror;
	FriendGame game;
	mirror.Set(2, { LIGHT, true });
	mirror.Set(3, { LIGHT, false });
	CHECK(mirror.Wanted({ 2, LIGHT }) == std::optional{ true });
	CHECK(mirror.Wanted({ 3, LIGHT }) == std::optional{ false });
	mirror.Forget({ 2, LIGHT });
	CHECK(!mirror.Wanted({ 2, LIGHT }));
	CHECK(mirror.Size() == 1);
}

TEST("workshop power: a copy shown powered is shown unpowered for a save and powered again after it (a save keeps what its graph was told)")
{
	Mirror     mirror;
	FriendGame game;
	const Key  lit{ BUILDER, GENERATOR };
	const Key  dark{ BUILDER, LIGHT };
	auto       t = Clock::now();
	mirror.Set(BUILDER, { GENERATOR, true });
	mirror.Set(BUILDER, { LIGHT, false });
	game.drawn[lit] = 0x1000;
	game.drawn[dark] = 0x2000;
	CHECK(mirror.Refresh(game, t) == 2);
	CHECK(game.shown.at(lit));
	CHECK(!game.shown.at(dark));
	t += SETTLE * 2;
	CHECK(mirror.Refresh(game, t) == 2);  // each once more, settled
	game.shown.clear();
	CHECK(mirror.Hush(game, t) == 1);  // only the one running
	CHECK(!game.shown.at(lit));         // told unpowered: what the save keeps
	CHECK(!game.shown.contains(dark));
	CHECK(mirror.Hushed() == 1);
	CHECK(mirror.Hush(game, t) == 0);   // once for a save
	t += REFRESH;
	CHECK(mirror.Refresh(game, t) == 0);  // the save isn't over
	CHECK(!game.shown.at(lit));
	mirror.SaveDone();
	CHECK(mirror.Refresh(game, t) == 1);  // at once, not at the next look
	CHECK(game.shown.at(lit));
	t += SETTLE * 3;
	CHECK(mirror.Refresh(game, t) == 0);  // and no more
	CHECK(mirror.Hush(game, t) == 1);     // the next save hushes it again
	game.shown.clear();
	mirror.SaveDone();
	CHECK(mirror.Refresh(game, t + REFRESH) == 1);
	CHECK(game.shown.at(lit));
}

TEST("workshop power: a copy hushed for a save is shown again after HUSH_LIMIT if nothing says the save is over")
{
	Mirror     mirror;
	FriendGame game;
	const Key  key{ BUILDER, GENERATOR };
	const auto t = Clock::now();
	mirror.Set(BUILDER, { GENERATOR, true });
	game.drawn[key] = 0x1000;
	CHECK(mirror.Refresh(game, t) == 1);
	CHECK(mirror.Hush(game, t) == 1);
	CHECK(mirror.Refresh(game, t + REFRESH) == 0);
	CHECK(!game.shown.at(key));
	CHECK(mirror.Refresh(game, t + HUSH_LIMIT + REFRESH) == 1);
	CHECK(game.shown.at(key));
}

TEST("workshop power: a frozen copy (its builder left) is hushed for a save and keeps what it showed after it")
{
	Mirror     mirror;
	FriendGame game;
	const Key  key{ BUILDER, GENERATOR };
	auto       t = Clock::now();
	mirror.Set(BUILDER, { GENERATOR, true });
	game.drawn[key] = 0x1000;
	CHECK(mirror.Refresh(game, t) == 1);
	mirror.Freeze(BUILDER);
	CHECK(mirror.Hush(game, t) == 1);
	CHECK(!game.shown.at(key));
	mirror.SaveDone();
	CHECK(mirror.Refresh(game, t + REFRESH) == 1);
	CHECK(game.shown.at(key));
	// Their game comes back while the copy is hushed (no save message yet): shown at once, not after the limit.
	mirror.Freeze(BUILDER);
	CHECK(mirror.Hush(game, t + 2 * REFRESH) == 1);
	mirror.Set(BUILDER, { GENERATOR, true });
	CHECK(mirror.Refresh(game, t + 3 * REFRESH) == 1);
	CHECK(game.shown.at(key));
	// A copy drawn anew between the hush and the show after is not shown again for it.
	CHECK(mirror.Hush(game, t + 4 * REFRESH) == 1);
	game.drawn[key] = 0x2000;
	mirror.SaveDone();
	CHECK(mirror.Refresh(game, t + 5 * REFRESH) == 1);
	CHECK(mirror.Refresh(game, t + 6 * REFRESH) == 0);
	// A second save while it is hushed renews the hush: the first one's time doesn't end it.
	const auto later = t + 10 * REFRESH;
	CHECK(mirror.Hush(game, later) == 1);
	CHECK(mirror.Hush(game, later + HUSH_LIMIT - REFRESH) == 0);
	CHECK(mirror.Refresh(game, later + HUSH_LIMIT + REFRESH) == 0);
	CHECK(mirror.Refresh(game, later + 2 * HUSH_LIMIT) == 1);
}

TEST("workshop power: a hush ends when its builder's game says otherwise, or a load comes, and a game that won't take it is let be")
{
	Mirror     mirror;
	FriendGame game;
	const Key  key{ BUILDER, GENERATOR };
	auto       t = Clock::now();
	mirror.Set(BUILDER, { GENERATOR, true });
	game.drawn[key] = 0x1000;
	CHECK(mirror.Refresh(game, t) == 1);
	CHECK(mirror.Hush(game, t) == 1);
	mirror.Set(BUILDER, { GENERATOR, false });  // their generator went off in the meantime
	mirror.SaveDone();
	t += REFRESH;
	CHECK(mirror.Refresh(game, t) == 1);  // shown as it is now, once
	CHECK(!game.shown.at(key));
	CHECK(mirror.Refresh(game, t + REFRESH) == 0);
	// A load after a hush: every copy is shown again as usual.
	mirror.Set(BUILDER, { GENERATOR, true });
	t += REFRESH * 2;
	CHECK(mirror.Refresh(game, t) == 1);
	CHECK(mirror.Hush(game, t) == 1);
	mirror.Invalidate();
	t += REFRESH;
	CHECK(mirror.Refresh(game, t) == 1);
	CHECK(game.shown.at(key));
	// The game takes the hush but not the show after: it is shown as any copy is, with the usual tries, and is not left
	// unpowered for good while they last.
	CHECK(mirror.Hush(game, t) == 1);
	game.graphReady = false;
	mirror.SaveDone();
	t += REFRESH;
	CHECK(mirror.Refresh(game, t) == 0);
	game.graphReady = true;
	t += REFRESH;
	CHECK(mirror.Refresh(game, t) == 1);  // the graph came back: powered again
	CHECK(game.shown.at(key));
	CHECK(mirror.GivenUp() == 0);
	// ... and after MAX_TRIES of them, let be.
	CHECK(mirror.Hush(game, t) == 1);
	game.graphReady = false;
	mirror.SaveDone();
	for (std::uint8_t i = 0; i < MAX_TRIES + 1; ++i) {
		t += REFRESH;
		CHECK(mirror.Refresh(game, t) == 0);
	}
	CHECK(mirror.GivenUp() == 1);
	// A hush the game won't take is no hush: nothing is shown again for it.
	FriendGame deaf;
	deaf.drawn[key] = 0x1000;
	deaf.graphReady = false;
	CHECK(mirror.Hush(deaf, t) == 0);
}

TEST("workshop power: a copy gone between a save's hush and the show after is shown powered when it is made again, and one scrapped is let go")
{
	Mirror     mirror;
	FriendGame game;
	const Key  gone{ BUILDER, GENERATOR };
	const Key  scrapped{ BUILDER, LIGHT };
	auto       t = Clock::now();
	mirror.Set(BUILDER, { GENERATOR, true });
	mirror.Set(BUILDER, { LIGHT, true });
	game.drawn[gone] = 0x1000;
	game.drawn[scrapped] = 0x2000;
	CHECK(mirror.Refresh(game, t) == 2);
	CHECK(mirror.Hush(game, t) == 2);
	// The save is over, but the copy's cell went (a travel's save) and the other was scrapped by its builder.
	game.drawn.erase(gone);
	game.drawn.erase(scrapped);
	mirror.Forget(scrapped);
	game.shown.clear();
	mirror.SaveDone();
	t += REFRESH;
	CHECK(mirror.Refresh(game, t) == 0);  // nothing to show it on
	CHECK(game.shown.empty());
	CHECK(mirror.Size() == 1);
	t += HUSH_LIMIT;
	CHECK(mirror.Refresh(game, t) == 0);  // nor later, while it isn't drawn
	game.drawn[gone] = 0x3000;            // made again
	t += REFRESH;
	CHECK(mirror.Refresh(game, t) == 1);
	CHECK(game.shown.at(gone));
	CHECK(!game.shown.contains(scrapped));
}

TEST("workshop power: no more than MAX_MIRRORED pieces are kept")
{
	Mirror mirror;
	for (std::uint32_t i = 1; i <= MAX_MIRRORED; ++i) {
		CHECK(mirror.Set(BUILDER, { 0xFF000000 + i, true }));
	}
	CHECK(!mirror.Set(BUILDER, { 0xFF100000, true }));
	CHECK(mirror.Set(BUILDER, { 0xFF000001, false }));  // known already: changed
	CHECK(mirror.Dropped() == 1);
}

TEST("workshop power: a power message names only pieces a game made, and each state is on or off")
{
	using namespace Protocol;
	const PowerStates power{ 4, { { 0xFF000042, true }, { 0xFF001234, false } } };
	const auto          back = DecodePowerStates(Encode(power, MessageType::kReportWorkshopPower));
	REQUIRE(back.has_value());
	CHECK(*back == power);
	CHECK(!DecodePowerStates(Encode(PowerStates{ 4, {} }, MessageType::kWorkshopPower)));                        // names nothing
	CHECK(!DecodePowerStates(Encode(PowerStates{ 4, { { 0x0001F00D, true } } }, MessageType::kWorkshopPower)));  // a piece of the world
	CHECK(!DecodePowerStates(Encode(PowerStates{ 4, { { 0xFF000000, true } } }, MessageType::kWorkshopPower)));
	auto bytes = Encode(power, MessageType::kWorkshopPower);
	bytes[1 + 4 + 1 + 4] = 2;  // the first entry's state
	CHECK(!DecodePowerStates(bytes));
	std::vector<PowerState> many(MAX_POWER_ENTRIES + 10, { 0xFF000042, true });
	const auto                      capped = DecodePowerStates(Encode(PowerStates{ 4, many }, MessageType::kWorkshopPower));
	REQUIRE(capped.has_value());
	CHECK(capped->entries.size() == MAX_POWER_ENTRIES);
}
