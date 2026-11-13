// A friend's supply line or move for a settler at the host's settlement is a request the host's game carries out
// (game/SettlerTransfers.h): what the friend's menus show, what its game does with the pick, what the host's game checks,
// what the friend's game makes of the answer, and the host's settlements as told and heard.

#include "Test.h"

#include "game/SettlerTransfers.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

using SettlerTransfers::Action;
using SettlerTransfers::Facts;
using SettlerTransfers::Pick;
using SettlerTransfers::Picked;
using Why = Protocol::WorkshopAnswer::Why;
namespace Op = Protocol::WorkshopRequestOp;

namespace
{
	constexpr std::uint32_t BENCH = 0x000250FE;      // Sanctuary's workbench
	constexpr std::uint32_t RED_ROCKET = 0x000250FF;
	constexpr std::uint32_t ABERNATHY = 0x0006F5C5;
	constexpr std::uint32_t RUNTIME_BENCH = 0xFF000900;

	// A friend in the caravan menu at the host's settlement, a settler the session knows, a plugin's location picked.
	constexpr Picked Asked(Action a_action = Action::kSupplyLine)
	{
		Picked picked;
		picked.action = a_action;
		picked.handsOver = true;
		picked.bench = BENCH;
		picked.hostRuns = true;
		picked.settlerShared = true;
		picked.destination = true;
		picked.editAllowed = true;
		return picked;
	}

	template <class Change>
	SettlerTransfers::Decision PickWith(Change a_change)
	{
		auto picked = Asked();
		a_change(picked);
		return SettlerTransfers::OnPick(picked);
	}

	bool Says(const SettlerTransfers::Decision& a_decision, Pick a_pick, std::string_view a_notice)
	{
		return a_decision.pick == a_pick && a_decision.notice && std::string_view{ a_decision.notice } == a_notice;
	}

	// Everything the host's game needs to send the settler.
	constexpr Facts Fine(Action a_action = Action::kSupplyLine)
	{
		Facts facts;
		facts.action = a_action;
		facts.benchKnown = true;
		facts.hostOwned = true;
		facts.benchLoaded = true;
		facts.editAllowed = true;
		facts.settlerKnown = true;
		facts.settlerOfBench = true;
		facts.offered = true;
		facts.destinationKnown = true;
		facts.destinationOwned = true;
		facts.destinationOther = true;
		facts.destinationFits = true;
		facts.localLeader = true;
		return facts;
	}

	template <class Change>
	std::optional<Why> RefusedWith(Change a_change, Action a_action = Action::kSupplyLine)
	{
		auto facts = Fine(a_action);
		a_change(facts);
		return SettlerTransfers::Refusal(facts);
	}
}

TEST("settler transfers: the menu's action is the request's op, and back")
{
	CHECK(SettlerTransfers::OpOf(Action::kSupplyLine) == Op::kSupplyLine);
	CHECK(SettlerTransfers::OpOf(Action::kMove) == Op::kTransfer);
	CHECK(SettlerTransfers::ActionOf(Op::kSupplyLine) == Action::kSupplyLine);
	CHECK(SettlerTransfers::ActionOf(Op::kTransfer) == Action::kMove);
	CHECK(SettlerTransfers::ActionOf(Op::kAssign) == Action::kNone);
	CHECK(Op::IsTransfer(Op::kSupplyLine) && Op::IsTransfer(Op::kTransfer) && !Op::IsTransfer(Op::kAssign));
	CHECK(Op::IsSettler(Op::kSupplyLine) && Op::IsSettler(Op::kTransfer) && Op::IsKnown(Op::kTransfer));
}

