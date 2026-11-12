#pragma once

#include "Protocol.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

// A friend's supply line or move for a settler at a settlement the host's game runs (the shared workshop, bSharedWorkshop).
// Where a settler lives and which settlements its supply line links are the host's save's (the workshop's scripts, the
// locations' links), so the friend's game sends nobody anywhere there. Its player picks Supply Line or Move in workshop
// mode as in its own game (both are offered on the settlers the host's game names there, as Command is: SettlerRoster.h),
// and the caravan menu lists the settlements the host's player owns (the host's game tells them: HostSettlements), not the
// friend's own. The pick goes to the host's game as a numbered request (WorkshopRequestOp::kSupplyLine or kTransfer: the
// settler, and the destination's location, what the menu picks), and the host's game, after its own checks, sends the
// engine's own event for it (TESObjectREFR_Events::SendWNPCTransferEvent, what its own caravan menu sends), which the
// settler's workshop script turns into the supply line or the move. Local Leader is the host's player's: a friend's menu
// asks for its own player's rank and is told the host's, and the host's game refuses a supply line without it.
// Nothing of it is written into the friend's game or save. These are the rules of both ends, game-free, for the tests.
namespace SettlerTransfers
{
	using Why = Protocol::WorkshopAnswer::Why;

	// What the caravan menu was opened for (its action keyword: the menu's own, as workshop mode's Supply Line or Move set it).
	enum class Action : std::uint8_t
	{
		kNone,        // any other keyword: not ours to change
		kSupplyLine,  // WorkshopAssignCaravan's
		kMove,        // WorkshopAssignHome's
	};

	[[nodiscard]] constexpr std::uint8_t OpOf(Action a_action)
	{
		return a_action == Action::kSupplyLine ? Protocol::WorkshopRequestOp::kSupplyLine : Protocol::WorkshopRequestOp::kTransfer;
	}

	[[nodiscard]] constexpr Action ActionOf(std::uint8_t a_op)
	{
		return a_op == Protocol::WorkshopRequestOp::kSupplyLine ? Action::kSupplyLine : a_op == Protocol::WorkshopRequestOp::kTransfer ? Action::kMove : Action::kNone;
	}

	// ---- The friend's game: the menu ---------------------------------------------------------------------------

	// The Local Leader rank workshop mode is told for our player: the host's at a settlement the host's game runs (none until
	// its game has said: no supply line is offered), the engine's own everywhere else.
	[[nodiscard]] constexpr std::uint8_t PerkRank(std::uint8_t a_engine, bool a_hostRunsOpen, std::optional<std::uint8_t> a_hostRank)
	{
		return a_hostRunsOpen ? a_hostRank.value_or(0) : a_engine;
	}

	// The "player owns it" value (workshopPlayerOwned) the caravan menu's list reads off each workbench: at a settlement the
	// host's game runs (a_hostRunsOpen: the workbench workshop mode is open at, 0 for none), whether the host's player owns
	// a_bench (no settlement of our own is listed: the host's game couldn't send its settler there), and never the open
	// workbench itself. That one is the settler's own settlement as the host's game named it: Supply Line and Move are offered
	// only on the settlers it named there, and it refuses a settler not linked to that workbench and a destination that is
	// the settler's own. The host's own menu leaves it out because the actor is that workshop's settler; our copy is no
	// settler of our game, so the engine's filter keeps it unless this says 0. Anywhere else, and for any other reader, the
	// engine's own.
	[[nodiscard]] constexpr float Owned(float a_engine, bool a_inList, std::uint32_t a_hostRunsOpen, std::uint32_t a_bench, bool a_hostOwns)
	{
		if (!a_inList || a_hostRunsOpen == 0) {
			return a_engine;
		}
		return a_hostOwns && a_bench != a_hostRunsOpen ? 1.0f : 0.0f;
	}

	// ---- The friend's game: the pick -----------------------------------------------------------------------------

	struct Picked
	{
		Action        action = Action::kNone;
		bool          handsOver = false;      // the rule hands requests to a host's game (SharedWorkshop::HandsOver, a host in the session)
		std::uint32_t bench = 0;              // the workbench workshop mode is open at (0: not in workshop mode)
		bool          hostRuns = false;       // SettlerRequests::HostRuns for that workbench
		bool          settlerShared = false;  // the session knows the settler (a plugin's NPC, or a spawn): the host's game can be told which
		bool          settlerCopy = false;    // the settler is our copy of another player's runtime NPC (Spawns::CopyIdOf)
		bool          destination = false;    // the pick is a plugin's location (the same in every game), not one our game made
		bool          editAllowed = false;    // the host's bSharedWorkshopEdit
		bool          pending = false;        // a request for this settler waits for its answer
		bool          listedFromHost = false; // the menu's list was made from the host's settlements (its filter answered for the host)
	};

