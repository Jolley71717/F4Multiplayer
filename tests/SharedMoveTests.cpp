#include "Test.h"
#include "game/SharedMove.h"

#include <algorithm>
#include <array>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

// A friend's move of the host's piece (src/game/SharedMove.h): the grab key on a copy, the hold followed to its drop, what a
// drop asks, the answer's verdict, the host's checks and the order of its move.
namespace
{
	using namespace SharedMove;
	using namespace std::chrono_literals;
	namespace Op = Protocol::WorkshopRequestOp;

	constexpr std::uint32_t BENCH = 0x000250FE;
	constexpr std::uint32_t COPY = 0xFF001001;
	constexpr std::uint32_t OTHER = 0xFF001002;

	constexpr Transform HERE{ { 100.0f, 200.0f, 300.0f }, { 0.0f, 0.0f, 1.0f }, 1.0f };
	constexpr Transform THERE{ { 164.0f, 200.0f, 300.0f }, { 0.0f, 0.0f, 1.0f }, 1.0f };
	constexpr Transform TURNED{ { 100.0f, 200.0f, 300.0f }, { 0.0f, 0.0f, 1.5f }, 1.0f };

	// A copy the grab key admits.
	constexpr GrabFacts Admissible()
	{
		GrabFacts facts;
		facts.hostsCopy = true;
		facts.grabOn = true;
		facts.editAllowed = true;
		return facts;
	}

	// A move the host's game makes.
	constexpr SharedWorkshop::Facts Servable()
	{
		SharedWorkshop::Facts facts;
		facts.benchKnown = true;
		facts.hostOwned = true;
		facts.benchLoaded = true;
		facts.benchOpened = true;
		facts.editAllowed = true;
		facts.targetKnown = true;
		facts.targetAtBench = true;
		facts.targetLoaded = true;
		return facts;
	}

	constexpr MoveFacts Movable()
	{
		MoveFacts move;
		move.inArea = true;
		move.inReach = true;
		move.sameCell = true;
		return move;
	}

	// The engine as Move drives it: records the steps in order, and whether the workbench and the moving byte are set while
	// they run.
	struct FakeEngine
	{
		std::vector<MoveStep>   done;
		bool                    benchSet = false;
		bool                    movingSet = false;
		bool                    bothSetForEach = true;
		std::optional<MoveStep> throwAt;

		struct Scope
		{
			bool& flag;
			explicit Scope(bool& a_flag) :
				flag(a_flag)
			{
				flag = true;
			}
			Scope(const Scope&) = delete;
			Scope& operator=(const Scope&) = delete;
			~Scope() { flag = false; }
		};

		Scope Bench() { return Scope{ benchSet }; }
		Scope Moving() { return Scope{ movingSet }; }

		void Do(MoveStep a_step)
		{
			bothSetForEach = bothSetForEach && benchSet && movingSet;
			if (throwAt && *throwAt == a_step) {
				throw std::runtime_error("the engine threw");
			}
			done.push_back(a_step);
		}
	};
}

TEST("shared move: the grab key is the engine's own on anything but the host's copy, whatever else is set")
{
	GrabFacts facts;
	CHECK(OnGrabKey(facts) == GrabKey::kEngine);
	facts.grabOn = true;
	facts.editAllowed = true;
	facts.power = true;
	facts.stacked = true;
	facts.pending = true;
	facts.holding = true;
	CHECK(OnGrabKey(facts) == GrabKey::kEngine);
	CHECK(Notice(GrabKey::kEngine) == nullptr);
}

TEST("shared move: with the switch off a copy's grab is refused as it was before the move, whatever the piece")
{
	auto facts = Admissible();
	facts.grabOn = false;
	CHECK(OnGrabKey(facts) == GrabKey::kRefuseOff);
	facts.editAllowed = false;
	facts.power = true;
	CHECK(OnGrabKey(facts) == GrabKey::kRefuseOff);
	CHECK(std::string{ Notice(GrabKey::kRefuseOff) } == "workshop.shared.no_grab");
}

