#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string_view>

// Which of the game's quests a session shares, by what the game's files say about each one: the type it
// is filed under, whether it has objectives, and the name its designers gave it. Kept free of the game so
// it can be tested; QuestSync reads these off TESQuest.
namespace QuestFilter
{
	// A quest as the filter sees it.
	struct Quest
	{
		std::uint32_t    formId = 0;
		std::int8_t      type = 0;               // QUEST_DATA::questType
		bool             hasObjectives = false;  // it has objectives at all, shown or not
		std::string_view editorId;               // the designers' name for it (TESQuest::formEditorID)
	};

	// QUEST_DATA::questType values that are shared: main quest, the four factions, side quests
	// and DLC quests. 0 is "none" (internal quests) and 6 is "miscellaneous" (radiant/ambient).
	inline bool IsSharedType(std::int8_t a_type)
	{
		return a_type > 0 && a_type != 6;
	}

	// The factions' last quests, which the game files under no type (0, like its internal quests). They
	// are shared with the rest of the story when the host shares it; their stages award achievements.
	// Fallout4.esm: Inst305 "Powering Up", BoS302 "Blind Betrayal", BoS304 "Ad Victoriam".
	inline constexpr std::array<std::uint32_t, 3> STORY_FINALES{ 0x000A274B, 0x000B9F9D, 0x00173ED9 };

	inline bool IsFinale(const Quest& a_quest)
	{
		return a_quest.type == 0 && std::ranges::find(STORY_FINALES, a_quest.formId) != STORY_FINALES.end();
	}

	// Whether this is a name the game gives its dialogue quests: Dialogue<Place> (DialogueDiamondCity,
	// DialogueGoodneighbor, DialogueDiamondCityEntrance), after the add-on's prefix in an add-on's files
	// (DLC03DialogueFarHarbor, DLC04_DialogueNukaTown).
	inline bool IsDialogueName(std::string_view a_editorId)
	{
		if (a_editorId.starts_with("DLC")) {
			auto        rest = a_editorId.substr(3);
			std::size_t digits = 0;
			while (digits < rest.size() && std::isdigit(static_cast<unsigned char>(rest[digits]))) {
				++digits;
			}
			if (digits > 0) {
				rest.remove_prefix(digits);
				if (rest.starts_with("_")) {
					rest.remove_prefix(1);
				}
				a_editorId = rest;
			}
		}
		return a_editorId.starts_with("Dialogue");
	}

	// A dialogue quest: what a place's people say and the scenes they play, which the game files under no
	// type (0) with no objectives (nothing of it is ever in the Pip-Boy), and names Dialogue<Place>. Some
	// of their stages change the world on the story's way: Diamond City's entrance scene (Piper and Danny
	// Sullivan at the intercom, DialogueDiamondCityEntrance) opens the city's gate at its 32, and without
	// it a friend's gate stays shut where the host's opened. So they are shared with the rest of the story
	// when the host shares it. With no objective ever shown, such a quest counts as started once it is
	// running or has stages done, its stages aren't steps in order (only one done here is past), and once
	// it ran and stopped it is over (see QuestSync). The game's other quests under no type stay its own:
	// its random encounters, companions' talks, tutorials and dungeons are each game's, or run in the
	// background with nothing to show for them.
	inline bool IsDialogue(const Quest& a_quest)
	{
		return a_quest.type == 0 && !a_quest.hasObjectives && IsDialogueName(a_quest.editorId);
	}

	// Quests whose progress is watched and shared: the shared types, the factions' finales and the
	// dialogue quests (the latter two with the story).
	inline bool Watched(const Quest& a_quest)
	{
		return IsSharedType(a_quest.type) || IsFinale(a_quest) || IsDialogue(a_quest);
	}
}