TEST("settler transfers: at the host's settlement the menus list the host's settlements and ask the host's Local Leader")
{
	// The rank workshop mode is told: the host's there (none until said: no supply line offered), ours anywhere else.
	CHECK(SettlerTransfers::PerkRank(0, true, std::uint8_t{ 2 }) == 2);
	CHECK(SettlerTransfers::PerkRank(2, true, std::uint8_t{ 0 }) == 0);
	CHECK(SettlerTransfers::PerkRank(2, true, std::nullopt) == 0);
	CHECK(SettlerTransfers::PerkRank(1, false, std::uint8_t{ 0 }) == 1);
	// The list's "player owns it": the host's word there, inside the list only; the engine's anywhere else. Workshop mode is
	// open at Sanctuary's workbench (a settlement the host's game runs); Red Rocket's is another workbench.
	constexpr std::uint32_t SANCTUARY = 0x000250FE;
	constexpr std::uint32_t RED_ROCKET = 0x00054BAE;
	CHECK(SettlerTransfers::Owned(0.0f, true, SANCTUARY, RED_ROCKET, true) == 1.0f);
	CHECK(SettlerTransfers::Owned(1.0f, true, SANCTUARY, RED_ROCKET, false) == 0.0f);  // our own settlement isn't listed there
	CHECK(SettlerTransfers::Owned(1.0f, false, SANCTUARY, RED_ROCKET, false) == 1.0f); // any other reader: the engine's
	CHECK(SettlerTransfers::Owned(0.0f, true, 0, RED_ROCKET, true) == 0.0f);           // our own world's settlement: the engine's
	CHECK(SettlerTransfers::Owned(1.0f, true, 0, RED_ROCKET, false) == 1.0f);
}

TEST("settler transfers: the friend's caravan list leaves out the settler's own settlement, as the host's own list does")
{
	constexpr std::uint32_t SANCTUARY = 0x000250FE;
	constexpr std::uint32_t RED_ROCKET = 0x00054BAE;
	// The host owns the open settlement, but it is the settler's own (the host's game named the settler there and refuses it
	// as a destination): not listed, whatever the engine or the host's list say.
	CHECK(SettlerTransfers::Owned(0.0f, true, SANCTUARY, SANCTUARY, true) == 0.0f);
	CHECK(SettlerTransfers::Owned(1.0f, true, SANCTUARY, SANCTUARY, true) == 0.0f);
	// Another settlement the host owns is still listed, whichever workbench workshop mode is open at.
	CHECK(SettlerTransfers::Owned(0.0f, true, SANCTUARY, RED_ROCKET, true) == 1.0f);
	CHECK(SettlerTransfers::Owned(0.0f, true, RED_ROCKET, SANCTUARY, true) == 1.0f);
	CHECK(SettlerTransfers::Owned(0.0f, true, RED_ROCKET, RED_ROCKET, true) == 0.0f);
	// Any other reader of the open workbench's value, or workshop mode at our own world's settlement: the engine's.
	CHECK(SettlerTransfers::Owned(1.0f, false, SANCTUARY, SANCTUARY, true) == 1.0f);
	CHECK(SettlerTransfers::Owned(1.0f, true, 0, SANCTUARY, true) == 1.0f);
}

TEST("settler transfers: a friend's pick at the host's settlement is asked of the host's game, or refused with a notice")
{
	CHECK(SettlerTransfers::OnPick(Asked()).pick == Pick::kAsk);
	CHECK(SettlerTransfers::OnPick(Asked(Action::kMove)).pick == Pick::kAsk);
	// Not ours to change: any other action, the rule not handing over, no workshop mode.
	CHECK(PickWith([](Picked& a_picked) { a_picked.action = Action::kNone; }).pick == Pick::kEngine);
	CHECK(PickWith([](Picked& a_picked) { a_picked.handsOver = false; }).pick == Pick::kEngine);
	CHECK(PickWith([](Picked& a_picked) { a_picked.bench = 0; }).pick == Pick::kEngine);
	// Our own world's settlement: the game's own, but never our copy of another player's NPC.
	CHECK(PickWith([](Picked& a_picked) { a_picked.hostRuns = false; }).pick == Pick::kEngine);
	CHECK(Says(PickWith([](Picked& a_picked) {
		a_picked.hostRuns = false;
		a_picked.settlerCopy = true;
	}), Pick::kRefuse, "workshop.shared.assign_their_settler"));
	// At the host's: our game's own settler, the rule's edit off, one waiting, a location our game made.
	CHECK(Says(PickWith([](Picked& a_picked) { a_picked.settlerShared = false; }), Pick::kRefuse, "workshop.shared.transfer_not_hosts"));
	CHECK(Says(PickWith([](Picked& a_picked) { a_picked.editAllowed = false; }), Pick::kRefuse, "workshop.shared.transfer_off"));
	CHECK(Says(PickWith([](Picked& a_picked) { a_picked.pending = true; }), Pick::kRefuse, "workshop.shared.assign_pending"));
	CHECK(Says(PickWith([](Picked& a_picked) { a_picked.destination = false; }), Pick::kRefuse, "workshop.shared.transfer_bad_destination"));
}