TEST("shared move: a copy is picked up only when the host allows edits and the piece is one the host's game would move")
{
	CHECK(OnGrabKey(Admissible()) == GrabKey::kAdmit);
	CHECK(Notice(GrabKey::kAdmit) == nullptr);

	auto facts = Admissible();
	facts.editAllowed = false;
	CHECK(OnGrabKey(facts) == GrabKey::kRefuseEditOff);
	CHECK(std::string{ Notice(GrabKey::kRefuseEditOff) } == "workshop.shared.move_off");

	facts = Admissible();
	facts.wire = true;
	CHECK(OnGrabKey(facts) == GrabKey::kRefuseKind);
	facts = Admissible();
	facts.actor = true;
	CHECK(OnGrabKey(facts) == GrabKey::kRefuseKind);
	// A damaged piece: the engine's Accept on it is its repair, never a grab (and never the engine's repair of a copy).
	facts = Admissible();
	facts.damaged = true;
	CHECK(OnGrabKey(facts) == GrabKey::kRefuseKind);
	CHECK(std::string{ Notice(GrabKey::kRefuseKind) } == "workshop.shared.no_grab");

	// A power piece never leaves the friend's hands: the engine's grab would take it off grids and wires here too.
	facts = Admissible();
	facts.power = true;
	CHECK(OnGrabKey(facts) == GrabKey::kRefusePower);
	facts = Admissible();
	facts.tied = true;
	CHECK(OnGrabKey(facts) == GrabKey::kRefusePower);
	CHECK(std::string{ Notice(GrabKey::kRefusePower) } == "workshop.shared.move_wired");

	facts = Admissible();
	facts.stacked = true;
	CHECK(OnGrabKey(facts) == GrabKey::kRefuseStacked);
	CHECK(std::string{ Notice(GrabKey::kRefuseStacked) } == "workshop.shared.move_stacked");

	facts = Admissible();
	facts.pending = true;
	CHECK(OnGrabKey(facts) == GrabKey::kRefusePending);
	CHECK(Notice(GrabKey::kRefusePending) == nullptr);  // the caller names what is asked
	CHECK(std::string{ PENDING_NOTICE } == "workshop.shared.move_pending");

	facts = Admissible();
	facts.holding = true;
	CHECK(OnGrabKey(facts) == GrabKey::kRefuseHolding);
	CHECK(std::string{ Notice(GrabKey::kRefuseHolding) } == "workshop.shared.no_grab");
}

TEST("shared move: the grab key's refusals come in a fixed order: the host's setting before the piece's kind, power before what stands on it")
{
	auto facts = Admissible();
	facts.editAllowed = false;
	facts.wire = true;
	facts.power = true;
	facts.stacked = true;
	facts.pending = true;
	CHECK(OnGrabKey(facts) == GrabKey::kRefuseEditOff);
	facts.editAllowed = true;
	CHECK(OnGrabKey(facts) == GrabKey::kRefuseKind);
	facts.wire = false;
	CHECK(OnGrabKey(facts) == GrabKey::kRefusePower);
	facts.power = false;
	CHECK(OnGrabKey(facts) == GrabKey::kRefuseStacked);
	facts.stacked = false;
	CHECK(OnGrabKey(facts) == GrabKey::kRefusePending);
	facts.pending = false;
	CHECK(OnGrabKey(facts) == GrabKey::kAdmit);
}

TEST("shared move: a power piece is one that makes, radiates, needs or passes power, or that a wire can end on")
{
	CHECK(!IsPowerPiece(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, false));  // a wall, a chair, a bed
	CHECK(IsPowerPiece(3.0f, 0.0f, 0.0f, 0.0f, 0.0f, false));   // a generator
	CHECK(IsPowerPiece(0.0f, 500.0f, 0.0f, 0.0f, 0.0f, false)); // a pylon's radius
	CHECK(IsPowerPiece(0.0f, 0.0f, 1.0f, 0.0f, 0.0f, false));   // a light
	CHECK(IsPowerPiece(0.0f, 0.0f, 0.0f, 1.0f, 0.0f, false));   // a conduit's snap
	CHECK(IsPowerPiece(0.0f, 0.0f, 0.0f, 0.0f, 1.0f, false));   // a connector
	CHECK(IsPowerPiece(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, true));    // the keyword alone
}

