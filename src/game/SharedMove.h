#pragma once

#include "Protocol.h"
#include "game/SharedWorkshop.h"
#include "game/WorkshopMove.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

// A friend's move of the host's piece at a settlement the host owns (the shared workshop, SharedWorkshop.h): the friend's
// workshop mode picks its copy up and puts it down as it would any piece, and where it was put down is asked of the host as
// a numbered request (Protocol::WorkshopRequestOp::kMove); the host's game moves its own piece with the engine's steps, and
// the move reaches every game through the usual placed report. The game-free half (WorkshopSync.cpp runs it against the
// engine, tests/SharedMoveTests.cpp against stand-ins):
//
// - The friend's side. The grab key on a copy (OnGrabKey): admitted only for a piece that has nothing to do with power, no
//   wire, no actor, nothing standing on it, nothing asked of it already, the host allowing edits, and (for now) the dev
//   switch on. The hold is followed frame by frame (Grab): once the engine lets go of the copy (a drop, a cancel, workshop
//   mode closed) the copy is taken off whatever the engine's drop tied it into, and the move is asked only when the copy
//   stands somewhere else than it did (OnDrop): a cancelled grab sends nothing. The answer decides (JudgeMove): applied,
//   nothing more (the host's report puts the copy where the host's piece went); refused, or no answer, the copy goes back
//   to where it stood, with a notice.
// - The host's side. What it refuses (Refusal): anything on or for a power grid (a move takes a piece off its grid and the
//   wires strung to it, and nothing read so far says how a drop outside workshop mode ties them back), a piece something
//   stands on (the engine's own move scraps what can't follow), an actor, a piece the host has under its crosshair, a spot
//   outside the build area or in another cell than the piece stands in. What it does for the rest (MOVE_STEPS, Move): the
//   engine's own drop of a moved piece, minus the grab's grid work (none to undo) and the stacking.
namespace SharedMove
{
	using Clock = SharedWorkshop::Clock;
	using namespace std::chrono_literals;
	using Why = Protocol::WorkshopAnswer::Why;
	using Transform = WorkshopMove::Transform;

	// ---- The grab key on a copy ------------------------------------------------------------------------------

	// What the friend's game knows of the piece under its grab key (the menu's Accept on the highlighted piece).
	struct GrabFacts
	{
		bool hostsCopy = false;    // our copy of the host's piece, at a workbench the host's game hasn't refused (AdmitsCopy)
		bool grabOn = false;       // the dev switch: copies may be picked up at all (off by default until the rig has proven it)
		bool editAllowed = false;  // the host's bSharedWorkshopEdit
		bool wire = false;         // a wire (never moved: its ends are)
		bool actor = false;        // a turret, a robot, a settler
		bool damaged = false;      // a piece the engine would repair at this key (Workshop::ItemNeedsRepair): never the grab
		bool power = false;        // a piece that makes, takes, passes or radiates power, or a wire can end on (its base says so)
		bool tied = false;         // ...or one our own game finds on a grid or with power links (it never should be: refused)
		bool stacked = false;      // something stands on it here (the engine's own move would carry or scrap those)
		bool pending = false;      // something is asked of this copy already (a scrap, a store, a move)
		bool holding = false;      // a grab of a copy is still being followed (its drop not seen yet)
	};

	enum class GrabKey : std::uint8_t
	{
		kEngine,         // not the host's copy: the engine's own test, as before
		kAdmit,          // the host's copy: the engine picks it up, and its drop is asked of the host
		kRefuseOff,      // the host's copy, and copies aren't picked up here (the switch off): refused as before the move
		kRefuseEditOff,  // ...the host's bSharedWorkshopEdit is off
		kRefuseKind,     // ...a wire, an actor, or a damaged piece (the key is its repair): never moved for a friend
		kRefusePower,    // ...a power piece: the host's game refuses it, so the friend's never picks it up
		kRefuseStacked,  // ...something stands on it
		kRefusePending,  // ...something is asked of it already
		kRefuseHolding,  // ...another grab is still being followed
	};