TEST("settler transfers: a list made from the host's settlements never becomes our game's own pick, even with the host gone")
{
	// Listed from the host's settlements and the host still runs it: asked as ever.
	CHECK(PickWith([](Picked& a_picked) { a_picked.listedFromHost = true; }).pick == Pick::kAsk);
	// The host's game left the session, workshop mode closed, or the settlement was found not to be the host's, between the
	// list and the pick: neither our game's event (the destination is the host's) nor a request.
	const auto away = "workshop.shared.transfer_host_away";
	CHECK(Says(PickWith([](Picked& a_picked) {
		a_picked.listedFromHost = true;
		a_picked.handsOver = false;
	}), Pick::kRefuse, away));
	CHECK(Says(PickWith([](Picked& a_picked) {
		a_picked.listedFromHost = true;
		a_picked.hostRuns = false;
	}), Pick::kRefuse, away));
	CHECK(Says(PickWith([](Picked& a_picked) {
		a_picked.listedFromHost = true;
		a_picked.bench = 0;
	}), Pick::kRefuse, away));
	// A plugin's settler our own save holds too (Codsworth): still nothing of ours.
	CHECK(Says(PickWith([](Picked& a_picked) {
		a_picked.listedFromHost = true;
		a_picked.hostRuns = false;
		a_picked.settlerShared = true;
		a_picked.settlerCopy = false;
	}), Pick::kRefuse, away));
	// A list our own game made (the host's game never ran it): the game's own, as before.
	CHECK(PickWith([](Picked& a_picked) { a_picked.handsOver = false; }).pick == Pick::kEngine);
	CHECK(PickWith([](Picked& a_picked) { a_picked.hostRuns = false; }).pick == Pick::kEngine);
	// Any other action of the menu: not ours.
	CHECK(PickWith([](Picked& a_picked) {
		a_picked.listedFromHost = true;
		a_picked.action = Action::kNone;
	}).pick == Pick::kEngine);
}

TEST("settler transfers: the pick's hook passes it on at once only when every fact it can't know gives the game's own path")
{
	const std::array<Action, 3>        actions{ Action::kNone, Action::kSupplyLine, Action::kMove };
	const std::array<std::uint32_t, 2> benches{ 0, BENCH };
	int                                checked = 0;
	for (const auto action : actions) {
		for (const auto bench : benches) {
			for (unsigned bits = 0; bits < 128; ++bits) {
				Picked picked;
				picked.action = action;
				picked.bench = bench;
				picked.handsOver = (bits & 1) != 0;
				picked.hostRuns = (bits & 2) != 0;
				picked.settlerShared = (bits & 4) != 0;
				picked.settlerCopy = (bits & 8) != 0;
				picked.destination = (bits & 16) != 0;
				picked.editAllowed = (bits & 32) != 0;
				picked.listedFromHost = (bits & 64) != 0;
				for (unsigned known = 0; known < 4; ++known) {
					const bool runsKnown = (known & 1) != 0;
					const bool settlerKnown = (known & 2) != 0;
					bool       all = true;
					for (unsigned v = 0; v < 16; ++v) {
						auto p = picked;
						p.hostRuns = runsKnown ? picked.hostRuns : (v & 1) != 0;
						p.settlerShared = settlerKnown ? picked.settlerShared : (v & 2) != 0;
						p.settlerCopy = settlerKnown ? picked.settlerCopy : (v & 4) != 0;
						p.pending = (v & 8) != 0;
						all = all && SettlerTransfers::OnPick(p).pick == Pick::kEngine;
					}
					CHECK(SettlerTransfers::PassesNow(picked, runsKnown, settlerKnown) == all);
					++checked;
				}
				auto pending = picked;
				pending.pending = true;
				CHECK(SettlerTransfers::PassesNow(picked, true, true) == (SettlerTransfers::OnPick(picked).pick == Pick::kEngine));
				CHECK(SettlerTransfers::PassesNow(picked, true, true) == (SettlerTransfers::OnPick(pending).pick == Pick::kEngine));
				// a list made from the host's settlements is never passed on (the hook takes it once, for the main thread)
				if (picked.listedFromHost && action != Action::kNone) {
					CHECK(!SettlerTransfers::PassesNow(picked, true, true));
				}
			}
		}
	}
	CHECK(checked == 3 * 2 * 128 * 4);
	// the host's settler: kept whatever the hook can't tell; our own plugin settler at our own settlement: on at once
	CHECK(!SettlerTransfers::PassesNow(Asked(), false, false));
	CHECK(!SettlerTransfers::PassesNow(Asked(Action::kMove), true, true));
	auto own = Asked();
	own.hostRuns = false;
	CHECK(SettlerTransfers::PassesNow(own, true, true));
	CHECK(!SettlerTransfers::PassesNow(own, true, false));  // a runtime settler the view doesn't name: may be another player's copy
	CHECK(!SettlerTransfers::PassesNow(own, false, true));  // a settlement the view doesn't know: may be the host's
}

