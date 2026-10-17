#include "Test.h"
#include "game/SavedScraps.h"

#include <algorithm>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

// The host's walk for settlement junk its save had scrapped before the session: what it reports, per
// cell, and once; what waits for its workbench; and how much it does a frame.
namespace
{
	using SavedScraps::Cell;
	using SavedScraps::Seen;
	struct FakeGame;
	using Walk = SavedScraps::Walk<FakeGame>;
	using Report = std::pair<std::uint32_t, std::uint32_t>;  // (reference, its workbench)

	constexpr std::uint32_t COMMONWEALTH = 0x0000003C;  // the worldspace: its persistent cell is walked under its ID
	constexpr std::uint32_t SANCTUARY = 0x0000E0C2;     // an exterior cell of it
	constexpr std::uint32_t RED_ROCKET = 0x0000E0C3;    // another
	constexpr std::uint32_t HOME_PLATE = 0x0002B3A4;    // an interior cell
	constexpr std::uint32_t BENCH = 0x000250FE;         // Sanctuary's workbench, among the worldspace's persistent references
	constexpr std::uint32_t BENCH_RR = 0x0001F2AB;      // Red Rocket's
	constexpr std::uint32_t BENCH_HP = 0x0002B3B0;      // Home Plate's, in its cell

	// The game as the walk sees it: cells of references, each of them something to the walk, and the
	// workbench whose build area each piece of junk stands in.
	struct FakeGame
	{
		struct Piece
		{
			std::uint32_t cell;
			Seen          seen;
			std::uint32_t bench;  // the workbench whose build area it stands in (0: none)
		};

		std::map<std::uint32_t, Piece> pieces;
		std::set<std::uint32_t>        attached{ COMMONWEALTH, SANCTUARY, RED_ROCKET, HOME_PLATE };
		std::vector<Report>            reports;
		bool                           sending = true;
		int                            examined = 0;

		void Add(std::uint32_t a_id, std::uint32_t a_cell, Seen a_seen, std::uint32_t a_bench = 0) { pieces[a_id] = { a_cell, a_seen, a_bench }; }

		static std::uint32_t SpaceOf(std::uint32_t a_cell) { return a_cell == HOME_PLATE ? HOME_PLATE : COMMONWEALTH; }

		std::optional<SavedScraps::Snapshot> Refs(const Cell& a_cell)
		{
			if (!attached.contains(a_cell.id)) {
				return std::nullopt;
			}
			SavedScraps::Snapshot snapshot{ a_cell.persistent ? a_cell.id : SpaceOf(a_cell.id), {} };
			for (const auto& [id, piece] : pieces) {
				if (piece.cell == a_cell.id) {
					snapshot.refs.push_back(id);
				}
			}
			return snapshot;
		}

		bool Attached(const Cell& a_cell) { return attached.contains(a_cell.id); }

		std::optional<Seen> Examine(std::uint32_t a_id, bool)
		{
			++examined;
			const auto it = pieces.find(a_id);
			return it != pieces.end() ? std::optional{ it->second.seen } : std::nullopt;
		}

		bool Loaded(std::uint32_t a_id) { return pieces.contains(a_id); }

		bool InBuildArea(std::uint32_t a_bench, std::uint32_t a_ref)
		{
			const auto it = pieces.find(a_ref);
			return it != pieces.end() && it->second.bench == a_bench && pieces.contains(a_bench);
		}

		// Both must still be here, as in the game.
		bool Report(std::uint32_t a_ref, std::uint32_t a_bench)
		{
			if (!sending || !pieces.contains(a_ref) || !pieces.contains(a_bench)) {
				return false;
			}
			reports.emplace_back(a_ref, a_bench);
			return true;
		}
	};

	// Walks everything queued to the end; the last round's summary.
	std::optional<SavedScraps::Round> WalkAll(Walk& a_walk, FakeGame& a_game)
	{
		std::optional<SavedScraps::Round> round;
		while (a_walk.Queued() > 0) {
			round = a_walk.WalkSome(a_game);
		}
		return round;
	}