	[[nodiscard]] constexpr GrabKey OnGrabKey(const GrabFacts& a_facts)
	{
		if (!a_facts.hostsCopy) {
			return GrabKey::kEngine;
		}
		if (!a_facts.grabOn) {
			return GrabKey::kRefuseOff;
		}
		if (!a_facts.editAllowed) {
			return GrabKey::kRefuseEditOff;
		}
		if (a_facts.wire || a_facts.actor || a_facts.damaged) {
			return GrabKey::kRefuseKind;
		}
		if (a_facts.power || a_facts.tied) {
			return GrabKey::kRefusePower;
		}
		if (a_facts.stacked) {
			return GrabKey::kRefuseStacked;
		}
		if (a_facts.pending) {
			return GrabKey::kRefusePending;
		}
		if (a_facts.holding) {
			return GrabKey::kRefuseHolding;
		}
		return GrabKey::kAdmit;
	}

	// The notice a refused grab says (a text key), or none: the engine's own answer, an admitted grab, and a pending one
	// (whose notice names what is asked: the caller's).
	[[nodiscard]] constexpr const char* Notice(GrabKey a_key)
	{
		switch (a_key) {
		case GrabKey::kRefuseOff:
		case GrabKey::kRefuseKind:
		case GrabKey::kRefuseHolding:
			return "workshop.shared.no_grab";
		case GrabKey::kRefuseEditOff:
			return "workshop.shared.move_off";
		case GrabKey::kRefusePower:
			return "workshop.shared.move_wired";
		case GrabKey::kRefuseStacked:
			return "workshop.shared.move_stacked";
		case GrabKey::kEngine:
		case GrabKey::kAdmit:
		case GrabKey::kRefusePending:
			break;
		}
		return nullptr;
	}

	// The notice for a key pressed on a copy whose move is asked already.
	inline constexpr const char* PENDING_NOTICE = "workshop.shared.move_pending";

	// Whether a piece has to do with power, by its base's values (a copy reads as its builder's piece): it makes, radiates or
	// needs power, a snap of it passes power, or a wire can end on it (SharedWorkshop::TakesWire).
	[[nodiscard]] constexpr bool IsPowerPiece(float a_generated, float a_radiated, float a_required, float a_snapTransmits, float a_connection, bool a_powerKeyword)
	{
		return a_generated != 0.0f || a_radiated != 0.0f || a_required != 0.0f || a_snapTransmits != 0.0f || SharedWorkshop::TakesWire(a_connection, a_powerKeyword);
	}

	// ---- The friend's hold of a copy -------------------------------------------------------------------------

	// The engine picks the copy up inside the key's own handler, so the hold shows in the next frame; a grab the engine
	// didn't take after all (its own repair of a damaged piece runs instead, or another test of its handler refused) is
	// forgotten after this long.
	inline constexpr auto GRAB_WAIT = 1s;

	// One grab at a time (workshop mode holds one piece): admitted at the key, taken once the engine holds the copy, let go
	// once it no longer does. On the main thread.
	class Grab
	{
	public:
		enum class Event : std::uint8_t
		{
			kNothing,
			kTaken,       // the engine holds the copy now
			kNeverTaken,  // admitted, but the engine never held it: forgotten, nothing to do
			kLetGo,       // the engine held it and no longer does (a drop, a cancel, workshop mode closed)
		};

		struct Result
		{
			Event         event = Event::kNothing;
			std::uint32_t copy = 0;
			std::uint32_t bench = 0;
			Transform     from{};  // where the copy stood when it was admitted
		};

		// The key admitted this copy. False (and nothing changes) while another grab is taken and not yet let go.
		bool Admit(std::uint32_t a_copy, std::uint32_t a_bench, const Transform& a_from, Clock::time_point a_now)
		{
			if (taken || a_copy == 0) {
				return false;
			}
			copy = a_copy;
			bench = a_bench;
			from = a_from;
			admittedAt = a_now;
			return true;
		}

		// Once a frame: a_held, whether the engine holds the admitted copy now.
		Result Frame(bool a_held, Clock::time_point a_now)
		{
			if (copy == 0) {
				return {};
			}
			if (!taken) {
				if (a_held) {
					taken = true;
					return { Event::kTaken, copy, bench, from };
				}
				if (a_now - admittedAt >= GRAB_WAIT) {
					return End(Event::kNeverTaken);
				}
				return {};
			}
			return a_held ? Result{} : End(Event::kLetGo);
		}

