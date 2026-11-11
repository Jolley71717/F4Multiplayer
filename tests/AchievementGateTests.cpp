// Which Game.AddAchievement calls are a caught-up stage's and so dropped (game/AchievementGate.h): the quest fragment a
// call was made from, the stages a caught-up one marks (its own, and every award it reaches by stage sets:
// game/AchievementStages.h), that a live one takes its marks away, that a mark never outlives MARK_FOR, and the hook's
// own tries, give-up and start-over on a load.
#include "Test.h"

#include "game/AchievementGate.h"
#include "game/AchievementStages.h"
#include "game/StageRules.h"

#include <chrono>
#include <iterator>
#include <optional>
#include <tuple>
#include <vector>

namespace
{
	using namespace std::chrono_literals;
	using AchievementGate::AwardOf;
	using AchievementGate::Fragment;
	using AchievementGate::HookTries;
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
	constexpr Key MQ206_1200{ FALLOUT4, 0x0B1752, 1200 };
	constexpr Key MQ302_1000{ FALLOUT4, 0x0229EE, 1000 };
	constexpr Key BoS200_200{ FALLOUT4, 0x02BF20, 200 };
	constexpr Key BoS201_0{ FALLOUT4, 0x02BF21, 0 };
	constexpr Key BoSM04_1{ FALLOUT4, 0x0ADCE5, 1 };