TEST("shared move: a grab is followed from the key to the engine's hold to its letting go")
{
	Grab       grab;
	const auto t0 = Clock::time_point{} + 1h;
	CHECK(grab.Copy() == 0);
	CHECK(!grab.Holds(COPY));
	CHECK(grab.Frame(false, t0).event == Grab::Event::kNothing);

	REQUIRE(grab.Admit(COPY, BENCH, HERE, t0));
	CHECK(grab.Holds(COPY));
	CHECK(!grab.Holds(OTHER));
	CHECK(!grab.Holds(0));
	CHECK(!grab.Taken());

	// The engine holds it from the next frame.
	CHECK(grab.Frame(false, t0 + 10ms).event == Grab::Event::kNothing);
	const auto taken = grab.Frame(true, t0 + 20ms);
	CHECK(taken.event == Grab::Event::kTaken);
	CHECK(taken.copy == COPY);
	CHECK(taken.bench == BENCH);
	CHECK(grab.Taken());

	// Held for as long as the player likes: no deadline once taken.
	CHECK(grab.Frame(true, t0 + 10min).event == Grab::Event::kNothing);
	CHECK(grab.Holds(COPY));

	const auto let = grab.Frame(false, t0 + 10min + 16ms);
	CHECK(let.event == Grab::Event::kLetGo);
	CHECK(let.copy == COPY);
	CHECK(let.bench == BENCH);
	CHECK(WorkshopMove::Same(let.from, HERE));
	CHECK(!grab.Holds(COPY));
	CHECK(grab.Copy() == 0);
	CHECK(grab.Frame(false, t0 + 11min).event == Grab::Event::kNothing);
}

TEST("shared move: a grab the engine never took (its own repair ran instead) is forgotten after a second, with nothing to do")
{
	Grab       grab;
	const auto t0 = Clock::time_point{} + 1h;
	REQUIRE(grab.Admit(COPY, BENCH, HERE, t0));
	CHECK(grab.Frame(false, t0 + 999ms).event == Grab::Event::kNothing);
	const auto never = grab.Frame(false, t0 + GRAB_WAIT);
	CHECK(never.event == Grab::Event::kNeverTaken);
	CHECK(never.copy == COPY);
	CHECK(!grab.Holds(COPY));
	// A hold that shows after that is not ours to follow.
	CHECK(grab.Frame(true, t0 + 2s).event == Grab::Event::kNothing);
}

TEST("shared move: one grab at a time: a second key replaces one not yet taken, and is refused while one is held")
{
	Grab       grab;
	const auto t0 = Clock::time_point{} + 1h;
	CHECK(!grab.Admit(0, BENCH, HERE, t0));
	REQUIRE(grab.Admit(COPY, BENCH, HERE, t0));
	REQUIRE(grab.Admit(OTHER, BENCH, THERE, t0 + 100ms));  // the engine didn't take the first: the second key is the one followed
	CHECK(grab.Holds(OTHER));
	CHECK(!grab.Holds(COPY));
	CHECK(grab.Frame(true, t0 + 120ms).event == Grab::Event::kTaken);
	CHECK(!grab.Admit(COPY, BENCH, HERE, t0 + 200ms));
	CHECK(grab.Holds(OTHER));
	const auto let = grab.Frame(false, t0 + 1s);
	CHECK(let.event == Grab::Event::kLetGo);
	CHECK(let.copy == OTHER);
	CHECK(WorkshopMove::Same(let.from, THERE));
	CHECK(grab.Admit(COPY, BENCH, HERE, t0 + 2s));
}

TEST("shared move: clearing a grab says what was followed, so a held copy is still cleaned up")
{
	Grab       grab;
	const auto t0 = Clock::time_point{} + 1h;
	CHECK(grab.Clear().event == Grab::Event::kNothing);

	REQUIRE(grab.Admit(COPY, BENCH, HERE, t0));
	const auto untaken = grab.Clear();
	CHECK(untaken.event == Grab::Event::kNeverTaken);
	CHECK(untaken.copy == COPY);
	CHECK(grab.Copy() == 0);

	REQUIRE(grab.Admit(COPY, BENCH, HERE, t0));
	CHECK(grab.Frame(true, t0 + 16ms).event == Grab::Event::kTaken);
	const auto held = grab.Clear();
	CHECK(held.event == Grab::Event::kLetGo);
	CHECK(held.copy == COPY);
	CHECK(held.bench == BENCH);
	CHECK(!grab.Taken());
	CHECK(grab.Clear().event == Grab::Event::kNothing);
}

