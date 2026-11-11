#include "Test.h"
#include "game/SharedWorkshop.h"

#include <algorithm>
#include <array>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The shared workshop (src/game/SharedWorkshop.h): a friend's build or wire at the host's settlement as a numbered request the
// host's game performs, the answer's verdict, the host's checks and finish order, and the friend's grid assertion.
namespace
{
	using namespace SharedWorkshop;
	using namespace std::chrono_literals;
	namespace Op = Protocol::WorkshopRequestOp;

	constexpr std::uint32_t BENCH = 0x000250FE;
	constexpr std::uint32_t PIECE = 0xFF001001;
	constexpr std::uint32_t OTHER = 0xFF001002;
	constexpr std::uint32_t WIRE = 0xFF001003;

	// The engine as Finish drives it: records the steps in order, and whether the workbench is set while they run.
	struct FakeEngine
	{
		std::vector<Step> done;
		bool              benchSet = false;
		bool              benchWasSetForEach = true;
		std::optional<Step> throwAt;

		struct Scope
		{
			FakeEngine& engine;
			explicit Scope(FakeEngine& a_engine) :
				engine(a_engine)
			{
				engine.benchSet = true;
			}
			~Scope() { engine.benchSet = false; }
		};

		Scope Bench() { return Scope{ *this }; }

		void Do(Step a_step)
		{
			benchWasSetForEach = benchWasSetForEach && benchSet;
			if (throwAt && *throwAt == a_step) {
				throw std::runtime_error("the engine threw");
			}
			done.push_back(a_step);
		}
	};
}

TEST("shared workshop: a friend's game hands over under the host's rule in a session, and the host's game serves then; neither otherwise")
{
	CHECK(HandsOver(true, true, false));
	CHECK(!HandsOver(true, true, true));   // the host's pieces are its own already
	CHECK(!HandsOver(false, true, false));  // the rule off: as before
	CHECK(!HandsOver(true, false, false));  // out of a session
	CHECK(Serves(true, true, true));
	CHECK(!Serves(true, true, false));
	CHECK(!Serves(false, true, true));
	CHECK(!Serves(true, false, true));
}

TEST("shared workshop: requests are numbered from 1, answered once by number, expire unanswered, and know their pending pieces")
{
	Requests   requests;
	const auto t0 = Clock::time_point{} + 1h;
	const auto a = requests.Open(Op::kBuild, PIECE, BENCH, t0);
	const auto b = requests.Open(Op::kWire, 0, BENCH, t0 + 1s);
	REQUIRE(a && b);
	CHECK(*a == 1);
	CHECK(*b == 2);
	CHECK(requests.Size() == 2);
	CHECK(requests.IsPending(PIECE));
	CHECK(!requests.IsPending(OTHER));
	CHECK(!requests.IsPending(0));  // a wire's "piece" is none, never pending
	CHECK(requests.Pieces() == std::vector<std::uint32_t>{ PIECE });
	REQUIRE(requests.Builds().size() == 1);  // the builds with their workbenches (a wire has no piece to drop)
	CHECK(requests.Builds()[0].piece == PIECE);
	CHECK(requests.Builds()[0].bench == BENCH);

	const auto answered = requests.Take(1);
	REQUIRE(answered);
	CHECK(answered->op == Op::kBuild);
	CHECK(answered->piece == PIECE);
	CHECK(answered->bench == BENCH);
	CHECK(!requests.Take(1));   // once
	CHECK(!requests.Take(99));  // never asked
	CHECK(!requests.IsPending(PIECE));

	CHECK(requests.Expire(t0 + 1s + ANSWER_WAIT - 1ms).empty());
	const auto late = requests.Expire(t0 + 1s + ANSWER_WAIT);
	REQUIRE(late.size() == 1);
	CHECK(late[0].first == 2);
	CHECK(late[0].second.op == Op::kWire);
	CHECK(requests.Size() == 0);

	// The numbers go on after a clear (a session's answers never match an earlier session's requests).
	requests.Clear();
	CHECK(*requests.Open(Op::kBuild, OTHER, BENCH, t0) == 3);
}