		// The copy a grab is followed for (admitted or taken), or 0.
		[[nodiscard]] std::uint32_t Copy() const { return copy; }
		[[nodiscard]] bool          Holds(std::uint32_t a_copy) const { return copy != 0 && copy == a_copy; }
		[[nodiscard]] bool          Taken() const { return taken; }

		// The session ended, a load, the copy went: nothing is followed any more. What was followed, for the caller's cleanup.
		Result Clear()
		{
			return copy == 0 ? Result{} : End(taken ? Event::kLetGo : Event::kNeverTaken);
		}

	private:
		Result End(Event a_event)
		{
			const Result result{ a_event, copy, bench, from };
			copy = 0;
			bench = 0;
			taken = false;
			return result;
		}

		std::uint32_t     copy = 0;
		std::uint32_t     bench = 0;
		Transform         from{};
		Clock::time_point admittedAt{};
		bool              taken = false;
	};

	// ---- The drop --------------------------------------------------------------------------------------------

	enum class Drop : std::uint8_t
	{
		kLeave,    // the copy stands where it stood (a cancelled grab, or put down in place): nothing is sent
		kAsk,      // it stands elsewhere: the move is asked of the host
		kPutBack,  // it stands elsewhere, but nothing can be asked (the rule or the host's setting went off, the session ended,
		           // something else is asked of it, too many requests wait): put back where it stood
	};

	[[nodiscard]] inline Drop OnDrop(const Transform& a_from, const Transform& a_now, bool a_mayAsk)
	{
		if (WorkshopMove::Same(a_from, a_now)) {
			return Drop::kLeave;
		}
		return a_mayAsk ? Drop::kAsk : Drop::kPutBack;
	}

	// ---- The answer ------------------------------------------------------------------------------------------

	struct Verdict
	{
		bool        putBack = false;        // the copy goes back to where it stood before the grab
		bool        benchNotOwned = false;  // the workbench is not the host's: remembered (SharedWorkshop::Benches)
		const char* notice = nullptr;       // the text key shown to the player
	};

	// What the friend's game does with the answer to a move (or with no answer in time). Applied: nothing; the host's placed
	// report says where its piece stands now, and the copy follows it as any moved piece's copy does.
	[[nodiscard]] constexpr Verdict JudgeMove(Why a_why)
	{
		if (a_why == Why::kApplied) {
			return {};
		}
		Verdict verdict{ true, a_why == Why::kNotHostOwned, "workshop.shared.move_refused" };
		switch (a_why) {
		case Why::kNotAllowed:
			verdict.notice = "workshop.shared.move_off";
			break;
		case Why::kPieceWired:
			verdict.notice = "workshop.shared.move_wired";
			break;
		case Why::kPieceStacked:
			verdict.notice = "workshop.shared.move_stacked";
			break;
		case Why::kHostBusy:
		case Why::kPieceBusy:
			verdict.notice = "workshop.shared.piece_busy";
			break;
		case Why::kOutOfReach:
			verdict.notice = "workshop.shared.move_out_of_reach";
			break;
		case Why::kNoSuchPiece:
			verdict.notice = "workshop.shared.move_no_piece";
			break;
		default:
			break;
		}
		return verdict;
	}

	// Whether a refused move's copy is put back: only while it still stands where the friend dropped it. Moved since (the
	// host moved its piece meanwhile, and the copy followed the report), it is where the host's piece is: left alone.
	[[nodiscard]] inline bool StillWhereDropped(const Transform& a_now, const Transform& a_droppedAt)
	{
		return WorkshopMove::Same(a_now, a_droppedAt);
	}

	// ---- The host's side -------------------------------------------------------------------------------------

	// What the host's game finds of a move besides SharedWorkshop::Facts (the workbench's and the target's, as a store's).
	struct MoveFacts
	{
		bool hostHolds = false;     // the host holds something in workshop mode (a ghost or a grabbed piece)
		bool targetPicked = false;  // workshop mode's placement item is the piece (under the host's crosshair, or held)
		bool actor = false;         // a turret, a robot, a settler: never moved for a friend
		bool power = false;         // a power piece by its base (IsPowerPiece), or one with power links, or one a grid names
		bool stacked = false;       // something stands on it (a stack child linked to it)
		bool inArea = false;        // the destination is inside the workbench's build area
		bool inReach = false;       // ...and within the workbench's reach
		bool sameCell = false;      // ...and in the cell the piece stands in (SameCell)
	};