TEST("shared move: a cancelled grab sends nothing; a drop elsewhere is asked of the host, or put back when nothing can be asked")
{
	// Cancelled (the engine puts the piece back), or put down where it stood.
	CHECK(OnDrop(HERE, HERE, true) == Drop::kLeave);
	CHECK(OnDrop(HERE, HERE, false) == Drop::kLeave);
	Transform nudged = HERE;
	nudged.position[0] += 0.25f;  // within the same-place tolerance
	CHECK(OnDrop(HERE, nudged, true) == Drop::kLeave);

	CHECK(OnDrop(HERE, THERE, true) == Drop::kAsk);
	CHECK(OnDrop(HERE, TURNED, true) == Drop::kAsk);  // turned in place is a move too
	CHECK(OnDrop(HERE, THERE, false) == Drop::kPutBack);
	CHECK(OnDrop(HERE, TURNED, false) == Drop::kPutBack);
}

TEST("shared move: an applied move leaves the copy to the host's report; a refused or unanswered one puts it back with a notice")
{
	const auto applied = JudgeMove(Why::kApplied);
	CHECK(!applied.putBack);
	CHECK(!applied.benchNotOwned);
	CHECK(applied.notice == nullptr);

	const auto expect = [](Why a_why, const char* a_notice, bool a_notOwned = false) {
		const auto verdict = JudgeMove(a_why);
		CHECK(verdict.putBack);
		CHECK(verdict.benchNotOwned == a_notOwned);
		REQUIRE(verdict.notice != nullptr);
		CHECK(std::string{ verdict.notice } == a_notice);
	};
	expect(Why::kNotAllowed, "workshop.shared.move_off");
	expect(Why::kPieceWired, "workshop.shared.move_wired");
	expect(Why::kPieceStacked, "workshop.shared.move_stacked");
	expect(Why::kHostBusy, "workshop.shared.piece_busy");
	expect(Why::kPieceBusy, "workshop.shared.piece_busy");
	expect(Why::kOutOfReach, "workshop.shared.move_out_of_reach");
	expect(Why::kNoSuchPiece, "workshop.shared.move_no_piece");
	expect(Why::kNotHostOwned, "workshop.shared.move_refused", true);
	expect(Why::kBenchNotLoaded, "workshop.shared.move_refused");
	expect(Why::kBenchUnopened, "workshop.shared.move_refused");
	expect(Why::kNotWorkshopItem, "workshop.shared.move_refused");
	expect(Why::kFailed, "workshop.shared.move_refused");
	expect(Why::kNoAnswer, "workshop.shared.move_refused");

	// Every reason the protocol has puts the copy back, but applied.
	for (std::uint8_t why = 0; why <= static_cast<std::uint8_t>(Protocol::WorkshopAnswer::LAST_WHY); ++why) {
		const auto verdict = JudgeMove(static_cast<Why>(why));
		CHECK(verdict.putBack == (static_cast<Why>(why) != Why::kApplied));
	}
}

TEST("shared move: a refused move's copy is put back only while it still stands where the friend dropped it")
{
	CHECK(StillWhereDropped(THERE, THERE));
	CHECK(!StillWhereDropped(HERE, THERE));    // it followed a report meanwhile: it is where the host's piece is
	CHECK(!StillWhereDropped(TURNED, HERE));
}

TEST("shared move: the host's game moves a plain piece of its own workbench to a spot inside the build area")
{
	CHECK(!Refusal(Servable(), Movable()));
}

TEST("shared move: the host's game refuses a move at a workbench that isn't its own, loaded, opened, or with edits off")
{
	auto facts = Servable();
	facts.benchKnown = false;
	CHECK(Refusal(facts, Movable()) == Why::kNotHostOwned);
	facts = Servable();
	facts.hostOwned = false;
	CHECK(Refusal(facts, Movable()) == Why::kNotHostOwned);
	facts = Servable();
	facts.benchLoaded = false;
	CHECK(Refusal(facts, Movable()) == Why::kBenchNotLoaded);
	facts = Servable();
	facts.benchOpened = false;
	CHECK(Refusal(facts, Movable()) == Why::kBenchUnopened);
	facts = Servable();
	facts.editAllowed = false;
	CHECK(Refusal(facts, Movable()) == Why::kNotAllowed);
	// The setting off wins over anything about the piece: the friend hears the one thing it can't change.
	auto move = Movable();
	move.power = true;
	move.stacked = true;
	CHECK(Refusal(facts, move) == Why::kNotAllowed);
}

