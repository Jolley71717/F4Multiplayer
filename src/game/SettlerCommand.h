#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// Giving a settler a job in workshop mode (select the settler, then a crop, a bed or a post). The game runs the command on
// the settler in our world: it asks the settler's own AI, and the workshop's scripts assign the object there. A settler the
// game's data places (not one made at runtime) is shared, and run by one game; in every other game it is a mirror with its
// AI off, which takes no command. So while our player is in workshop mode at a settlement, its shared settlers are run by
// our game: taken over (kInteract, as talking to one is) and kept while the mode lasts. Game-free, for the tests.
namespace SettlerCommand
{
	// Takeovers asked for at most per round (NpcSync's talk claims, every 2 s): a settlement's settlers aren't all asked
	// for at once, and the one our player selects is usually among the first (each NPC waits 5 s between takeovers).
	inline constexpr std::size_t MAX_TAKEOVERS = 4;

	enum class Step : std::uint8_t
	{
		kNone,      // not one to command now
		kKeep,      // ours: claimed again, so nobody takes it over (nor is it handed off) while workshop mode lasts
		kTakeOver,  // another game runs it: taken over
	};

	// a_settlerBench: the workbench the settler is linked to as one of its workshop's (0: none, not a settler).
	// a_activeBench: the workbench of the settlement our player is in workshop mode at (0: not in workshop mode).
	// a_owner: the game that runs the NPC (0: nobody; ours is offered as usual).
	constexpr Step Decide(std::uint32_t a_settlerBench, std::uint32_t a_activeBench, std::uint32_t a_owner, std::uint32_t a_localId)
	{
		if (a_activeBench == 0 || a_settlerBench != a_activeBench || a_localId == 0 || a_owner == 0) {
			return Step::kNone;
		}
		return a_owner == a_localId ? Step::kKeep : Step::kTakeOver;
	}

	// The settlers (as Decide judged them, in scan order) to claim this round: every one we run, and the first
	// MAX_TAKEOVERS of the others.
	struct Round
	{
		std::vector<std::uint32_t> keep;
		std::vector<std::uint32_t> takeOver;
	};

	struct Judged
	{
		std::uint32_t id = 0;
		Step          step = Step::kNone;
	};

	inline Round Plan(const std::vector<Judged>& a_settlers)
	{
		Round round;
		for (const auto& settler : a_settlers) {
			if (settler.step == Step::kKeep) {
				round.keep.push_back(settler.id);
			} else if (settler.step == Step::kTakeOver && round.takeOver.size() < MAX_TAKEOVERS) {
				round.takeOver.push_back(settler.id);
			}
		}
		return round;
	}
}