TEST("settler transfers: the host's game sends the settler only when every fact holds, refusing in order")
{
	CHECK(!SettlerTransfers::Refusal(Fine()));
	CHECK(!SettlerTransfers::Refusal(Fine(Action::kMove)));
	CHECK(RefusedWith([](Facts& a_facts) { a_facts.action = Action::kNone; }) == Why::kFailed);
	CHECK(RefusedWith([](Facts& a_facts) { a_facts.benchKnown = false; }) == Why::kNotHostOwned);
	CHECK(RefusedWith([](Facts& a_facts) { a_facts.hostOwned = false; }) == Why::kNotHostOwned);
	CHECK(RefusedWith([](Facts& a_facts) { a_facts.benchLoaded = false; }) == Why::kBenchNotLoaded);
	CHECK(RefusedWith([](Facts& a_facts) { a_facts.editAllowed = false; }) == Why::kNotAllowed);
	CHECK(RefusedWith([](Facts& a_facts) { a_facts.settlerKnown = false; }) == Why::kNoSuchSettler);
	CHECK(RefusedWith([](Facts& a_facts) { a_facts.settlerOfBench = false; }) == Why::kNoSuchSettler);
	// What the host's own workshop mode wouldn't offer it on (no caravan or move keyword on the live actor), or its companion.
	CHECK(RefusedWith([](Facts& a_facts) { a_facts.offered = false; }) == Why::kNoSuchSettler);
	CHECK(RefusedWith([](Facts& a_facts) { a_facts.companion = true; }) == Why::kNoSuchSettler);
	// Nowhere the host owns, or the settler's own settlement.
	CHECK(RefusedWith([](Facts& a_facts) { a_facts.destinationKnown = false; }) == Why::kBadDestination);
	CHECK(RefusedWith([](Facts& a_facts) { a_facts.destinationOwned = false; }) == Why::kBadDestination);
	CHECK(RefusedWith([](Facts& a_facts) { a_facts.destinationOther = false; }) == Why::kBadDestination);
	// One the host's own menu wouldn't list from there (the workshop keyword on one workbench and not the other).
	CHECK(RefusedWith([](Facts& a_facts) { a_facts.destinationFits = false; }) == Why::kBadDestination);
	CHECK(RefusedWith([](Facts& a_facts) { a_facts.destinationFits = false; }, Action::kMove) == Why::kBadDestination);
	// Local Leader is the host's, and only a supply line needs it.
	CHECK(RefusedWith([](Facts& a_facts) { a_facts.localLeader = false; }) == Why::kNoLocalLeader);
	CHECK(!RefusedWith([](Facts& a_facts) { a_facts.localLeader = false; }, Action::kMove));
	// A supply line the host's game has already (either way): never a second one; a move doesn't care.
	CHECK(RefusedWith([](Facts& a_facts) { a_facts.routeLinked = true; }) == Why::kAlreadyLinked);
	CHECK(!RefusedWith([](Facts& a_facts) { a_facts.routeLinked = true; }, Action::kMove));
	CHECK(RefusedWith([](Facts& a_facts) {
		a_facts.routeLinked = true;
		a_facts.localLeader = false;
	}) == Why::kNoLocalLeader);
	CHECK(RefusedWith([](Facts& a_facts) {
		a_facts.routeLinked = true;
		a_facts.destinationOwned = false;
	}) == Why::kBadDestination);
	// In order: the settlement before the settler before the destination before the perk.
	CHECK(RefusedWith([](Facts& a_facts) {
		a_facts.hostOwned = false;
		a_facts.offered = false;
		a_facts.localLeader = false;
	}) == Why::kNotHostOwned);
	CHECK(RefusedWith([](Facts& a_facts) {
		a_facts.offered = false;
		a_facts.destinationOwned = false;
	}) == Why::kNoSuchSettler);
	CHECK(SettlerTransfers::Refusal(Facts{}) == Why::kFailed);
}