	// Reports everything found, however long that takes.
	void SendAll(Walk& a_walk, FakeGame& a_game)
	{
		while (a_walk.ToSend() > 0) {
			a_walk.SendSome(a_game, 1.0);
		}
	}

	bool Reported(const FakeGame& a_game, std::uint32_t a_ref, std::uint32_t a_bench)
	{
		return std::ranges::find(a_game.reports, Report{ a_ref, a_bench }) != a_game.reports.end();
	}
}

TEST("saved scraps: the ledger notes a piece once, until it's forgotten")
{
	SavedScraps::Ledger ledger;
	CHECK(ledger.Note(0x1000));
	CHECK(!ledger.Note(0x1000));
	CHECK(ledger.Holds(0x1000));
	ledger.Forget(0x1000);
	CHECK(!ledger.Holds(0x1000));
	CHECK(ledger.Note(0x1000));
	ledger.Clear();
	CHECK(!ledger.Holds(0x1000));
}

TEST("saved scraps: a cell's scrapped junk in a workbench's build area is reported with its workbench, and nothing else in the cell is")
{
	FakeGame game;
	game.Add(BENCH, COMMONWEALTH, Seen::kBench);
	game.Add(0x1001, SANCTUARY, Seen::kStanding);
	game.Add(0x1002, SANCTUARY, Seen::kOff);       // switched off by the game, or a friend's scrap
	game.Add(0x1003, SANCTUARY, Seen::kDeleted);   // a quest's doing
	game.Add(0x1004, SANCTUARY, Seen::kScrapped, BENCH);
	game.Add(0x1005, SANCTUARY, Seen::kScrapped);  // scrapped junk, but in no workbench's build area
	SavedScraps::Ledger ledger;
	Walk                walk{ ledger };
	walk.QueueWorld(COMMONWEALTH);
	walk.QueueCell(SANCTUARY);
	const auto round = WalkAll(walk, game);
	REQUIRE(round.has_value());
	CHECK(round->counts.refs == 6);
	CHECK(round->counts.benches == 1);
	CHECK(round->counts.disabled == 4);
	CHECK(round->counts.deleted == 3);
	CHECK(round->toSend == 1);
	CHECK(round->waiting == 1);
	SendAll(walk, game);
	REQUIRE(game.reports.size() == 1);
	CHECK(Reported(game, 0x1004, BENCH));
	CHECK(walk.Reported() == 1);
	CHECK(walk.Waiting() == 1);
	CHECK(ledger.Holds(0x1004));
	CHECK(ledger.Holds(0x1005));  // waiting counts as on its way: found again, it isn't filed twice
	CHECK(!ledger.Holds(0x1003));
	REQUIRE(walk.Benches().size() == 1);
	CHECK(walk.Benches()[0].id == BENCH);
	CHECK(walk.Benches()[0].space == COMMONWEALTH);
}

TEST("saved scraps: each cell's junk is reported once; a cell attached again is walked again but brings nothing new")
{
	FakeGame game;
	game.Add(BENCH, COMMONWEALTH, Seen::kBench);
	game.Add(BENCH_RR, COMMONWEALTH, Seen::kBench);
	game.Add(0x1001, SANCTUARY, Seen::kScrapped, BENCH);
	game.Add(0x1002, SANCTUARY, Seen::kScrapped, BENCH);
	game.Add(0x2001, RED_ROCKET, Seen::kScrapped, BENCH_RR);
	SavedScraps::Ledger ledger;
	Walk                walk{ ledger };
	walk.QueueWorld(COMMONWEALTH);
	walk.QueueCell(SANCTUARY);
	walk.QueueCell(RED_ROCKET);
	WalkAll(walk, game);
	SendAll(walk, game);
	REQUIRE(game.reports.size() == 3);
	CHECK(Reported(game, 0x1001, BENCH));
	CHECK(Reported(game, 0x1002, BENCH));
	CHECK(Reported(game, 0x2001, BENCH_RR));

	// The player walks away and back: the cell attaches again.
	const auto examined = game.examined;
	walk.QueueCell(SANCTUARY);
	WalkAll(walk, game);
	SendAll(walk, game);
	CHECK(game.examined > examined);
	CHECK(game.reports.size() == 3);
	CHECK(walk.Reported() == 3);
}

