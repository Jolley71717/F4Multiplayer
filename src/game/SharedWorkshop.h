#pragma once

#include "Protocol.h"
#include "game/WorkshopPower.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <utility>
#include <vector>

// The shared workshop (the host's bSharedWorkshop, SessionRules::sharedWorkshop): at a settlement the host owns, what a
// friend builds or wires is made by the host's game as the host's own piece, and the friend's game shows it as a copy, as it
// shows any of the host's pieces. One grid, one engine computing it, one set of scripts running a machine. The game-free
// half (WorkshopSync.cpp runs it against the engine, tests/SharedWorkshopTests.cpp against stand-ins):
//
// - The friend's side. Its workshop mode places the piece as ever (so the friend paid for it, from the shared workbench
//   when bSharedStorage is on); the placed event becomes a numbered request (Requests) instead of a report, and the answer
//   decides (Judge): applied, the friend's piece is adopted as its copy of the host's (unlinked from the friend's workbench,
//   off its grids, blocked from workshop mode: what every copy is); refused, the piece is reported as the friend's own, as
//   before the rule. A wire the friend strings between two copies is taken off in the same frame it was placed (nothing of
//   it may reach a save) and asked of the host instead; the host's wire comes back as a copy, drawn from its shape.
// - The host's side. A request is checked (Refusal), the piece made where the friend's stood, and finished with the engine's
//   own placement steps, each a plain function, in the engine's order (FINISH_STEPS, Finish): what workshop mode does after
//   the ghost is confirmed, minus the cost and XP (the friend paid), the stacking and the move bookkeeping.
// - The friend's grids. No in-session piece of the friend's at a host-owned settlement may stay on them: an adopted piece is
//   a copy (the copy audit, WorkshopPower::Audit, takes it off), and the ids of what was adopted or detached are checked
//   besides (Strays), before every save and every few seconds.
namespace SharedWorkshop
{
	using Clock = std::chrono::steady_clock;
	using namespace std::chrono_literals;
	using Why = Protocol::WorkshopAnswer::Why;

	// ---- The rule --------------------------------------------------------------------------------------

	// Whether a game hands its builds over: the host's rule on (or the dev override), in a session, and not the host's own game
	// (whose pieces are its own already). The host's game takes requests under the same rule.
	[[nodiscard]] constexpr bool HandsOver(bool a_ruleOn, bool a_inSession, bool a_host)
	{
		return a_ruleOn && a_inSession && !a_host;
	}

	[[nodiscard]] constexpr bool Serves(bool a_ruleOn, bool a_inSession, bool a_host)
	{
		return a_ruleOn && a_inSession && a_host;
	}

	// ---- What the answers taught about each workbench ---------------------------------------------------

	// A workbench the host's game refused as not the host's settlement: further builds there stay ours without asking (and
	// the host's copies there aren't offered for wiring); one it applied at is the host's. Cleared when the session ends.
	class Benches
	{
	public:
		void NotOwned(std::uint32_t a_bench)
		{
			owned.erase(a_bench);
			notOwned.insert(a_bench);
		}

		void Owned(std::uint32_t a_bench)
		{
			notOwned.erase(a_bench);
			owned.insert(a_bench);
		}

		[[nodiscard]] bool IsNotOwned(std::uint32_t a_bench) const { return notOwned.contains(a_bench); }
		[[nodiscard]] bool IsOwned(std::uint32_t a_bench) const { return owned.contains(a_bench); }
		[[nodiscard]] std::size_t Size() const { return owned.size() + notOwned.size(); }

		void Clear()
		{
			owned.clear();
			notOwned.clear();
		}

	private:
		std::set<std::uint32_t> owned;
		std::set<std::uint32_t> notOwned;
	};

	// ---- The friend's requests ---------------------------------------------------------------------------

	inline constexpr auto        ANSWER_WAIT = 15s;   // no answer in this long: taken as refused (the host's game is gone or stuck)
	inline constexpr std::size_t MAX_PENDING = 256;   // requests waiting for an answer at once

	struct Pending
	{
		std::uint8_t      op = Protocol::WorkshopRequestOp::kBuild;
		std::uint32_t     piece = 0;  // a build: our own piece as workshop mode placed it (a wire: 0, it is gone already)
		std::uint32_t     bench = 0;
		Clock::time_point sentAt{};
	};