TEST("settler transfers: the friend's notice for each answer, and what it learns of the settlement")
{
	const auto done = SettlerTransfers::Judge(Why::kApplied, Action::kSupplyLine);
	CHECK(done.benchOwned && !done.benchNotOwned && std::string_view{ done.notice } == "workshop.shared.supply_done");
	CHECK(std::string_view{ SettlerTransfers::Judge(Why::kApplied, Action::kMove).notice } == "workshop.shared.move_done");
	const auto notOwned = SettlerTransfers::Judge(Why::kNotHostOwned, Action::kMove);
	CHECK(notOwned.benchNotOwned && !notOwned.benchOwned);
	CHECK(std::string_view{ SettlerTransfers::Judge(Why::kBadDestination, Action::kMove).notice } == "workshop.shared.transfer_bad_destination");
	CHECK(std::string_view{ SettlerTransfers::Judge(Why::kNoLocalLeader, Action::kSupplyLine).notice } == "workshop.shared.transfer_no_leader");
	CHECK(std::string_view{ SettlerTransfers::Judge(Why::kNoSuchSettler, Action::kSupplyLine).notice } == "workshop.shared.transfer_no_settler");
	CHECK(std::string_view{ SettlerTransfers::Judge(Why::kNotAllowed, Action::kSupplyLine).notice } == "workshop.shared.transfer_off");
	CHECK(std::string_view{ SettlerTransfers::Judge(Why::kOff, Action::kSupplyLine).notice } == "workshop.shared.transfer_off");  // the server's rule off
	CHECK(std::string_view{ SettlerTransfers::Judge(Why::kHostAway, Action::kMove).notice } == "workshop.shared.transfer_host_away");
	CHECK(std::string_view{ SettlerTransfers::Judge(Why::kAlreadyLinked, Action::kSupplyLine).notice } == "workshop.shared.transfer_already_linked");
	CHECK(std::string_view{ SettlerTransfers::Judge(Why::kBenchNotLoaded, Action::kSupplyLine).notice } == "workshop.shared.transfer_host_away");
	CHECK(std::string_view{ SettlerTransfers::Judge(Why::kNoAnswer, Action::kSupplyLine).notice } == "workshop.shared.assign_no_answer");
	CHECK(std::string_view{ SettlerTransfers::Judge(Why::kFailed, Action::kSupplyLine).notice } == "workshop.shared.transfer_refused");
}