TEST("shared workshop: a build dropped before a save no longer waits, and its late answer is still matched")
{
	Requests   requests;
	const auto t0 = Clock::time_point{} + 1h;
	const auto build = requests.Open(Op::kBuild, PIECE, BENCH, t0);
	const auto wire = requests.Open(Op::kWire, 0, BENCH, t0);
	REQUIRE(build && wire);
	CHECK(requests.Waiting() == 2);
	CHECK(requests.Dropped() == 0);

	const auto dropped = requests.DropBuilds();
	REQUIRE(dropped.size() == 1);  // the build's piece goes; the wire had none
	CHECK(dropped[0].piece == PIECE);
	CHECK(dropped[0].bench == BENCH);
	CHECK(dropped[0].dropped);
	CHECK(requests.DropBuilds().empty());  // once
	CHECK(requests.Waiting() == 1);        // the wire still waits
	CHECK(requests.Dropped() == 1);
	CHECK(requests.Size() == 2);           // both still open
	CHECK(!requests.IsPending(PIECE));     // gone: nothing of ours waits on it
	CHECK(requests.Pieces().empty());
	CHECK(requests.Builds().empty());      // not dropped twice (leaving, another save)

	const auto answered = requests.Take(*build);  // the host built it after all: the answer is ours, matched
	REQUIRE(answered);
	CHECK(answered->dropped);
	CHECK(answered->piece == PIECE);
	CHECK(requests.Dropped() == 0);
	CHECK(requests.Waiting() == 1);

	// a dropped build that is never answered expires as any other
	requests.Open(Op::kBuild, OTHER, BENCH, t0);
	CHECK(requests.DropBuilds().size() == 1);
	const auto late = requests.Expire(t0 + ANSWER_WAIT);
	CHECK(late.size() == 2);
	CHECK(requests.Size() == 0);
	CHECK(requests.Waiting() == 0);
}

TEST("shared workshop: no more than MAX_PENDING requests wait at once")
{
	Requests requests;
	for (std::size_t i = 0; i < MAX_PENDING; ++i) {
		CHECK(requests.Open(Op::kBuild, 0xFF002000 + static_cast<std::uint32_t>(i), BENCH, {}));
	}
	CHECK(!requests.Open(Op::kBuild, PIECE, BENCH, {}));
	CHECK(requests.Take(1));
	CHECK(requests.Open(Op::kBuild, PIECE, BENCH, {}));
}

TEST("shared workshop: the answer's verdict: a build applied is adopted, refused is kept as our own (and a settlement not the host's is remembered); a wire is nothing either way, with its notice")
{
	const auto built = Judge(Op::kBuild, Why::kApplied);
	CHECK(built.outcome == Outcome::kAdopt);
	CHECK(!built.benchNotOwned);
	CHECK(std::string{ built.notice } == "workshop.shared.built");

	const auto notOwned = Judge(Op::kBuild, Why::kNotHostOwned);
	CHECK(notOwned.outcome == Outcome::kKeepOwn);
	CHECK(notOwned.benchNotOwned);
	CHECK(std::string{ notOwned.notice } == "workshop.shared.not_owned");

	for (const auto why : { Why::kBenchNotLoaded, Why::kHostBusy, Why::kHostAway, Why::kOff, Why::kNotWorkshopItem, Why::kOverBudget, Why::kOutOfReach, Why::kFailed, Why::kNoAnswer, Why::kBenchUnopened }) {
		const auto verdict = Judge(Op::kBuild, why);
		CHECK(verdict.outcome == Outcome::kKeepOwn);
		CHECK(!verdict.benchNotOwned);
		CHECK(verdict.notice != nullptr);
	}
	CHECK(std::string{ Judge(Op::kBuild, Why::kBenchNotLoaded).notice } == "workshop.shared.host_away");
	CHECK(std::string{ Judge(Op::kBuild, Why::kHostBusy).notice } == "workshop.shared.host_busy");
	CHECK(std::string{ Judge(Op::kBuild, Why::kHostAway).notice } == "workshop.shared.no_host");
	CHECK(std::string{ Judge(Op::kBuild, Why::kFailed).notice } == "workshop.shared.refused");
	CHECK(std::string{ Judge(Op::kBuild, Why::kNoAnswer).notice } == "workshop.shared.no_answer");  // no answer in time: not "no host"
	CHECK(std::string{ Judge(Op::kBuild, Why::kBenchUnopened).notice } == "workshop.shared.bench_unopened");

	const auto wired = Judge(Op::kWire, Why::kApplied);
	CHECK(wired.outcome == Outcome::kNothing);
	CHECK(std::string{ wired.notice } == "workshop.shared.wired");
	const auto noPiece = Judge(Op::kWire, Why::kNoSuchPiece);
	CHECK(noPiece.outcome == Outcome::kNothing);
	CHECK(!noPiece.benchNotOwned);
	CHECK(std::string{ noPiece.notice } == "workshop.shared.wire_no_piece");
	const auto wireNotOwned = Judge(Op::kWire, Why::kNotHostOwned);
	CHECK(wireNotOwned.outcome == Outcome::kNothing);
	CHECK(wireNotOwned.benchNotOwned);
	CHECK(std::string{ wireNotOwned.notice } == "workshop.shared.wire_refused");
}