TEST("saved scraps: junk found before its workbench waits, and goes when the round that found the workbench ends")
{
	FakeGame game;
	game.Add(BENCH, COMMONWEALTH, Seen::kBench);
	game.Add(0x1001, SANCTUARY, Seen::kScrapped, BENCH);
	game.Add(0x1002, SANCTUARY, Seen::kScrapped);  // in nobody's build area: waits for good
	SavedScraps::Ledger ledger;
	Walk                walk{ ledger };
	walk.QueueCell(SANCTUARY);  // before the worldspace's persistent references, where the workbench is
	walk.WalkSome(game);
	CHECK(walk.Waiting() == 2);
	CHECK(walk.ToSend() == 0);
	walk.QueueWorld(COMMONWEALTH);
	const auto round = WalkAll(walk, game);
	REQUIRE(round.has_value());
	CHECK(round->toSend == 1);
	CHECK(round->waiting == 1);
	SendAll(walk, game);
	REQUIRE(game.reports.size() == 1);
	CHECK(Reported(game, 0x1001, BENCH));
	CHECK(walk.Waiting() == 1);
}

TEST("saved scraps: a workbench's build area counts in its own space only")
{
	FakeGame game;
	game.Add(BENCH_HP, HOME_PLATE, Seen::kBench);
	game.Add(0x1001, SANCTUARY, Seen::kScrapped, BENCH_HP);  // the game would say it's in the area (a test's whim); the spaces differ
	game.Add(0x3001, HOME_PLATE, Seen::kScrapped, BENCH_HP);
	SavedScraps::Ledger ledger;
	Walk                walk{ ledger };
	walk.QueueCell(HOME_PLATE);
	walk.QueueCell(SANCTUARY);
	WalkAll(walk, game);
	SendAll(walk, game);
	REQUIRE(game.reports.size() == 1);
	CHECK(Reported(game, 0x3001, BENCH_HP));
	CHECK(walk.Waiting() == 1);
	CHECK(walk.BenchFor(game, 0x3001, HOME_PLATE) == BENCH_HP);
	CHECK(walk.BenchFor(game, 0x1001, COMMONWEALTH) == 0);
	CHECK(walk.BenchFor(game, 0x3001, 0) == 0);  // in no space at all
}

TEST("saved scraps: a report that can't be sent is let go, and made when its cell is walked again")
{
	FakeGame game;
	game.Add(BENCH, COMMONWEALTH, Seen::kBench);
	game.Add(0x1001, SANCTUARY, Seen::kScrapped, BENCH);
	SavedScraps::Ledger ledger;
	Walk                walk{ ledger };
	walk.QueueWorld(COMMONWEALTH);
	walk.QueueCell(SANCTUARY);
	WalkAll(walk, game);
	game.sending = false;
	SendAll(walk, game);
	CHECK(game.reports.empty());
	CHECK(walk.Reported() == 0);
	CHECK(!ledger.Holds(0x1001));

	game.sending = true;
	walk.QueueCell(SANCTUARY);
	WalkAll(walk, game);
	SendAll(walk, game);
	CHECK(Reported(game, 0x1001, BENCH));
	CHECK(walk.Reported() == 1);
}

