#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>

// Which settlement's junk a crafting station reads (chemistry, cooking, armor, weapons and the other workbench menus).
// Read from disk in 1.11.240: as a workbench menu opens (WorkbenchMenuBase, and each time it fills its lists again), the engine
// gathers what the recipes may use into a container of its own (WorkbenchMenuBase::sharedContainerRef). Its gather
// (2224943) asks Workshop::GetWorkshop (2194973) for the workbench's settlement: the reference the station is linked to
// with WorkshopItemKeyword (the engine's GetLinkedRef on it). With one, it takes the station's own inventory, the
// settlement's workshop container (Workshop::GetWorkshopContainer, 2195187: the workbench itself, a container) and those
// of the settlements its supply lines reach (Workshop::AddOtherContainers, by the workbench's location). With none, the
// station's inventory and the player's alone: the player's components are all that count.
//
// A friend's station is a copy here (WorkshopSync): placed by us, never linked to our workbench (the power audit takes
// any link the game makes off a copy, and that no link is what tells a copy from our own built piece). So crafting at a
// station a friend built read only our player's inventory, never the junk in our settlement's workbench (a 0.10.1
// player report: at the host's crafting stations only the host's own components counted). Their station is linked to
// the workbench their report names, the same form in our game (a copy is placed only beside one, WorkshopSync's
// InReach): the gather's question is answered with that workbench for our copies, as the engine answers it for a
// station of ours. Nothing is linked, so nothing of it is saved or seen by workshop mode.
//
// Kept free of the game so it can be tested; WorkshopSync holds the copies' workbenches and patches the call.
namespace CraftingWorkshop
{
	// The gather's call to Workshop::GetWorkshop(TESObjectREFR&) with the menu's workbench (WorkbenchMenuBase::workbenchRef):
	// 0x140B28540 + 0x10D in 1.11.240 (E8 to 0x140385A80).
	inline constexpr std::size_t GET_WORKSHOP_CALL = 0x10D;

	// The settlement the gather reads (its workbench's form ID; 0: none, only the station and the player). a_linked: the one
	// the engine found (the station's link). a_copyBench: for one of our copies of a friend's piece, the workbench their
	// report named (0 when the station isn't one). a_benchHere: that is a settlement's workbench in our game. The engine's
	// own answer always stands.
	[[nodiscard]] constexpr std::uint32_t Pick(std::uint32_t a_linked, std::uint32_t a_copyBench, bool a_benchHere)
	{
		if (a_linked != 0) {
			return a_linked;
		}
		return a_copyBench != 0 && a_benchHere ? a_copyBench : 0;
	}

	// Our copies of friends' pieces, by form ID, with the workbench each one's report named. Not locked: WorkshopSync keeps it
	// under the lock it keeps its copies' IDs under (the gather runs on the menu's thread).
	class Benches
	{
	public:
		void Note(std::uint32_t a_copy, std::uint32_t a_bench)
		{
			if (a_copy == 0) {
				return;
			}
			if (a_bench == 0) {
				byCopy.erase(a_copy);
				return;
			}
			byCopy[a_copy] = a_bench;
		}

		void Forget(std::uint32_t a_copy) { byCopy.erase(a_copy); }

		void Clear() { byCopy.clear(); }

		// The workbench the copy's report named; 0 if it is none of our copies.
		[[nodiscard]] std::uint32_t Of(std::uint32_t a_copy) const
		{
			const auto it = byCopy.find(a_copy);
			return it != byCopy.end() ? it->second : 0;
		}

		[[nodiscard]] std::size_t Size() const { return byCopy.size(); }

	private:
		std::unordered_map<std::uint32_t, std::uint32_t> byCopy;
	};
}