TEST("settler transfers: the host's game tells its settlements on a change, and a friend's game keeps what it said")
{
	SettlerTransfers::Teller teller;
	// Nothing owned, never told: nothing to say.
	CHECK(!teller.Round({}, 1));
	// Sorted, each once, plugins' references only.
	auto told = teller.Round({ RED_ROCKET, BENCH, RUNTIME_BENCH, BENCH, 0 }, 1);
	REQUIRE(told);
	CHECK((told->benches == std::vector<std::uint32_t>{ BENCH, RED_ROCKET }));
	CHECK(told->localLeader == 1);
	CHECK(!teller.Round({ BENCH, RED_ROCKET }, 1));  // the same: nothing
	told = teller.Round({ BENCH, RED_ROCKET }, 2);   // the perk's next rank
	REQUIRE(told);
	CHECK(told->localLeader == 2);
	told = teller.Round({ BENCH, RED_ROCKET, ABERNATHY }, 2);
	REQUIRE(told);
	CHECK(told->benches.size() == 3);
	// None owned now: "forget them", whatever the rank.
	told = teller.Round({}, 2);
	REQUIRE(told);
	CHECK((*told == Protocol::HostSettlements{}));
	CHECK(teller.Reports() == 4);
	// Forgotten (a welcome): the same is told again.
	CHECK(teller.Round({ BENCH }, 0));
	teller.Forget();
	CHECK(teller.Round({ BENCH }, 0));
	// Out of the session (Forget): none owned after is nothing to say (the server forgot ours itself).
	teller.Forget();
	CHECK(!teller.Round({}, 0));
	// A load into a world where our player owns none, after the old world's were told: "forget them" goes out once.
	CHECK(teller.Round({ BENCH, RED_ROCKET }, 1));
	teller.Reload();
	told = teller.Round({}, 1);
	REQUIRE(told);
	CHECK((*told == Protocol::HostSettlements{}));
	CHECK(!teller.Round({}, 1));
	// ... and a load when nothing was told: nothing.
	teller.Reload();
	CHECK(!teller.Round({}, 1));
	// ... and a load into a world that owns some: told again, even the same.
	CHECK(teller.Round({ BENCH }, 1));
	teller.Reload();
	CHECK(teller.Round({ BENCH }, 1));

	SettlerTransfers::Heard heard;
	CHECK(!heard.LocalLeader());
	CHECK(heard.Take(Protocol::HostSettlements{ 1, { BENCH, RED_ROCKET } }));
	CHECK(heard.Owns(BENCH) && heard.Owns(RED_ROCKET) && !heard.Owns(ABERNATHY));
	CHECK(heard.LocalLeader() == std::uint8_t{ 1 });
	CHECK(!heard.Take(Protocol::HostSettlements{ 1, { RED_ROCKET, BENCH } }));  // the same
	CHECK((heard.Message() == Protocol::HostSettlements{ 1, { BENCH, RED_ROCKET } }));
	CHECK(heard.Take(Protocol::HostSettlements{}));  // forget them
	CHECK(!heard.Owns(BENCH) && !heard.LocalLeader());
	CHECK(!heard.Clear());
	CHECK(heard.Take(Protocol::HostSettlements{ 0, { BENCH } }));
	CHECK(heard.LocalLeader() == std::uint8_t{ 0 });
	CHECK(heard.Clear());
}

TEST("settler transfers: the wire: requests name a settler and a plugin's location, the host's settlements a plugin's workbenches")
{
	Protocol::WorkshopRequest request{ 0, 7, Op::kSupplyLine, BENCH, 0, {}, {}, 1.0f, 0, 0, 0x0001F222, 0x0001A4D7 };
	CHECK(Protocol::IsValid(request));
	CHECK(Protocol::DecodeWorkshopRequest(Protocol::Encode(request, Protocol::MessageType::kReportWorkshopRequest)) == request);
	request.op = Op::kTransfer;
	CHECK(Protocol::IsValid(request));
	auto bad = request;
	bad.target = 0xFF000123;  // a location some game made: no other game has it
	CHECK(!Protocol::IsValid(bad));
	bad = request;
	bad.target = BENCH;  // the settler's own workbench
	CHECK(!Protocol::IsValid(bad));
	bad = request;
	bad.settler = 0;
	CHECK(!Protocol::IsValid(bad));
	bad = request;
	bad.base = 0x0001F223;
	CHECK(!Protocol::IsValid(bad));

	const Protocol::HostSettlements told{ 2, { BENCH, RED_ROCKET } };
	CHECK(Protocol::DecodeHostSettlements(Protocol::Encode(told)) == told);
	CHECK(Protocol::DecodeHostSettlements(Protocol::Encode(told, Protocol::MessageType::kHostSettlements)) == told);
	CHECK(Protocol::DecodeHostSettlements(Protocol::Encode(Protocol::HostSettlements{})) == Protocol::HostSettlements{});
	// Out of order, twice, or one some game made: refused.
	CHECK(!Protocol::DecodeHostSettlements(Protocol::Encode(Protocol::HostSettlements{ 1, { RED_ROCKET, BENCH } })));
	CHECK(!Protocol::DecodeHostSettlements(Protocol::Encode(Protocol::HostSettlements{ 1, { BENCH, BENCH } })));
	CHECK(!Protocol::DecodeHostSettlements(Protocol::Encode(Protocol::HostSettlements{ 1, { BENCH, RUNTIME_BENCH } })));
	// Past the most: cut.
	Protocol::HostSettlements many;
	for (std::uint32_t i = 0; i < Protocol::MAX_HOST_SETTLEMENTS + 5; ++i) {
		many.benches.push_back(0x00010000 + i);
	}
	const auto cut = Protocol::DecodeHostSettlements(Protocol::Encode(many));
	REQUIRE(cut);
	CHECK(cut->benches.size() == Protocol::MAX_HOST_SETTLEMENTS);
}
