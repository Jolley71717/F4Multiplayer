#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>

// A player whose own press on a memory's lounger took the controls and never seated them.
//
// Dangerous Minds' way into Kellogg's memory (Fallout4.esm, MQ202): a press on the memory lounger (the alias
// PlayerMemoryLounger, DefaultAliasOnActivate) sets 160 even with the lounger's activation blocked (155 blocks it), and
// 160's fragment makes an input layer (the quest script's MQ202EnableLayer), turns off the movement, the fighting, the
// menus (the Pip-Boy) and activation with it, blocks the lounger with its prompt hidden, and then seats the player itself
// (lounger.Activate(player, True)). Only MQ202PlayerScript.OnSit goes on from there: sat in that lounger with 155 done
// and 170 not, it sets 170, whose scene takes the player into the memory, where MQ203 gives everything back at the end.
// When the seat fails (the lounger taken: a friend's stand-in in it, which StandInSeat::NEVER_SEATS now keeps out), 170
// never comes, nothing gives the controls back, and a save made then loads the same way (the layer lives in the quest
// script's property; 160 is done, so a second press does nothing).
//
// So, from each game's start, in a session or not: once our player has been in the world in that state (160 done, 170
// not, the layer held, in no furniture) for GRACE without a break, the seat 160 asked for is asked again (the same
// lounger.Activate(player, True)), up to SEATS times RETRY apart: the player sits down and the memory goes on as their
// press meant. If none of those seats them, the press is undone (GiveBack): the layer is deleted (the controls back),
// 160 is no longer done (the quest's current stage back to the highest done below it), and the lounger blocked as 155
// left it (activation off, its prompt shown), with the two the fragment made ghosts (Amari, Nick) made mortal again: as
// before the press, so the next press runs 160 again. 160 marked done without its script (QuestSync's ShutsDoor, a
// friend's memory closed here) has no layer and is never touched. Kept free of the game so it can be tested.
namespace LoungerRescue
{
	using Clock = std::chrono::steady_clock;

	// A memory's way in whose seat is its own fragment's.
	struct Door
	{
		std::uint32_t                   quest;         // Fallout4.esm's form ID
		std::uint16_t                   sitStage;      // takes the controls and seats the player
		std::uint16_t                   seatedStage;   // set once the player sits (OnSit)
		std::string_view                lounger;       // the alias that holds the lounger
		std::string_view                layer;         // the quest script's property that holds the sit stage's layer
		std::array<std::string_view, 2> ghosts;        // the aliases whose actors the sit stage made ghosts
	};

	// Dangerous Minds (MQ202), Kellogg's memory. The Memory Den's own memory (FFGoodneighbor01 30) seats the player the
	// same way, but its lounger scene starts from 30 itself and takes the player in, seat or no seat: not a door here.
	constexpr std::array<Door, 1> DOORS{ {
		{ 0x000229E9, 160, 170, "PlayerMemoryLounger", "MQ202EnableLayer", { "Amari", "NickValentine" } },
	} };

	// How long the state must have held, without a break, before the seat is asked again: the fragment's own seat starts
	// the frame it runs (the player is on the way in, which is furniture already), so this is never one under way.
	constexpr auto GRACE = std::chrono::seconds(5);
	// Between the seats asked, and from the last to the give-back: a seat's entry takes a second or two.
	constexpr auto RETRY = std::chrono::seconds(4);
	constexpr int  SEATS = 3;

	// What the game says of a door, each look.
	struct Facts
	{
		bool inWorld = false;      // our player is in a cell with its 3D, no loading screen up, the game not paused
		bool sitDone = false;      // the sit stage is done
		bool seatedDone = false;   // the seated stage is done
		bool layerHeld = false;    // the quest script's layer property holds a layer (the sit stage's script ran)
		bool inFurniture = false;  // our player sits, lies, or is on the way into or out of furniture
	};

	// Between the sit stage's script and the seat: the sit stage ran (its layer held) and nothing seated the player yet.
	constexpr bool Pending(const Facts& a_facts)
	{
		return a_facts.sitDone && !a_facts.seatedDone && a_facts.layerHeld;
	}

	// Stuck: pending, in the world, and in no furniture.
	constexpr bool Stuck(const Facts& a_facts)
	{
		return a_facts.inWorld && Pending(a_facts) && !a_facts.inFurniture;
	}

	enum class Step
	{
		kNothing,
		kSeat,      // ask the lounger's seat again
		kGiveBack,  // undo the press (once a stuck spell)
	};

	class Guard
	{
	public:
		// A game starts (a new game, a load): looked at afresh.
		void Arm()
		{
			since.reset();
			seats = 0;
			gaveBack = false;
		}

		Step Look(const Facts& a_facts, Clock::time_point a_now)
		{
			if (!a_facts.inWorld) {
				return Step::kNothing;  // a load or a pause: neither ends the spell nor counts as a break
			}
			if (!Pending(a_facts)) {
				Arm();  // seated (the seated stage), undone, or never so: over
				return Step::kNothing;
			}
			if (a_facts.inFurniture) {
				since.reset();  // in it, or on the way: a seat under way. The seats asked still count: one that let the
				return Step::kNothing;  // player out again is not asked for ever
			}
			if (!since) {
				since = a_now;
				return Step::kNothing;
			}
			if (gaveBack || a_now - *since < GRACE) {
				return Step::kNothing;
			}
			if (seats > 0 && a_now - lastAsk < RETRY) {
				return Step::kNothing;
			}
			lastAsk = a_now;
			if (seats < SEATS) {
				++seats;
				return Step::kSeat;
			}
			gaveBack = true;
			return Step::kGiveBack;
		}

		[[nodiscard]] int  Seats() const { return seats; }
		[[nodiscard]] bool GaveBack() const { return gaveBack; }

	private:
		std::optional<Clock::time_point> since;
		Clock::time_point                lastAsk{};
		int                              seats = 0;
		bool                             gaveBack = false;
	};

	// The quest's current stage once the sit stage is no longer done: the highest stage done below it (the stage the
	// press came from), or the current one if that is below the sit stage already.
	template <class Range>
	constexpr std::uint16_t StageBefore(std::uint16_t a_current, std::uint16_t a_sitStage, const Range& a_done)
	{
		if (a_current < a_sitStage) {
			return a_current;
		}
		std::uint16_t best = 0;
		for (const auto stage : a_done) {
			if (stage < a_sitStage && stage > best) {
				best = stage;
			}
		}
		return best;
	}
}