TEST("shared workshop: the workbenches the answers taught: refused as not the host's, or the host's; the newest word stands")
{
	Benches benches;
	CHECK(!benches.IsNotOwned(BENCH));
	CHECK(!benches.IsOwned(BENCH));
	benches.NotOwned(BENCH);
	CHECK(benches.IsNotOwned(BENCH));
	benches.Owned(BENCH);
	CHECK(!benches.IsNotOwned(BENCH));
	CHECK(benches.IsOwned(BENCH));
	CHECK(benches.Size() == 1);
	benches.Clear();
	CHECK(benches.Size() == 0);
	CHECK(!benches.IsOwned(BENCH));
}

TEST("shared workshop: a wire is ours with both ends ours, a request with both ends one builder's copies, and mixed otherwise")
{
	CHECK(KindOfWire(false, false, false) == WireKind::kOwn);
	CHECK(KindOfWire(true, true, true) == WireKind::kRequest);
	CHECK(KindOfWire(true, true, false) == WireKind::kMixed);  // two friends' copies: no one game holds both
	CHECK(KindOfWire(true, false, false) == WireKind::kMixed);
	CHECK(KindOfWire(false, true, false) == WireKind::kMixed);
}

TEST("shared workshop: the host's game refuses in the order it looks: the settlement, the host being there, then the object or the wire's ends")
{
	Facts facts;
	CHECK(Refusal(Op::kBuild, facts) == Why::kNotHostOwned);
	facts.benchKnown = true;
	CHECK(Refusal(Op::kBuild, facts) == Why::kNotHostOwned);
	facts.hostOwned = true;
	CHECK(Refusal(Op::kBuild, facts) == Why::kBenchNotLoaded);
	facts.benchLoaded = true;
	CHECK(Refusal(Op::kBuild, facts) == Why::kBenchUnopened);  // no workshop data yet: the host opens the workshop once
	CHECK(Refusal(Op::kWire, facts) == Why::kBenchUnopened);
	facts.benchOpened = true;
	CHECK(Refusal(Op::kBuild, facts) == Why::kNotWorkshopItem);
	facts.objectKnown = true;
	CHECK(Refusal(Op::kBuild, facts) == Why::kNotWorkshopItem);
	facts.buildable = true;
	CHECK(Refusal(Op::kBuild, facts) == Why::kOutOfReach);
	facts.inReach = true;
	CHECK(!Refusal(Op::kBuild, facts));

	CHECK(Refusal(Op::kWire, facts) == Why::kNoSuchPiece);
	facts.endsKnown = true;
	CHECK(Refusal(Op::kWire, facts) == Why::kNoSuchPiece);
	facts.endsAtBench = true;
	CHECK(!Refusal(Op::kWire, facts));
	facts.hostOwned = false;
	CHECK(Refusal(Op::kWire, facts) == Why::kNotHostOwned);
}