	// An exterior cell's side, in game units.
	inline constexpr float CELL_SIZE = 4096.0f;

	// Whether a move stays inside the piece's cell: always in an interior; outside, both spots in one cell of the grid. The
	// steps below put the reference's place and its 3D there and nothing more: what the engine's own carry does for a piece
	// that crosses into the next cell (the cell that holds the reference, and so the one it is saved and loaded with) is not
	// among them, so a move across a cell's border is refused rather than made halfway.
	[[nodiscard]] inline bool SameCell(const Transform& a_from, const Transform& a_to, bool a_interior)
	{
		if (a_interior) {
			return true;
		}
		const auto cell = [](float a_at) { return std::floor(a_at / CELL_SIZE); };
		return cell(a_from.position[0]) == cell(a_to.position[0]) && cell(a_from.position[1]) == cell(a_to.position[1]);
	}

	// Why the move is refused, or none: it can be made. In the order the host's game looks.
	[[nodiscard]] constexpr std::optional<Why> Refusal(const SharedWorkshop::Facts& a_facts, const MoveFacts& a_move)
	{
		if (!a_facts.benchKnown || !a_facts.hostOwned) {
			return Why::kNotHostOwned;
		}
		if (!a_facts.benchLoaded) {
			return Why::kBenchNotLoaded;
		}
		if (!a_facts.benchOpened) {
			return Why::kBenchUnopened;
		}
		if (!a_facts.editAllowed) {
			return Why::kNotAllowed;
		}
		if (a_facts.otherBenchOpen || a_move.hostHolds) {
			// The move's steps run with workshop mode's current workbench and its moving byte set for them: never under a
			// workshop mode that is open elsewhere, or that holds something itself. Refused outright: the friend tries again.
			return Why::kHostBusy;
		}
		if (!a_facts.targetKnown || !a_facts.targetAtBench) {
			return Why::kNoSuchPiece;
		}
		if (a_move.actor) {
			return Why::kNotWorkshopItem;
		}
		if (!a_facts.targetLoaded) {
			return Why::kBenchNotLoaded;  // its 3D not in: nothing to move but the reference's place
		}
		if (a_facts.targetHeld || a_move.targetPicked) {
			return Why::kPieceBusy;
		}
		if (a_move.power) {
			return Why::kPieceWired;
		}
		if (a_move.stacked) {
			return Why::kPieceStacked;
		}
		if (!a_move.inArea || !a_move.inReach || !a_move.sameCell) {
			return Why::kOutOfReach;
		}
		return std::nullopt;
	}

	// The host's game moves its piece for a friend with the engine's own drop of a moved piece (Workshop::UpdateActiveEdit's
	// second half, 1.11.240: 2195152), each step a plain function, in the engine's order, inside the bench scope (workshop
	// mode's current workbench set to the piece's) and with workshop mode's "moving, not new" byte set (so
	// Workshop::MakeReferenceStatic takes its moved branch: no scripts started again, no loaded event, no initial power
	// state). Left out on purpose: the grab's grid work and its undo (Workshop::ExtraData::RemoveItem/AddItem,
	// DisconnectSpline, the snapped power connections: a piece that has any is refused, so there is none), the stack parent
	// found again at the new spot (Workshop::CheckStackItem reads workshop mode's placement data), the budget (a move
	// changes none), the grabbed and placed events (the engine's own move sends them from workshop mode's menu; Papyrus
	// hears the moved event, which is what a move is).
	enum class MoveStep : std::uint8_t
	{
		kFurnitureTaken,  // Workshop::SetFurnitureUsed(piece, true): its furniture markers taken while it moves (the grab's own call)
		kStackLink,       // its own stack-parent link cleared (it no longer stands on what it stood on), as the drop does
		kTransform,       // TESObjectREFR::SetLocationOnReference, SetAngleOnReference, and its 3D moved (Update3DPosition)
		kStatic,          // Workshop::MakeReferenceStatic(piece): its havok body where it stands now, in the cell it stands in
		kFurnitureFree,   // Workshop::SetFurnitureUsed(piece, false): its furniture markers free again
		kMovedEvent,      // ItemMovedEvent{bench, piece}: Papyrus (OnWorkshopObjectMoved), and our own sink reports the piece as placed
		kNavmesh,         // TESObjectREFR::UpdateDynamicNavmesh(true): its embedded navmesh where it stands now
		kResources,       // Workshop::RecalculateResources(bench): beds, food, water, defense counted again
	};