TEST("saved scraps: a piece gone from the game before its turn is let go too")
{
	FakeGame game;
	game.Add(BENCH, COMMONWEALTH, Seen::kBench);
	game.Add(0x1001, SANCTUARY, Seen::kScrapped, BENCH);
	game.Add(0x1002, SANCTUARY, Seen::kScrapped);  // waits for a workbench
	SavedScraps::Ledger ledger;
	Walk                walk{ ledger };
	walk.QueueCell(SANCTUARY);
	walk.WalkSome(game);
	game.pieces.erase(0x1001);  // its cell detached between the walk and the round's end
	game.pieces.erase(0x1002);
	walk.QueueWorld(COMMONWEALTH);
	WalkAll(walk, game);  // the round's end tries the waiting pieces again: gone
	CHECK(walk.Waiting() == 0);
	CHECK(walk.ToSend() == 0);
	CHECK(!ledger.Holds(0x1001));
	CHECK(!ledger.Holds(0x1002));

	game.Add(0x1003, SANCTUARY, Seen::kScrapped, BENCH);
	walk.QueueCell(SANCTUARY);
	WalkAll(walk, game);
	game.pieces.erase(0x1003);  // gone between being queued to go and its turn
	SendAll(walk, game);
	CHECK(game.reports.empty());
	CHECK(!ledger.Holds(0x1003));
}

TEST("saved scraps: the walk looks at no more than its budget a frame, and carries on next frame")
{
	FakeGame game;
	for (std::uint32_t i = 0; i < 1000; ++i) {
		game.Add(0x1000 + i, SANCTUARY, Seen::kStanding);
	}
	SavedScraps::Ledger ledger;
	Walk                walk{ ledger };
	walk.QueueCell(SANCTUARY);
	CHECK(!walk.WalkSome(game, 512).has_value());
	CHECK(walk.SeenCounts().refs == 512);
	CHECK(walk.Queued() == 1);
	const auto round = walk.WalkSome(game, 512);
	REQUIRE(round.has_value());
	CHECK(round->counts.refs == 1000);
	CHECK(walk.SeenCounts().refs == 1000);
	CHECK(walk.Queued() == 0);
	CHECK(SavedScraps::WALK_PER_FRAME == 512);
}

TEST("saved scraps: a cell gone since the look is skipped, and one that goes mid-walk is given up")
{
	FakeGame game;
	game.Add(0x2001, RED_ROCKET, Seen::kStanding);
	for (std::uint32_t i = 0; i < 20; ++i) {
		game.Add(0x1000 + i, SANCTUARY, Seen::kStanding);
	}
	game.attached.erase(RED_ROCKET);
	SavedScraps::Ledger ledger;
	Walk                walk{ ledger };
	walk.QueueCell(RED_ROCKET);
	walk.QueueCell(SANCTUARY);
	CHECK(!walk.WalkSome(game, 10).has_value());  // Red Rocket skipped; ten of Sanctuary's
	CHECK(walk.SeenCounts().refs == 10);
	CHECK(walk.Queued() == 1);
	game.attached.erase(SANCTUARY);
	const auto round = walk.WalkSome(game, 10);
	REQUIRE(round.has_value());
	CHECK(round->counts.refs == 10);
	CHECK(walk.Queued() == 0);
}

TEST("saved scraps: the worldspace's persistent references are walked once, for workbenches only")
{
	FakeGame game;
	game.Add(BENCH, COMMONWEALTH, Seen::kBench);
	game.Add(0x0101, COMMONWEALTH, Seen::kScrapped, BENCH);  // a persistent piece: not the walk's business
	game.Add(0x0102, COMMONWEALTH, Seen::kOff);
	SavedScraps::Ledger ledger;
	Walk                walk{ ledger };
	walk.QueueWorld(COMMONWEALTH);
	walk.QueueWorld(COMMONWEALTH);
	CHECK(walk.Queued() == 1);
	const auto round = WalkAll(walk, game);
	REQUIRE(round.has_value());
	CHECK(round->counts.refs == 3);
	CHECK(round->counts.benches == 1);
	CHECK(round->counts.disabled == 0);
	CHECK(round->toSend == 0);
	CHECK(walk.Walked(COMMONWEALTH));
	walk.QueueWorld(COMMONWEALTH);
	CHECK(walk.Queued() == 0);

	// Given up (the worldspace changed before its walk started): queued again next time.
	Walk other{ ledger };
	game.attached.erase(COMMONWEALTH);
	other.QueueWorld(COMMONWEALTH);
	WalkAll(other, game);
	CHECK(!other.Walked(COMMONWEALTH));
	other.QueueWorld(COMMONWEALTH);
	CHECK(other.Queued() == 1);
}