TEST("shared move: the host's game refuses a move while its own workshop mode holds anything or is open elsewhere")
{
	auto facts = Servable();
	facts.otherBenchOpen = true;
	CHECK(Refusal(facts, Movable()) == Why::kHostBusy);
	auto move = Movable();
	move.hostHolds = true;
	CHECK(Refusal(Servable(), move) == Why::kHostBusy);
}

TEST("shared move: the host's game refuses a piece it doesn't know, an actor, one whose 3D isn't in, and one it has picked or holds")
{
	auto facts = Servable();
	facts.targetKnown = false;
	CHECK(Refusal(facts, Movable()) == Why::kNoSuchPiece);
	facts = Servable();
	facts.targetAtBench = false;  // a copy, the workbench, a wire, another workbench's piece
	CHECK(Refusal(facts, Movable()) == Why::kNoSuchPiece);

	auto move = Movable();
	move.actor = true;
	CHECK(Refusal(Servable(), move) == Why::kNotWorkshopItem);

	facts = Servable();
	facts.targetLoaded = false;
	CHECK(Refusal(facts, Movable()) == Why::kBenchNotLoaded);

	facts = Servable();
	facts.targetHeld = true;
	CHECK(Refusal(facts, Movable()) == Why::kPieceBusy);
	move = Movable();
	move.targetPicked = true;  // under the host's crosshair: the host's own grab would race the move
	CHECK(Refusal(Servable(), move) == Why::kPieceBusy);
}

TEST("shared move: the host's game refuses a power piece and a piece something stands on, rather than run the engine's grid and stack work")
{
	auto move = Movable();
	move.power = true;
	CHECK(Refusal(Servable(), move) == Why::kPieceWired);
	move.stacked = true;
	CHECK(Refusal(Servable(), move) == Why::kPieceWired);  // power first
	move.power = false;
	CHECK(Refusal(Servable(), move) == Why::kPieceStacked);
}

TEST("shared move: the host's game refuses a spot outside the workbench's build area or reach")
{
	auto move = Movable();
	move.inArea = false;
	CHECK(Refusal(Servable(), move) == Why::kOutOfReach);
	move = Movable();
	move.inReach = false;
	CHECK(Refusal(Servable(), move) == Why::kOutOfReach);
	// The piece's own refusals come first: a wired piece is refused as wired wherever it was dropped.
	move.power = true;
	CHECK(Refusal(Servable(), move) == Why::kPieceWired);
}

TEST("shared move: the host's game refuses a move across an exterior cell's border, and none inside an interior")
{
	auto move = Movable();
	move.sameCell = false;
	CHECK(Refusal(Servable(), move) == Why::kOutOfReach);
	// One cell of the grid: 4096 units a side, the border at every multiple (negative coordinates too).
	const Transform inside{ { 4000.0f, 100.0f, 0.0f }, {}, 1.0f };
	const Transform sameCell{ { 100.0f, 4000.0f, 50.0f }, {}, 1.0f };
	const Transform nextEast{ { 4100.0f, 100.0f, 0.0f }, {}, 1.0f };
	const Transform nextNorth{ { 4000.0f, 4100.0f, 0.0f }, {}, 1.0f };
	const Transform west{ { -10.0f, 100.0f, 0.0f }, {}, 1.0f };
	const Transform westToo{ { -4000.0f, 100.0f, 0.0f }, {}, 1.0f };
	CHECK(SameCell(inside, sameCell, false));
	CHECK(!SameCell(inside, nextEast, false));
	CHECK(!SameCell(inside, nextNorth, false));
	CHECK(!SameCell(inside, west, false));  // 0 is a border: -10 and +4000 are two cells
	CHECK(SameCell(west, westToo, false));
	CHECK(SameCell(inside, nextEast, true));  // an interior is one cell
}

