// Which Game.AddAchievement calls are a caught-up stage's and so dropped (game/AchievementGate.h): the quest fragment a
// call was made from, the stages a caught-up one marks (its own, and the award it reaches by stage sets:
// game/AchievementStages.h), and that a mark never outlives MARK_FOR.
#include "Test.h"

#include "game/AchievementGate.h"
#include "game/AchievementStages.h"
#include "game/StageRules.h"

#include <chrono>
#include <optional>
#include <iterator>
#include <tuple>

namespace
{
	using namespace std::chrono_literals;
	using AchievementGate::Fragment;
	using AchievementGate::Key;
	using AchievementGate::Marks;
	using AchievementGate::Reaches;
	using Clock = AchievementGate::Clock;
	using StageRules::File;

	const Clock::time_point T0 = Clock::time_point{} + 1h;

	constexpr std::uint8_t FALLOUT4 = static_cast<std::uint8_t>(File::kFallout4);
	constexpr std::uint8_t COAST = static_cast<std::uint8_t>(File::kCoast);
	constexpr std::uint8_t ROBOT = static_cast<std::uint8_t>(File::kRobot);
	constexpr std::uint8_t NUKA = static_cast<std::uint8_t>(File::kNukaWorld);

	constexpr Key MQ102_10{ FALLOUT4, 0x01CC2A, 10 };
	constexpr Key MQ104_205{ FALLOUT4, 0x01F25E, 205 };
	constexpr Key MQ104_210{ FALLOUT4, 0x01F25E, 210 };
	constexpr Key MQ201_150{ FALLOUT4, 0x0229E8, 150 };
	constexpr Key MQ106_2000{ FALLOUT4, 0x0229E7, 2000 };
}

TEST("achievement gate: a quest stage fragment of the game's masters is parsed to its file, quest and stage")
{
	CHECK(Fragment("Fragments:Quests:QF_MQ102_0001CC2A", "Fragment_Stage_0010_Item_00") == MQ102_10);
	CHECK((Fragment("Fragments:Quests:QF_MM01Misc_0005DEE4", "Fragment_Stage_1500_Item_00") == Key{ FALLOUT4, 0x05DEE4, 1500 }));  // the fragment's name is not the quest's editor id
	CHECK((Fragment("Fragments:Quests:QF_MQ302_000229EE", "Fragment_Stage_1000_Item_01") == Key{ FALLOUT4, 0x0229EE, 1000 }));   // a later item of the stage
	CHECK((Fragment("DLC03:Fragments:Quests:QF_DLC03MQ01_01001B3F", "Fragment_Stage_0500_Item_00") == Key{ COAST, 0x001B3F, 500 }));  // the load index as compiled is masked off
	CHECK((Fragment("DLC01:Fragments:Quests:QF_DLC01MQ01_01000806", "Fragment_Stage_3000_Item_00") == Key{ ROBOT, 0x000806, 3000 }));
	CHECK((Fragment("DLC04:Fragments:Quests:QF_DLC04MQ04_01000804", "Fragment_Stage_0500_Item_00") == Key{ NUKA, 0x000804, 500 }));
	CHECK((Fragment("Fragments:Quests:QF_Big_00000001", "Fragment_Stage_10000_Item_00") == Key{ FALLOUT4, 0x000001, 10000 }));  // five digits
}

TEST("achievement gate: anything but a master's quest stage fragment is nothing: the game's call runs")
{
	CHECK(!Fragment("Fragments:Scenes:SF_MQ102_CodsworthSearch01_0001CC2B", "Fragment_0"));  // a scene's
	CHECK(!Fragment("Fragments:Terminals:TERM_MQ102Terminal_0001CC2C", "Fragment_Terminal_01"));
	CHECK(!Fragment("MQ102Script", "Fragment_Stage_0010_Item_00"));  // the quest's own script
	CHECK(!Fragment("Fragments:Quests:QF_MQ102_0001CC2A", "OnInit"));
	CHECK(!Fragment("Fragments:Quests:QF_MQ102_0001CC2A", "Fragment_Stage_0010"));  // no item
	CHECK(!Fragment("Fragments:Quests:QF_MQ102_0001CC2A", "Fragment_Stage__Item_00"));  // no stage
	CHECK(!Fragment("Fragments:Quests:QF_MQ102_0001CC2A", "Fragment_Stage_70000_Item_00"));  // not a stage
	CHECK(!Fragment("Fragments:Quests:QF_MQ102_0001CC2", "Fragment_Stage_0010_Item_00"));  // seven hex digits
	CHECK(!Fragment("Fragments:Quests:QF_MQ102_0001CC2G", "Fragment_Stage_0010_Item_00"));  // not hex
	CHECK(!Fragment("Fragments:Quests:QF_MQ1020001CC2A", "Fragment_Stage_0010_Item_00"));  // no underscore before the id
	CHECK(!Fragment("MyMod:Fragments:Quests:QF_MyQuest_01000800", "Fragment_Stage_0010_Item_00"));  // a mod's namespace
	CHECK(!Fragment("DLC02:Fragments:Quests:QF_DLC02X_01000800", "Fragment_Stage_0010_Item_00"));  // a workshop add-on: no quests shared
	CHECK(!Fragment("", "Fragment_Stage_0010_Item_00"));
	CHECK(!Fragment("Fragments:Quests:QF_MQ102_0001CC2A", ""));
}