	// The awards a stage reaches, as keys.
	std::vector<Key> Awards(Key a_key)
	{
		std::vector<Key> out;
		for (const auto& reach : Reaches(a_key)) {
			out.push_back(AwardOf(reach));
		}
		return out;
	}
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

TEST("achievement gate: a fragment's names are read in any case, as the VM interns them (the first spelling seen wins)")
{
	CHECK(Fragment("fragments:quests:qf_mq102_0001cc2a", "fragment_stage_0010_item_00") == MQ102_10);
	CHECK(Fragment("FRAGMENTS:QUESTS:QF_MQ102_0001CC2A", "FRAGMENT_STAGE_0010_ITEM_00") == MQ102_10);
	CHECK((Fragment("dlc03:Fragments:Quests:QF_DLC03MQ01_01001B3F", "Fragment_Stage_0500_Item_00") == Key{ COAST, 0x001B3F, 500 }));
	CHECK(!Fragment("dlc02:Fragments:Quests:QF_DLC02X_01000800", "Fragment_Stage_0010_Item_00"));  // still not a quest add-on
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

TEST("achievement stages: the table is sorted by file, quest, stage and award, and every award it names awards in its own fragment or is a quest never shared")
{
	const auto key = [](const AchievementStages::Reach& a_reach) {
		return std::tuple{ a_reach.file, a_reach.quest, a_reach.stage, a_reach.awardFile, a_reach.awardQuest, a_reach.awardStage };
	};
	for (std::size_t i = 1; i < std::size(AchievementStages::REACHES); ++i) {
		CHECK(key(AchievementStages::REACHES[i - 1]) < key(AchievementStages::REACHES[i]));
	}
	for (const auto& reach : AchievementStages::REACHES) {
		const auto award = AwardOf(reach);
		const auto own = Awards(award);
		CHECK(own.empty() || std::ranges::find(own, award) != own.end());  // no row of its own: DLC03_Achievements, never shared (denied by name), whose 67 Far Harbor's quests set
		CHECK(reach.file <= static_cast<std::uint8_t>(File::kNukaWorld));
		CHECK(reach.awardFile <= static_cast<std::uint8_t>(File::kNukaWorld));
	}
}

TEST("achievement stages: a stage reaches its own fragment's award, those its fragment sets, of its quest or another, and so on, else nothing")
{
	CHECK(Awards(MQ102_10) == std::vector{ MQ102_10 });
	CHECK(Awards(MQ104_205) == std::vector{ MQ104_210 });  // its own quest's
	CHECK(Awards(MQ201_150) == std::vector{ MQ106_2000 });  // another quest's
	CHECK((Awards(Key{ ROBOT, 0x0010F5, 0 }) == std::vector{ MQ102_10, Key{ ROBOT, 0x000806, 3000 }, Key{ ROBOT, 0x002833, 255 } }));  // Automatron's quest reaches the base game's and its own
	CHECK((Awards(Key{ COAST, 0x001B40, 650 }) == std::vector{ Key{ COAST, 0x057152, 67 } }));  // DLC03MQ02 650 -> DLC03_Achievements 67
	CHECK((Awards(BoSM04_1) == std::vector{ BoS200_200, MQ206_1200 }));  // two awards: BoS200 200 by stage sets, and MQ206 1200, which its fragment sets outright
	CHECK((Awards(Key{ FALLOUT4, 0x10C64A, 2000 }) == std::vector{ MQ302_1000 }));  // MQ302Min 2000 -> MQ302 1000: the ending
	CHECK(Awards(Key{ FALLOUT4, 0x01CC2A, 20 }).empty());  // Out of Time's 20 awards nothing
	CHECK(Awards(Key{ FALLOUT4, 0x0229E6, 410 }).empty());  // MQ105 410: a companion set, the game's own award, left out
	CHECK(Awards(Key{ NUKA, 0x01CC2A, 10 }).empty());  // MQ102's form ID in another file
}

TEST("achievement gate: a caught-up stage marks its own award and every one it reaches; a live stage marks nothing")
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
	CHECK(marks.CaughtUp(BoSM04_1, T0) == 3);  // itself and both awards it reaches
	CHECK(marks.Drops(BoS200_200, T0 + 1s));
	CHECK(marks.Drops(MQ206_1200, T0 + 1s));
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

TEST("achievement gate: a stage that comes live takes away the marks an earlier catch-up left on it, so its award is the game's")
{
	// A joiner caught up BoS201 0, which marks BoS200 200 (its fragment sets it, if the Brotherhood is met); the friend
	// then does BoS200 200 live within the mark's 5 minutes: it awards.
	Marks marks;
	CHECK(marks.CaughtUp(BoS201_0, T0) == 2);
	CHECK(marks.Drops(BoS200_200, T0 + 1min));
	CHECK(marks.Live(BoS200_200) == 1);
	CHECK(!marks.Drops(BoS200_200, T0 + 1min));
	CHECK(marks.Drops(BoS201_0, T0 + 1min));  // the caught-up stage's own mark stays
	CHECK(marks.Size() == 1);
	// A live stage takes away the marks on every award it reaches too: the game sets those stages from its fragment.
	CHECK(marks.CaughtUp(MQ106_2000, T0) == 1);
	CHECK(marks.Live(MQ201_150) == 1);  // never marked itself; reaches MQ106 2000
	CHECK(!marks.Drops(MQ106_2000, T0 + 1s));
	CHECK(marks.CaughtUp(BoSM04_1, T0) == 3);
	CHECK(marks.Live(BoSM04_1) == 3);
	CHECK(marks.Size() == 1);
	// Nothing marked: nothing to take away.
	CHECK(marks.Live(MQ102_10) == 0);
	CHECK(marks.Live(MQ104_205) == 0);
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

TEST("achievement hook: tried every RETRY until hooked, given up after TRIES in a row, and never while given up")
{
	HookTries tries;
	CHECK(tries.Due(T0));
	CHECK(!tries.Hooked());
	tries.Tried(T0);
	CHECK(!tries.Due(T0 + HookTries::RETRY - 1ms));
	CHECK(tries.Due(T0 + HookTries::RETRY));
	for (int n = 1; n < HookTries::TRIES; ++n) {
		CHECK(!tries.Failed());
	}
	CHECK(tries.Failed());  // the TRIES'th
	CHECK(tries.GivenUp());
	CHECK(tries.Tries() == HookTries::TRIES);
	CHECK(!tries.Due(T0 + 1h));
	CHECK(!tries.Failed());  // given up: nothing more counts
}

TEST("achievement hook: once hooked it is looked at again every RECHECK and a miss keeps it, as the object it holds is still ours")
{
	HookTries tries;
	tries.Tried(T0);
	CHECK(!tries.Failed());
	tries.Hooked(T0 + 5s);
	CHECK(tries.Hooked());
	CHECK(tries.Tries() == 0);
	CHECK(!tries.Due(T0 + 5s + HookTries::RETRY));
	CHECK(!tries.Due(T0 + 5s + HookTries::RECHECK - 1ms));
	CHECK(tries.Due(T0 + 5s + HookTries::RECHECK));
	tries.Tried(T0 + 5s + HookTries::RECHECK);
	CHECK(!tries.Failed());  // the type isn't there for a moment: still hooked
	CHECK(tries.Hooked());
	CHECK(!tries.GivenUp());
}

TEST("achievement hook: a load starts the tries over when it isn't hooked, given up included, and looks again at once when it is")
{
	HookTries tries;
	tries.Tried(T0);
	for (int n = 0; n < HookTries::TRIES; ++n) {
		tries.Failed();
	}
	CHECK(tries.GivenUp());
	CHECK(tries.Loaded());  // had given up: said so, for the log
	CHECK(!tries.GivenUp());
	CHECK(!tries.Hooked());
	CHECK(tries.Tries() == 0);
	CHECK(tries.Due(T0 + 1ms));  // at once
	// Not given up yet, a few tries in: starts over too.
	tries.Tried(T0 + 1h);
	tries.Failed();
	tries.Failed();
	CHECK(!tries.Loaded());
	CHECK(tries.Tries() == 0);
	CHECK(tries.Due(T0 + 1h + 1ms));
	// Hooked: stays hooked, looked at again at the next frame.
	tries.Hooked(T0 + 2h);
	CHECK(!tries.Due(T0 + 2h + 1s));
	CHECK(!tries.Loaded());
	CHECK(tries.Hooked());
	CHECK(tries.Due(T0 + 2h + 1s));
}