	enum class Pick : std::uint8_t
	{
		kEngine,  // the game's own path: our game's event (our own world's settlement, or the rule not in force)
		kAsk,     // asked of the host's game; nothing is sent anywhere here
		kRefuse,  // nothing is sent and nothing is asked: the notice says why
	};

	struct Decision
	{
		Pick        pick = Pick::kEngine;
		const char* notice = nullptr;  // the text key shown to the player (a refusal)
	};

	[[nodiscard]] constexpr Decision OnPick(const Picked& a_picked)
	{
		if (a_picked.action == Action::kNone) {
			return { Pick::kEngine, nullptr };
		}
		// A list made from the host's settlements whose pick comes when the host's game no longer runs this settlement (it
		// left the session, or the settlement was found not to be its): the destination is the host's, not one our own game
		// listed, so our game sends nothing and asks nothing.
		if (a_picked.listedFromHost && (!a_picked.handsOver || a_picked.bench == 0 || !a_picked.hostRuns)) {
			return { Pick::kRefuse, "workshop.shared.transfer_host_away" };
		}
		if (!a_picked.handsOver || a_picked.bench == 0) {
			return { Pick::kEngine, nullptr };
		}
		if (!a_picked.hostRuns) {
			// Our own world's settlement, as before the rule; but our scripts never send our copy of another player's NPC.
			return a_picked.settlerCopy ? Decision{ Pick::kRefuse, "workshop.shared.assign_their_settler" } : Decision{ Pick::kEngine, nullptr };
		}
		// At the host's settlement the menu lists the host's settlements: our own game's settler (made here at run time) can't
		// be named to the host's game, and our game can't send it to a settlement only the host owns.
		if (!a_picked.settlerShared) {
			return { Pick::kRefuse, "workshop.shared.transfer_not_hosts" };
		}
		if (!a_picked.editAllowed) {
			return { Pick::kRefuse, "workshop.shared.transfer_off" };
		}
		if (a_picked.pending) {
			return { Pick::kRefuse, "workshop.shared.assign_pending" };
		}
		if (!a_picked.destination) {
			return { Pick::kRefuse, "workshop.shared.transfer_bad_destination" };
		}
		return { Pick::kAsk, nullptr };
	}

	// ---- The host's game: what it finds of the request ---------------------------------------------------------

	struct Facts
	{
		Action action = Action::kNone;
		bool   benchKnown = false;        // the workbench is one in the host's game
		bool   hostOwned = false;         // ...and the host's player owns its settlement
		bool   benchLoaded = false;       // ...and it is loaded there (the settler with it)
		bool   editAllowed = false;       // the host's bSharedWorkshopEdit
		bool   settlerKnown = false;      // the settler is an actor of the host's game, alive, enabled
		bool   settlerOfBench = false;    // ...linked to that workbench as one of its workshop's
		bool   offered = false;           // ...and the host's own workshop mode offers that action on it (the engine's keyword on the live actor)
		bool   companion = false;         // ...and the player's current companion (the companion handling's, never a settler's)
		bool   destinationKnown = false;  // the location is a settlement's in the host's game (a workbench's settlement location)
		bool   destinationOwned = false;  // ...one the host's player owns
		bool   destinationOther = false;  // ...not the settler's own
		bool   destinationFits = false;   // ...and one the host's own caravan menu would list from the settler's (its workbench
		                                  // carries the menu's workshop keyword exactly when the settler's does)
		bool   localLeader = false;       // the host's player has Local Leader (asked for a supply line only)
		bool   routeLinked = false;       // a supply line already links the settler's settlement and that one, either way (a
		                                  // supply line's only: the host's menu never lists them)
	};

	// Why the host's game refuses the request, or nothing: it sends the event. In this order.
	[[nodiscard]] constexpr std::optional<Why> Refusal(const Facts& a_facts)
	{
		if (a_facts.action == Action::kNone) {
			return Why::kFailed;
		}
		if (!a_facts.benchKnown || !a_facts.hostOwned) {
			return Why::kNotHostOwned;
		}
		if (!a_facts.benchLoaded) {
			return Why::kBenchNotLoaded;
		}
		if (!a_facts.editAllowed) {
			return Why::kNotAllowed;
		}
		if (!a_facts.settlerKnown || !a_facts.settlerOfBench || !a_facts.offered || a_facts.companion) {
			return Why::kNoSuchSettler;
		}
		if (!a_facts.destinationKnown || !a_facts.destinationOwned || !a_facts.destinationOther || !a_facts.destinationFits) {
			return Why::kBadDestination;
		}
		if (a_facts.action == Action::kSupplyLine && !a_facts.localLeader) {
			return Why::kNoLocalLeader;
		}
		if (a_facts.action == Action::kSupplyLine && a_facts.routeLinked) {
			return Why::kAlreadyLinked;
		}
		return std::nullopt;
	}

	// ---- The friend's game: the answer -----------------------------------------------------------------------

	struct Verdict
	{
		bool        benchNotOwned = false;  // the workbench is not the host's: remembered (the game's own path from then on)
		bool        benchOwned = false;     // the host's game ran the request: the workbench is the host's
		const char* notice = nullptr;       // the text key shown to the player
	};