	class Requests
	{
	public:
		// A request sent: its number, or none when too many wait.
		std::optional<std::uint32_t> Open(std::uint8_t a_op, std::uint32_t a_piece, std::uint32_t a_bench, Clock::time_point a_now)
		{
			if (pending.size() >= MAX_PENDING) {
				return std::nullopt;
			}
			const auto seq = ++last;
			pending.emplace(seq, Pending{ a_op, a_piece, a_bench, a_now });
			return seq;
		}

		// The answer to this number came: what was asked, or none (answered already, or never asked).
		std::optional<Pending> Take(std::uint32_t a_seq)
		{
			const auto it = pending.find(a_seq);
			if (it == pending.end()) {
				return std::nullopt;
			}
			auto taken = it->second;
			pending.erase(it);
			return taken;
		}

		// The requests no answer came for in ANSWER_WAIT: each taken out, for the caller to treat as refused.
		std::vector<std::pair<std::uint32_t, Pending>> Expire(Clock::time_point a_now)
		{
			std::vector<std::pair<std::uint32_t, Pending>> late;
			for (auto it = pending.begin(); it != pending.end();) {
				if (a_now - it->second.sentAt >= ANSWER_WAIT) {
					late.emplace_back(it->first, it->second);
					it = pending.erase(it);
				} else {
					++it;
				}
			}
			return late;
		}

		// Whether this piece of ours waits for an answer (its moves and scraps are not reported meanwhile).
		[[nodiscard]] bool IsPending(std::uint32_t a_piece) const
		{
			if (a_piece == 0) {
				return false;
			}
			for (const auto& [seq, request] : pending) {
				if (request.piece == a_piece) {
					return true;
				}
			}
			return false;
		}

		// The pieces still waiting (leaving the session: none may stay ours).
		[[nodiscard]] std::vector<std::uint32_t> Pieces() const
		{
			std::vector<std::uint32_t> pieces;
			for (const auto& [seq, request] : pending) {
				if (request.piece != 0) {
					pieces.push_back(request.piece);
				}
			}
			return pieces;
		}

		// The builds still waiting, with their workbenches (a save, or leaving: each piece goes, told to its workbench's scripts).
		[[nodiscard]] std::vector<Pending> Builds() const
		{
			std::vector<Pending> builds;
			for (const auto& [seq, request] : pending) {
				if (request.piece != 0) {
					builds.push_back(request);
				}
			}
			return builds;
		}

		[[nodiscard]] std::size_t   Size() const { return pending.size(); }
		[[nodiscard]] std::uint32_t Last() const { return last; }
		void                        Clear() { pending.clear(); }

	private:
		std::map<std::uint32_t, Pending> pending;
		std::uint32_t                    last = 0;
	};

	// What the friend's game does with an answer (or with no answer in time).
	enum class Outcome : std::uint8_t
	{
		kAdopt,    // our piece becomes our copy of the host's (the answer names it)
		kKeepOwn,  // our piece is reported as our own, as before the rule
		kNothing,  // a wire: ours went when it was asked; the host's comes back as a copy, or nothing does
	};

	struct Verdict
	{
		Outcome     outcome = Outcome::kNothing;
		bool        benchNotOwned = false;  // the workbench is not the host's: remembered (Benches)
		const char* notice = nullptr;       // the text key shown to the player
	};

	[[nodiscard]] constexpr Verdict Judge(std::uint8_t a_op, Why a_why)
	{
		const bool wire = a_op == Protocol::WorkshopRequestOp::kWire;
		if (a_why == Why::kApplied) {
			return { wire ? Outcome::kNothing : Outcome::kAdopt, false, wire ? "workshop.shared.wired" : "workshop.shared.built" };
		}
		Verdict verdict{ wire ? Outcome::kNothing : Outcome::kKeepOwn, a_why == Why::kNotHostOwned, nullptr };
		if (wire) {
			verdict.notice = a_why == Why::kNoSuchPiece ? "workshop.shared.wire_no_piece" : "workshop.shared.wire_refused";
			return verdict;
		}
		switch (a_why) {
		case Why::kNotHostOwned:
			verdict.notice = "workshop.shared.not_owned";
			break;
		case Why::kBenchNotLoaded:
			verdict.notice = "workshop.shared.host_away";
			break;
		case Why::kHostBusy:
			verdict.notice = "workshop.shared.host_busy";
			break;
		case Why::kBenchUnopened:
			verdict.notice = "workshop.shared.bench_unopened";
			break;
		case Why::kHostAway:
		case Why::kOff:
			verdict.notice = "workshop.shared.no_host";
			break;
		case Why::kNoAnswer:
			verdict.notice = "workshop.shared.no_answer";
			break;
		default:
			verdict.notice = "workshop.shared.refused";
			break;
		}
		return verdict;
	}

