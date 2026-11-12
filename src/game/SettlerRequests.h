#pragma once

#include "Protocol.h"

#include <cstdint>
#include <format>
#include <optional>
#include <string>

// A friend's job for a settler at a settlement the host owns (the shared workshop, bSharedWorkshop). The settlement's
// settlers are the host's game's: their jobs live in the host's save (the crop's or the bed's owner, the settler's worker
// flags in the workshop's scripts). So the friend's game assigns nothing itself there: when its player, in workshop mode,
// gives a selected settler a crop, a bed or a post (Command is offered on the settlers the host's game names there,
// SettlerRoster.h, not only on those the friend's own save made settlers), the order goes to the host's game as a numbered request
// (Protocol::WorkshopRequestOp::kAssign, the settler and the host's piece), and the host's game gives the settler the job
// with the workshop's own script (WorkshopParentScript.AssignActorToObjectPUBLIC, what the game's own command runs). The
// friend sees the result as it sees everything the host's settlers do: the settler walks to the job. Nothing of it is
// written into the friend's game or save. These are the rules of both ends, game-free, for the tests.
namespace SettlerRequests
{
	using Why = Protocol::WorkshopAnswer::Why;

	// The engine's command for "work here" / "sleep here" (COMMAND_TYPE's workshop assign), as command mode gives it.
	inline constexpr std::uint32_t COMMAND_ASSIGN = 10;

	// Whether a friend's game knows a settlement to be run by the host's game: the rule hands requests over, no answer said
	// the workbench isn't the host's, and there is evidence it is (an applied answer for it, one of the host's pieces
	// standing there as our copy, or the host's game naming its settlers there: SettlerRoster.h). Without evidence nothing
	// is assumed: the settlement is treated as before the rule.
	[[nodiscard]] constexpr bool HostRuns(bool a_handsOver, bool a_taughtNotOwned, bool a_taughtOwned, bool a_hostsCopyThere)
	{
		return a_handsOver && !a_taughtNotOwned && (a_taughtOwned || a_hostsCopyThere);
	}

	// ---- The friend's game: the command as it is given -------------------------------------------------------

	// What the settler is told to use.
	enum class Target : std::uint8_t
	{
		kHostsPiece,  // our copy of one of the host's pieces at the settlement workshop mode is open at
		kCopy,        // any other copy of another player's piece
		kPlaced,      // a reference a plugin places (the same in every game): a settlement's own bed or crop
		kOwn,         // a piece of our own game's (made at runtime, no copy)
	};

	struct Given
	{
		std::uint32_t command = 0;         // the engine's command type
		bool          hasTarget = false;   // the command names a reference
		bool          handsOver = false;   // the rule hands requests to a host's game (SharedWorkshop::HandsOver, a host in the session)
		std::uint32_t bench = 0;           // the workbench workshop mode is open at (0: not in workshop mode)
		bool          hostRuns = false;    // HostRuns for that workbench
		bool          settlerShared = false;  // the session knows the settler (a plugin's NPC, or a spawn): the host's game can be told which
		Target        target = Target::kOwn;
		bool          editAllowed = false;  // the host's bSharedWorkshopEdit
		bool          pending = false;      // a request for this settler waits for its answer
	};

	enum class Give : std::uint8_t
	{
		kEngine,  // the game's own path: the script in our game assigns (our own world's settlement, or the rule not in force)
		kAsk,     // asked of the host's game; nothing is assigned here
		kRefuse,  // nothing is assigned and nothing is asked: the notice says why
	};

	struct Decision
	{
		Give        give = Give::kEngine;
		const char* notice = nullptr;  // the text key shown to the player (a refusal)
	};

	[[nodiscard]] constexpr bool IsCopy(Target a_target)
	{
		return a_target == Target::kHostsPiece || a_target == Target::kCopy;
	}