	[[nodiscard]] constexpr Verdict Judge(Why a_why, Action a_action)
	{
		switch (a_why) {
		case Why::kApplied:
			return { false, true, a_action == Action::kSupplyLine ? "workshop.shared.supply_done" : "workshop.shared.move_done" };
		case Why::kNotHostOwned:
			return { true, false, "workshop.shared.assign_not_owned" };
		case Why::kBenchNotLoaded:
		case Why::kHostAway:
			return { false, false, "workshop.shared.transfer_host_away" };
		case Why::kNoAnswer:
			return { false, false, "workshop.shared.assign_no_answer" };
		case Why::kNotAllowed:
		case Why::kOff:
			return { false, false, "workshop.shared.transfer_off" };
		case Why::kNoSuchSettler:
			return { false, false, "workshop.shared.transfer_no_settler" };
		case Why::kBadDestination:
			return { false, false, "workshop.shared.transfer_bad_destination" };
		case Why::kNoLocalLeader:
			return { false, false, "workshop.shared.transfer_no_leader" };
		case Why::kAlreadyLinked:
			return { false, false, "workshop.shared.transfer_already_linked" };
		default:
			return { false, false, "workshop.shared.transfer_refused" };
		}
	}

	// ---- The host's settlements: told by the host's game, kept by the server and a friend's game --------------

	// The list as it goes on the wire: plugins' references only, each once, ascending, at most MAX_HOST_SETTLEMENTS.
	[[nodiscard]] inline std::vector<std::uint32_t> Normal(std::vector<std::uint32_t> a_benches)
	{
		std::erase_if(a_benches, [](std::uint32_t a_bench) { return !Protocol::IsShareableRef(a_bench); });
		std::ranges::sort(a_benches);
		a_benches.erase(std::unique(a_benches.begin(), a_benches.end()), a_benches.end());
		if (a_benches.size() > Protocol::MAX_HOST_SETTLEMENTS) {
			a_benches.resize(Protocol::MAX_HOST_SETTLEMENTS);
		}
		return a_benches;
	}

	// The host's game: what it last told, so it tells only a change.
	class Teller
	{
	public:
		// a_owned: the workbenches of every settlement our player owns now; a_rank: its Local Leader rank. What to send, if it
		// differs from what was sent last (none sent yet counts as none owned).
		std::optional<Protocol::HostSettlements> Round(std::vector<std::uint32_t> a_owned, std::uint8_t a_rank)
		{
			Protocol::HostSettlements now{ a_rank, Normal(std::move(a_owned)) };
			if (now.benches.empty()) {
				now.localLeader = 0;  // none owned: told as "forget them", whatever the rank
			}
			if (now == sent && !owed) {
				return std::nullopt;
			}
			sent = now;
			owed = false;
			++reports;
			return now;
		}

		// Everything is told again at the next round (a welcome, or out of the session: the server forgets ours itself).
		void Forget()
		{
			sent = {};
			owed = false;
		}

		// A load: the old world's settlements are told again at the next round, and if it told some, the next round tells
		// something even when the new world owns none ("forget them"), so nobody keeps the old world's.
		void Reload()
		{
			owed = !sent.benches.empty();
			sent = {};
		}

		[[nodiscard]] std::size_t   Told() const { return sent.benches.size(); }
		[[nodiscard]] std::uint32_t Reports() const { return reports; }

	private:
		Protocol::HostSettlements sent;
		bool                      owed = false;  // a load after something was told: the next round tells, even none
		std::uint32_t             reports = 0;
	};

	// The server and a friend's game: what the host's game last said. None said, or none owned: nothing known.
	class Heard
	{
	public:
		// Whether it changed what is known.
		bool Take(const Protocol::HostSettlements& a_told)
		{
			auto benches = Normal(a_told.benches);
			std::optional<std::uint8_t> rank = benches.empty() ? std::nullopt : std::optional<std::uint8_t>{ a_told.localLeader };
			if (benches == owned && rank == localLeader) {
				return false;
			}
			owned = std::move(benches);
			localLeader = rank;
			return true;
		}

		// Whether anything was known (and is now forgotten).
		bool Clear()
		{
			const bool had = !owned.empty();
			owned.clear();
			localLeader.reset();
			return had;
		}

		[[nodiscard]] bool Owns(std::uint32_t a_bench) const { return std::ranges::binary_search(owned, a_bench); }
		[[nodiscard]] std::optional<std::uint8_t> LocalLeader() const { return localLeader; }
		[[nodiscard]] const std::vector<std::uint32_t>& Owned() const { return owned; }
		[[nodiscard]] Protocol::HostSettlements Message() const { return { localLeader.value_or(0), owned }; }

	private:
		std::vector<std::uint32_t>  owned;  // ascending
		std::optional<std::uint8_t> localLeader;
	};
}