	inline constexpr std::array<MoveStep, 8> MOVE_STEPS{ MoveStep::kFurnitureTaken, MoveStep::kStackLink, MoveStep::kTransform, MoveStep::kStatic, MoveStep::kFurnitureFree,
		MoveStep::kMovedEvent, MoveStep::kNavmesh, MoveStep::kResources };

	[[nodiscard]] constexpr const char* Name(MoveStep a_step)
	{
		switch (a_step) {
		case MoveStep::kFurnitureTaken:
			return "furnitureTaken";
		case MoveStep::kStackLink:
			return "stackLink";
		case MoveStep::kTransform:
			return "transform";
		case MoveStep::kStatic:
			return "static";
		case MoveStep::kFurnitureFree:
			return "furnitureFree";
		case MoveStep::kMovedEvent:
			return "movedEvent";
		case MoveStep::kNavmesh:
			return "navmesh";
		case MoveStep::kResources:
			return "resources";
		}
		return "?";
	}

	// What the order must keep, whatever is added to it later: every step once; the furniture taken before the piece moves
	// and freed after it stands; the piece made static only where it ends up; and the moved event (which our sink turns into
	// the report every friend's copy follows) only once the piece stands there, static.
	[[nodiscard]] constexpr bool InOrder(std::span<const MoveStep> a_steps)
	{
		const auto at = [&](MoveStep a_step) -> std::optional<std::size_t> {
			std::optional<std::size_t> found;
			for (std::size_t i = 0; i < a_steps.size(); ++i) {
				if (a_steps[i] == a_step) {
					if (found) {
						return std::nullopt;  // twice
					}
					found = i;
				}
			}
			return found;
		};
		const auto taken = at(MoveStep::kFurnitureTaken);
		const auto transform = at(MoveStep::kTransform);
		const auto still = at(MoveStep::kStatic);
		const auto freed = at(MoveStep::kFurnitureFree);
		const auto event = at(MoveStep::kMovedEvent);
		if (!taken || !transform || !still || !freed || !event) {
			return false;
		}
		if (const auto stack = at(MoveStep::kStackLink); !stack || *stack > *still) {
			return false;
		}
		return *taken < *transform && *transform < *still && *still < *freed && *freed < *event;
	}

	// Runs the steps in order, with the workbench set as workshop mode's current one and its moving byte set for all of them,
	// both put back after, whatever happens in between. The Engine gives:
	//
	//   auto Bench()        an object that sets the current workbench and restores it when it goes out of scope
	//   auto Moving()       an object that sets workshop mode's "moving, not new" byte and restores it when it goes out of scope
	//   void Do(MoveStep)   one step (may throw: the rest are skipped, both restored)
	//
	// How many steps ran.
	template <class Engine>
	std::size_t Move(Engine& a_engine)
	{
		std::size_t                 done = 0;
		[[maybe_unused]] const auto bench = a_engine.Bench();
		[[maybe_unused]] const auto moving = a_engine.Moving();
		for (const auto step : MOVE_STEPS) {
			a_engine.Do(step);
			++done;
		}
		return done;
	}

	// Whether the host's piece stands where the request put it once the steps ran (the answer says applied only then).
	[[nodiscard]] inline bool Arrived(const Transform& a_now, const Transform& a_asked)
	{
		return WorkshopMove::Same(a_now, a_asked);
	}

	// The transform a request names: its position and rotation, with the piece's own scale (a move changes no size).
	[[nodiscard]] inline Transform Asked(const Protocol::WorkshopRequest& a_request, float a_scale)
	{
		return { { a_request.position[0], a_request.position[1], a_request.position[2] }, { a_request.rotation[0], a_request.rotation[1], a_request.rotation[2] }, a_scale };
	}
}
