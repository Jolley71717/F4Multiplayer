#pragma once

#include "Protocol.h"
#include "game/WorkshopPower.h"

#include <algorithm>
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
		bool              dropped = false;  // a build whose piece went before its answer came (a save): kept open, so the answer is matched
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

		// Whether this piece of ours waits for an answer (its moves and scraps are not reported meanwhile). A dropped build's
		// piece is gone: it waits for nothing.
		[[nodiscard]] bool IsPending(std::uint32_t a_piece) const
		{
			if (a_piece == 0) {
				return false;
			}
			for (const auto& [seq, request] : pending) {
				if (request.piece == a_piece && !request.dropped) {
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
				if (request.piece != 0 && !request.dropped) {
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
				if (request.piece != 0 && !request.dropped) {
					builds.push_back(request);
				}
			}
			return builds;
		}

		// The builds still waiting, each marked dropped (its piece goes now: a save) and returned for the caller to delete. The
		// requests stay open: the host may have built them already, and its answer is matched when it comes (the host's copy
		// then comes on its own); they no longer count as waiting.
		std::vector<Pending> DropBuilds()
		{
			std::vector<Pending> dropped;
			for (auto& [seq, request] : pending) {
				if (request.piece != 0 && !request.dropped) {
					request.dropped = true;
					dropped.push_back(request);
				}
			}
			return dropped;
		}

		// Requests whose answer we wait for with something of ours waiting on it: every wire, and the builds not dropped.
		[[nodiscard]] std::size_t Waiting() const
		{
			return static_cast<std::size_t>(std::ranges::count_if(pending, [](const auto& a_entry) { return !a_entry.second.dropped; }));
		}
		[[nodiscard]] std::size_t Dropped() const { return pending.size() - Waiting(); }

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
		bool endsTakeWire = false;  // ...each one a piece a wire can end on (TakesWire)
		bool alreadyWired = false;  // ...and the workbench's grids already join the two (Joined)
	};

	// Whether a wire can end on a piece, by the engine's own rule (Workshop's wire filter, 2195584 in 1.11.240: the reference's
	// workshopPowerConnection actor value, else the WorkshopPowerConnection keyword). Both are its base's, so a copy reads as
	// its builder's piece. A light bulb (000560DF) has neither: no player strings a wire to one.
	[[nodiscard]] constexpr bool TakesWire(float a_powerConnection, bool a_powerKeyword)
	{
		return a_powerConnection != 0.0f || a_powerKeyword;
	}

	// Whether a workbench's grids already join two pieces: an edge between them, either way round (a wire, or a snap). The wire
	// filter refuses such a second end, so workshop mode never strings a second wire between the same two pieces.
	[[nodiscard]] inline bool Joined(std::span<const WorkshopPower::Grid> a_grids, std::uint32_t a_one, std::uint32_t a_other)
	{
		for (const auto& grid : a_grids) {
			for (const auto& edge : grid.edges) {
				if ((edge.node == a_one && edge.connection == a_other) || (edge.node == a_other && edge.connection == a_one)) {
					return true;
				}
			}
		}
		return false;
	}

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
			if (!a_facts.endsTakeWire) {
				return Why::kNotWireEnd;  // workshop mode never strings one there: a friend's dev path or a modded game asked
			}
			if (a_facts.alreadyWired) {
				return Why::kAlreadyWired;  // two requests for the same wire (the friend's game asked twice before the first came back)
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

	// A piece made for a request is finished once its 3D is there. Nothing of the host's workshop mode holds a finish off
	// (building together is the point): the engine's "new item mode" byte, which MakeReferenceStatic reads, is 0 only while
	// a new piece is being placed and 1 otherwise, at idle too (read in 1.11.240: QNewItemMode is "byte == 0"), so it says
	// nothing about the host's hands, and the finish sets it to 0 for its own steps and puts it back (NewItemScope). One
	// whose 3D never comes is refused.
	inline constexpr auto MODEL_WAIT = 10s;  // the piece's 3D not there in this long: kFailed

	enum class Readiness : std::uint8_t
	{
		kReady,
		kWait,
		kNoModel,  // refused: the piece never got its 3D
	};

	[[nodiscard]] constexpr Readiness Ready(bool a_hasModel, Clock::duration a_waited)
	{
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

	// ---- The host's wire for a friend -----------------------------------------------------------------------

	// The host's game strings a wire a friend asked for between two of its pieces: the rig-proven wswire path (the spline, the
	// grid connection, the bench link, the spline's 3D) plus what the engine's own wiring adds that changes nothing of the
	// grid (the wire static, the bench's power and resources, the placed event). The connection decides the ends' power:
	// Workshop::ExtraData::AddConnection merges the two grids and evaluates them at once, so the light is lit by it.
	//
	// The engine's own wiring does one more thing: Workshop::PlaceCurrentReference (1.11.240), for a placed spline, runs
	// PowerUtils::UpdateMovingWirelessItem on both ends right after AddConnection (kEndWireless), then the totals. It does
	// that inside workshop mode, where a wired light still ends up lit. The host strings a friend's
	// wire outside workshop mode, where nothing powers it again. On a receiver end no radiator reaches, the call takes the
	// end out of its grid's powered set and fires its PowerOff (the wired edge itself stays: the rig's grid kept both
	// edges), and nothing turns the light back on: the light a friend wired stayed dark on the host and on the friend's
	// copy, with the grid saying powered (seen on the rig with the first version, which copied the engine's order). So
	// kEndWireless is never in WIRE_STEPS: adding it back for parity with the engine unpowers the light on this path.
	enum class WireStep : std::uint8_t
	{
		kLink,         // Workshop::SetIsWorkshopItem(wire, true, bench): the bench link, a special ref of its location
		kSpline,       // SplineUtils::ConnectSpline(from, to, wire): the wire's ends
		kConnect,      // Workshop::ExtraData::AddConnection(from, to, wire): the grids merged and their power evaluated
		kCount,        // its budget counted once its spline is drawn (WorkshopSync's CountWiresLater: no 3D yet)
		kRebuild,      // the spline's 3D rebuilt (queued)
		kStatic,       // Workshop::MakeReferenceStatic(wire)
		kResources,    // Workshop::CalculatePowerRating and RecalculateResources(bench): the totals recounted (no grid evaluated)
		kEvent,        // ItemPlacedEvent{bench, wire}
		kEndWireless,  // PowerUtils::UpdateMovingWirelessItem on an end: the engine's next step in workshop mode; here it unpowers the light (never in WIRE_STEPS)
	};

	inline constexpr std::array<WireStep, 8> WIRE_STEPS{ WireStep::kLink, WireStep::kSpline, WireStep::kConnect, WireStep::kCount, WireStep::kRebuild,
		WireStep::kStatic, WireStep::kResources, WireStep::kEvent };

	[[nodiscard]] constexpr const char* Name(WireStep a_step)
	{
		switch (a_step) {
		case WireStep::kLink:
			return "link";
		case WireStep::kSpline:
			return "spline";
		case WireStep::kConnect:
			return "connect";
		case WireStep::kCount:
			return "count";
		case WireStep::kRebuild:
			return "rebuild";
		case WireStep::kStatic:
			return "static";
		case WireStep::kResources:
			return "resources";
		case WireStep::kEvent:
			return "event";
		case WireStep::kEndWireless:
			return "endWireless";
		}
		return "?";
	}

	// Whether a step decides an end's power on its own (after the connection, it undoes what the connection decided).
	[[nodiscard]] constexpr bool DecidesEndPower(WireStep a_step)
	{
		return a_step == WireStep::kEndWireless;
	}

	// Whether an order strings a wire that keeps the power the connection gave its ends: the spline before the connection, the
	// connection once, and nothing after it that decides an end's power; the totals and the event after the connection.
	[[nodiscard]] constexpr bool KeepsEndPower(std::span<const WireStep> a_steps)
	{
		bool spline = false;
		bool connected = false;
		for (const auto step : a_steps) {
			if (step == WireStep::kSpline) {
				spline = true;
			} else if (step == WireStep::kConnect) {
				if (connected || !spline) {
					return false;
				}
				connected = true;
			} else if ((step == WireStep::kResources || step == WireStep::kEvent) && !connected) {
				return false;
			} else if (DecidesEndPower(step) && connected) {
				return false;
			}
		}
		return connected;
	}

	// Strings the wire: WIRE_STEPS in order, with the workbench set as workshop mode's current one for all of them and put
	// back after (the Engine as for Finish, its Do taking a WireStep). How many steps ran.
	template <class Engine>
	std::size_t StringWire(Engine& a_engine)
	{
		static_assert(KeepsEndPower(WIRE_STEPS));
		std::size_t done = 0;
		[[maybe_unused]] const auto scope = a_engine.Bench();
		for (const auto step : WIRE_STEPS) {
			a_engine.Do(step);
			++done;
		}
		return done;
	}

	// ---- A held wire's status -----------------------------------------------------------------------------

	// What the engine's spline status (1.11.240: Workshop's 2195059, run by the placement update each frame a wire is held)
	// left in the byte Workshop::CanPlaceItem reads: 1, the wire may be put down; else why not. Named for the logs and the rig.
	[[nodiscard]] constexpr const char* WireStatusName(std::uint8_t a_status)
	{
		switch (a_status) {
		case 1:
			return "ok";
		case 2:
			return "no-end";  // no second end the wire pick's filter passed, or it is the first end
		case 3:
			return "blocked";  // the engine's cast along the wire met something between the ends
		case 6:
			return "too-long";
		case 8:
			return "already-wired";  // the first end's power links already hold the target
		default:
			return "other";
		}
	}

	// ---- The engine's scrap test -------------------------------------------------------------------------

	// WorkshopCanScrapRef (1.11.240: 2194959, +384910) as the engine reads it, for the logs and `wsmode scraptest`: what it
	// finds of the reference, and which test refused it. Read in full with cdb; the order below is the engine's.
	struct ScrapFacts
	{
		bool base = false;            // the reference has a base object
		bool blockKeyword = false;    // ...the scrap block keyword (the engine's [430f7400]): never scrapped
		bool inventoryType = false;   // the base is an armor, a misc item or a weapon
		bool loose = false;           // not a projectile, and TESContainer::ContainerCanHoldForm(base): a thing one carries
		bool recipe = false;          // the base's inGameFormFlags bit 0 (an actor's: its actor base's). The only setter is
		                              // BGSConstructibleObject::InitItemImpl: a recipe makes it (or a form list one makes)
		bool miscComponents = false;  // a misc item with components
		bool objectInstance = false;  // the reference's extra data has an object instance (kObjectInstance, 0x35)
	};

	enum class ScrapFail : std::uint8_t
	{
		kNone,          // scrapped: the engine's confirmation opens
		kNoBase,
		kBlockKeyword,
		kLoose,         // asked with the flag: a thing one carries is never scrapped that way
		kLooseNotItem,  // asked without it: a thing one carries that is no armor, misc item or weapon
		kNoRecipe,      // nothing makes the base (no recipe, no components, no object instance): every wire's case, in any game
	};

	[[nodiscard]] constexpr ScrapFail ScrapRefusal(const ScrapFacts& a_facts, bool a_flag)
	{
		if (!a_facts.base) {
			return ScrapFail::kNoBase;
		}
		if (a_facts.blockKeyword) {
			return ScrapFail::kBlockKeyword;
		}
		if (a_flag ? a_facts.loose : (a_facts.loose && !a_facts.inventoryType)) {
			return a_flag ? ScrapFail::kLoose : ScrapFail::kLooseNotItem;
		}
		if (a_facts.recipe || a_facts.miscComponents) {
			return ScrapFail::kNone;
		}
		if (a_facts.loose && !a_flag && a_facts.objectInstance) {
			return ScrapFail::kNone;
		}
		return ScrapFail::kNoRecipe;
	}

	[[nodiscard]] constexpr const char* ScrapFailName(ScrapFail a_fail)
	{
		switch (a_fail) {
		case ScrapFail::kNone:
			return "none";
		case ScrapFail::kNoBase:
			return "base";
		case ScrapFail::kBlockKeyword:
			return "blockKeyword";
		case ScrapFail::kLoose:
			return "loose";
		case ScrapFail::kLooseNotItem:
			return "looseNotItem";
		case ScrapFail::kNoRecipe:
			return "recipe";
		}
		return "?";
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