TEST("saved scraps: reports go out a burst at once, then a hundred a second")
{
	FakeGame game;
	game.Add(BENCH, COMMONWEALTH, Seen::kBench);
	for (std::uint32_t i = 0; i < 50; ++i) {
		game.Add(0x1000 + i, SANCTUARY, Seen::kScrapped, BENCH);
	}
	SavedScraps::Ledger ledger;
	Walk                walk{ ledger };
	walk.QueueWorld(COMMONWEALTH);
	walk.QueueCell(SANCTUARY);
	WalkAll(walk, game);
	CHECK(walk.ToSend() == 50);
	CHECK(walk.SendSome(game, 0.0) == 0);  // the first frame: no time has passed
	CHECK(walk.SendSome(game, 1.0) == 20);
	CHECK(walk.SendSome(game, 0.1) == 10);
	CHECK(walk.SendSome(game, 0.05) == 5);
	CHECK(walk.SendSome(game, 0.0) == 0);
	CHECK(walk.Reported() == 35);
	CHECK(walk.ToSend() == 15);
	CHECK(game.reports.size() == 35);
}

TEST("saved scraps: a load forgets the walk: what waited or was queued to go is found again, what was reported stays reported")
{
	FakeGame game;
	game.Add(BENCH, COMMONWEALTH, Seen::kBench);
	game.Add(0x1001, SANCTUARY, Seen::kScrapped, BENCH);
	game.Add(0x1002, SANCTUARY, Seen::kScrapped, BENCH);
	game.Add(0x1003, SANCTUARY, Seen::kScrapped);  // waits for a workbench
	SavedScraps::Ledger ledger;
	Walk                walk{ ledger };
	walk.QueueWorld(COMMONWEALTH);
	walk.QueueCell(SANCTUARY);
	WalkAll(walk, game);
	CHECK(walk.SendSome(game, 0.015) == 1);  // one token's worth, and a bit
	REQUIRE(game.reports.size() == 1);
	const auto sent = game.reports[0].first;
	walk.Clear();
	CHECK(walk.ToSend() == 0);
	CHECK(walk.Waiting() == 0);
	CHECK(walk.Benches().empty());
	CHECK(walk.Queued() == 0);
	CHECK(ledger.Holds(sent));
	CHECK(!ledger.Holds(sent == 0x1001 ? 0x1002 : 0x1001));
	CHECK(!ledger.Holds(0x1003));
	CHECK(!walk.Walked(COMMONWEALTH));

	walk.QueueWorld(COMMONWEALTH);
	walk.QueueCell(SANCTUARY);
	WalkAll(walk, game);
	SendAll(walk, game);
	REQUIRE(game.reports.size() == 2);
	CHECK(Reported(game, 0x1001, BENCH));
	CHECK(Reported(game, 0x1002, BENCH));
	CHECK(walk.Waiting() == 1);
	CHECK(walk.Reported() == 2);
}

TEST("saved scraps: pieces waiting for a workbench are capped; past the cap they are let go, to be found again")
{
	FakeGame game;
	game.Add(0x1001, SANCTUARY, Seen::kScrapped);
	game.Add(0x1002, SANCTUARY, Seen::kScrapped);
	game.Add(0x1003, SANCTUARY, Seen::kScrapped);
	SavedScraps::Ledger ledger;
	Walk                walk{ ledger, 2 };
	walk.QueueCell(SANCTUARY);
	WalkAll(walk, game);
	CHECK(walk.Waiting() == 2);
	CHECK(ledger.Holds(0x1001));
	CHECK(ledger.Holds(0x1002));
	CHECK(!ledger.Holds(0x1003));
	CHECK(SavedScraps::MAX_WAITING == 4096);
}