	// ---- A wire's ends -----------------------------------------------------------------------------------

	// What a wire the friend's workshop mode just strung is: ours as ever (both ends our own pieces: reported as before), a
	// request (both ends copies of one builder's pieces, so the host's game can string it between its pieces), or mixed (one
	// end ours: nothing can string it in one game; taken off and refused).
	enum class WireKind : std::uint8_t
	{
		kOwn,
		kRequest,
		kMixed,
	};

	[[nodiscard]] constexpr WireKind KindOfWire(bool a_fromCopy, bool a_toCopy, bool a_sameBuilder)
	{
		if (!a_fromCopy && !a_toCopy) {
			return WireKind::kOwn;
		}
		return a_fromCopy && a_toCopy && a_sameBuilder ? WireKind::kRequest : WireKind::kMixed;
	}

	// ---- The host's side -------------------------------------------------------------------------------------

	// What the host's game finds of a request, in the order it looks.
	struct Facts
	{
		bool benchKnown = false;   // the workbench is a workbench here
		bool hostOwned = false;    // ...of a settlement the host owns (the player's NPC or faction owns the workbench)
		bool benchLoaded = false;  // ...whose cell at the spot is loaded here
		bool benchOpened = false;  // ...with its workshop data (the engine makes it when its workshop is first opened: grids, deleted items)
		bool inWorkshopMode = false;
		bool objectKnown = false;  // a build: the object is in this game
		bool buildable = false;    // ...and one workshop mode builds
		bool inReach = false;      // ...and the spot within the workbench's reach
		bool endsKnown = false;    // a wire: both ends are references here
		bool endsAtBench = false;  // ...each one of this workbench's pieces, and neither a wire
	};

	// Why the request is refused, or none: it can be made.
	[[nodiscard]] constexpr std::optional<Why> Refusal(std::uint8_t a_op, const Facts& a_facts)
	{
		if (!a_facts.benchKnown || !a_facts.hostOwned) {
			return Why::kNotHostOwned;
		}
		if (!a_facts.benchLoaded) {
			return Why::kBenchNotLoaded;
		}
		if (!a_facts.benchOpened) {
			return Why::kBenchUnopened;  // no workshop data to add the piece to: the host opens the workshop once (it is made then)
		}
		if (a_op == Protocol::WorkshopRequestOp::kWire) {
			if (!a_facts.endsKnown || !a_facts.endsAtBench) {
				return Why::kNoSuchPiece;
			}
			return std::nullopt;
		}
		if (!a_facts.objectKnown || !a_facts.buildable) {
			return Why::kNotWorkshopItem;
		}
		if (!a_facts.inReach) {
			return Why::kOutOfReach;
		}
		return std::nullopt;
	}

	// A piece made for a request is finished once its 3D is there and the host isn't holding a piece of its own in workshop mode
	// (a grabbed piece sets the engine's "moving an existing item" byte, which MakeReferenceStatic reads: ours would be put
	// back as a moved piece, its scripts never started). The byte is 0 only while a new piece is being placed and 1 otherwise,
	// at idle too (read in 1.11.240: QNewItemMode is "byte == 0"), so no state of the host's hands is read from it: the
	// finish sets it to 0 for its own steps and puts it back (NewItemScope), and nothing of the host's workshop mode holds a
	// finish off (building together is the point); a_hostHoldsPiece stays for a game with a reason to wait. One that waits
	// too long is refused.
	inline constexpr auto HOST_WAIT = 10s;   // held off longer than this: kHostBusy
	inline constexpr auto MODEL_WAIT = 10s;  // the piece's 3D not there in this long: kFailed

	enum class Readiness : std::uint8_t
	{
		kReady,
		kWait,
		kBusy,     // refused: the host kept holding a piece
		kNoModel,  // refused: the piece never got its 3D
	};

	[[nodiscard]] constexpr Readiness Ready(bool a_hostHoldsPiece, bool a_hasModel, Clock::duration a_waited)
	{
		if (a_hostHoldsPiece) {
			return a_waited >= HOST_WAIT ? Readiness::kBusy : Readiness::kWait;
		}
		if (!a_hasModel) {
			return a_waited >= MODEL_WAIT ? Readiness::kNoModel : Readiness::kWait;
		}
		return Readiness::kReady;
	}