TEST("shared workshop: a piece is finished once its 3D is there (nothing of the host's workshop mode holds it off); one whose 3D never comes is refused")
{
	CHECK(Ready(true, 0s) == Readiness::kReady);
	CHECK(Ready(true, MODEL_WAIT) == Readiness::kReady);
	CHECK(Ready(false, 0s) == Readiness::kWait);
	CHECK(Ready(false, MODEL_WAIT - 1ms) == Readiness::kWait);
	CHECK(Ready(false, MODEL_WAIT) == Readiness::kNoModel);
}

TEST("shared workshop: the finish runs the engine's eleven placement steps in the engine's order, the workbench set for every one, and restored even when a step throws")
{
	FakeEngine engine;
	CHECK(Finish(engine) == 11);
	CHECK(engine.done == std::vector<Step>(FINISH_STEPS.begin(), FINISH_STEPS.end()));
	CHECK(engine.benchWasSetForEach);
	CHECK(!engine.benchSet);
	// The order the engine's own Workshop::PlaceCurrentReference has (docs/DESIGN.md "Shared workshop"): linked first (the
	// navmesh and the wireless steps ask IsWorkshopItem), the event last (Papyrus and our report see a finished piece).
	CHECK(FINISH_STEPS.front() == Step::kLink);
	CHECK(FINISH_STEPS.back() == Step::kEvent);
	CHECK(std::ranges::find(FINISH_STEPS, Step::kStatic) < std::ranges::find(FINISH_STEPS, Step::kSnap));
	CHECK(std::ranges::find(FINISH_STEPS, Step::kSnap) < std::ranges::find(FINISH_STEPS, Step::kAddItem));
	CHECK(std::ranges::find(FINISH_STEPS, Step::kPower) < std::ranges::find(FINISH_STEPS, Step::kResources));
	for (const auto step : FINISH_STEPS) {
		CHECK(std::string{ Name(step) } != "?");
	}

	FakeEngine throwing;
	throwing.throwAt = Step::kSnap;
	bool threw = false;
	try {
		Finish(throwing);
	} catch (const std::runtime_error&) {
		threw = true;
	}
	CHECK(threw);
	CHECK(throwing.done.size() == 4);
	CHECK(!throwing.benchSet);
}

namespace
{
	// The host's grid as StringWire drives it, outside workshop mode (1.11.240): the connection merges the generator's grid
	// with the light's and evaluates it (the light powered). UpdateMovingWirelessItem on a wired end no radiator reaches
	// (kEndWireless) takes it out of the powered set and fires its PowerOff; the wired edge stays. The engine makes that call
	// too, after its own connection, but in workshop mode, where a wired light still ends up lit; nothing powers it again on
	// this path, so the light stays dark (seen on the rig). The fake encodes that reading: it guards the order, not the engine.
	struct FakeWireEngine
	{
		std::vector<WireStep> done;
		bool                  benchSet = false;
		bool                  benchWasSetForEach = true;
		bool                  splined = false;
		bool                  connected = false;
		bool                  lightPowered = false;

		struct Scope
		{
			FakeWireEngine& engine;
			explicit Scope(FakeWireEngine& a_engine) :
				engine(a_engine)
			{
				engine.benchSet = true;
			}
			~Scope() { engine.benchSet = false; }
		};

		Scope Bench() { return Scope{ *this }; }

		void Do(WireStep a_step)
		{
			benchWasSetForEach = benchWasSetForEach && benchSet;
			done.push_back(a_step);
			switch (a_step) {
			case WireStep::kSpline:
				splined = true;
				break;
			case WireStep::kConnect:
				connected = splined;
				lightPowered = connected;
				break;
			case WireStep::kEndWireless:
				lightPowered = false;  // out of the powered set, PowerOff fired; the edge stays
				break;
			default:
				break;
			}
		}

		// The steps in this order, as StringWire runs WIRE_STEPS.
		void Run(std::span<const WireStep> a_steps)
		{
			[[maybe_unused]] const auto scope = Bench();
			for (const auto step : a_steps) {
				Do(step);
			}
		}
	};
}