TEST("saved scraps: a frame's budget always ends the frame, to the reference, whatever the cells hold")
{
	// Three cells of 100, 250 and 7 references, the middle one a mix of what a walk can meet. Every call is made a
	// bounded number of times: a walk that overran its budget, or never ended a frame, fails here rather than hanging.
	FakeGame game;
	for (std::uint32_t i = 0; i < 100; ++i) {
		game.Add(0x1000 + i, SANCTUARY, Seen::kStanding);
	}
	for (std::uint32_t i = 0; i < 250; ++i) {
		game.Add(0x2000 + i, RED_ROCKET, i % 3 == 0 ? Seen::kOff : (i % 3 == 1 ? Seen::kDeleted : Seen::kStanding));
	}
	for (std::uint32_t i = 0; i < 7; ++i) {
		game.Add(0x3000 + i, HOME_PLATE, Seen::kStanding);
	}
	SavedScraps::Ledger ledger;
	Walk                walk{ ledger };
	walk.QueueCell(SANCTUARY);
	walk.QueueCell(RED_ROCKET);
	walk.QueueCell(HOME_PLATE);

	// A budget of nothing looks at nothing, and the call returns.
	CHECK(!walk.WalkSome(game, 0).has_value());
	CHECK(game.examined == 0);
	CHECK(walk.Queued() == 3);

	// Each frame of 64 looks at exactly 64, across the end of one cell into the next, until the last has none to
	// give: 357 references, so five full frames and a short one that ends the round.
	int  frames = 0;
	int  before = 0;
	bool ended = false;
	while (!ended && frames < 20) {
		before = game.examined;
		const auto round = walk.WalkSome(game, 64);
		++frames;
		ended = round.has_value();
		if (!ended) {
			CHECK(game.examined - before == 64);
		} else {
			CHECK(game.examined - before == 357 - 5 * 64);
			CHECK(round->counts.refs == 357);
		}
	}
	CHECK(ended);
	CHECK(frames == 6);
	CHECK(game.examined == 357);
	CHECK(walk.Queued() == 0);

	// Once the queue is dry a call returns having looked at nothing, however large the budget.
	CHECK(!walk.WalkSome(game, SavedScraps::WALK_PER_FRAME).has_value());
	CHECK(!walk.WalkSome(game, (std::numeric_limits<std::size_t>::max)()).has_value());
	CHECK(game.examined == 357);
}

TEST("saved scraps: a budget that lands exactly on the end of a cell ends that cell's walk in the same frame")
{
	FakeGame game;
	for (std::uint32_t i = 0; i < 128; ++i) {
		game.Add(0x1000 + i, SANCTUARY, Seen::kStanding);
	}
	game.Add(0x2000, RED_ROCKET, Seen::kStanding);
	SavedScraps::Ledger ledger;
	Walk                walk{ ledger };
	walk.QueueCell(SANCTUARY);
	walk.QueueCell(RED_ROCKET);

	CHECK(!walk.WalkSome(game, 64).has_value());
	CHECK(game.examined == 64);
	// The second 64 are the last of Sanctuary's: its walk is done this frame, not the next, and Red Rocket's waits.
	CHECK(!walk.WalkSome(game, 64).has_value());
	CHECK(game.examined == 128);
	CHECK(walk.Queued() == 1);
	// Red Rocket's one reference ends the round with room to spare.
	const auto round = walk.WalkSome(game, 64);
	REQUIRE(round.has_value());
	CHECK(game.examined == 129);
	CHECK(round->counts.refs == 129);
	CHECK(walk.Queued() == 0);
}