	// One rule holds everywhere: a game never gives one of its own world's settlers a copy of another player's piece (the
	// job would be written into its save against a reference that is nobody's there).
	[[nodiscard]] constexpr Decision OnGive(const Given& a_given)
	{
		if (a_given.command != COMMAND_ASSIGN || !a_given.hasTarget || !a_given.handsOver || a_given.bench == 0) {
			return { Give::kEngine, nullptr };
		}
		if (!a_given.hostRuns) {
			// Not known to be the host's: our own world's settlement, as before the rule.
			return IsCopy(a_given.target) ? Decision{ Give::kRefuse, "workshop.shared.assign_copy" } : Decision{ Give::kEngine, nullptr };
		}
		if (!a_given.settlerShared) {
			// A settler only our game has (made at runtime here): not one the host's game could be told about.
			return IsCopy(a_given.target) ? Decision{ Give::kRefuse, "workshop.shared.assign_not_hosts" } : Decision{ Give::kEngine, nullptr };
		}
		if (!a_given.editAllowed) {
			return { Give::kRefuse, "workshop.shared.assign_off" };
		}
		if (a_given.pending) {
			return { Give::kRefuse, "workshop.shared.assign_pending" };
		}
		if (a_given.target == Target::kHostsPiece || a_given.target == Target::kPlaced) {
			return { Give::kAsk, nullptr };
		}
		return { Give::kRefuse, "workshop.shared.assign_wrong_piece" };
	}

	// ---- The host's game: what it finds of the request ---------------------------------------------------------

	struct Facts
	{
		bool benchKnown = false;      // the workbench is one in the host's game
		bool hostOwned = false;       // ...and the host's player owns its settlement
		bool benchLoaded = false;     // ...and it is loaded there (the workshop's scripts work on loaded references)
		bool editAllowed = false;     // the host's bSharedWorkshopEdit
		bool settlerKnown = false;    // the settler is an actor of the host's game, alive, enabled
		bool settlerOfBench = false;  // ...linked to that workbench as one of its workshop's
		bool companion = false;       // ...and the player's current companion (the game's own command refuses those)
		bool pieceKnown = false;      // the piece is a reference of the host's game, neither deleted nor disabled
		bool pieceOfBench = false;    // ...one of that workbench's own pieces (no copy of a friend's, not the bench)
		bool assignable = false;      // ...a bed, or something a settler works at (the script assigns nothing else)
	};

	// Why the host's game refuses the request, or nothing: it assigns. In this order.
	[[nodiscard]] constexpr std::optional<Why> Refusal(const Facts& a_facts)
	{
		if (!a_facts.benchKnown || !a_facts.hostOwned) {
			return Why::kNotHostOwned;
		}
		if (!a_facts.benchLoaded) {
			return Why::kBenchNotLoaded;
		}
		if (!a_facts.editAllowed) {
			return Why::kNotAllowed;
		}
		if (!a_facts.settlerKnown || !a_facts.settlerOfBench || a_facts.companion) {
			return Why::kNoSuchSettler;
		}
		if (!a_facts.pieceKnown || !a_facts.pieceOfBench) {
			return Why::kNoSuchPiece;
		}
		if (!a_facts.assignable) {
			return Why::kNotAssignable;
		}
		return std::nullopt;
	}

	// The console line that has the workshop's own script assign (as the rig's steps type it; the game packs the arguments).
	[[nodiscard]] inline std::string AssignCommand(std::uint32_t a_actor, std::uint32_t a_piece)
	{
		return std::format("cqf WorkshopParent \"WorkshopParentScript.AssignActorToObjectPUBLIC\" {:08X} {:08X}", a_actor, a_piece);
	}

	// ---- The friend's game: the answer -----------------------------------------------------------------------

	struct Verdict
	{
		bool        benchNotOwned = false;  // the workbench is not the host's: remembered (the game's own path from then on)
		bool        benchOwned = false;     // the host's game ran the request: the workbench is the host's
		const char* notice = nullptr;       // the text key shown to the player
	};

	[[nodiscard]] constexpr Verdict Judge(Why a_why)
	{
		switch (a_why) {
		case Why::kApplied:
			return { false, true, "workshop.shared.assign_done" };
		case Why::kNotHostOwned:
			return { true, false, "workshop.shared.assign_not_owned" };
		case Why::kBenchNotLoaded:
		case Why::kHostAway:
		case Why::kOff:
			return { false, false, "workshop.shared.assign_host_away" };
		case Why::kNoAnswer:
			return { false, false, "workshop.shared.assign_no_answer" };
		case Why::kNotAllowed:
			return { false, false, "workshop.shared.assign_off" };
		case Why::kNoSuchSettler:
			return { false, false, "workshop.shared.assign_no_settler" };
		case Why::kNoSuchPiece:
			return { false, false, "workshop.shared.assign_no_piece" };
		case Why::kNotAssignable:
			return { false, false, "workshop.shared.assign_not_assignable" };
		default:
			return { false, false, "workshop.shared.assign_refused" };
		}
	}
}