TEST("shared workshop: the host strings a friend's wire with the connection last to decide its ends' power, so the light it wires stays powered")
{
	FakeWireEngine engine;
	CHECK(StringWire(engine) == WIRE_STEPS.size());
	CHECK(engine.done == std::vector<WireStep>(WIRE_STEPS.begin(), WIRE_STEPS.end()));
	CHECK(engine.benchWasSetForEach);
	CHECK(!engine.benchSet);
	CHECK(engine.connected);
	CHECK(engine.lightPowered);
	CHECK(KeepsEndPower(WIRE_STEPS));
	CHECK(std::ranges::find(WIRE_STEPS, WireStep::kEndWireless) == WIRE_STEPS.end());
	CHECK(WIRE_STEPS.front() == WireStep::kLink);
	CHECK(WIRE_STEPS.back() == WireStep::kEvent);
	for (const auto step : WIRE_STEPS) {
		CHECK(std::string{ Name(step) } != "?");
		CHECK(!DecidesEndPower(step));
	}

	// The first version's order, the engine's own in workshop mode: both ends' wireless state updated after the connection.
	// Outside workshop mode nothing powers the light again: it went dark on the rig, with its edge still in the grid. Don't
	// add kEndWireless back to WIRE_STEPS for parity with the engine.
	constexpr std::array<WireStep, 10> FIRST{ WireStep::kLink, WireStep::kSpline, WireStep::kConnect, WireStep::kCount, WireStep::kRebuild, WireStep::kStatic,
		WireStep::kEndWireless, WireStep::kEndWireless, WireStep::kResources, WireStep::kEvent };
	CHECK(!KeepsEndPower(FIRST));
	FakeWireEngine first;
	first.Run(FIRST);
	CHECK(!first.lightPowered);
	CHECK(first.connected);  // the wired edge stays; only the light's power is lost

	// Other orders that can't keep it: no connection, the connection before the spline or twice, the totals or the event
	// before the connection.
	constexpr std::array<WireStep, 3> UNCONNECTED{ WireStep::kLink, WireStep::kSpline, WireStep::kEvent };
	constexpr std::array<WireStep, 3> BEFORE_SPLINE{ WireStep::kLink, WireStep::kConnect, WireStep::kSpline };
	constexpr std::array<WireStep, 3> TWICE{ WireStep::kSpline, WireStep::kConnect, WireStep::kConnect };
	constexpr std::array<WireStep, 3> EVENT_FIRST{ WireStep::kSpline, WireStep::kEvent, WireStep::kConnect };
	constexpr std::array<WireStep, 3> TOTALS_FIRST{ WireStep::kSpline, WireStep::kResources, WireStep::kConnect };
	CHECK(!KeepsEndPower(UNCONNECTED));
	CHECK(!KeepsEndPower(BEFORE_SPLINE));
	CHECK(!KeepsEndPower(TWICE));
	CHECK(!KeepsEndPower(EVENT_FIRST));
	CHECK(!KeepsEndPower(TOTALS_FIRST));
	// An end's wireless state updated before the connection is undone by it (the connection decides last): kept.
	constexpr std::array<WireStep, 3> WIRELESS_FIRST{ WireStep::kSpline, WireStep::kEndWireless, WireStep::kConnect };
	CHECK(KeepsEndPower(WIRELESS_FIRST));
}

TEST("shared workshop: the budget's totals are live only with workshop mode open at the very bench the engine holds current; a bench our own scope set there, another bench, or no menu loads them")
{
	constexpr std::uint32_t HANDLE = 0x00A01234;
	constexpr std::uint32_t OTHER_HANDLE = 0x00A05678;
	CHECK(BudgetLive(HANDLE, HANDLE, true));
	CHECK(!BudgetLive(HANDLE, HANDLE, false));      // the menu closed: the statics are whatever was left
	CHECK(!BudgetLive(OTHER_HANDLE, HANDLE, true));  // workshop mode at another settlement: its totals
	CHECK(!BudgetLive(0, HANDLE, true));             // the engine holds none (not in workshop mode, whatever the menu says)
	CHECK(!BudgetLive(0, HANDLE, false));            // out of workshop mode after a load: 0s
	// What our own BenchScope sets as current must not count as the engine's: the decision takes the engine's value from
	// before the scope, so a finish inside the scope still loads the bench's totals.
	CHECK(!BudgetLive(0, HANDLE, true));
}