	// The engine's own finish of a placed piece (Workshop::PlaceCurrentReference after the ghost is confirmed, 1.11.240), as
	// plain functions in its order. Skipped on purpose: the cost and the XP (the friend paid in its own workshop mode), the
	// stack parent (moving stacks only) and the moving-stack commit.
	enum class Step : std::uint8_t
	{
		kLink,       // Workshop::SetIsWorkshopItem(piece, true, bench): linked to the workbench, a special ref of its location
		kBudget,     // Workshop::Budget::AddObject: its triangles and draws into the settlement's budget
		kStatic,     // Workshop::MakeReferenceStatic: its havok body in the cell's world, scripts initialised, the loaded event, the power's initial state
		kFurniture,  // Workshop::SetFurnitureUsed(piece, false): its furniture markers free
		kSnap,       // Workshop::CheckSnappedPowerConnections: a conduit or a pylon snapped to a powered neighbour is connected
		kAddItem,    // Workshop::ExtraData::AddItem: its wireless connections (radiators)
		kTerminals,  // TerminalUtils::EstablishTerminalLinks: a terminal's links to its turrets
		kNavmesh,    // TESObjectREFR::UpdateDynamicNavmesh(true): its embedded navmesh in the settlement's
		kPower,      // Workshop::CalculatePowerRating(bench)
		kResources,  // Workshop::RecalculateResources(bench): food, water, defense, beds, power recounted
		kEvent,      // ItemPlacedEvent{bench, piece}: Papyrus registers it (OnWorkshopObjectPlaced), and our sink reports it as the host's own
	};

	inline constexpr std::array<Step, 11> FINISH_STEPS{ Step::kLink, Step::kBudget, Step::kStatic, Step::kFurniture, Step::kSnap, Step::kAddItem, Step::kTerminals,
		Step::kNavmesh, Step::kPower, Step::kResources, Step::kEvent };

	[[nodiscard]] constexpr const char* Name(Step a_step)
	{
		switch (a_step) {
		case Step::kLink:
			return "link";
		case Step::kBudget:
			return "budget";
		case Step::kStatic:
			return "static";
		case Step::kFurniture:
			return "furniture";
		case Step::kSnap:
			return "snap";
		case Step::kAddItem:
			return "addItem";
		case Step::kTerminals:
			return "terminals";
		case Step::kNavmesh:
			return "navmesh";
		case Step::kPower:
			return "power";
		case Step::kResources:
			return "resources";
		case Step::kEvent:
			return "event";
		}
		return "?";
	}

	// Runs the steps in order, with the workbench set as workshop mode's current one for all of them (the engine's steps read
	// it) and put back after, whatever happens in between. The Engine gives:
	//
	//   auto Bench()        an object that sets the current workbench and restores it when it goes out of scope
	//   void Do(Step)       one step (may throw: the rest are skipped, the bench restored)
	//
	// How many steps ran.
	template <class Engine>
	std::size_t Finish(Engine& a_engine)
	{
		std::size_t done = 0;
		[[maybe_unused]] const auto scope = a_engine.Bench();
		for (const auto step : FINISH_STEPS) {
			a_engine.Do(step);
			++done;
		}
		return done;
	}

	// ---- The budget's totals -----------------------------------------------------------------------------

	// Whether the engine's budget totals are the workbench's own right now, so an add or remove may use them as they are:
	// workshop mode is open, and the workbench the engine itself holds as current (a_engineBench: what it had before any
	// scope of ours set the piece's bench there; 0 outside workshop mode) is this one. Otherwise the totals are another
	// settlement's, or 0 after a load, and must be loaded from the bench first (WorkshopSync's BudgetScope).
	[[nodiscard]] constexpr bool BudgetLive(std::uint32_t a_engineBench, std::uint32_t a_bench, bool a_menuOpen)
	{
		return a_menuOpen && a_engineBench != 0 && a_engineBench == a_bench;
	}

	// ---- The friend's grids -------------------------------------------------------------------------------

	// The in-session pieces of ours (adopted as copies, or wires taken off) that the grids still name: none may. Sorted, each
	// once.
	[[nodiscard]] inline std::vector<std::uint32_t> Strays(std::span<const WorkshopPower::Grid> a_grids, const std::set<std::uint32_t>& a_made)
	{
		return WorkshopPower::NamedWhere(a_grids, [&](std::uint32_t a_id) { return a_made.contains(a_id); });
	}
}