TEST("achievement stages: the table is sorted by file, quest and stage, and every award it names awards in its own fragment or is a quest never shared")
{
	const auto key = [](const AchievementStages::Reach& a_reach) { return std::tuple{ a_reach.file, a_reach.quest, a_reach.stage }; };
	for (std::size_t i = 1; i < std::size(AchievementStages::REACHES); ++i) {
		CHECK(key(AchievementStages::REACHES[i - 1]) < key(AchievementStages::REACHES[i]));
	}
	for (const auto& reach : AchievementStages::REACHES) {
		const Key award{ reach.awardFile, reach.awardQuest, reach.awardStage };
		CHECK(!Reaches(award) || Reaches(award) == award);  // no row of its own: DLC03_Achievements, never shared (denied by name), whose 67 Far Harbor's quests set
		CHECK(reach.file <= static_cast<std::uint8_t>(File::kNukaWorld));
		CHECK(reach.awardFile <= static_cast<std::uint8_t>(File::kNukaWorld));
	}
}

TEST("achievement stages: a stage reaches its own fragment's award, the one its fragment sets, of its quest or another, else nothing")
{
	CHECK(Reaches(MQ102_10) == MQ102_10);
	CHECK(Reaches(MQ104_205) == MQ104_210);  // its own quest's
	CHECK(Reaches(MQ201_150) == MQ106_2000);  // another quest's
	CHECK((Reaches(Key{ ROBOT, 0x0010F5, 0 }) == MQ102_10));  // Automatron's quest reaches the base game's
	CHECK((Reaches(Key{ COAST, 0x001B40, 650 }) == Key{ COAST, 0x057152, 67 }));  // DLC03MQ02 650 -> DLC03_Achievements 67
	CHECK((!Reaches(Key{ FALLOUT4, 0x01CC2A, 20 })));  // Out of Time's 20 awards nothing
	CHECK((!Reaches(Key{ FALLOUT4, 0x0229E6, 410 })));  // MQ105 410: a companion set, the game's own award, left out
	CHECK((!Reaches(Key{ NUKA, 0x01CC2A, 10 })));  // MQ102's form ID in another file
}

TEST("achievement gate: a caught-up stage marks its own award and the one it reaches; a live stage marks nothing")
{
	Marks marks;
	CHECK(marks.CaughtUp(MQ104_205, T0) == 2);
	CHECK(marks.Size() == 2);
	CHECK(marks.Drops(MQ104_205, T0 + 1s));
	CHECK(marks.Drops(MQ104_210, T0 + 1s));
	CHECK(!marks.Drops(MQ102_10, T0 + 1s));  // never marked: the game's call runs
	CHECK(marks.CaughtUp(MQ102_10, T0) == 1);  // its own fragment awards: one mark
	CHECK((marks.CaughtUp(Key{ FALLOUT4, 0x01CC2A, 20 }, T0) == 1));  // no award known: marked all the same, should the table have missed one
	CHECK(marks.Size() == 4);
}

TEST("achievement gate: a mark drops the award until MARK_FOR after the stage was set, then the game's call runs again")
{
	Marks marks;
	marks.CaughtUp(MQ102_10, T0);
	CHECK(marks.Drops(MQ102_10, T0));
	CHECK(marks.Drops(MQ102_10, T0 + AchievementGate::MARK_FOR - 1ms));
	CHECK(*marks.Drops(MQ102_10, T0 + 90s) == 90s);  // how long ago, for the log
	CHECK(!marks.Drops(MQ102_10, T0 + AchievementGate::MARK_FOR));
	CHECK(!marks.Drops(MQ102_10, T0 + 1h));
	CHECK(marks.Size() == 1);  // still held until expired
	marks.Expire(T0 + AchievementGate::MARK_FOR - 1ms);
	CHECK(marks.Size() == 1);
	marks.Expire(T0 + AchievementGate::MARK_FOR);
	CHECK(marks.Size() == 0);
}

TEST("achievement gate: a stage marked again keeps its first mark, so the award's clock runs from when it was set")
{
	Marks marks;
	marks.CaughtUp(MQ201_150, T0);  // marks MQ106 2000 too
	CHECK(marks.CaughtUp(MQ106_2000, T0 + 4min) == 0);  // the friend's MQ106 2000 arrives after MQ201 150's script set it here
	CHECK(!marks.Drops(MQ106_2000, T0 + AchievementGate::MARK_FOR));
	CHECK(marks.CaughtUp(MQ201_150, T0 + 4min) == 0);
	CHECK(!marks.Drops(MQ201_150, T0 + AchievementGate::MARK_FOR));
}

TEST("achievement gate: a load forgets every mark: the save's script queue holds nothing set before it")
{
	Marks marks;
	marks.CaughtUp(MQ102_10, T0);
	marks.CaughtUp(MQ104_205, T0);
	marks.Clear();
	CHECK(marks.Size() == 0);
	CHECK(!marks.Drops(MQ102_10, T0 + 1s));
	CHECK(!marks.Drops(MQ104_210, T0 + 1s));
}