TEST("shared move: the host's move runs the engine's drop in order, with the workbench and the moving byte set for every step")
{
	FakeEngine engine;
	CHECK(Move(engine) == MOVE_STEPS.size());
	CHECK(engine.done == std::vector<MoveStep>(MOVE_STEPS.begin(), MOVE_STEPS.end()));
	CHECK(engine.bothSetForEach);
	CHECK(!engine.benchSet);
	CHECK(!engine.movingSet);

	CHECK(InOrder(MOVE_STEPS));
	static_assert(InOrder(MOVE_STEPS));
	CHECK(MOVE_STEPS.front() == MoveStep::kFurnitureTaken);
	CHECK(std::ranges::find(MOVE_STEPS, MoveStep::kTransform) < std::ranges::find(MOVE_STEPS, MoveStep::kStatic));
	CHECK(std::ranges::find(MOVE_STEPS, MoveStep::kStatic) < std::ranges::find(MOVE_STEPS, MoveStep::kMovedEvent));
	CHECK(std::ranges::find(MOVE_STEPS, MoveStep::kMovedEvent) < std::ranges::find(MOVE_STEPS, MoveStep::kResources));
	for (const auto step : MOVE_STEPS) {
		CHECK(std::string{ Name(step) } != "?");
	}

	FakeEngine throwing;
	throwing.throwAt = MoveStep::kStatic;
	bool threw = false;
	try {
		Move(throwing);
	} catch (const std::runtime_error&) {
		threw = true;
	}
	CHECK(threw);
	CHECK(throwing.done.size() == 3);
	CHECK(!throwing.benchSet);
	CHECK(!throwing.movingSet);
}

TEST("shared move: the order guard catches a piece made static before it moved, an event before it stands, and a step twice or missing")
{
	using S = MoveStep;
	const std::array good{ S::kFurnitureTaken, S::kStackLink, S::kTransform, S::kStatic, S::kFurnitureFree, S::kMovedEvent, S::kNavmesh, S::kResources };
	CHECK(InOrder(good));
	const std::array staticFirst{ S::kFurnitureTaken, S::kStackLink, S::kStatic, S::kTransform, S::kFurnitureFree, S::kMovedEvent, S::kNavmesh, S::kResources };
	CHECK(!InOrder(staticFirst));
	const std::array eventEarly{ S::kFurnitureTaken, S::kStackLink, S::kTransform, S::kMovedEvent, S::kStatic, S::kFurnitureFree, S::kNavmesh, S::kResources };
	CHECK(!InOrder(eventEarly));
	const std::array furnitureLate{ S::kStackLink, S::kTransform, S::kFurnitureTaken, S::kStatic, S::kFurnitureFree, S::kMovedEvent, S::kNavmesh, S::kResources };
	CHECK(!InOrder(furnitureLate));
	const std::array twice{ S::kFurnitureTaken, S::kStackLink, S::kTransform, S::kTransform, S::kStatic, S::kFurnitureFree, S::kMovedEvent, S::kNavmesh };
	CHECK(!InOrder(twice));
	const std::array missing{ S::kFurnitureTaken, S::kStackLink, S::kTransform, S::kStatic, S::kMovedEvent, S::kNavmesh, S::kResources };
	CHECK(!InOrder(missing));
	const std::array stackLate{ S::kFurnitureTaken, S::kTransform, S::kStatic, S::kStackLink, S::kFurnitureFree, S::kMovedEvent, S::kNavmesh, S::kResources };
	CHECK(!InOrder(stackLate));
}

TEST("shared move: a request names where the piece goes, with the piece's own size, and the answer says applied only once it stands there")
{
	Protocol::WorkshopRequest request{};
	request.op = Op::kMove;
	request.position[0] = 164.0f;
	request.position[1] = 200.0f;
	request.position[2] = 300.0f;
	request.rotation[2] = 1.0f;
	request.scale = 3.0f;  // never read: a move changes no size
	const auto asked = Asked(request, 1.0f);
	CHECK(WorkshopMove::Same(asked, THERE));
	CHECK(Arrived(THERE, asked));
	CHECK(!Arrived(HERE, asked));
}
