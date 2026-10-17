#include "Test.h"

#include "game/QuestFilter.h"

namespace
{
	using QuestFilter::Quest;

	// Fallout4.esm's quests, as the game files them.
	constexpr std::uint32_t DC_GATE = 0x000190BC;  // DialogueDiamondCityEntrance: no type, no objectives
	constexpr std::uint32_t MQ104 = 0x0001F25E;    // Unlikely Valentine: main quest (type 1)
	constexpr std::uint32_t MS17 = 0x00022A08;     // Human Error: side quest (type 7)
	constexpr std::uint32_t INST305 = 0x000A274B;  // Powering Up: a faction's finale, filed under no type
	constexpr std::uint32_t BOS302 = 0x000B9F9D;   // Blind Betrayal
	constexpr std::uint32_t BOS304 = 0x00173ED9;   // Ad Victoriam
	constexpr std::uint32_t OTHER = 0x00012345;    // any other quest (only the finales are told by form ID)

	Quest Filed(std::uint32_t a_formId, std::int8_t a_type, bool a_hasObjectives, std::string_view a_editorId)
	{
		Quest quest;
		quest.formId = a_formId;
		quest.type = a_type;
		quest.hasObjectives = a_hasObjectives;
		quest.editorId = a_editorId;
		return quest;
	}
}

TEST("quest filter: the main story, the factions, side quests and the add-ons' quests are shared by type; internal and misc quests aren't")
{
	for (const int type : { 1, 2, 3, 4, 5, 7, 8, 9, 10, 11, 12 }) {
		CHECK(QuestFilter::IsSharedType(static_cast<std::int8_t>(type)));
	}
	CHECK(!QuestFilter::IsSharedType(0));
	CHECK(!QuestFilter::IsSharedType(6));
	CHECK(!QuestFilter::IsSharedType(-1));

	CHECK(QuestFilter::Watched(Filed(MQ104, 1, true, "MQ104")));
	CHECK(QuestFilter::Watched(Filed(MS17, 7, true, "MS17")));
	CHECK(!QuestFilter::Watched(Filed(OTHER, 6, true, "FFDiamondCity01")));
	CHECK(!QuestFilter::Watched(Filed(OTHER, 0, false, "Followers")));
}

TEST("quest filter: the factions' finales are shared though the game files them under no type")
{
	for (const auto id : { INST305, BOS302, BOS304 }) {
		CHECK(QuestFilter::IsFinale(Filed(id, 0, true, "")));
		CHECK(QuestFilter::Watched(Filed(id, 0, true, "")));
	}
	CHECK(!QuestFilter::IsFinale(Filed(OTHER, 0, false, "Followers")));
	CHECK(!QuestFilter::IsFinale(Filed(INST305, 7, true, "")));  // a typed quest is shared by its type
	CHECK(!QuestFilter::IsDialogue(Filed(INST305, 0, true, "Inst305")));
}

TEST("quest filter: a dialogue quest with no objectives is shared: Diamond City's entrance scene opens the gate for friends")
{
	const auto gate = Filed(DC_GATE, 0, false, "DialogueDiamondCityEntrance");
	CHECK(QuestFilter::IsDialogue(gate));
	CHECK(QuestFilter::Watched(gate));
	CHECK(!QuestFilter::IsFinale(gate));
	CHECK(!QuestFilter::IsSharedType(gate.type));

	// Any place's: the rule is the game's naming, not a list of form IDs.
	CHECK(QuestFilter::IsDialogue(Filed(OTHER, 0, false, "DialogueGoodneighbor")));
	CHECK(QuestFilter::IsDialogue(Filed(OTHER, 0, false, "DialogueCovenant")));
	CHECK(QuestFilter::IsDialogue(Filed(OTHER, 0, false, "DLC03DialogueFarHarbor")));
	CHECK(QuestFilter::IsDialogue(Filed(OTHER, 0, false, "DLC04_DialogueNukaTown")));
	CHECK(QuestFilter::IsDialogueName("Dialogue"));
}

TEST("quest filter: a dialogue quest with objectives, or one of the game's other untyped quests, isn't shared by that rule")
{
	// With objectives it is started by having one shown, like any quest; the type decides whether it is shared.
	CHECK(!QuestFilter::IsDialogue(Filed(DC_GATE, 0, true, "DialogueDiamondCityEntrance")));
	CHECK(!QuestFilter::Watched(Filed(DC_GATE, 0, true, "DialogueDiamondCityEntrance")));
	// A typed quest is shared by its type, not as a dialogue quest (it is started by an objective).
	CHECK(!QuestFilter::IsDialogue(Filed(DC_GATE, 7, false, "DialogueDiamondCityEntrance")));
	CHECK(QuestFilter::Watched(Filed(DC_GATE, 7, false, "DialogueDiamondCityEntrance")));
	// The game's other quests under no type are each game's own, or run in the background.
	for (const auto name : { "REChokepointCT01", "RESceneKMK01", "COMPiper", "TutorialQuest", "WorkshopParent", "DN015", "Followers", "MQ101", "DLC03", "DLC03_", "" }) {
		CHECK(!QuestFilter::IsDialogueName(name));
		CHECK(!QuestFilter::Watched(Filed(OTHER, 0, false, name)));
	}
	// The name must begin with the word, after an add-on's prefix only.
	CHECK(!QuestFilter::IsDialogueName("MQ101PrewarDialogue"));
	CHECK(!QuestFilter::IsDialogueName("COMDialogueCait"));
	CHECK(!QuestFilter::IsDialogueName("DLCDialogue"));  // not an add-on's prefix (no number)
}
