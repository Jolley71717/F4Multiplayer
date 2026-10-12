#pragma once

#include <cstddef>
#include <unordered_set>

// The host's world is the session's: an NPC already dead in it (killed before the session, or while
// nobody was joined, and saved that way) is dead for everyone, though no death event says so. The
// host's game finds those as they load and reports them (WorldSync). Its rules, kept free of the game
// so they can be tested.
namespace FoundDead
{
	// What the host's game knows of a dead NPC.
	struct Npc
	{
		bool unique = false;      // companions, quest characters (never used for stand-ins either)
		bool essential = false;   // the game keeps them alive: essential, or protected (only the player may kill them)
		bool questAlias = false;  // a quest holds them in an alias (as with the quest items every player keeps their own of)
	};

	// A character of a player's own story.
	constexpr bool IsStoryCharacter(const Npc& a_npc)
	{
		return a_npc.unique || a_npc.essential || a_npc.questAlias;
	}

	// Whether its death in the host's save is everyone's. With a shared story the host's story is the
	// session's. With everyone on their own, the host's save decides nothing about another player's story:
	// Danse dead there (after the Brotherhood's quests) lives on in a friend's game. A death seen happening
	// (a death event) is shared either way; this is only about what the save says.
	constexpr bool Shared(bool a_sharedStory, const Npc& a_npc)
	{
		return a_sharedStory || !IsStoryCharacter(a_npc);
	}

	// The actors whose 3D is loaded, kept from the game's load and unload events (checked for deaths when a
	// session starts). Should unload events go missing, the set is pruned when it reaches MAX_LOADED: the
	// ones a_isLoaded says are gone first, then any others, down to PRUNED_TO, so a set that stays full isn't
	// pruned again on every load. (The game never has that many actors loaded; this is a guard.)
	constexpr std::size_t MAX_LOADED = 1024;
	constexpr std::size_t PRUNED_TO = 768;

	template <class Id, class IsLoaded>
	void TrackLoad(std::unordered_set<Id>& a_loaded, Id a_id, IsLoaded a_isLoaded)
	{
		if (a_loaded.contains(a_id)) {
			return;
		}
		if (a_loaded.size() >= MAX_LOADED) {
			std::erase_if(a_loaded, [&](const Id& a_entry) { return !a_isLoaded(a_entry); });
			while (a_loaded.size() > PRUNED_TO) {
				a_loaded.erase(a_loaded.begin());
			}
		}
		a_loaded.insert(a_id);
	}
}