TEST("shared workshop: the friend's grids may name none of the in-session pieces adopted or taken off")
{
	WorkshopPower::Grid grid;
	grid.nodes = { PIECE, 0xFF000500 };
	grid.edges = { { PIECE, OTHER, WIRE } };
	grid.powered = { 0xFF000500 };
	const std::vector<WorkshopPower::Grid> grids{ grid };
	const std::set<std::uint32_t>          none;
	const std::set<std::uint32_t>          elsewhere{ 0xFF000600 };
	const std::set<std::uint32_t>          three{ WIRE, PIECE, OTHER };
	const std::set<std::uint32_t>          wireOnly{ WIRE };
	const std::vector<std::uint32_t>       sorted{ PIECE, OTHER, WIRE };
	const std::vector<std::uint32_t>       justWire{ WIRE };
	CHECK(Strays(grids, none).empty());
	CHECK(Strays(grids, elsewhere).empty());
	CHECK(Strays(grids, three) == sorted);
	CHECK(Strays(grids, wireOnly) == justWire);  // a deleted wire's id still on a grid
}

TEST("shared workshop: a held wire's status is named as the engine's spline status sets it")
{
	CHECK(std::string_view{ WireStatusName(1) } == "ok");
	CHECK(std::string_view{ WireStatusName(2) } == "no-end");
	CHECK(std::string_view{ WireStatusName(3) } == "blocked");
	CHECK(std::string_view{ WireStatusName(6) } == "too-long");
	CHECK(std::string_view{ WireStatusName(8) } == "already-wired");
	CHECK(std::string_view{ WireStatusName(0) } == "other");  // never set by it: no wire held, or another piece's verdict
	CHECK(std::string_view{ WireStatusName(4) } == "other");
}

TEST("shared workshop: the engine's scrap test refuses in its own order, and refuses every wire for want of a recipe")
{
	// A wire (BlackWireSpline01, a BendableSpline): a base, nothing one carries, and no recipe makes it, in any game.
	constexpr ScrapFacts wire{ .base = true };
	CHECK(ScrapRefusal(wire, false) == ScrapFail::kNoRecipe);
	CHECK(ScrapRefusal(wire, true) == ScrapFail::kNoRecipe);
	CHECK(std::string_view{ ScrapFailName(ScrapRefusal(wire, false)) } == "recipe");

	// A built piece: its base marked by the recipe that makes it.
	constexpr ScrapFacts built{ .base = true, .recipe = true };
	CHECK(ScrapRefusal(built, false) == ScrapFail::kNone);
	CHECK(ScrapRefusal(ScrapFacts{}, false) == ScrapFail::kNoBase);
	CHECK(ScrapRefusal(ScrapFacts{ .base = true, .blockKeyword = true, .recipe = true }, false) == ScrapFail::kBlockKeyword);

	// A thing one carries: never with the flag; without it, only an armor, misc item or weapon, then by recipe, components or
	// an object instance.
	constexpr ScrapFacts junk{ .base = true, .inventoryType = true, .loose = true, .miscComponents = true };
	CHECK(ScrapRefusal(junk, true) == ScrapFail::kLoose);
	CHECK(ScrapRefusal(junk, false) == ScrapFail::kNone);
	CHECK(ScrapRefusal(ScrapFacts{ .base = true, .loose = true, .recipe = true }, false) == ScrapFail::kLooseNotItem);
	CHECK(ScrapRefusal(ScrapFacts{ .base = true, .inventoryType = true, .loose = true, .objectInstance = true }, false) == ScrapFail::kNone);
	CHECK(ScrapRefusal(ScrapFacts{ .base = true, .inventoryType = true, .loose = true }, false) == ScrapFail::kNoRecipe);
	CHECK(ScrapRefusal(ScrapFacts{ .base = true, .objectInstance = true }, false) == ScrapFail::kNoRecipe);  // not loose
}
